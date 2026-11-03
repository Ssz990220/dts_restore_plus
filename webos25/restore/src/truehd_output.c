/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Follow the decoder's own output graph and publish bounded format proof to
 * the plugin's root policy watcher. No RM allocation and no ALSA writes here.
 */
#include "truehd_output.h"
#include <string.h>
#if defined(__linux__)
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static GstPad *
unique_link (GstIterator *iter)
{
  GValue value = G_VALUE_INIT;
  GstPad *found = NULL;
  GstIteratorResult result;
  if (!iter) return NULL;
  while ((result = gst_iterator_next (iter, &value)) == GST_ITERATOR_OK) {
    GstPad *pad = g_value_get_object (&value);
    if (gst_pad_is_linked (pad)) {
      if (found) {
        gst_object_unref (found); found = NULL;
        g_value_reset (&value); goto done;
      }
      found = gst_object_ref (pad);
    }
    g_value_reset (&value);
  }
  if (result != GST_ITERATOR_DONE && found) {
    gst_object_unref (found); found = NULL;
  }
done:
  if (G_VALUE_TYPE (&value)) g_value_unset (&value);
  gst_iterator_free (iter);
  return found;
}

GstElement *
thd_linked_audio_sink (GstPad *initial)
{
  GstPad *output = gst_object_ref (initial);
  guint hops;
  for (hops = 0; output && hops < 64; hops++) {
    GstPad *peer = gst_pad_get_peer (output), *input;
    GstElement *element;
    GstElementFactory *factory;
    guint proxies = 0;
    gst_object_unref (output); output = NULL;
    while (peer && GST_IS_PROXY_PAD (peer) && proxies++ < 16) {
      GstPad *next = NULL;
      if (GST_IS_GHOST_PAD (peer) && GST_PAD_DIRECTION (peer) == GST_PAD_SINK)
        next = gst_ghost_pad_get_target (GST_GHOST_PAD (peer));
      else {
        GstProxyPad *external = gst_proxy_pad_get_internal (GST_PROXY_PAD (peer));
        if (external) {
          next = gst_pad_get_peer (GST_PAD (external));
          gst_object_unref (external);
        }
      }
      gst_object_unref (peer); peer = next;
    }
    if (!peer) break;
    if (GST_IS_PROXY_PAD (peer)) { gst_object_unref (peer); break; }
    element = gst_pad_get_parent_element (peer);
    if (!element) { gst_object_unref (peer); break; }
    factory = gst_element_get_factory (element);
    if (factory && g_str_equal (G_OBJECT_TYPE_NAME (element), "GstStreamSynchronizer") &&
        g_str_equal (gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)), "streamsynchronizer")) {
      GstPad *reverse = NULL;
      output = unique_link (gst_pad_iterate_internal_links (peer));
      if (output) reverse = unique_link (gst_pad_iterate_internal_links (output));
      if (reverse != peer || (output && GST_PAD_DIRECTION (output) != GST_PAD_SRC)) {
        if (output) gst_object_unref (output);
        output = NULL;
      }
      if (reverse) gst_object_unref (reverse);
      gst_object_unref (peer); gst_object_unref (element); continue;
    }
    input = unique_link (gst_element_iterate_sink_pads (element));
    if (input != peer) {
      if (input) gst_object_unref (input);
      gst_object_unref (peer); gst_object_unref (element); break;
    }
    gst_object_unref (input); gst_object_unref (peer);
    if (factory && g_str_equal (gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)), "audiosink")) {
      GParamSpec *index = g_object_class_find_property (G_OBJECT_GET_CLASS (element), "index");
      if (index && G_PARAM_SPEC_VALUE_TYPE (index) == G_TYPE_UINT && (index->flags & G_PARAM_READABLE))
        return element;
      gst_object_unref (element); break;
    }
    /* An ES payload only borrows raw-audio caps to enter LG's private sink.
     * Never send it through an unknown PCM converter, resampler or volume
     * element. A valid resource index alone is not evidence of byte safety. */
    const gchar *name = factory ? gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)) : "";
    if (!(g_str_equal (name, "queue") || g_str_equal (name, "queue2") ||
          g_str_equal (name, "identity") || g_str_equal (name, "tee") ||
          g_str_equal (name, "capsfilter"))) {
      gst_object_unref (element); break;
    }
    output = unique_link (gst_element_iterate_src_pads (element));
    gst_object_unref (element);
  }
  if (output) gst_object_unref (output);
  return NULL;
}

