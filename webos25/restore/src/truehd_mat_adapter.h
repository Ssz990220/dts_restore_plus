/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CODEX_TRUEHD_MAT_ADAPTER_H
#define CODEX_TRUEHD_MAT_ADAPTER_H
#include <stddef.h>
#include <stdint.h>
/* Callback is synchronous; buffer belongs to the adapter. Return 0 on success.
 * This API emits ENCODED data, never PCM. The caller must use a separate ES
 * appsrc and verify its actual sink/resource ownership before authorizing. */
typedef int (*thd_mat_output)(void *, const uint8_t *, size_t, uint64_t,
                              uint64_t, uint32_t, uint32_t);
typedef struct thd_mat thd_mat;
uint32_t thd_mat_abi(void);
thd_mat *thd_mat_create(unsigned channels, thd_mat_output, void *);
void thd_mat_destroy(thd_mat *);
int thd_mat_authorize(thd_mat *, unsigned native_codec, unsigned data_type,
                      unsigned data_channels, unsigned data_bits,
                      unsigned data_rate, int pcm_bypass, int pcm_multi_ch);
/* Reset ONLY after a flushing seek/rebuild, never to hide a timestamp error.
 * origin_ns is the original media presentation time, shared with video. */
int thd_mat_new_segment(thd_mat *, uint64_t origin_ns, uint32_t epoch);
int thd_mat_push_au(thd_mat *, const uint8_t *, size_t, uint64_t pts_ns,
                    uint64_t pts_tolerance_ns);
/* EOS tail drain is experimental and must be separately receiver-validated. */
int thd_mat_finish(thd_mat *);
const char *thd_mat_error(thd_mat *);
#endif
