/* LGPL-2.0-or-later. C5 native PCM format contract, not a channel remapper.
 * Raw buffers stay untouched. Only a fully positioned 48 kHz 7.1 layout is
 * eligible; unknown masks, hardware audio/x-media and 5.1 stay unchanged. */
#ifndef GST_WEBOS_PCM71_H
#define GST_WEBOS_PCM71_H
#include <gst/gst.h>

static GstCaps *
dts_pcm71_caps (const GstCaps * caps)
{
  const GstStructure *s;
  const gchar *format, *layout;
  gint channels, rate;
  guint64 mask;
  GstCaps *result;
  if (caps == NULL || !gst_caps_is_fixed (caps) ||
      gst_caps_get_size (caps) != 1)
    return NULL;
  s = gst_caps_get_structure (caps, 0);
  format = gst_structure_get_string (s, "format");
  layout = gst_structure_get_string (s, "layout");
  if (!gst_structure_has_name (s, "audio/x-raw") ||
      !gst_structure_get_int (s, "channels", &channels) || channels != 8 ||
      !gst_structure_get_int (s, "rate", &rate) || rate != 48000 ||
      !gst_structure_get (s, "channel-mask", GST_TYPE_BITMASK, &mask, NULL) ||
      mask != G_GUINT64_CONSTANT (0xc3f) ||
      g_strcmp0 (layout, "interleaved") != 0 ||
      !(g_strcmp0 (format, "S16LE") == 0 ||
        g_strcmp0 (format, "S24LE") == 0 ||
        g_strcmp0 (format, "S32LE") == 0))
    return NULL;
  result = gst_caps_copy (caps);
  /* The native reader uses get_uint for acmod and get_int for out-channels.
   * Do not copy the unrelated Opus source's codec=17 field. */
  gst_structure_set (gst_caps_get_structure (result, 0),
      "acmod", G_TYPE_UINT, 21u,
      "out-channels", G_TYPE_INT, 8,
      "lfe-enable", G_TYPE_BOOLEAN, TRUE, NULL);
  return result;
}
#endif
