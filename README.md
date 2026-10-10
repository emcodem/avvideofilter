# avvideofilter

A GStreamer element that runs an arbitrary [libavfilter](https://ffmpeg.org/ffmpeg-filters.html)
(FFmpeg) video filter graph inside a GStreamer pipeline.

```
gst-launch-1.0 videotestsrc ! avvideofilter filter-desc="framerate=fps=25" ! autovideosink
```

**`filter-desc` takes an FFmpeg-style video filter chain**, written exactly as
for `ffmpeg -vf`: filters separated by commas, options as `name=value` joined
by colons, e.g. `filter-desc="yadif=mode=send_field,framerate=fps=50"` or
`filter-desc="hqdn3d,framerate=fps=25:scene=100"`. Any video filter in the
[FFmpeg filter documentation](https://ffmpeg.org/ffmpeg-filters.html#Video-Filters)
can be used, as long as the chain has one input and one output (no `split`
into several outputs, no audio filters).

The main use is **blended frame-rate conversion** with FFmpeg's `framerate`
filter. GStreamer's `videorate` can only drop or duplicate frames; `framerate`
blends neighbouring frames, so motion stays smooth when, for example, 30 fps or
50 fps sources are converted to a 25 fps broadcast channel. It was written for
[pipeline-controller](https://github.com/infantilo/pipeline-controller) (a GStreamer-based playout system), but it is a generic GStreamer plugin and works in any pipeline.

The releases are **self-contained**: each one ships its own FFmpeg, so the
plugin works the same on every supported system, whatever FFmpeg version (if
any) the system has installed.

## Install

The latest release, into your user's GStreamer plugin directory:

```
mkdir -p ~/.local/share/gstreamer-1.0/plugins
wget -qO- https://github.com/emcodem/avvideofilter/releases/latest/download/avvideofilter-linux-amd64-glibc2.35-gst1.20.tar.gz | tar -xz -C ~/.local/share/gstreamer-1.0/plugins --strip-components=1
gst-inspect-1.0 avvideofilter
```

Running it again updates to the newest release. Older versions are on the
[releases](https://github.com/emcodem/avvideofilter/releases) page. The file
name has no version (so the link above stays the same), but states what the
system needs:

```
avvideofilter-linux-amd64-glibc2.35-gst1.20.tar.gz
              │           │         └ GStreamer 1.20 or newer
              │           └ glibc 2.35 or newer (Ubuntu 22.04+, Debian 12+)
              └ Linux, x86_64
```

`ldd --version` shows the system's glibc, `gst-inspect-1.0 --version` its
GStreamer. The version is in the directory inside the tarball
(`avvideofilter-<version>-linux-amd64-.../`), in `BUILDINFO.txt` and in
`gst-inspect-1.0 avvideofilter`.

`~/.local/share/gstreamer-1.0/plugins` is in the home directory of the user
who unpacks it: no root needed, and only that user's processes see the plugin.
GStreamer searches it automatically. For all users (e.g. a service running
under another account), unpack into the distribution's plugin directory
instead, as root: `/usr/lib/x86_64-linux-gnu/gstreamer-1.0` on Debian/Ubuntu,
`/usr/lib64/gstreamer-1.0` on Fedora/RHEL.

Any other directory works if it is listed in `GST_PLUGIN_PATH`. Keep the
`avvideofilter-ffmpeg/` folder next to `libgstavvideofilter.so`.

**Requirements:** Linux x86_64 (amd64) with glibc 2.35 or newer and GStreamer
1.20 or newer, e.g. Ubuntu 22.04+, Debian 12+. **No FFmpeg installation is
needed:** the release brings its own, and an FFmpeg installed on the system (of
any version) is neither used nor disturbed. Every release lists the exact
target, the GStreamer version it was built against and the systems it was
tested on (also in `BUILDINFO.txt` inside the tarball). Not supported: musl
systems (Alpine), and older glibc/GStreamer (e.g. RHEL/Rocky 9: glibc 2.34,
GStreamer 1.18).

## Usage

| Property | Default | Meaning |
|---|---|---|
| `filter-desc` | `null` | libavfilter chain with one input and one output, e.g. `framerate=fps=25`, `yadif`, `hqdn3d`. Changes apply at the next frame. |
| `latency-frames` | 0 | Latency reported in the LATENCY query, in output frames. |
| `latency-input-frames` | 0 | Latency in input frames, added to `latency-frames`. `framerate` holds back exactly one input frame for any rate pair, so use `latency-input-frames=1` in live pipelines. |
| `threads` | 0 | Filter graph threads (0 = libavfilter default). |

- **Formats:** 39 raw video formats whose memory layout is identical in
  GStreamer and FFmpeg (planar/semi-planar/packed YUV, RGB, gray, 8 to 16 bit,
  including NV12/P010 from hardware decoders). The output has the input's
  format. GPU memory is not supported (CPU-only element).
- **N:M:** the element can drop, add or hold back frames. Caps changes, EOS and
  GAP events drain the graph first; flushes discard held frames.
- **Interlacing:** interlace info is passed to FFmpeg as frame flags; the output
  caps follow the output frames (stays interlaced through `framerate`, becomes
  progressive after `yadif`).
- **Pin the size and rate in front of the element.** The graph may change size
  and frame rate, so the element does not pass downstream caps upstream. A
  `videoscale` or a source with flexible caps in front of it does not learn the
  target from behind it. Put a capsfilter before it:

  ```
  ... ! videoscale ! video/x-raw,width=1920,height=1080 ! avvideofilter filter-desc="framerate=fps=25" ! video/x-raw,framerate=25/1 ! ...
  ```

- **Hard cuts are not blended.** `framerate` detects scene changes (`scene=8.2`
  by default) and copies the nearest frame there instead of blending two shots
  into a ghost frame. `framerate=fps=25:scene=100` blends always.
- **Duration:** the output covers the input's time span and ends less than one
  output frame after it (FFmpeg rounds up). The last output frame is timed on
  the nominal frame interval, so a last input buffer that claims to last longer
  than that is not covered by the excess.
- **Debugging:** FFmpeg's log goes into GStreamer debug, categories
  `avvideofilter` and `libav` (e.g. `GST_DEBUG=avvideofilter:5`).

## How the release is built

```
third_party/ffmpeg/build-ffmpeg.sh   # once per FFmpeg version, result committed
scripts/make-bundle.sh <version>     # every release, in CI
scripts/check-bundle.sh <dir>        # static checks of the bundle
scripts/check-bindings.sh <dir>      # runtime check: FFmpeg stays separate
.github/workflows/release.yml        # build, test on several distros, release
```

### 1. FFmpeg, prebuilt once

`third_party/ffmpeg/build-ffmpeg.sh` builds FFmpeg from the official source
tarball (checksum pinned) in an **Ubuntu 22.04** Docker container and writes
`ffmpeg-<version>-avvf-linux-amd64.tar.xz`, which is committed to this repo.
FFmpeg changes rarely, so it is not rebuilt for every release.

- **Ubuntu 22.04 (glibc 2.35):** the oldest target system. A library built on a
  newer glibc does not load on an older one; built on the oldest, it runs on
  all newer ones.
- **All built-in filters, no external libraries** (`--disable-autodetect`): the
  bundle only needs libc/libm, so nothing else has to be shipped or installed.
  Filters that need external libraries (`drawtext`, `subtitles`, `zscale`,
  `libplacebo`, ...) are not included.
- **LGPL:** no `--enable-gpl` / `--enable-nonfree`, so GPL-only filters are not
  included and the bundle stays LGPL-2.1-or-later.
- **Own library names and symbol versions** (`--build-suffix=_avvf` plus
  patched version scripts), see [Two FFmpegs in one process](#two-ffmpegs-in-one-process).
- **RUNPATH `$ORIGIN`** on every library, so they find each other wherever the
  bundle is unpacked.

The exact configure line is in `avvideofilter-ffmpeg/BUILDINFO.txt` in every
release.

### 2. Plugin and bundle, every release

`scripts/make-bundle.sh` unpacks the prebuilt FFmpeg, builds the plugin against
it with Meson (`-Dffmpeg_suffix=_avvf -Dffmpeg_rpath=$ORIGIN/avvideofilter-ffmpeg`)
and assembles the tarball:

```
libgstavvideofilter.so          RUNPATH $ORIGIN/avvideofilter-ffmpeg
avvideofilter-ffmpeg/
  libavfilter_avvf.so.11        one file per library, named by its SONAME
  libavutil_avvf.so.60
  libavcodec_avvf.so.62
  libavformat_avvf.so.62
  libswscale_avvf.so.9
  libswresample_avvf.so.6
  BUILDINFO.txt, licences
BUILDINFO.txt                   target, GStreamer version, build system
README.txt, COPYING
```

- The plugin finds its FFmpeg through its RUNPATH, relative to its own
  location; no `LD_LIBRARY_PATH`, no system FFmpeg needed.
- The libraries are in a subfolder and there are no `*.so` symlinks:
  GStreamer scans plugin directories recursively and tries to load every file
  ending in `.so` as a plugin. Files ending in `.so.N` are skipped.
- GStreamer itself (and GLib) always comes from the system: a plugin runs inside
  the host's GStreamer. The GStreamer 1.x ABI is stable, so a plugin built
  against 1.20 works with every newer 1.x.
- Only the two plugin entry points are exported (`gnu_symbol_visibility: hidden`).

### 3. CI and releases

Every push to `master` runs `.github/workflows/release.yml`:

1. **build** in `ubuntu:22.04` (GStreamer 1.20): `make-bundle.sh`, then
   `check-bundle.sh`.
2. **test** on Ubuntu 22.04, Debian 12 and Ubuntu 24.04: the bundle is
   installed like a user would (`~/.local/share/gstreamer-1.0/plugins`), then
   `check-bundle.sh`, `check-bindings.sh` and the full test suite. Each system
   has gst-libav, i.e. a different system FFmpeg (4.4, 5.1, 6.1), loaded in the
   same process.
3. **release:** a GitHub release `v<major>.<minor>.<commit count>` (major.minor
   from `meson.build`) with the tarball, its SHA-256, the target (glibc,
   GStreamer) and the tested systems with their GStreamer versions. The
   tarball name carries the minimum glibc and GStreamer, both read from the
   build environment (`ldd --version`, `gst_req` in `meson.build`).

## Two FFmpegs in one process

GStreamer itself does not use FFmpeg, but the common gst-libav plugin
(`avdec_h264` and friends, also picked by `decodebin`) links the **system**
FFmpeg. In a pipeline that decodes with gst-libav and filters with
avvideofilter, two FFmpeg builds are loaded into the same process. That only
works if they cannot get mixed up, and three things could mix them:

1. **Same SONAME.** If both were called `libavutil.so.60`, the dynamic loader
   would load only one of them and both plugins would share it, whatever its
   version. **Fix:** `--build-suffix=_avvf`, so ours are `libavutil_avvf.so.60`
   etc.
2. **Symbol lookup across libraries.** When our `libavfilter` calls
   `av_frame_alloc`, the loader searches the process's global scope first. If
   a system FFmpeg sits there (a program linked against it, or `LD_PRELOAD`),
   the call could land in the system copy. Two things prevent that:
   - GStreamer loads plugins with `RTLD_LOCAL`, so neither gst-libav's FFmpeg
     nor ours is global in a normal pipeline.
   - FFmpeg versions every symbol (`av_frame_alloc@LIBAVUTIL_60`). Our build
     uses its **own version tags** (`LIBAVUTIL_AVVF_60`, by patching FFmpeg's
     version scripts before building). A reference to
     `av_frame_alloc@LIBAVUTIL_AVVF_60` can only bind to our library, even if a
     system FFmpeg of the same major version is loaded globally.
3. **FFmpeg objects crossing over.** An `AVFrame` from one copy must never reach
   the other. Between gst-libav and avvideofilter only GStreamer buffers (plain
   memory) are exchanged. Each copy keeps its own global state (log callback,
   CPU flags), so the two plugins also don't overwrite each other's settings.

**Verified by:**

- `check-bundle.sh`: every FFmpeg library resolves from the bundle (`ldd`), and
  every FFmpeg symbol version the plugin and the libraries define or reference
  is an `_AVVF_` one (`objdump -T`).
- `check-bindings.sh`: preloads the system `libavutil` into the global scope
  (the worst case), runs a pipeline with `LD_DEBUG=bindings` and fails if any
  call from the plugin or the bundled libraries binds to the system FFmpeg.
  This also runs against a system FFmpeg 8 (same major version as the bundle).
- test `coexists_with_system_libav`: gst-libav (MJPEG encode/decode) and
  avvideofilter in one pipeline; with the bundle, both FFmpeg copies must be
  loaded side by side.

## Building from source

Only needed for development; to use the plugin, take a release.

**Quick development build** against the FFmpeg development packages of your
system (FFmpeg 8 / libavfilter 11, which e.g. Ubuntu 26.04 ships; older FFmpeg
versions are untested). The result links that system FFmpeg and is not
self-contained:

```
sudo apt install meson ninja-build pkg-config libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev libavfilter-dev libavutil-dev
meson setup build && ninja -C build
GST_PLUGIN_PATH=build gst-inspect-1.0 avvideofilter
```

Needs Meson >= 0.62.

**The self-contained bundle**, as the releases are built. Uses the prebuilt
FFmpeg from `third_party/ffmpeg`, so no FFmpeg packages are needed; Docker
provides the Ubuntu 22.04 build environment:

```
docker run --rm -v "$PWD:/src" -w /src ubuntu:22.04 bash -c '
  apt-get update && apt-get install -y build-essential pkg-config ninja-build \
    python3-pip xz-utils binutils git libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
  pip3 install meson==1.5.2
  scripts/make-bundle.sh 0.1.0-local'
```

### Updating FFmpeg

1. Set `FFMPEG_VERSION` and `FFMPEG_SHA256` in `third_party/ffmpeg/build-ffmpeg.sh`.
2. Run it (needs Docker); it fails if any symbol version lacks the `_AVVF_` tag.
3. Remove the old tarball, commit the new one and its `.sha256`.

## Tests

```
GST_PLUGIN_PATH=build python3 tests/run_tests.py [test-name ...]
```

Needs `python3-gi`, `gir1.2-gst-plugins-base-1.0`, `gstreamer1.0-plugins-good`;
the coexistence test also `gstreamer1.0-libav` (skipped without it). The tests
cover seeking, flushing, caps and `filter-desc` changes, bit-exact passthrough
in every format, interlacing, GAP events, latency, output duration across rate
pairs and timestamp noise, and running next to gst-libav. They also pass under
AddressSanitizer:

```
meson setup build-asan -Db_sanitize=address && ninja -C build-asan
LD_PRELOAD=$(gcc -print-file-name=libasan.so) ASAN_OPTIONS=detect_leaks=0 \
  GST_PLUGIN_PATH=build-asan GST_REGISTRY=/tmp/asan-registry.bin python3 tests/run_tests.py
```

## License

LGPL-2.1-or-later (`COPYING`). The bundled FFmpeg is LGPL-2.1-or-later as well;
its source is the official release tarball named in
`avvideofilter-ffmpeg/BUILDINFO.txt`, built with `third_party/ffmpeg/build-ffmpeg.sh`.
