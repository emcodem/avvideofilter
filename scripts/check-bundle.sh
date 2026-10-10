#!/bin/bash
# Checks an unpacked bundle directory: the plugin resolves FFmpeg only from
# the bundle, nothing is missing, and only the plugin entry points are
# exported. Used by make-bundle.sh and on every test system in CI.
#
#   scripts/check-bundle.sh <bundle-dir>
set -euo pipefail

DIR="$(cd "$1" && pwd)"
PLUGIN="$DIR/libgstavvideofilter.so"
FFDIR="$DIR/avvideofilter-ffmpeg"
fail() { echo "check-bundle: $*" >&2; exit 1; }

[ -f "$PLUGIN" ] || fail "missing $PLUGIN"
compgen -G "$DIR/*.so.*" >/dev/null && fail "versioned libraries next to the plugin"
compgen -G "$FFDIR/*.so" >/dev/null && fail ".so files in $FFDIR (GStreamer would scan them)"

deps="$(ldd "$PLUGIN" "$FFDIR"/*.so.*)"
grep -q "not found" <<<"$deps" && fail "unresolved libraries:
$(echo "$deps" | grep "not found")"
# every libav*/libsw* must come from the bundle, never from the system
bad="$(echo "$deps" | grep -E '\blib(av|sw|postproc)' | grep -v "$FFDIR/" || true)"
[ -z "$bad" ] || fail "FFmpeg libraries resolved outside the bundle:
$bad"
grep -qE '\blib(av|sw)[a-z]*_avvf' <<<"$deps" || fail "plugin does not link the bundled FFmpeg"

# FFmpeg symbol versions referenced or defined anywhere in the bundle must be
# the bundle's own (_AVVF_), so nothing can bind to a system FFmpeg
vers="$(objdump -T "$PLUGIN" "$FFDIR"/*.so.* | grep -oE '\bLIB(AV|SW|POSTPROC)[A-Z]*_[A-Z0-9_]+' | sort -u)"
grep -q '_AVVF_' <<<"$vers" || fail "no _AVVF_ symbol versions found"
plain="$(echo "$vers" | grep -v '_AVVF_' || true)"
[ -z "$plain" ] || fail "plain FFmpeg symbol versions: $plain"

exports="$(nm -D --defined-only "$PLUGIN" | awk '$2 ~ /[TDBR]/ {print $3}' | grep -v '^_' | sort)"
[ "$exports" = "$(printf 'gst_plugin_avvideofilter_get_desc\ngst_plugin_avvideofilter_register')" ] \
  || fail "unexpected exported symbols:
$exports"

echo "check-bundle: OK ($DIR)"
