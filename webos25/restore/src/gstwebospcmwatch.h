/* LGPL-2.0-or-later. Catch late-created native sinks before their own events.
 * Probe/signal closures retain the watch, while decoder/root/pad references
 * are weak. Teardown disables callbacks before restoring and detaching. */
#ifndef GST_WEBOS_PCM_WATCH_H
#define GST_WEBOS_PCM_WATCH_H
#include "gstwebospcm.h"
#include "gstwebospcm71.h"
#ifdef __linux__
#include <syslog.h>
#include <unistd.h>
#endif

typedef struct {
  GWeakRef pad;
  gulong id;
} DtsPcmProbe;

typedef struct _DtsPcmWatch {
  gint refs;
  gint resolved;
  GMutex lock;
  gboolean active;
  gboolean multichannel;
  guint attempts;
  GWeakRef decoder;
  GWeakRef root;
  gulong root_signal;
  GPtrArray *probes;
  DtsPcmTarget *target;
} DtsPcmWatch;

static DtsPcmWatch *
dts_pcm_watch_ref (DtsPcmWatch * watch)
{
  g_atomic_int_inc (&watch->refs);
  return watch;
}

static void
dts_pcm_watch_unref (gpointer data)
{
  DtsPcmWatch *watch = data;
  if (g_atomic_int_dec_and_test (&watch->refs)) {
    g_weak_ref_clear (&watch->decoder);
    g_weak_ref_clear (&watch->root);
    g_mutex_clear (&watch->lock);
    g_free (watch);
  }
}

static void
dts_pcm_watch_signal_done (gpointer data, GClosure * closure)
{
  (void) closure;
  dts_pcm_watch_unref (data);
}

static gboolean
dts_pcm_watch_try (DtsPcmWatch * watch, const gchar *stage)
{
  GstElement *decoder = NULL;
  gboolean enabled = FALSE;
  if (g_atomic_int_get (&watch->resolved))
    return TRUE;
  g_mutex_lock (&watch->lock);
  if (watch->active) {
    GString *trace = g_string_new ("");
    decoder = g_weak_ref_get (&watch->decoder);
    if (decoder != NULL)
      enabled = dts_pcm_target_configure_full (decoder, &watch->target, trace,
          watch->multichannel);
    if (enabled)
      g_atomic_int_set (&watch->resolved, 1);
#ifdef __linux__
    /* One bounded startup diagnostic, containing types only, never properties
     * such as authenticated source URIs or HTTP headers. */
    if (watch->attempts < 16 || enabled)
      syslog (LOG_USER | LOG_NOTICE, "dts25-pcm pid=%ld stage=%s enabled=%d path=%s",
          (long) getpid (), stage, enabled, trace->str);
#else
    (void) stage;
#endif
    g_string_free (trace, TRUE);
    if (watch->attempts < 16)
      watch->attempts++;
  }
  g_mutex_unlock (&watch->lock);
  /* Never drop the decoder's last reference while holding the watch lock:
   * its finalizer may tear the watch down. */
  if (decoder != NULL)
    gst_object_unref (decoder);
  return enabled;
}

static GstPadProbeReturn
dts_pcm_watch_probe (GstPad * pad, GstPadProbeInfo * info, gpointer data)
{
  DtsPcmWatch *watch = data;
  dts_pcm_watch_try (data, (GST_PAD_PROBE_INFO_TYPE (info) &
          GST_PAD_PROBE_TYPE_BUFFER) ? "sink-buffer" :
      (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM) ?
      "sink-query" : "sink-event");
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
    if (GST_EVENT_TYPE (event) == GST_EVENT_CAPS) {
      GstElement *parent = gst_pad_get_parent_element (pad), *target = NULL;
      gboolean own = FALSE;
      g_mutex_lock (&watch->lock);
      if (watch->active && !watch->multichannel && watch->target != NULL) {
        target = g_weak_ref_get (&watch->target->sink);
        own = target == parent;
      }
      g_mutex_unlock (&watch->lock);
      if (target != NULL) gst_object_unref (target);
      if (parent != NULL) gst_object_unref (parent);
      if (own) {
        GstCaps *caps, *replacement;
        gst_event_parse_caps (event, &caps);
        replacement = dts_pcm71_caps (caps);
        if (replacement != NULL) {
          GstEvent *next = gst_event_new_caps (replacement);
          gst_event_set_seqnum (next, gst_event_get_seqnum (event));
          GST_PAD_PROBE_INFO_DATA (info) = next;
          gst_event_unref (event);
          gst_caps_unref (replacement);
        }
      }
    }
  }
  return GST_PAD_PROBE_OK;
}

