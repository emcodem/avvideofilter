#!/bin/bash
# Builds the plugin against the prebuilt FFmpeg in third_party/ffmpeg and packs
# a self-contained download:
#
#   avvideofilter-linux-amd64-glibc<min>-gst<min>.tar.gz
#     avvideofilter-<version>-linux-amd64-glibc<min>-gst<min>/
#     libgstavvideofilter.so          RUNPATH $ORIGIN/avvideofilter-ffmpeg
#     avvideofilter-ffmpeg/           libav*_avvf.so.N, licences, BUILDINFO
#
# Unpacked into any GStreamer plugin directory (or a GST_PLUGIN_PATH entry),
# the plugin loads its bundled FFmpeg, independent of the system's.
#
#   scripts/make-bundle.sh [version]      (default: version from meson.build)
#
# Needs: meson >= 0.62, ninja, pkg-config, GStreamer dev packages, patchelf.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/build-bundle"
VERSION="${1:-$(sed -n "s/^ *version *: *'\([^']*\)'.*/\1/p" "$ROOT/meson.build" | head -1)}"
# Build environment, read from the system that builds the bundle. The minimum
# glibc and GStreamer go into the file name (like manylinux wheels) and, with
# the rest, into BUILDINFO.txt, which the workflow copies into the release notes.
GST_BUILT="$(pkg-config --modversion gstreamer-1.0)"
GST_MIN="$(sed -n "s/^gst_req *= *'>= *\([0-9.]*\)'.*/\1/p" "$ROOT/meson.build")"
GLIBC="$(ldd --version | head -1 | grep -oE '[0-9]+\.[0-9]+$')"
BUILD_OS="$(. /etc/os-release && echo "$PRETTY_NAME")"
COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
# The file name has no version, so releases/latest/download/<name> is a stable
# URL; the directory inside carries the version.
NAME="avvideofilter-${VERSION}-linux-amd64-glibc${GLIBC}-gst${GST_MIN}"
TARBALL="avvideofilter-linux-amd64-glibc${GLIBC}-gst${GST_MIN}.tar.gz"
FFDIR="avvideofilter-ffmpeg"

shopt -s nullglob
tarballs=("$ROOT"/third_party/ffmpeg/ffmpeg-*-avvf-linux-amd64.tar.xz)
[ ${#tarballs[@]} -eq 1 ] || { echo "expected exactly one prebuilt FFmpeg tarball, found ${#tarballs[@]}" >&2; exit 1; }
FFTAR="${tarballs[0]}"
(cd "$(dirname "$FFTAR")" && sha256sum -c --quiet "$(basename "$FFTAR").sha256")

rm -rf "$WORK"
mkdir -p "$WORK/dist/$NAME/$FFDIR"
tar -C "$WORK" -xJf "$FFTAR"
FF="$WORK/ffmpeg-avvf"
# The .pc files carry the build container's absolute paths (prefix, libdir,
# includedir); point them at the unpacked copy.
sed -i "s|/opt/ffmpeg-avvf|$FF|g" "$FF"/lib/pkgconfig/*.pc

meson setup "$WORK/build" "$ROOT" --buildtype=release --prefix=/usr --libdir=lib \
  -Dpkg_config_path="$FF/lib/pkgconfig" \
  -Dffmpeg_suffix=_avvf -Dffmpeg_rpath="\$ORIGIN/$FFDIR" \
  -Drelease_version="$VERSION"
meson compile -C "$WORK/build"
DESTDIR="$WORK/stage" meson install -C "$WORK/build" --quiet

OUT="$WORK/dist/$NAME"
cp "$WORK/stage/usr/lib/gstreamer-1.0/libgstavvideofilter.so" "$OUT/"
# Runtime files only: one file per library, named by its SONAME (no .so
# symlinks: GStreamer would try to load anything ending in .so as a plugin).
for lib in "$FF"/lib/lib*_avvf.so.*.*.*; do
  soname="$(objdump -p "$lib" | awk '/SONAME/ {print $2}')"
  cp "$lib" "$OUT/$FFDIR/$soname"
done
cp "$FF/COPYING.LGPLv2.1" "$FF/LICENSE.md" "$FF/BUILDINFO.txt" "$OUT/$FFDIR/"
cp "$ROOT/COPYING" "$OUT/COPYING"

cat > "$OUT/BUILDINFO.txt" <<EOF
avvideofilter ${VERSION} (commit ${COMMIT})
Target:    Linux x86_64 (amd64), glibc >= ${GLIBC}
GStreamer: >= ${GST_MIN} (built against ${GST_BUILT})
Built on:  ${BUILD_OS}
FFmpeg:    $(head -1 "$FF/BUILDINFO.txt" | sed "s/^FFmpeg //"), bundled in ${FFDIR}/
EOF

cat > "$OUT/README.txt" <<EOF
avvideofilter ${VERSION}: GStreamer element applying a libavfilter graph.

Install: unpack this directory's contents into a GStreamer plugin directory,
e.g. ~/.local/share/gstreamer-1.0/plugins/, or into a directory listed in
GST_PLUGIN_PATH. Keep ${FFDIR}/ next to libgstavvideofilter.so.

Check:   gst-inspect-1.0 avvideofilter
Example: gst-launch-1.0 videotestsrc ! avvideofilter filter-desc="framerate=fps=25" ! autovideosink

Needs GStreamer >= ${GST_MIN} and glibc >= ${GLIBC}; see BUILDINFO.txt.
The bundled FFmpeg (${FFDIR}/) is used instead of the system's; see
${FFDIR}/BUILDINFO.txt. Plugin and FFmpeg are LGPL-2.1-or-later (COPYING).
Documentation: https://github.com/emcodem/avvideofilter
EOF
cp "$OUT/BUILDINFO.txt" "$WORK/dist/BUILDINFO.txt"

"$ROOT/scripts/check-bundle.sh" "$OUT"
tar -C "$WORK/dist" -czf "$WORK/dist/$TARBALL" "$NAME"
(cd "$WORK/dist" && sha256sum "$TARBALL" > "$TARBALL.sha256")
echo "$WORK/dist/$TARBALL"
