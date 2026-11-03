/* SPDX-License-Identifier: LGPL-2.1-or-later
 * C5-specific auxiliary native MAT discovery. Never allocates an RM resource,
 * connects MAIN, changes eARC/ALSA, writes files or installs a library.
 * The caller must supply its already-granted ADEC port. Routing stays under
 * the bounded test supervisor until normal-player lifecycle tests pass.
 */
#include "truehd_native.h"
#include <string.h>
#ifdef __linux__
#include <dlfcn.h>
#include <sys/ioctl.h>
#endif

struct _ThdNative {
  GstElement *owner; /* borrowed; owner closes the context before disposal */
  gpointer ctx;
  gboolean opened, connected, started, ready_sent;
  gint fd, port;
  guint primed;
  gpointer (*open) (void);
  gint (*close) (gpointer);
  gint (*decoder_open) (gpointer);
  gint (*decoder_close) (gpointer);
  gint (*connect) (gpointer);
  gint (*disconnect) (gpointer);
  gint (*start) (gpointer);
  gint (*stop) (gpointer);
  gint (*feed) (gpointer, gconstpointer, guint, guint64);
  gint (*flush) (gpointer);
  gint (*set_flushing) (gpointer, gint);
};

static gboolean
file_hash (const gchar *path, const gchar *expected)
{
  gchar *bytes = NULL, *hash;
  gsize size;
  gboolean ok;
  if (!g_file_get_contents (path, &bytes, &size, NULL)) return FALSE;
  hash = g_compute_checksum_for_data (G_CHECKSUM_SHA256,
      (const guchar *) bytes, size);
  ok = g_strcmp0 (hash, expected) == 0;
  g_free (hash); g_free (bytes);
  return ok;
}

gboolean
thd_native_abi_available (void)
{
#if defined(__linux__) && defined(__arm__)
  return sizeof (gpointer) == 4 &&
      !g_file_test ("/tmp/lgadec_dump_enable", G_FILE_TEST_EXISTS) &&
      file_hash ("/usr/lib/gstreamer-1.0/libgstlgaudiodec.so",
        "84db367e1ea3eeb4b589bf63910140cfd43ea77f2db124ae6882fbee8ebb6bb7") &&
      file_hash ("/usr/lib/gstreamer-1.0/libgstlgaudiosink.so",
        "90dba3ef32704ce4096bf1e6045d2879d0c2192d507bc04d1e8e98437f420a43");
#else
  (void) file_hash;
  return FALSE;
#endif
}

void
thd_native_close (ThdNative **owned)
{
  ThdNative *n = *owned;
  if (!n) return;
  *owned = NULL;
  if (n->ctx) {
    if (n->started) n->stop (n->ctx);
    if (n->connected) n->disconnect (n->ctx);
    if (n->opened) n->decoder_close (n->ctx);
    n->close (n->ctx);
  }
  if (n->started) gst_element_post_message (n->owner, gst_message_new_element (
      GST_OBJECT (n->owner), gst_structure_new ("codex-truehd-native-closed",
      "audio-port", G_TYPE_INT, n->port, NULL)));
  g_free (n);
}

ThdNative *
thd_native_open (GstElement *owner, gint port, gint channels)
{
#if defined(__linux__) && defined(__arm__)
  static gpointer library;
  static GMutex library_lock;
  gpointer module;
  GstElementFactory *factory;
  GstPluginFeature *feature;
  ThdNative *n;
  guint codec = 5, uid = 0;
  if (port < 0 || port > 1 || (channels != 6 && channels != 8) ||
      !thd_native_abi_available ()) return NULL;
  factory = gst_element_factory_find ("ac3_audiodec");
  if (!factory) return NULL;
  feature = gst_plugin_feature_load (GST_PLUGIN_FEATURE (factory));
  gst_object_unref (factory);
  if (!feature) return NULL;
  gst_object_unref (feature);
  /* Keep the stock module resident: its callbacks and registered types can
   * outlive an individual auxiliary context. Never dlclose a GStreamer module. */
  g_mutex_lock (&library_lock);
  if (!library) library = dlopen (
      "/usr/lib/gstreamer-1.0/libgstlgaudiodec.so", RTLD_NOW | RTLD_LOCAL);
  module = library;
  g_mutex_unlock (&library_lock);
  if (!module) return NULL;
  n = g_new0 (ThdNative, 1); n->owner = owner; n->port = port;
#define SYMBOL(field, symbol) do { \
  *(gpointer *) &n->field = dlsym (module, symbol); \
  if (!n->field) goto failed; \
} while (0)
  SYMBOL (open, "lgadec_open"); SYMBOL (close, "lgadec_close");
  SYMBOL (decoder_open, "lgadec_decoder_open");
  SYMBOL (decoder_close, "lgadec_decoder_close");
  SYMBOL (connect, "lgadec_decoder_connect");
  SYMBOL (disconnect, "lgadec_aimc_disconnect");
  SYMBOL (start, "lgadec_decoder_start"); SYMBOL (stop, "lgadec_decoder_stop");
  SYMBOL (feed, "lgadec_decoder_feed"); SYMBOL (flush, "lgadec_flush");
  SYMBOL (set_flushing, "lgadec_set_flushing");