static void
dts_pcm_watch_add_sink (DtsPcmWatch * watch, GstElement * element)
{
  GstPad *pad;
  guint i;
  if (!dts_pcm_is_lg_sink (element))
    return;
  pad = gst_element_get_static_pad (element, "sink");
  if (pad == NULL)
    return;
  g_mutex_lock (&watch->lock);
  if (watch->active) {
    gboolean exists = FALSE;
    for (i = 0; i < watch->probes->len; i++) {
      DtsPcmProbe *entry = g_ptr_array_index (watch->probes, i);
      GstPad *previous = g_weak_ref_get (&entry->pad);
      if (previous == pad)
        exists = TRUE;
      if (previous != NULL)
        gst_object_unref (previous);
    }
    if (!exists) {
      DtsPcmProbe *entry = g_new0 (DtsPcmProbe, 1);
      g_weak_ref_init (&entry->pad, pad);
      entry->id = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
          GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
          dts_pcm_watch_probe,
          dts_pcm_watch_ref (watch), dts_pcm_watch_unref);
      g_ptr_array_add (watch->probes, entry);
    }
  }
  g_mutex_unlock (&watch->lock);
  gst_object_unref (pad);
}

static void
dts_pcm_watch_added (GstBin * root, GstBin * parent, GstElement * element,
    gpointer data)
{
  (void) root;
  (void) parent;
  dts_pcm_watch_add_sink (data, element);
}

static DtsPcmWatch *
dts_pcm_watch_new_full (GstElement * decoder, gboolean multichannel)
{
  DtsPcmWatch *watch = g_new0 (DtsPcmWatch, 1);
  GstObject *root = gst_object_ref (decoder), *parent;
  watch->refs = 1;
  watch->active = TRUE;
  watch->multichannel = multichannel;
  g_mutex_init (&watch->lock);
  g_weak_ref_init (&watch->decoder, decoder);
  watch->probes = g_ptr_array_new ();
  while ((parent = gst_object_get_parent (root)) != NULL) {
    gst_object_unref (root);
    root = parent;
  }
  g_weak_ref_init (&watch->root, root);
  if (GST_IS_BIN (root)) {
    GstIterator *iter;
    GstIteratorResult status;
    GValue item = G_VALUE_INIT;
    guint resyncs = 0;
    watch->root_signal = g_signal_connect_data (root, "deep-element-added",
        G_CALLBACK (dts_pcm_watch_added), dts_pcm_watch_ref (watch),
        dts_pcm_watch_signal_done, 0);
    iter = gst_bin_iterate_recurse (GST_BIN (root));
    while ((status = gst_iterator_next (iter, &item)) != GST_ITERATOR_DONE) {
      if (status == GST_ITERATOR_OK) {
        dts_pcm_watch_add_sink (watch, g_value_get_object (&item));
        g_value_reset (&item);
      } else if (status == GST_ITERATOR_RESYNC && resyncs++ < 4) {
        gst_iterator_resync (iter);
      } else {
        break;
      }
    }
    if (G_VALUE_TYPE (&item))
      g_value_unset (&item);
    gst_iterator_free (iter);
  }
  gst_object_unref (root);
  dts_pcm_watch_try (watch, "renegotiate");
  return watch;
}

static inline DtsPcmWatch *
dts_pcm_watch_new (GstElement * decoder)
{
  return dts_pcm_watch_new_full (decoder, TRUE);
}

static void
dts_pcm_watch_stop (DtsPcmWatch ** owned)
{
  DtsPcmWatch *watch = *owned;
  GstObject *root;
  GPtrArray *probes;
  guint i;
  if (watch == NULL)
    return;
  *owned = NULL;
  g_mutex_lock (&watch->lock);
  watch->active = FALSE;
  probes = watch->probes;
  watch->probes = NULL;
  dts_pcm_target_release (&watch->target);
  g_mutex_unlock (&watch->lock);
  root = g_weak_ref_get (&watch->root);
  if (root != NULL) {
    if (watch->root_signal)
      g_signal_handler_disconnect (root, watch->root_signal);
    gst_object_unref (root);
  }
  for (i = 0; i < probes->len; i++) {
    DtsPcmProbe *entry = g_ptr_array_index (probes, i);
    GstPad *pad = g_weak_ref_get (&entry->pad);
    if (pad != NULL) {
      gst_pad_remove_probe (pad, entry->id);
      gst_object_unref (pad);
    }
    g_weak_ref_clear (&entry->pad);
    g_free (entry);
  }
  g_ptr_array_free (probes, TRUE);
  dts_pcm_watch_unref (watch);
}
#endif