gint
thd_linked_audio_port (GstPad *output)
{
  GstElement *sink = thd_linked_audio_sink (output);
  guint index = G_MAXUINT;
  if (sink) { g_object_get (sink, "index", &index, NULL); gst_object_unref (sink); }
  return index <= 1 ? (gint) index : -1;
}

struct _ThdPolicy {
  GstElement *owner; /* borrowed, closed in dispose */
  gchar *proof, *ack, *token;
  guint64 start_ticks;
  gint port, channels;
  gboolean ready, activated;
};

#if defined(__linux__) && defined(__arm__)
static ThdPolicy *
policy_failure (GstElement *owner, const gchar *stage)
{
  gst_element_post_message (owner, gst_message_new_element (GST_OBJECT (owner),
      gst_structure_new ("codex-truehd-policy-error", "stage", G_TYPE_STRING,
          stage, "errno", G_TYPE_INT, errno, NULL)));
  return NULL;
}
#endif

#if defined(__linux__) && defined(__arm__)
#define POLICY_DIRECTORY "/run/dts25-truehd-es"
/* Zig 0.13's old-glibc lstat/fstat shims pass ABI version 0 on ARM32.
 * C5 returns EINVAL for 0, and version 1 has a different structure layout.
 * Read-only probes confirmed version 3 matches this target's native stat.
 * Keep this compatibility fix local; do not patch the toolchain or TV libc. */
extern int __lxstat (int, const char *, struct stat *);
extern int __fxstat (int, int, struct stat *);
#define target_lstat(path, st) __lxstat (3, path, st)
#define target_fstat(fd, st) __fxstat (3, fd, st)

