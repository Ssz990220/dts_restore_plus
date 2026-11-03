/* SPDX-License-Identifier: LGPL-2.1-or-later
 * MAT placement derives from FFmpeg n9.0.2 libavformat/spdifenc.c.
 * Copyright 2009 Bartlomiej Wolowiec; 2010/2020 Anssi Hannula;
 * 2010 Carl Eugen Hoyos. See ../truehd-es-prototype/reference/.
 * Added: strict ES authorization, shared-media PTS checking, explicit epochs,
 * bounded input, deterministic EOS, and fail-closed callbacks. No device I/O,
 * configuration changes, sample-volume operations, or decode fallback.
 */
#include "truehd_mat_adapter.h"
#include <stdlib.h>
#include <string.h>
#define MAT_SIZE 61424u
#define BURST_SIZE 61440u
#define ES_FLAG 0x200000u
#define NONE UINT64_MAX
static const uint8_t start_code[]={0x07,0x9e,0,3,0x84,1,1,1,0x80,0,0x56,0xa5,0x3b,0xf4,0x81,0x83,0x49,0x80,0x77,0xe0};
static const uint8_t middle_code[]={0xc3,0xc1,0x42,0x49,0x3b,0xfa,0x82,0x83,0x49,0x80,0x77,0xe0};
static const uint8_t end_code[]={0xc3,0xc2,0xc0,0xc4,0,0,0,0,0,0,0,0,0,0,0x97,0x11};
_Static_assert(sizeof(start_code)==20 && sizeof(middle_code)==12 && sizeof(end_code)==16,"MAT marker lengths");
static const unsigned marker_pos[]={0,30708,61408};
static const unsigned marker_size[]={sizeof(start_code),sizeof(middle_code),sizeof(end_code)};
static const uint8_t *const marker[]={start_code,middle_code,end_code};
struct thd_mat {
    thd_mat_output output; void *opaque;
    unsigned channels,authorized,ready,closed,failed,epoch,rate,samples;
    unsigned prev_time,prev_size,have_prev,used,has_audio,output_time,have_output;
    uint64_t origin,au_count,burst_count;
    uint8_t frame[BURST_SIZE];
    const char *error;
};
static int fail(thd_mat *s,const char *e){if(s){s->failed=1;s->authorized=0;s->error=e;}return -1;}
uint32_t thd_mat_abi(void){return 0x00010000u;}
thd_mat *thd_mat_create(unsigned ch,thd_mat_output out,void *arg){
    if((ch!=6&&ch!=8)||!out)return NULL;
    thd_mat *s=calloc(1,sizeof(*s));if(s){s->channels=ch;s->output=out;s->opaque=arg;s->error="none";}return s;
}
void thd_mat_destroy(thd_mat *s){if(s){memset(s,0,sizeof(*s));free(s);}}
int thd_mat_authorize(thd_mat *s,unsigned codec,unsigned type,unsigned ch,unsigned bits,unsigned rate,int bypass,int multi){
    if(!s||s->failed)return -1;
    if(codec!=21||type!=1||ch!=8||bits!=16||rate!=192000||bypass||multi)
        return fail(s,"requires explicit native MAT/ES, 8x16x192k, both PCM modes off");
    s->authorized=1;return 0;
}
int thd_mat_new_segment(thd_mat *s,uint64_t origin,uint32_t epoch){
    if(!s||!s->authorized||s->failed||origin==NONE)return -1;
    if(s->ready&&epoch<=s->epoch)return fail(s,"segment epoch must increase after flush");
    s->ready=1;s->origin=origin;s->epoch=epoch;s->rate=0;s->samples=0;
    s->closed=s->have_prev=s->used=s->has_audio=s->have_output=0;
    s->au_count=s->burst_count=0;memset(s->frame,0,sizeof(s->frame));return 0;
}
static unsigned be16(const uint8_t *p){return (unsigned)p[0]*256+p[1];}
static int bits(const uint8_t *p,size_t n,size_t *cursor,unsigned count,unsigned *out){
    if(*cursor+count>n*8)return -1;
    unsigned v=0;while(count--){v=(v<<1)|((p[*cursor/8]>>(7-*cursor%8))&1);++*cursor;}
    *out=v;return 0;
}
static int restart_time(const uint8_t *au,size_t n,unsigned *out){
    if(n<31)return 0;
    const uint8_t *d=au+4;size_t len=n-4,major=28,c=0;unsigned v,nsub=d[16]>>4;
    if(d[3]==0xba&&(d[25]&1))major+=2+(d[26]>>4)*2;
    if(len<major||!nsub)return 0;
    d+=major;len-=major;
    for(unsigned i=0;i<nsub;i++){
        unsigned extra;
        if(bits(d,len,&c,1,&extra)||bits(d,len,&c,15,&v))return 0;
        if(extra&&bits(d,len,&c,16,&v))return 0;
    }
    if(bits(d,len,&c,1,&v)||!v||bits(d,len,&c,1,&v)||!v)return 0;
    if(bits(d,len,&c,13,&v)||v!=(0x31ea>>1)||bits(d,len,&c,1,&v)||bits(d,len,&c,16,&v))return 0;
    *out=v;return 1;
}
static int emit(thd_mat *s){
    static const uint8_t preamble[]={0xf8,0x72,0x4e,0x1f,0,0x16,0xef,0xf0};
    if(s->used!=MAT_SIZE||!s->authorized||s->failed)return fail(s,"incomplete or unauthorized ES frame");
    memcpy(s->frame,preamble,8);
    for(unsigned i=0;i<3;i++)if(memcmp(s->frame+8+marker_pos[i],marker[i],marker_size[i]))return fail(s,"MAT marker mismatch");
    for(unsigned i=BURST_SIZE-8;i<BURST_SIZE;i++)if(s->frame[i])return fail(s,"nonzero tail");
    if(s->burst_count>(NONE-s->origin)/20000000u)return fail(s,"PTS overflow");
    uint64_t pts=s->origin+s->burst_count*20000000u;
    if(s->output(s->opaque,s->frame,BURST_SIZE,pts,20000000u,s->epoch,ES_FLAG))return fail(s,"ES output rejected; no PCM fallback");
    ++s->burst_count;s->used=s->has_audio=0;memset(s->frame,0,sizeof(s->frame));return 0;
}
int thd_mat_push_au(thd_mat *s,const uint8_t *au,size_t n,uint64_t pts,uint64_t tolerance){
    if(!s||!s->authorized||!s->ready||s->closed||s->failed)return -1;
    if(!au||n<10||n>8190||(be16(au)&0xfff)*2!=n)return fail(s,"invalid complete TrueHD AU");
    int major=au[4]==0xf8&&au[5]==0x72&&au[6]==0x6f;
    if(major){
        /* MLP is deliberately excluded from this TrueHD-only trial. */
        if(au[7]!=0xba)return fail(s,"not TrueHD major sync");
        unsigned r=au[8]>>4;
        if((r&8)||(r&7)>2)return fail(s,"only 48k-family TrueHD tested");
        unsigned rate=48000u<<(r&7),samples=40u<<(r&3);
        if(s->rate&&(s->rate!=rate||s->samples!=samples))return fail(s,"rate change requires new pipeline");
        s->rate=rate;s->samples=samples;
    }else if(!s->rate)return fail(s,"segment must begin at major sync");
    if(s->au_count>(NONE-s->origin)/s->samples/1000000000u)return fail(s,"sample counter overflow");
    uint64_t relative=s->au_count*s->samples*1000000000ull/s->rate;
    if(relative>NONE-s->origin||pts==NONE||tolerance>1000000u)return fail(s,"invalid presentation timestamp");
    uint64_t expected=s->origin+relative,delta_pts=pts>expected?pts-expected:expected-pts;
    if(delta_pts>tolerance)return fail(s,"media PTS discontinuity; flush and new segment required");
    unsigned output;
    if(major&&restart_time(au,n,&output)){
        if(s->have_output&&s->output_time!=output)return fail(s,"output timing discontinuity");
        s->output_time=output;s->have_output=1;
    }
    unsigned timing=be16(au+2),padding=0,total=n;
    if(s->have_prev){
        unsigned dt=(timing-s->prev_time)&0xffff;
        if(!dt||(dt*2560u)%s->samples)return fail(s,"invalid AU input timing");
        unsigned gap=dt*2560u/s->samples;
        if(gap<s->prev_size||gap-s->prev_size>=MAT_SIZE/2)return fail(s,"unsupported AU delivery gap");
        padding=gap-s->prev_size;
    }
    unsigned remaining=n,offset=0,i=0;
    while(i<3&&s->used>marker_pos[i])++i;
    if(i==3)return fail(s,"invalid MAT write position");
    while(padding||remaining||marker_pos[i]==s->used){
        if(s->used==marker_pos[i]){
            memcpy(s->frame+8+s->used,marker[i],marker_size[i]);s->used+=marker_size[i];
            unsigned counted=marker_size[i];
            if(++i==3){if(emit(s))return -1;i=0;counted+=BURST_SIZE-MAT_SIZE;}
            unsigned used=padding<counted?padding:counted;padding-=used;total+=counted-used;
        }
        if(padding){unsigned k=marker_pos[i]-s->used;if(k>padding)k=padding;s->used+=k;padding-=k;if(padding)continue;}
        if(remaining){unsigned k=marker_pos[i]-s->used;if(k>remaining)k=remaining;
            if(k){memcpy(s->frame+8+s->used,au+offset,k);s->used+=k;offset+=k;remaining-=k;s->has_audio=1;}}
    }
    s->have_prev=1;s->prev_time=timing;s->prev_size=total;++s->au_count;
    if(s->have_output)s->output_time=(s->output_time+s->samples)&0xffff;
    return 0;
}
int thd_mat_finish(thd_mat *s){
    if(!s||!s->authorized||s->failed)return -1;
    if(s->closed)return 0;
    s->closed=1;
    if(s->has_audio){
        for(unsigned i=0;i<3;i++){
            if(s->used<=marker_pos[i]){
                memcpy(s->frame+8+marker_pos[i],marker[i],marker_size[i]);
                s->used=marker_pos[i]+marker_size[i];
            }
        }
        return emit(s);
    }
    return 0;
}
const char *thd_mat_error(thd_mat *s){return s?s->error:"null adapter";}
