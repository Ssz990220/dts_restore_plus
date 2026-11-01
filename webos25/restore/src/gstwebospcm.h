/* Experimental webOS PCM routing; LGPL-2.0-or-later, as gstdtsdec.c.
 * Only follow this decoder's unambiguous linked audio path. Never search the
 * whole pipeline by an element name or change an unrelated player's sink.
 * This sets an existing sink property; it does not select MAT/eARC policy. */
#ifndef GST_WEBOS_PCM_H
#define GST_WEBOS_PCM_H

#include <gst/gst.h>

typedef struct _DtsPcmTarget {
  GWeakRef sink;
  gboolean original;
  gboolean requested;
  gboolean changed;
} DtsPcmTarget;

static void
dts_pcm_target_release (DtsPcmTarget ** target)
{
  GstElement *sink;
  gboolean current = FALSE;

  if (*target == NULL)
    return;
  sink = g_weak_ref_get (&(*target)->sink);
  if (sink != NULL) {
    /* Restore only the property change still owned by this helper. */
    g_object_get (sink, "pcm-multi-ch", &current, NULL);
    if ((*target)->changed && current == (*target)->requested)
      g_object_set (sink, "pcm-multi-ch", (*target)->original, NULL);
    gst_object_unref (sink);
  }
  g_weak_ref_clear (&(*target)->sink);
  g_free (*target);
  *target = NULL;
}

/* Consume an iterator and return its sole linked pad. */
static GstPad *
dts_pcm_unique_link (GstIterator *iter)
{
  GValue item = G_VALUE_INIT;
  GstPad *result = NULL;
  GstIteratorResult status;

  if (iter == NULL)
    return NULL;
  while ((status = gst_iterator_next (iter, &item)) == GST_ITERATOR_OK) {
    GstPad *pad = g_value_get_object (&item);
    if (gst_pad_is_linked (pad)) {
      if (result != NULL) {
        gst_object_unref (result);
        result = NULL;
        g_value_reset (&item);
        goto done;
      }
      result = gst_object_ref (pad);
    }
    g_value_reset (&item);
  }
  if (status != GST_ITERATOR_DONE && result != NULL) {
    gst_object_unref (result);
    result = NULL;
  }
done:
  if (G_VALUE_TYPE (&item) != 0)
    g_value_unset (&item);
  gst_iterator_free (iter);
  return result;
}

static GstPad *
dts_pcm_single_link (GstElement * element, GstPadDirection direction)
{
  return dts_pcm_unique_link (direction == GST_PAD_SRC ?
      gst_element_iterate_src_pads (element) : gst_element_iterate_sink_pads (element));
}

/* streamsynchronizer is a collection of independent sink/src pairs for
 * audio, video and subtitles. It is not a mixer. Follow its documented
 * internal-link iterator, requiring the reverse pairing as well. Do not
 * extend this exception to selectors, mixers or fan-out elements. */
static GstPad *
dts_pcm_sync_output (GstPad * input)
{
  GstPad *output = dts_pcm_unique_link (gst_pad_iterate_internal_links (input));
  GstPad *reverse;
  if (output == NULL)
    return NULL;
  reverse = dts_pcm_unique_link (gst_pad_iterate_internal_links (output));
  if (reverse != input || GST_PAD_DIRECTION (output) != GST_PAD_SRC) {
    gst_object_unref (output);
    output = NULL;
  }
  if (reverse != NULL)
    gst_object_unref (reverse);
  return output;
}

static gboolean
dts_pcm_is_lg_sink (GstElement * element)
{
  GstElementFactory *factory = gst_element_get_factory (element);
  GParamSpec *property = g_object_class_find_property (
      G_OBJECT_GET_CLASS (element), "pcm-multi-ch");
  gboolean identity = g_str_equal (G_OBJECT_TYPE_NAME (element), "GstLgAudioRender") ||
      (factory != NULL && g_str_equal (gst_plugin_feature_get_name (
          GST_PLUGIN_FEATURE (factory)), "audiosink"));
  return identity && property != NULL &&
      G_PARAM_SPEC_VALUE_TYPE (property) == G_TYPE_BOOLEAN &&
      (property->flags & G_PARAM_READWRITE) == G_PARAM_READWRITE &&
      !(property->flags & G_PARAM_CONSTRUCT_ONLY);
}