static GKeyFile *
read_private (const gchar *path)
{
  gchar text[4097];
  struct stat st;
  gssize size;
  GKeyFile *key;
  gint fd = open (path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return NULL;
  if (target_fstat (fd, &st) || !S_ISREG (st.st_mode) || st.st_uid != 0 ||
      (st.st_mode & 0777) != 0600 || st.st_nlink != 1 || st.st_size > 4096) {
    close (fd); return NULL;
  }
  do { size = read (fd, text, sizeof (text)); } while (size < 0 && errno == EINTR);
  close (fd);
  if (size < 1 || size > 4096) return NULL;
  key = g_key_file_new ();
  if (!g_key_file_load_from_data (key, text, size, G_KEY_FILE_NONE, NULL)) {
    g_key_file_unref (key); return NULL;
  }
  return key;
}

static gboolean
same_token (ThdPolicy *p, GKeyFile *key)
{
  gchar *token;
  gboolean same;
  if (!key) return FALSE;
  token = g_key_file_get_string (key, "truehd-es", "token", NULL);
  same = g_strcmp0 (token, p->token) == 0 &&
      g_key_file_get_uint64 (key, "truehd-es", "start_ticks", NULL) == p->start_ticks &&
      g_key_file_get_integer (key, "truehd-es", "pid", NULL) == getpid () &&
      g_key_file_get_integer (key, "truehd-es", "port", NULL) == p->port &&
      g_key_file_get_integer (key, "truehd-es", "channels", NULL) == p->channels;
  g_free (token); return same;
}

static gboolean
write_proof (ThdPolicy *p, gboolean exclusive)
{
  GKeyFile *key = g_key_file_new ();
  gchar *bytes, *temp = NULL;
  gsize size, offset = 0;
  gint fd;
  gboolean ok = FALSE;
  g_key_file_set_integer (key, "truehd-es", "version", 1);
  g_key_file_set_integer (key, "truehd-es", "pid", getpid ());
  g_key_file_set_uint64 (key, "truehd-es", "start_ticks", p->start_ticks);
  g_key_file_set_string (key, "truehd-es", "token", p->token);
  g_key_file_set_integer (key, "truehd-es", "port", p->port);
  g_key_file_set_integer (key, "truehd-es", "channels", p->channels);
  g_key_file_set_boolean (key, "truehd-es", "ready", p->ready);
  bytes = g_key_file_to_data (key, &size, NULL); g_key_file_unref (key);
  if (exclusive) fd = open (p->proof, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  else {
    GKeyFile *current = read_private (p->proof);
    gboolean own = same_token (p, current);
    if (current) g_key_file_unref (current);
    if (!own) { g_free (bytes); return FALSE; }
    temp = g_strconcat (p->proof, ".XXXXXX", NULL);
    fd = g_mkstemp_full (temp, O_WRONLY | O_CLOEXEC, 0600);
  }
  if (fd < 0) goto done;
  while (offset < size) {
    ssize_t n = write (fd, bytes + offset, size - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    offset += n;
  }
  ok = close (fd) == 0 && offset == size;
  if (ok && temp) ok = rename (temp, p->proof) == 0;
  if (!ok) unlink (temp ? temp : p->proof);
done:
  g_free (bytes); g_free (temp); return ok;
}

static gboolean
refresh_own_sink (ThdPolicy *p, GstPad *output)
{
  GstElement *sink = thd_linked_audio_sink (output);
  gpointer library, context;
  gint (*refresh) (gpointer);
  guint index = G_MAXUINT;
  gboolean ok = FALSE;
  GstPad *input;
  GstCaps *caps;
  if (!sink) return FALSE;
  g_object_get (sink, "index", &index, NULL);
  if (sizeof (gpointer) != 4 || index != (guint) p->port ||
      !g_str_equal (G_OBJECT_TYPE_NAME (sink), "GstLgAudioRender")) goto done;
  input = gst_element_get_static_pad (sink, "sink");
  caps = input ? gst_pad_get_current_caps (input) : NULL;
  if (input) gst_object_unref (input);
  if (!caps) goto done;
  const GstStructure *format = gst_caps_get_structure (caps, 0);
  guint codec = 0;
  gboolean es = FALSE;
  gint rate = 0, channels = 0, transport_rate = 0, transport_channels = 0, transport_bits = 0;
  gboolean valid = gst_structure_get_uint (format, "codec", &codec) && codec == 21 &&
      gst_structure_get_boolean (format, "es", &es) && es &&
      gst_structure_get_int (format, "rate", &rate) && rate == 48000 &&
      gst_structure_get_int (format, "channels", &channels) && channels == p->channels &&
      gst_structure_get_int (format, "data-rate", &transport_rate) && transport_rate == 192000 &&
      gst_structure_get_int (format, "data-channels", &transport_channels) && transport_channels == 8 &&
      gst_structure_get_int (format, "data-bps", &transport_bits) && transport_bits == 16;
  gst_caps_unref (caps);
  if (!valid) goto done;
  /* The native-discovery caller already checked both exact stock ABI hashes. */
  library = dlopen ("/usr/lib/gstreamer-1.0/libgstlgaudiosink.so", RTLD_NOW | RTLD_LOCAL);
  if (!library) goto done;
  *(gpointer *) &refresh = dlsym (library, "lgao_push_first_event");
  context = *(gpointer *) ((guint8 *) sink + 0x200);
  if (refresh && context) { refresh (context); ok = TRUE; }
  /* This is an already registered GStreamer module; keep it resident. */
done:
  gst_object_unref (sink); return ok;
}
#endif

ThdPolicy *
thd_policy_open (GstElement *owner, gint port, gint channels)
{
#if defined(__linux__) && defined(__arm__)
  ThdPolicy *p;
  struct stat st;
  gchar *stat_text = NULL, *tail, **fields;
  if (port < 0 || port > 1 || (channels != 6 && channels != 8)) return policy_failure (owner, "arguments");
  if (mkdir (POLICY_DIRECTORY, 0700) && errno != EEXIST) return policy_failure (owner, "mkdir");
  if (target_lstat (POLICY_DIRECTORY, &st) || !S_ISDIR (st.st_mode) || st.st_uid != 0 ||
      (st.st_mode & 0777) != 0700) return policy_failure (owner, "directory-metadata");
  if (!g_file_get_contents ("/proc/self/stat", &stat_text, NULL, NULL)) return policy_failure (owner, "proc-read");
  tail = strrchr (stat_text, ')');
  if (!tail || tail[1] != ' ') { g_free (stat_text); return policy_failure (owner, "proc-syntax"); }
  fields = g_strsplit (tail + 2, " ", 23);
  p = g_new0 (ThdPolicy, 1); p->owner = owner; p->port = port; p->channels = channels;
  p->start_ticks = g_strv_length (fields) > 19 ? g_ascii_strtoull (fields[19], NULL, 10) : 0;
  g_strfreev (fields); g_free (stat_text);
  p->token = g_uuid_string_random ();
  p->proof = g_strdup_printf (POLICY_DIRECTORY "/%ld.%d.ini", (long) getpid (), port);
  p->ack = g_strdup_printf (POLICY_DIRECTORY "/%ld.%d.ack", (long) getpid (), port);
  if (!p->start_ticks) { thd_policy_close (&p); return policy_failure (owner, "start-ticks"); }
  if (!write_proof (p, TRUE)) { thd_policy_close (&p); return policy_failure (owner, "write-proof"); }
  return p;
#else
  (void) owner; (void) port; (void) channels; return NULL;
#endif
}

gboolean
thd_policy_ready (ThdPolicy *p)
{
#if defined(__linux__) && defined(__arm__)
  if (!p) return FALSE;
  if (p->ready) return TRUE;
  p->ready = TRUE;
  return write_proof (p, FALSE);
#else
  (void) p; return FALSE;
#endif
}

gboolean
thd_policy_poll (ThdPolicy *p, GstPad *output)
{
#if defined(__linux__) && defined(__arm__)
  GKeyFile *key;
  gchar *route, *expected;
  gboolean matches;
  if (!p) return FALSE;
  if (!p->ready || p->activated) return TRUE;
  key = read_private (p->ack);
  if (!key) return TRUE;
  route = g_key_file_get_string (key, "truehd-es", "earc", NULL);
  expected = g_strdup_printf ("2,6,%d,0,0", p->channels);
  matches = same_token (p, key) && g_strcmp0 (route, expected) == 0 &&
      g_key_file_get_integer (key, "truehd-es", "version", NULL) == 1;
  g_free (expected); g_free (route); g_key_file_unref (key);
  if (!matches) return TRUE;
  if (!refresh_own_sink (p, output)) return FALSE;
  p->activated = TRUE;
  gst_element_post_message (p->owner, gst_message_new_element (GST_OBJECT (p->owner),
      gst_structure_new ("codex-truehd-policy-active", "audio-port", G_TYPE_INT,
      p->port, "source-channels", G_TYPE_INT, p->channels, NULL)));
  return TRUE;
#else
  (void) p; (void) output; return FALSE;
#endif
}

void
thd_policy_close (ThdPolicy **owned)
{
  ThdPolicy *p = *owned;
  if (!p) return;
  *owned = NULL;
#if defined(__linux__) && defined(__arm__)
  const gchar *paths[] = { p->proof, p->ack };
  for (guint i = 0; i < G_N_ELEMENTS (paths); i++) {
    GKeyFile *key = read_private (paths[i]);
    if (same_token (p, key)) unlink (paths[i]);
    if (key) g_key_file_unref (key);
  }
#endif
  g_free (p->proof); g_free (p->ack); g_free (p->token); g_free (p);
}