#undef SYMBOL
  n->ctx = n->open ();
  if (!n->ctx) goto failed;
  /* These fields belong to our own newly-created context. All offsets are
   * locked to the exact stock-library hash checked above. */
  *(gint *) ((guint8 *) n->ctx + 0x314) = port;
  *(guint *) ((guint8 *) n->ctx + 0x18) = 21;
  *(guint *) ((guint8 *) n->ctx + 0x2d0) = 48000;
  *((guint8 *) n->ctx + 0x2d4) = channels;
  *((guint8 *) n->ctx + 0x2d5) = 16;
  if (n->decoder_open (n->ctx) < 0) goto failed;
  n->opened = TRUE; n->fd = *(gint *) ((guint8 *) n->ctx + 4);
  if (ioctl (n->fd, 0x40044402UL, &codec) < 0 ||
      ioctl (n->fd, 0x80044404UL, &uid) < 0 || uid == 0) goto failed;
  if (n->connect (n->ctx) != 0) goto failed;
  n->connected = TRUE;
  if (n->start (n->ctx) != 0) goto failed;
  n->started = TRUE;
  gst_element_post_message (owner, gst_message_new_element (GST_OBJECT (owner),
      gst_structure_new ("codex-truehd-native-opened", "audio-port", G_TYPE_INT,
      port, "head-uid", G_TYPE_UINT, uid, NULL)));
  return n;
failed:
  thd_native_close (&n);
  return NULL;
#else
  (void) owner; (void) port; (void) channels;
  return NULL;
#endif
}

gboolean
thd_native_feed (ThdNative *n, const guint8 *data, gsize size, guint64 pts,
    gboolean atmos, gint source_channels)
{
#ifdef __linux__
  guint words[17] = { 0 };
  if (!n || !n->started || size != 61440) return FALSE;
  if (n->primed < 50) {
    if (n->feed (n->ctx, data, (guint) size, pts) != 0) return FALSE;
    n->primed++;
  }
  if (n->primed == 50 && !n->ready_sent) {
    if (ioctl (n->fd, 0xc044440cUL, words) < 0) return FALSE;
    /* A zero format means discovery is still asynchronous; try again on
     * the next ES burst. A valid result must match this inspected source. */
    if (!words[1]) return TRUE;
    if (words[1] != 5 || words[2] != 48000 ||
        !(words[4] == 6 || (source_channels == 8 && words[4] == 8) ||
          (atmos && words[4] == 16))) return FALSE;
    n->ready_sent = TRUE;
    gst_element_post_message (n->owner, gst_message_new_element (
        GST_OBJECT (n->owner), gst_structure_new ("codex-truehd-es-ready",
        "audio-port", G_TYPE_INT, n->port, "source-channels", G_TYPE_INT,
        source_channels, "native-presentation-channels", G_TYPE_UINT, words[4],
        "atmos", G_TYPE_BOOLEAN, atmos, "pts", G_TYPE_UINT64, pts, NULL)));
  }
  return TRUE;
#else
  (void) n; (void) data; (void) size; (void) pts;
  (void) atmos; (void) source_channels;
  return FALSE;
#endif
}

gboolean
thd_native_flush (ThdNative *n)
{
  if (!n) return TRUE;
  /* Keep the live native context across a GStreamer FLUSH. Do not destroy
   * a DSP decoder while a flushing seek is travelling through the pipeline. */
  if (n->set_flushing (n->ctx, 1) != 0) return FALSE;
  gint flushed = n->flush (n->ctx);
  gint resumed = n->set_flushing (n->ctx, 0);
  if (flushed != 0 || resumed != 0) return FALSE;
  n->primed = 0; n->ready_sent = FALSE;
  return TRUE;
}

gboolean
thd_native_ready (ThdNative *n)
{
  return n && n->ready_sent;
}
