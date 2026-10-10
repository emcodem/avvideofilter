#!/bin/bash
# Proves that the bundled FFmpeg stays separate from a system FFmpeg in the
# same process: the system libavutil is put into the global symbol scope
# (LD_PRELOAD, the worst case), then every symbol binding the dynamic loader
# makes for the plugin and the bundled libraries is checked. All FFmpeg calls
# must land in the bundle (*_avvf), none in the system copy.
#
#   scripts/check-bindings.sh <bundle-dir>
set -euo pipefail

DIR="$(cd "$1" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "check-bindings: $*" >&2; exit 1; }

sys="$(ldconfig -p | awk '/libavutil\.so\.[0-9]+ / {print $NF; exit}')"
if [ -n "$sys" ]; then
  echo "check-bindings: preloading system $(readlink -f "$sys")"
else
  echo "check-bindings: no system FFmpeg installed, checking without one"
fi

LD_PRELOAD="$sys" LD_DEBUG=bindings LD_DEBUG_OUTPUT="$TMP/ld" \
  GST_PLUGIN_PATH="$DIR" GST_REGISTRY="$TMP/registry.bin" \
  gst-launch-1.0 -q videotestsrc num-buffers=30 \
  ! avvideofilter filter-desc="framerate=fps=25" ! fakesink \
  || fail "pipeline failed"

# "binding file <from> [0] to <to> [0]: normal symbol `av_frame_alloc' [LIBAVUTIL_AVVF_60]"
cat "$TMP"/ld.* | grep -E "^ *[0-9]+:\s+binding file \S*(libgstavvideofilter\.so|_avvf\.so\.[0-9]+) " \
  > "$TMP/ours" || true
good="$(grep -cE " to \S*_avvf\.so\.[0-9]+ " "$TMP/ours" || true)"
bad="$(grep -E " to \S*/lib(av|sw|postproc)[a-z]*\.so\.[0-9]+ " "$TMP/ours" || true)"

[ -z "$bad" ] || fail "bundled code bound to the system FFmpeg:
$(echo "$bad" | head -20)"
[ "$good" -gt 0 ] || fail "no bindings into the bundled FFmpeg seen (plugin not loaded from $DIR?)"
echo "check-bindings: OK ($good bindings into the bundle, none into the system FFmpeg)"
