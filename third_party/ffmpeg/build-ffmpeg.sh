#!/bin/bash
# Builds the prebuilt FFmpeg that is committed to this repo and bundled with
# the plugin. Run it once per FFmpeg version, then commit the tarball:
#
#   third_party/ffmpeg/build-ffmpeg.sh            # needs Docker
#
# Why prebuilt and why like this:
# - Built in an Ubuntu 22.04 container: glibc 2.35 is the oldest target
#   (pipeline-controller supports Ubuntu 22.04 / Debian 12). Libraries built
#   on a newer system would not load there.
# - --build-suffix=_avvf: unique library names (libavutil_avvf.so.60). A
#   process that also loads the system FFmpeg (gst-libav's avdec_*) would
#   otherwise share one libavutil.so.60 between both by SONAME.
# - --disable-autodetect: no external libraries, so the bundle only needs
#   libc/libm. All of FFmpeg's built-in filters are included.
# - Own symbol versions (av_frame_alloc@LIBAVUTIL_AVVF_60 instead of
#   @LIBAVUTIL_60): references from the bundled libraries can never bind to
#   a system FFmpeg, even one of the same major version loaded globally.
# - No --enable-gpl / --enable-nonfree: the bundle stays LGPL-2.1-or-later.
# - RUNPATH $ORIGIN on each library, so they find each other in the bundle.
set -euo pipefail

FFMPEG_VERSION="${FFMPEG_VERSION:-8.0.1}"
# sha256 of ffmpeg-<version>.tar.xz; update together with the version
FFMPEG_SHA256="${FFMPEG_SHA256:-05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41}"
IMAGE="ubuntu:22.04"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="ffmpeg-${FFMPEG_VERSION}-avvf-linux-amd64.tar.xz"

if [ "${1:-}" != "--inside" ]; then
  exec docker run --rm --platform linux/amd64 \
    -e FFMPEG_VERSION="$FFMPEG_VERSION" -e FFMPEG_SHA256="$FFMPEG_SHA256" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -v "$HERE:/work" -w /work "$IMAGE" bash /work/build-ffmpeg.sh --inside
fi

# ── inside the container ──────────────────────────────────────────────────
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential nasm pkg-config curl ca-certificates xz-utils patchelf >/dev/null

cd /tmp
curl -fsSL -o ffmpeg.tar.xz "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
echo "$FFMPEG_SHA256  ffmpeg.tar.xz" | sha256sum -c --quiet
tar xf ffmpeg.tar.xz
cd "ffmpeg-${FFMPEG_VERSION}"

# Version scripts start with e.g. "LIBAVUTIL_MAJOR {"; the build replaces
# MAJOR with the major version. Insert our tag in front of it.
sed -i 's/^\(LIB[A-Z]*\)_MAJOR\b/\1_AVVF_MAJOR/' lib*/lib*.v
grep -qh '_AVVF_MAJOR' lib*/lib*.v || { echo "version scripts not patched" >&2; exit 1; }

CONFIGURE=(
  --prefix=/opt/ffmpeg-avvf
  --enable-shared --disable-static --enable-pic
  --build-suffix=_avvf
  --disable-autodetect
  --disable-programs --disable-doc --disable-avdevice --disable-network
  --disable-debug
)
./configure "${CONFIGURE[@]}" >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null

P=/opt/ffmpeg-avvf
for f in "$P"/lib/lib*_avvf.so.*.*.*; do
  strip --strip-unneeded "$f"
  patchelf --set-rpath '$ORIGIN' "$f"
  # every FFmpeg symbol version (defined or referenced) is an _AVVF_ one
  vers="$(objdump -T "$f" | grep -oE '\bLIB(AV|SW|POSTPROC)[A-Z]*_[A-Z0-9_]+' | sort -u)"
  grep -q '_AVVF_' <<<"$vers" || { echo "$f: no _AVVF_ symbol versions" >&2; exit 1; }
  grep -qv '_AVVF_' <<<"$vers" && { echo "$f: plain symbol versions left: $vers" >&2; exit 1; }
done
cp COPYING.LGPLv2.1 LICENSE.md "$P/"
{
  echo "FFmpeg ${FFMPEG_VERSION} (https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz)"
  echo "Built in ${IMAGE} ($(ldd --version 2>&1 | sed -n 1p))"
  echo "configure ${CONFIGURE[*]}"
} > "$P/BUILDINFO.txt"

tar -C /opt -cJf "/work/$OUT" ffmpeg-avvf
cd /work
sha256sum "$OUT" > "$OUT.sha256"
chown "$HOST_UID:$HOST_GID" "$OUT" "$OUT.sha256"
echo "built /work/$OUT"
tar -xOJf "$OUT" ffmpeg-avvf/BUILDINFO.txt
