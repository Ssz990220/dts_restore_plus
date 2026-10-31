#!/bin/sh
set -eu
HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
if ! pkg-config --exists gstreamer-1.0; then
  echo 'NOT RUN: GStreamer development files required' >&2
  exit 77
fi
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -std=c99 -Wall -Wextra -Werror \
  $(pkg-config --cflags gstreamer-1.0) \
  "$HERE/test-pcm-route.c" -o "$WORK/test-pcm-route" \
  $(pkg-config --libs gstreamer-1.0)
GST_REGISTRY_UPDATE=no "$WORK/test-pcm-route"