/* Called before the first decoded PCM caps/event. With a late/unlinked graph,
 * leave routing unchanged; this prototype does not attempt live rewiring. */
static gboolean
dts_pcm_target_configure_full (GstElement * decoder, DtsPcmTarget ** target,
    GString *trace, gboolean requested)
{
  GstPad *output;
  guint hops;

  if (*target != NULL)
    return TRUE;
  output = gst_element_get_static_pad (decoder, "src");
  for (hops = 0; output != NULL && hops < 64; hops++) {
    GstPad *peer = gst_pad_get_peer (output);
    GstElement *element;
    GstPad *only_input;
    guint proxies = 0;

    gst_object_unref (output);
    output = NULL;
    while (peer != NULL && GST_IS_PROXY_PAD (peer) && proxies++ < 16) {
      GstPad *inner = NULL;
      if (GST_IS_GHOST_PAD (peer) && GST_PAD_DIRECTION (peer) == GST_PAD_SINK) {
        inner = gst_ghost_pad_get_target (GST_GHOST_PAD (peer));
      } else {
        /* Leaving a bin: the internal sink proxy's counterpart is the
         * external source ghost pad. Continue through its actual peer. */
        GstProxyPad *external = gst_proxy_pad_get_internal (GST_PROXY_PAD (peer));
        if (external != NULL) {
          inner = gst_pad_get_peer (GST_PAD (external));
          gst_object_unref (external);
        }
      }
      gst_object_unref (peer);
      peer = inner;
    }
    if (peer == NULL) {
      if (trace) g_string_append (trace, "/unlinked");
      break;
    }
    if (GST_IS_PROXY_PAD (peer)) {
      gst_object_unref (peer);
      break;
    }
    element = gst_pad_get_parent_element (peer);
    if (element == NULL) {
      gst_object_unref (peer);
      break;
    }
    if (trace) g_string_append_printf (trace, "/%s", G_OBJECT_TYPE_NAME (element));
    if (g_str_equal (G_OBJECT_TYPE_NAME (element), "GstStreamSynchronizer")) {
      GstElementFactory *factory = gst_element_get_factory (element);
      if (factory != NULL && g_str_equal (gst_plugin_feature_get_name (
              GST_PLUGIN_FEATURE (factory)), "streamsynchronizer"))
        output = dts_pcm_sync_output (peer);
      if (output == NULL && trace)
        g_string_append (trace, "/no-paired-output");
      gst_object_unref (peer);
      gst_object_unref (element);
      continue;
    }
    /* Do not enter a mixer/selector shared by another decoder. */
    only_input = dts_pcm_single_link (element, GST_PAD_SINK);
    if (only_input != peer) {
      if (trace) g_string_append (trace, "/shared-input");
      if (only_input != NULL)
        gst_object_unref (only_input);
      gst_object_unref (peer);
      gst_object_unref (element);
      break;
    }
    gst_object_unref (only_input);
    gst_object_unref (peer);
    if (dts_pcm_is_lg_sink (element)) {
      gboolean current = FALSE, original;
      g_object_get (element, "pcm-multi-ch", &current, NULL);
      original = current;
      if (current != requested) {
        g_object_set (element, "pcm-multi-ch", requested, NULL);
        g_object_get (element, "pcm-multi-ch", &current, NULL);
      }
      if (current == requested && (current != original || !requested)) {
        *target = g_new0 (DtsPcmTarget, 1);
        g_weak_ref_init (&(*target)->sink, element);
        (*target)->original = original;
        (*target)->requested = requested;
        (*target)->changed = current != original;
      }
      gst_object_unref (element);
      return current == requested;
    }
    output = dts_pcm_single_link (element, GST_PAD_SRC);
    if (output == NULL && trace) g_string_append (trace, "/no-single-output");
    gst_object_unref (element);
  }
  if (output != NULL)
    gst_object_unref (output);
  return FALSE;
}

static inline gboolean
dts_pcm_target_enable_full (GstElement * decoder, DtsPcmTarget ** target,
    GString *trace)
{
  return dts_pcm_target_configure_full (decoder, target, trace, TRUE);
}

static inline gboolean
dts_pcm_target_enable (GstElement * decoder, DtsPcmTarget ** target)
{
  return dts_pcm_target_enable_full (decoder, target, NULL);
}

#endif
