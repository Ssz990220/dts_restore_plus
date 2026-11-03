/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CODEX_TRUEHD_OUTPUT_H
#define CODEX_TRUEHD_OUTPUT_H
#include <gst/gst.h>
typedef struct _ThdPolicy ThdPolicy;
/* Return a reference to the sole linked native audio sink, never a sibling. */
GstElement *thd_linked_audio_sink (GstPad *output);
gint thd_linked_audio_port (GstPad *output);
ThdPolicy *thd_policy_open (GstElement *owner, gint port, gint channels);
gboolean thd_policy_ready (ThdPolicy *policy);
gboolean thd_policy_poll (ThdPolicy *policy, GstPad *output);
void thd_policy_close (ThdPolicy **policy);
#endif
