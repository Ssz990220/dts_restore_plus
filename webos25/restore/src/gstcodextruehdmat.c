/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Local integration candidate. Rank NONE, explicit authorization required.
 * Packetizes TrueHD, optionally opens its own native discovery context, and
 * publishes policy proof. Never allocates RM ports, writes ALSA or installs.
 */
#include <gst/gst.h>
#ifdef __linux__
#include <unistd.h>
#endif
#include "truehd_mat_adapter.h"
#include "truehd_native.h"
#include "truehd_output.h"
#ifndef VERSION
#define VERSION "0.1.0-local"
#endif
#ifndef PACKAGE
#define PACKAGE "codex-truehd-local-candidate"
#endif
#define TYPE_CODEX_TRUEHD_MAT (codex_truehd_mat_get_type())
typedef struct {
  GstElement parent;
  GstPad *sink, *src;
  thd_mat *adapter;
  GstSegment segment;
  gboolean authorized, have_caps, have_segment, started, failed, atmos, saw_flush, output_configured;
  gint port, channels;
  guint epoch, fingerprint;
  guint64 tolerance, bursts;
  GstFlowReturn flow;
  GByteArray *pending;
  GstClockTime input_origin;
  guint64 input_aus;
  gboolean native_discovery;
  ThdNative *native;
  GRecMutex native_lock;
  gboolean automatic_port, managed_policy;
  gint flushing;
  ThdPolicy *policy;
  GRecMutex stream_lock;
} CodexTruehdMat;
typedef struct { GstElementClass parent; } CodexTruehdMatClass;
G_DEFINE_TYPE(CodexTruehdMat, codex_truehd_mat, GST_TYPE_ELEMENT)
enum { PROP_0, PROP_AUTH, PROP_PORT, PROP_TOLERANCE, PROP_BURSTS, PROP_ATMOS, PROP_NATIVE, PROP_AUTO_PORT, PROP_POLICY };
static GstStaticPadTemplate sink_template=GST_STATIC_PAD_TEMPLATE("sink",GST_PAD_SINK,GST_PAD_ALWAYS,
  GST_STATIC_CAPS("audio/x-true-hd, rate=(int)48000, channels=(int){6,8}"));
static GstStaticPadTemplate src_template=GST_STATIC_PAD_TEMPLATE("src",GST_PAD_SRC,GST_PAD_ALWAYS,
  GST_STATIC_CAPS("audio/x-raw, format=(string)S16LE, layout=(string)interleaved, rate=(int)48000, channels=(int){6,8}, codec=(uint)21, es=(boolean)true"));
