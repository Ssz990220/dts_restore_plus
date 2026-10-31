/* Host GStreamer graph tests; no LG hardware or FFmpeg is involved.
 * LGPL-2.0-or-later. The mock is registered only in this process. */
#include "../gstwebospcm.h"

typedef struct { GstElement parent; gboolean multi; } MockAudioSink;
typedef struct { GstElementClass parent; } MockAudioSinkClass;
G_DEFINE_TYPE (MockAudioSink, mock_audio_sink, GST_TYPE_ELEMENT)

static void
mock_set (GObject * object, guint id, const GValue * value, GParamSpec * spec)
{
  if (id == 1)
    ((MockAudioSink *) object)->multi = g_value_get_boolean (value);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, spec);
}

static void
mock_get (GObject * object, guint id, GValue * value, GParamSpec * spec)
{
  if (id == 1)
    g_value_set_boolean (value, ((MockAudioSink *) object)->multi);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, spec);
}

static void
mock_audio_sink_class_init (MockAudioSinkClass * klass)
{
  GstCaps *caps = gst_caps_new_any ();
  GstPadTemplate *pad = gst_pad_template_new ("sink", GST_PAD_SINK,
      GST_PAD_ALWAYS, caps);
  G_OBJECT_CLASS (klass)->set_property = mock_set;
  G_OBJECT_CLASS (klass)->get_property = mock_get;
  g_object_class_install_property (G_OBJECT_CLASS (klass), 1,
      g_param_spec_boolean ("pcm-multi-ch", "PCM", "Mock PCM flag", FALSE,
          G_PARAM_READWRITE));
  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Mock LG sink", "Sink/Audio", "Graph test only", "Test");
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass), pad);
  gst_caps_unref (caps);
}

static void
mock_audio_sink_init (MockAudioSink * sink)
{
  GstPadTemplate *pad = gst_element_class_get_pad_template (
      GST_ELEMENT_GET_CLASS (sink), "sink");
  gst_element_add_pad (GST_ELEMENT (sink), gst_pad_new_from_template (pad, "sink"));
}

static gboolean
enabled (GstElement * element)
{
  gboolean value = FALSE;
  g_object_get (element, "pcm-multi-ch", &value, NULL);
  return value;
}

static void
check_route (gboolean nested, gboolean preenabled)
{
  GstElement *pipeline = gst_pipeline_new (NULL);
  GstElement *decoder = gst_element_factory_make ("identity", NULL);
  GstElement *sink = gst_element_factory_make ("audiosink", NULL);
  GstElement *unrelated = gst_element_factory_make ("audiosink", NULL);
  DtsPcmTarget *target = NULL;
  gst_bin_add_many (GST_BIN (pipeline), decoder, sink, unrelated, NULL);
  if (nested) {
    GstElement *bin = gst_bin_new (NULL);
    GstElement *queue = gst_element_factory_make ("queue", NULL);
    GstPad *pad;
    gst_bin_add (GST_BIN (bin), queue);
    pad = gst_element_get_static_pad (queue, "sink");
    gst_element_add_pad (bin, gst_ghost_pad_new ("sink", pad));
    gst_object_unref (pad);
    pad = gst_element_get_static_pad (queue, "src");
    gst_element_add_pad (bin, gst_ghost_pad_new ("src", pad));
    gst_object_unref (pad);
    gst_bin_add (GST_BIN (pipeline), bin);
    g_assert_true (gst_element_link_many (decoder, bin, sink, NULL));
  } else {
    g_assert_true (gst_element_link (decoder, sink));
  }
  if (preenabled)
    g_object_set (sink, "pcm-multi-ch", TRUE, NULL);
  g_assert_true (dts_pcm_target_enable (decoder, &target));
  g_assert_true (enabled (sink));
  g_assert_false (enabled (unrelated));
  dts_pcm_target_release (&target);
  g_assert_cmpint (enabled (sink), ==, preenabled);
  g_assert_null (target);
  g_assert_false (enabled (unrelated));
  gst_object_unref (pipeline);
}

static void
check_shared_paths (gboolean joining)
{
  GstElement *pipeline = gst_pipeline_new (NULL);
  GstElement *decoder = gst_element_factory_make ("identity", NULL);
  GstElement *other = gst_element_factory_make (
      joining ? "identity" : "audiosink", NULL);
  GstElement *junction = gst_element_factory_make (joining ? "funnel" : "tee", NULL);
  GstElement *sink = gst_element_factory_make ("audiosink", NULL);
  DtsPcmTarget *target = NULL;
  gst_bin_add_many (GST_BIN (pipeline), decoder, other, junction, sink, NULL);
  g_assert_true (gst_element_link_many (decoder, junction, sink, NULL));
  g_assert_true (joining ? gst_element_link (other, junction) :
      gst_element_link (junction, other));
  g_assert_false (dts_pcm_target_enable (decoder, &target));
  g_assert_null (target);
  g_assert_false (enabled (sink));
  if (!joining)
    g_assert_false (enabled (other));
  gst_object_unref (pipeline);
}

int
main (int argc, char **argv)
{
  GstElement *decoder, *sink;
  DtsPcmTarget *target = NULL;
  gst_init (&argc, &argv);
  g_assert_true (gst_element_register (NULL, "audiosink", GST_RANK_NONE,
          mock_audio_sink_get_type ()));
  check_route (FALSE, FALSE);
  check_route (TRUE, FALSE);
  check_route (FALSE, TRUE);
  check_shared_paths (FALSE);
  check_shared_paths (TRUE);
  decoder = gst_element_factory_make ("identity", NULL);
  g_assert_false (dts_pcm_target_enable (decoder, &target));
  gst_object_unref (decoder);
  decoder = gst_element_factory_make ("identity", NULL);
  sink = gst_element_factory_make ("fakesink", NULL);
  g_assert_true (gst_element_link (decoder, sink));
  g_assert_false (dts_pcm_target_enable (decoder, &target));
  g_assert_null (target);
  gst_object_unref (sink);
  gst_object_unref (decoder);
  decoder = gst_element_factory_make ("identity", NULL);
  sink = gst_element_factory_make ("audiosink", NULL);
  g_assert_true (gst_element_link (decoder, sink));
  g_assert_true (dts_pcm_target_enable (decoder, &target));
  /* A destroyed target must not be kept alive or touched during cleanup. */
  gst_object_unref (sink);
  gst_object_unref (decoder);
  dts_pcm_target_release (&target);
  g_print ("PCM route graph checks passed\n");
  return 0;
}
