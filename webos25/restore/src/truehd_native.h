/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CODEX_TRUEHD_NATIVE_H
#define CODEX_TRUEHD_NATIVE_H
#include <gst/gst.h>
typedef struct _ThdNative ThdNative;
gboolean thd_native_abi_available (void);
ThdNative *thd_native_open (GstElement *owner, gint port, gint channels);
gboolean thd_native_feed (ThdNative *n, const guint8 *data, gsize size,
    guint64 pts, gboolean atmos, gint source_channels);
gboolean thd_native_flush (ThdNative *n);
gboolean thd_native_ready (ThdNative *n);
void thd_native_close (ThdNative **n);
#endif