static void reset(CodexTruehdMat *s) {
  if(s->adapter)thd_mat_destroy(s->adapter);
  s->adapter=NULL;s->started=FALSE;s->failed=FALSE;s->flow=GST_FLOW_OK;s->fingerprint=0;s->output_configured=FALSE;
  if(s->pending)g_byte_array_set_size(s->pending,0);
  s->input_origin=GST_CLOCK_TIME_NONE;s->input_aus=0;
}
static gboolean reject(CodexTruehdMat *s,const char *why) {
#ifdef __linux__
  if (s->managed_policy && !s->failed) {
    gchar *path = g_strdup_printf ("/run/dts25-truehd-es/seek-error-%ld", (long)getpid());
    gchar *message = g_strdup_printf ("%s; epoch=%u; input-aus=%" G_GUINT64_FORMAT
        "; segment-start=%" G_GUINT64_FORMAT "\n", why, s->epoch, s->input_aus, s->segment.start);
    g_file_set_contents (path, message, -1, NULL);
    g_free (message);
    g_free (path);
  }
#endif
  s->failed=TRUE;
  GST_ELEMENT_ERROR(s,STREAM,FORMAT,("TrueHD ES candidate rejected input"),("%s",why));
  return FALSE;
}
static gboolean push_caps(CodexTruehdMat *s) {
  gint ch=s->channels;
  GstCaps *out=gst_caps_new_simple("audio/x-raw","format",G_TYPE_STRING,"S16LE","layout",G_TYPE_STRING,"interleaved","rate",G_TYPE_INT,48000,"channels",G_TYPE_INT,ch,
    "channel-mask",GST_TYPE_BITMASK,(guint64)(ch==8?0xc3f:0x3f),"codec",G_TYPE_UINT,21u,"codec-str",G_TYPE_STRING,"mat","acmod",G_TYPE_UINT,ch==8?21u:7u,
    "out-channels",G_TYPE_INT,8,"atmos",G_TYPE_BOOLEAN,s->atmos,"es",G_TYPE_BOOLEAN,TRUE,"data-channels",G_TYPE_INT,8,"data-bps",G_TYPE_INT,16,"data-rate",G_TYPE_INT,192000,"clock-x4",G_TYPE_BOOLEAN,FALSE,NULL);
  gboolean ok=gst_pad_push_event(s->src,gst_event_new_caps(out));gst_caps_unref(out);return ok;
}
static int emit(void *opaque,const uint8_t *data,size_t size,uint64_t pts,uint64_t duration,uint32_t epoch,uint32_t flags) {
  CodexTruehdMat *s=opaque;(void)epoch;
  if(g_atomic_int_get(&s->flushing)){s->flow=GST_FLOW_FLUSHING;return -1;}
  if(s->automatic_port&&s->port<0){
    s->port=thd_linked_audio_port(s->src);
    if(s->port<0){reject(s,"linked native sink has no granted ADEC port");return -1;}
    gst_element_post_message(GST_ELEMENT(s),gst_message_new_element(GST_OBJECT(s),gst_structure_new("codex-truehd-port-resolved","audio-port",G_TYPE_INT,s->port,NULL)));
  }
  if(!s->authorized||s->port<0||s->failed)return -1;
  g_rec_mutex_lock(&s->native_lock);
  if(s->managed_policy&&!s->policy){
    s->policy=thd_policy_open(GST_ELEMENT(s),s->port,s->channels);
    if(!s->policy){g_rec_mutex_unlock(&s->native_lock);reject(s,"private policy proof unavailable");return -1;}
  }
  if(s->native_discovery){
    if(!s->native)s->native=thd_native_open(GST_ELEMENT(s),s->port,s->channels);
    if(!s->native||!thd_native_feed(s->native,data,size,pts,s->atmos,s->channels)){
      g_rec_mutex_unlock(&s->native_lock);reject(s,"native MAT discovery failed");return -1;
    }
    if(s->managed_policy&&thd_native_ready(s->native)&&
       (!thd_policy_ready(s->policy)||!thd_policy_poll(s->policy,s->src))){
      g_rec_mutex_unlock(&s->native_lock);reject(s,"managed route acknowledgement failed");return -1;
    }
  }
  g_rec_mutex_unlock(&s->native_lock);
  if(g_atomic_int_get(&s->flushing)){s->flow=GST_FLOW_FLUSHING;return -1;}
  GstBuffer *b=gst_buffer_new_allocate(NULL,size,NULL);
  if(!b)return -1;
  if(gst_buffer_fill(b,0,data,size)!=size){gst_buffer_unref(b);return -1;}
  GST_BUFFER_PTS(b)=pts;GST_BUFFER_DTS(b)=pts;GST_BUFFER_DURATION(b)=duration;
  GST_BUFFER_FLAGS(b)|=flags;
  s->flow=gst_pad_push(s->src,b);
  if(s->flow!=GST_FLOW_OK)return -1;
  s->bursts++;return 0;
}
static guint checksum(const guint8 *h,gsize n) {
  guint crc=0;
  for(gsize i=0;i<n-4;i++){
    crc^=(guint)h[i]<<8;
    for(guint b=0;b<8;b++)crc=((crc&0x8000)?(crc<<1)^0x2d:crc<<1)&0xffff;
  }
  return (((crc&255)<<8)|(crc>>8))^((guint)h[n-4]|((guint)h[n-3]<<8));
}
static gboolean validate_major(CodexTruehdMat *s,const guint8 *au,gsize n) {
  if(n<8||memcmp(au+4,"\xf8\x72\x6f\xba",4))return s->started?TRUE:reject(s,"first AU needs TrueHD major sync");
  const guint8 *m=au+4;gsize z=n-4,hs=28;
  if(z<28)return reject(s,"truncated major sync");
  if(m[25]&1){if(z<29)return reject(s,"truncated extension");hs+=2+(m[26]>>4)*2;}
  if(z<hs||checksum(m,hs)!=((guint)m[hs-2]|((guint)m[hs-1]<<8)))return reject(s,"major sync checksum");
  guint bits=((guint)m[4]<<24)|((guint)m[5]<<16)|((guint)m[6]<<8)|m[7];
  guint map=(bits&8191)?(bits&8191):((bits>>15)&31),ratebits=bits>>28,sub=m[16]>>4;
  gboolean atmos=(m[17]&128)!=0;
  if(ratebits||map!=(s->channels==8?0x4f:0xf)||sub<1||sub>4||(atmos&&sub!=4))return reject(s,"unverified rate/layout/substreams");
  guint fp=map|(sub<<13)|((guint)atmos<<17);
  if(s->fingerprint&&s->fingerprint!=fp)return reject(s,"format change requires flushed segment");
  s->fingerprint=fp;s->atmos=atmos;return TRUE;
}
static GstFlowReturn chain_unlocked(GstPad *pad,GstObject *parent,GstBuffer *b) {
  CodexTruehdMat *s=(CodexTruehdMat*)parent;GstMapInfo map;GstFlowReturn result=GST_FLOW_ERROR;(void)pad;
  if(s->failed||!s->authorized||(s->port<0&&!s->automatic_port)||!s->have_caps||!s->have_segment){reject(s,"not authorized/configured/segmented");goto done;}
  if(!GST_BUFFER_PTS_IS_VALID(b)&&!GST_CLOCK_TIME_IS_VALID(s->input_origin)){reject(s,"missing source media PTS");goto done;}
  if(s->started&&GST_BUFFER_FLAG_IS_SET(b,GST_BUFFER_FLAG_DISCONT)){reject(s,"DISCONT needs FLUSH/new segment");goto done;}
  if(!gst_buffer_map(b,&map,GST_MAP_READ))goto done;
  if(map.size>1048576){reject(s,"oversized TrueHD input chunk");goto unmap;}
  if(!GST_CLOCK_TIME_IS_VALID(s->input_origin))s->input_origin=GST_BUFFER_PTS(b);
  else if(!s->pending->len&&GST_BUFFER_PTS_IS_VALID(b)){
    guint64 expected=s->input_origin+gst_util_uint64_scale(s->input_aus,40000000000ULL,48000);
    guint64 observed=GST_BUFFER_PTS(b),diff=expected>observed?expected-observed:observed-expected;
    if(diff>s->tolerance){reject(s,"unflushed source PTS discontinuity");goto unmap;}
  }
  g_byte_array_append(s->pending,map.data,map.size);
  while(s->pending->len>=2){
    guint n=((((guint)s->pending->data[0]<<8)|s->pending->data[1])&4095)*2;
    guint64 pts=s->input_origin+gst_util_uint64_scale(s->input_aus,40000000000ULL,48000);
    if(n<10||n>8190){reject(s,"invalid TrueHD AU length");goto unmap;}
    if(s->pending->len<n)break;
    /* Container seeks may land before a major header. Discard whole AUs
     * only, advancing their original sample clock; never invent a header. */
    if(!s->started&&memcmp(s->pending->data+4,"\xf8\x72\x6f\xba",4)){
      g_byte_array_remove_range(s->pending,0,n);s->input_aus++;
      if(s->input_aus>256){reject(s,"major sync not found in bounded seek preroll");goto unmap;}
      continue;
    }
    if(!validate_major(s,s->pending->data,n))goto unmap;
    if(!s->output_configured){
    /* Negotiate from the verified major header, not a guessed Atmos hint.
     * CAPS must precede the saved SEGMENT and every encoded buffer. */
    if(!push_caps(s)||!gst_pad_push_event(s->src,gst_event_new_segment(&s->segment))){reject(s,"native ES metadata rejected");goto unmap;}
      s->output_configured=TRUE;
    }
    if(!s->adapter){
    s->adapter=thd_mat_create(s->channels,emit,s);
      if(!s->adapter||thd_mat_authorize(s->adapter,21,1,8,16,192000,0,0)||thd_mat_new_segment(s->adapter,pts,++s->epoch)){reject(s,"adapter initialization");goto unmap;}
    }
    /* Discarded preroll contributes to the input clock but not to the
     * adapter clock. The two integer divisions of the same sample offset
     * can differ by one nanosecond. Incoming PTS were checked above. */
    if(thd_mat_push_au(s->adapter,s->pending->data,n,pts,1)){
    if(s->flow!=GST_FLOW_FLUSHING)reject(s,thd_mat_error(s->adapter));result=s->flow==GST_FLOW_OK?GST_FLOW_ERROR:s->flow;goto unmap;
    }
    s->started=TRUE;g_byte_array_remove_range(s->pending,0,n);s->input_aus++;
  }
  result=GST_FLOW_OK;
unmap:gst_buffer_unmap(b,&map);
done:gst_buffer_unref(b);return result;
}
static GstFlowReturn chain(GstPad *pad,GstObject *parent,GstBuffer *b) {
  CodexTruehdMat *s=(CodexTruehdMat*)parent;GstFlowReturn result;
  g_rec_mutex_lock(&s->stream_lock);
  if(g_atomic_int_get(&s->flushing)){gst_buffer_unref(b);result=GST_FLOW_FLUSHING;}
  else result=chain_unlocked(pad,parent,b);
  g_rec_mutex_unlock(&s->stream_lock);return result;
}
static gboolean event_unlocked(GstPad *pad,GstObject *parent,GstEvent *e) {
  CodexTruehdMat *s=(CodexTruehdMat*)parent;(void)pad;
  switch(GST_EVENT_TYPE(e)){
  case GST_EVENT_CAPS:{
    GstCaps *caps;gst_event_parse_caps(e,&caps);const GstStructure *st=gst_caps_get_structure(caps,0);gint rate,ch;
    if(s->started||!gst_structure_has_name(st,"audio/x-true-hd")||!gst_structure_get_int(st,"rate",&rate)||rate!=48000||!gst_structure_get_int(st,"channels",&ch)||(ch!=6&&ch!=8)){gst_event_unref(e);return reject(s,"unsupported input caps or unflushed format change");}
    if(s->native&&s->channels!=ch){thd_policy_close(&s->policy);thd_native_close(&s->native);}
    s->channels=ch;s->have_caps=TRUE;
    s->atmos=FALSE;s->output_configured=FALSE;gst_event_unref(e);return TRUE;
  }
  case GST_EVENT_SEGMENT:{
    const GstSegment *seg;gst_event_parse_segment(e,&seg);
    if(seg->format!=GST_FORMAT_TIME||seg->rate!=1.0||seg->applied_rate!=1.0||(s->started&&!s->saw_flush)){gst_event_unref(e);return reject(s,"segment needs normal rate and explicit flush");}
    reset(s);s->segment=*seg;s->have_segment=TRUE;s->saw_flush=FALSE;gst_event_unref(e);return TRUE;
  }
  case GST_EVENT_FLUSH_STOP:reset(s);s->have_segment=FALSE;s->saw_flush=TRUE;break;
  case GST_EVENT_EOS:
    if(s->pending->len){gst_event_unref(e);return reject(s,"truncated TrueHD AU at EOS");}
    if(s->adapter&&!s->failed&&thd_mat_finish(s->adapter)){gst_event_unref(e);return reject(s,thd_mat_error(s->adapter));}
    break;
  default:break;
  }
  return gst_pad_push_event(s->src,e);
}
static gboolean event(GstPad *pad,GstObject *parent,GstEvent *e) {
  CodexTruehdMat *s=(CodexTruehdMat*)parent;gboolean ok;
  if(GST_EVENT_TYPE(e)==GST_EVENT_FLUSH_START){
    /* FLUSH_START is not serialized: unblock a downstream push before waiting
     * for the chain's lock, then reset its adapter/pending bytes exclusively. */
    g_atomic_int_set(&s->flushing,TRUE);
    ok=gst_pad_push_event(s->src,e);
    g_rec_mutex_lock(&s->stream_lock);g_rec_mutex_lock(&s->native_lock);
    thd_policy_close(&s->policy);
    gboolean native_ok=thd_native_flush(s->native);
    reset(s);s->have_segment=FALSE;s->saw_flush=TRUE;
    if(!native_ok)ok=reject(s,"native flushing seek failed");
    g_rec_mutex_unlock(&s->native_lock);g_rec_mutex_unlock(&s->stream_lock);
    return ok;
  }
  gboolean ending_flush=GST_EVENT_TYPE(e)==GST_EVENT_FLUSH_STOP;
  g_rec_mutex_lock(&s->stream_lock);ok=event_unlocked(pad,parent,e);
  if(ending_flush)g_atomic_int_set(&s->flushing,FALSE);
  g_rec_mutex_unlock(&s->stream_lock);return ok;
}
static void set_property(GObject *o,guint id,const GValue *v,GParamSpec *p) {
  CodexTruehdMat *s=(CodexTruehdMat*)o;
  if(GST_STATE(s)>GST_STATE_READY){GST_WARNING_OBJECT(s,"configuration is immutable while streaming");return;}
  switch(id){case PROP_AUTH:s->authorized=g_value_get_boolean(v);break;case PROP_PORT:s->port=g_value_get_int(v);break;case PROP_AUTO_PORT:s->automatic_port=g_value_get_boolean(v);break;case PROP_POLICY:s->managed_policy=g_value_get_boolean(v);break;case PROP_NATIVE:s->native_discovery=g_value_get_boolean(v);break;case PROP_TOLERANCE:s->tolerance=g_value_get_uint64(v);break;default:G_OBJECT_WARN_INVALID_PROPERTY_ID(o,id,p);}
}
static void get_property(GObject *o,guint id,GValue *v,GParamSpec *p) {
  CodexTruehdMat *s=(CodexTruehdMat*)o;
  switch(id){case PROP_AUTH:g_value_set_boolean(v,s->authorized);break;case PROP_PORT:g_value_set_int(v,s->port);break;case PROP_AUTO_PORT:g_value_set_boolean(v,s->automatic_port);break;case PROP_POLICY:g_value_set_boolean(v,s->managed_policy);break;case PROP_NATIVE:g_value_set_boolean(v,s->native_discovery);break;case PROP_TOLERANCE:g_value_set_uint64(v,s->tolerance);break;case PROP_BURSTS:g_value_set_uint64(v,s->bursts);break;case PROP_ATMOS:g_value_set_boolean(v,s->atmos);break;default:G_OBJECT_WARN_INVALID_PROPERTY_ID(o,id,p);}
}
static GstStateChangeReturn change_state(GstElement *e,GstStateChange t) {
  CodexTruehdMat *s=(CodexTruehdMat*)e;
  if(t==GST_STATE_CHANGE_READY_TO_PAUSED&&(!s->authorized||(s->port<0&&!s->automatic_port)||(s->managed_policy&&!s->native_discovery))){reject(s,"explicit authorization and granted port/native policy conditions required");return GST_STATE_CHANGE_FAILURE;}
  GstStateChangeReturn r=GST_ELEMENT_CLASS(codex_truehd_mat_parent_class)->change_state(e,t);
  if(t==GST_STATE_CHANGE_PAUSED_TO_READY){g_rec_mutex_lock(&s->stream_lock);g_rec_mutex_lock(&s->native_lock);thd_policy_close(&s->policy);thd_native_close(&s->native);g_rec_mutex_unlock(&s->native_lock);reset(s);s->have_segment=FALSE;s->have_caps=FALSE;s->saw_flush=FALSE;if(s->automatic_port)s->port=-1;g_atomic_int_set(&s->flushing,FALSE);g_rec_mutex_unlock(&s->stream_lock);}
  return r;
}
static void dispose(GObject *o){CodexTruehdMat*s=(CodexTruehdMat*)o;g_rec_mutex_lock(&s->stream_lock);thd_policy_close(&s->policy);thd_native_close(&s->native);g_rec_mutex_unlock(&s->stream_lock);G_OBJECT_CLASS(codex_truehd_mat_parent_class)->dispose(o);}
static void finalize(GObject *o){CodexTruehdMat*s=(CodexTruehdMat*)o;reset(s);g_byte_array_unref(s->pending);g_rec_mutex_clear(&s->native_lock);g_rec_mutex_clear(&s->stream_lock);G_OBJECT_CLASS(codex_truehd_mat_parent_class)->finalize(o);}
static void codex_truehd_mat_class_init(CodexTruehdMatClass *c) {
  GObjectClass *o=G_OBJECT_CLASS(c);GstElementClass *e=GST_ELEMENT_CLASS(c);o->set_property=set_property;o->get_property=get_property;o->dispose=dispose;o->finalize=finalize;e->change_state=change_state;
  g_object_class_install_property(o,PROP_AUTH,g_param_spec_boolean("authorize-es","Explicit ES authorization","Caller must own native audio resource and enforce receiver muting",FALSE,G_PARAM_READWRITE|GST_PARAM_MUTABLE_READY|G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(o,PROP_PORT,g_param_spec_int("resource-port","Owned resource port","No allocation or ownership claim is made by this element",-1,1,-1,G_PARAM_READWRITE|GST_PARAM_MUTABLE_READY|G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(o,PROP_AUTO_PORT,g_param_spec_boolean("automatic-resource","Follow granted sink port","Read index only from this decoder's unambiguous linked native audio sink",FALSE,G_PARAM_READWRITE|GST_PARAM_MUTABLE_READY|G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(o,PROP_POLICY,g_param_spec_boolean("managed-policy","Plugin policy handshake","Publish private native format proof and refresh only after the root policy watcher acknowledges",FALSE,G_PARAM_READWRITE|GST_PARAM_MUTABLE_READY|G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(o,PROP_NATIVE,g_param_spec_boolean("native-discovery","Native MAT discovery","Exact-hash C5 native context on the caller-granted port; no global routing changes",FALSE,G_PARAM_READWRITE|GST_PARAM_MUTABLE_READY|G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(o,PROP_TOLERANCE,g_param_spec_uint64("pts-tolerance-ns","Container PTS tolerance","Maximum observed input timestamp quantization; discontinuities still require flush",0,1000000,0,G_PARAM_READWRITE|GST_PARAM_MUTABLE_READY|G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(o,PROP_BURSTS,g_param_spec_uint64("bursts","Bursts emitted","Successful encoded output buffers",0,G_MAXUINT64,0,G_PARAM_READABLE|G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(o,PROP_ATMOS,g_param_spec_boolean("atmos-present","Atmos bitstream indication","16-channel presentation flag; no receiver/rendering claim",FALSE,G_PARAM_READABLE|G_PARAM_STATIC_STRINGS));
  gst_element_class_add_static_pad_template(e,&sink_template);gst_element_class_add_static_pad_template(e,&src_template);
  gst_element_class_set_static_metadata(e,"Experimental explicit TrueHD MAT/ES packetizer","Codec/Parser/Audio","Preserves all TrueHD substreams; does not configure output routing","Codex local prototype");
}
static void codex_truehd_mat_init(CodexTruehdMat *s) {
  s->port=-1;s->flow=GST_FLOW_OK;gst_segment_init(&s->segment,GST_FORMAT_TIME);
  s->pending=g_byte_array_new();s->input_origin=GST_CLOCK_TIME_NONE;g_rec_mutex_init(&s->native_lock);g_rec_mutex_init(&s->stream_lock);
  s->sink=gst_pad_new_from_static_template(&sink_template,"sink");s->src=gst_pad_new_from_static_template(&src_template,"src");
  gst_pad_set_chain_function(s->sink,GST_DEBUG_FUNCPTR(chain));gst_pad_set_event_function(s->sink,GST_DEBUG_FUNCPTR(event));
  gst_element_add_pad(GST_ELEMENT(s),s->sink);gst_element_add_pad(GST_ELEMENT(s),s->src);
}
static gboolean plugin_init(GstPlugin *p){return gst_element_register(p,"codextruehdmat",GST_RANK_NONE,TYPE_CODEX_TRUEHD_MAT);}
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,GST_VERSION_MINOR,codextruehdmat,"Explicit local TrueHD MAT/ES candidate",plugin_init,VERSION,"LGPL",PACKAGE,"https://gstreamer.freedesktop.org/")
