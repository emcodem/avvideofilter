#!/usr/bin/env python3
"""Robustness tests for avvideofilter (milestone 2).

Run:  GST_PLUGIN_PATH=build python3 tests/run_tests.py [test-name ...]
Each test builds a pipeline ending in an appsink, collects (pts, duration, caps)
of every output buffer and checks counts / timestamps / caps.
"""
import re
import sys
import threading
import time

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstVideo", "1.0")
from gi.repository import Gst, GLib, GstVideo  # noqa: E402

Gst.init(None)

MS = Gst.MSECOND
TIMEOUT_S = 30


class Run:
    """Pipeline runner. Records every buffer the element "f" pushes (probe on
    its src pad, so frames a downstream sink would clip are still seen),
    plus errors and EOS. The pipeline must end in a sink named "out"."""

    def __init__(self, desc):
        self.pipe = Gst.parse_launch(desc)
        self.sink = self.pipe.get_by_name("out")
        self.sink.set_property("sync", False)
        self.srcpad = self.pipe.get_by_name("f").get_static_pad("src")
        self.srcpad.add_probe(Gst.PadProbeType.BUFFER, self._on_buffer)
        self.bufs = []           # (pts, duration, (w, h, fps_n, fps_d))
        self.errors = []
        self.eos = threading.Event()
        self.lock = threading.Lock()
        self.on_buffer = None    # optional callback(run, count) from streaming thread
        bus = self.pipe.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self._on_msg)
        self.loop = GLib.MainLoop()

    def _on_buffer(self, pad, info):
        b = info.get_buffer()
        st = pad.get_current_caps().get_structure(0)
        fr = st.get_fraction("framerate")
        caps = (st.get_value("width"), st.get_value("height"), fr[1], fr[2])
        with self.lock:
            self.bufs.append((b.pts, b.duration, caps))
            n = len(self.bufs)
        if self.on_buffer:
            self.on_buffer(self, n)
        return Gst.PadProbeReturn.OK

    def _on_msg(self, bus, msg):
        if msg.type == Gst.MessageType.ERROR:
            err, dbg = msg.parse_error()
            self.errors.append(f"{err.message} | {dbg}")
            self.loop.quit()
        elif msg.type == Gst.MessageType.EOS:
            self.eos.set()
            self.loop.quit()

    def run(self, before_play=None):
        GLib.timeout_add_seconds(TIMEOUT_S, self._timeout)
        if before_play:
            before_play(self)
        self.pipe.set_state(Gst.State.PLAYING)
        self.loop.run()
        self.pipe.set_state(Gst.State.NULL)
        return self

    def _timeout(self):
        self.errors.append("TIMEOUT")
        self.loop.quit()
        return False

    def preroll(self):
        self.pipe.set_state(Gst.State.PAUSED)
        self.pipe.get_state(10 * Gst.SECOND)


# ── helpers ────────────────────────────────────────────────────────────────

class Fail(Exception):
    pass


class Skip(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def check_monotonic(bufs, label=""):
    for i in range(1, len(bufs)):
        check(bufs[i][0] > bufs[i - 1][0],
              f"{label}pts not increasing at {i}: {bufs[i-1][0]} -> {bufs[i][0]}")


def check_grid(bufs, step, start=None, tol=MS):
    """pts on a regular grid of `step` ns, starting at bufs[0] (or start)."""
    t0 = bufs[0][0] if start is None else start
    for i, (pts, dur, _) in enumerate(bufs):
        check(abs(pts - (t0 + i * step)) <= tol,
              f"pts[{i}]={pts} expected {t0 + i * step}")
        check(dur == step, f"duration[{i}]={dur} expected {step}")


def src(w=640, h=360, fps=30, n=-1, extra=""):
    return (f"videotestsrc num-buffers={n} {extra} ! "
            f"capsfilter name=cf caps=video/x-raw,format=I420,width={w},height={h},framerate={fps}/1")


FILTER = "avvideofilter name=f"
SINK = "fakesink name=out"


# ── tests ──────────────────────────────────────────────────────────────────

def t_rate_30_to_25():
    r = Run(f"{src(n=300)} ! {FILTER} filter-desc=framerate=fps=25 ! {SINK}").run()
    check(not r.errors, r.errors)
    check(len(r.bufs) == 250, f"got {len(r.bufs)} buffers, want 250")
    check(r.bufs[0][2] == (640, 360, 25, 1), f"caps {r.bufs[0][2]}")
    check_grid(r.bufs, 40 * MS, start=0)


def t_passthrough():
    r = Run(f"{src(n=100)} ! {FILTER} filter-desc=null ! {SINK}").run()
    check(not r.errors, r.errors)
    check(len(r.bufs) == 100, f"got {len(r.bufs)}")
    check_monotonic(r.bufs)
    check(abs(r.bufs[99][0] - 99 * Gst.SECOND // 30) <= MS, f"last pts {r.bufs[99][0]}")


def t_eos_without_buffers():
    r = Run(f"{src(n=0)} ! {FILTER} filter-desc=framerate=fps=25 ! {SINK}").run()
    check(not r.errors, r.errors)
    check(r.eos.is_set(), "no EOS")
    check(len(r.bufs) == 0, f"got {len(r.bufs)}")


def t_seek_preroll():
    """Flushing seek before playing: output starts at the seek target."""
    r = Run(f"{src()} ! {FILTER} filter-desc=framerate=fps=25 ! {SINK}")

    def seek(run):
        run.preroll()
        ok = run.pipe.seek(1.0, Gst.Format.TIME,
                           Gst.SeekFlags.FLUSH | Gst.SeekFlags.ACCURATE,
                           Gst.SeekType.SET, 5 * Gst.SECOND,
                           Gst.SeekType.SET, 8 * Gst.SECOND)
        check(ok, "seek failed")
        run.pipe.get_state(10 * Gst.SECOND)
        with run.lock:
            run.bufs.clear()   # drop the preroll buffer(s) from before the seek
    r.run(before_play=seek)
    check(not r.errors, r.errors)
    check(r.bufs, "no buffers after seek")
    check(abs(r.bufs[0][0] - 5 * Gst.SECOND) <= 40 * MS, f"first pts {r.bufs[0][0]}")
    check_monotonic(r.bufs)
    check(73 <= len(r.bufs) <= 77, f"got {len(r.bufs)} buffers for 3 s, want ~75")


def t_seek_midstream():
    """Flushing seek while streaming: no stale frames from before the seek."""
    r = Run(f"{src()} ! {FILTER} filter-desc=framerate=fps=25 ! {SINK}")
    state = {"seeked": False, "mark": None}

    def on_buf(run, n):
        if n == 50 and not state["seeked"]:
            state["seeked"] = True

            def do_seek():
                run.pipe.seek(1.0, Gst.Format.TIME,
                              Gst.SeekFlags.FLUSH | Gst.SeekFlags.ACCURATE,
                              Gst.SeekType.SET, 10 * Gst.SECOND,
                              Gst.SeekType.SET, 12 * Gst.SECOND)
                with run.lock:
                    state["mark"] = len(run.bufs)
                return False
            GLib.idle_add(do_seek)
    r.on_buffer = on_buf
    r.run()
    check(not r.errors, r.errors)
    after = [b for b in r.bufs if b[0] >= 10 * Gst.SECOND]
    stale = [b for b in r.bufs[state["mark"]:] if b[0] < 10 * Gst.SECOND]
    check(not stale, f"{len(stale)} stale pre-seek buffers pushed after the seek")
    check(after and abs(after[0][0] - 10 * Gst.SECOND) <= 40 * MS, f"first post-seek pts {after[0][0] if after else None}")
    check_monotonic(after, "post-seek ")
    check(48 <= len(after) <= 52, f"got {len(after)} post-seek buffers, want ~50")


def t_seek_backward():
    """Flushing seek backwards (2 s -> 0.5 s): output restarts at the target
    with a clean, monotonic grid. (Doesn't detect a stale graph: framerate
    resets itself on backward jumps; see flush_discards_held_frames.)"""
    r = Run(f"{src()} ! {FILTER} filter-desc=framerate=fps=25 ! {SINK}")
    state = {"seeked": False, "mark": None}

    def on_buf(run, n):
        if n == 50 and not state["seeked"]:
            state["seeked"] = True

            def do_seek():
                run.pipe.seek(1.0, Gst.Format.TIME,
                              Gst.SeekFlags.FLUSH | Gst.SeekFlags.ACCURATE,
                              Gst.SeekType.SET, 500 * MS,
                              Gst.SeekType.SET, 2500 * MS)
                with run.lock:
                    state["mark"] = len(run.bufs)
                return False
            GLib.idle_add(do_seek)
    r.on_buffer = on_buf
    r.run()
    check(not r.errors, r.errors)
    after = r.bufs[state["mark"]:]
    check(after and abs(after[0][0] - 500 * MS) <= 40 * MS,
          f"first post-seek pts {after[0][0] if after else None}, want 0.5 s")
    check_monotonic(after, "post-seek ")
    check(48 <= len(after) <= 52, f"got {len(after)} post-seek buffers, want ~50")


def t_flush_discards_held_frames():
    """"reverse" holds every frame until EOS. After a flushing seek, only the
    post-seek frames may come out; a stale graph would also emit the ~50
    frames buffered before the seek. (framerate can't detect this: it resets
    itself on pts jumps in either direction.)"""
    r = Run(f"{src()} ! {FILTER} filter-desc=reverse ! {SINK}")
    state = {"in": 0, "seeked": False}

    def probe(pad, info):
        state["in"] += 1
        if state["in"] == 50 and not state["seeked"]:
            state["seeked"] = True

            def do_seek():
                r.pipe.seek(1.0, Gst.Format.TIME,
                            Gst.SeekFlags.FLUSH | Gst.SeekFlags.ACCURATE,
                            Gst.SeekType.SET, 10 * Gst.SECOND,
                            Gst.SeekType.SET, 12 * Gst.SECOND)
                return False
            GLib.idle_add(do_seek)
        return Gst.PadProbeReturn.OK

    r.pipe.get_by_name("f").get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, probe)
    r.run()
    check(not r.errors, r.errors)
    check(state["seeked"], "seek never triggered")
    early = [b for b in r.bufs if b[0] < 10 * Gst.SECOND]
    check(not early, f"{len(early)} frames from before the seek were emitted")
    check(len(r.bufs) == 60, f"got {len(r.bufs)} frames, want 60 (2 s @ 30 fps)")


def _caps_change(new_caps, want_after):
    r = Run(f"{src(n=180)} ! {FILTER} filter-desc=framerate=fps=25 ! {SINK}")
    state = {"done": False}

    def on_buf(run, n):
        if n == 60 and not state["done"]:
            state["done"] = True
            run.pipe.get_by_name("cf").set_property("caps", Gst.Caps.from_string(new_caps))
    r.on_buffer = on_buf
    r.run()
    check(not r.errors, r.errors)
    caps_seq = []
    for b in r.bufs:
        if not caps_seq or caps_seq[-1] != b[2]:
            caps_seq.append(b[2])
    check(caps_seq == [(640, 360, 25, 1), want_after], f"caps sequence {caps_seq}")
    check_monotonic(r.bufs)
    return r


def t_caps_change_size():
    r = _caps_change("video/x-raw,format=I420,width=1280,height=720,framerate=30/1", (1280, 720, 25, 1))
    # 180 frames @30 = 6 s -> ~150 @25, allow a frame or two around the switch
    check(147 <= len(r.bufs) <= 151, f"got {len(r.bufs)}")


def t_caps_change_fps():
    """Input fps changes 30 -> 50 mid-stream; output stays 25 fps."""
    r = Run(f"{src(n=180)} ! {FILTER} filter-desc=framerate=fps=25 ! {SINK}")
    state = {"done": False}

    def on_buf(run, n):
        if n == 60 and not state["done"]:
            state["done"] = True
            run.pipe.get_by_name("cf").set_property(
                "caps", Gst.Caps.from_string("video/x-raw,format=I420,width=640,height=360,framerate=50/1"))
    r.on_buffer = on_buf
    r.run()
    check(not r.errors, r.errors)
    check(all(b[2] == (640, 360, 25, 1) for b in r.bufs), "output caps changed")
    check_monotonic(r.bufs)


def t_desc_change_runtime():
    """Changing filter-desc while playing takes effect without a caps change."""
    r = Run(f"{src(n=180)} ! {FILTER} filter-desc=framerate=fps=25 ! {SINK}")
    state = {"done": False}

    def on_buf(run, n):
        if n == 50 and not state["done"]:
            state["done"] = True
            run.pipe.get_by_name("f").set_property("filter-desc", "framerate=fps=50")
    r.on_buffer = on_buf
    r.run()
    check(not r.errors, r.errors)
    fps_seq = []
    for b in r.bufs:
        if not fps_seq or fps_seq[-1] != b[2][2]:
            fps_seq.append(b[2][2])
    check(fps_seq == [25, 50], f"fps sequence {fps_seq}")
    check_monotonic(r.bufs)


def _template_formats():
    """Every format in the element's sink template, so new formats get tested."""
    f = Gst.ElementFactory.find("avvideofilter")
    for t in f.get_static_pad_templates():
        if t.direction == Gst.PadDirection.SINK:
            # PyGObject can't unpack GstValueList; parse the caps string
            m = re.search(r"format=\(string\)\{([^}]*)\}", t.get_caps().to_string())
            # skip formats this GStreamer doesn't know (GBR_16LE before 1.24)
            return [x.strip() for x in m.group(1).split(",")
                    if GstVideo.VideoFormat.from_string(x.strip()) != GstVideo.VideoFormat.UNKNOWN]
    return []


FORMATS = _template_formats()


def t_framerate_formats():
    """framerate 30 -> 25 in every format (most get converted inside the graph)."""
    for fmt in FORMATS:
        r = Run(f"videotestsrc num-buffers=30 ! video/x-raw,format={fmt},width=320,height=180,framerate=30/1 ! "
                f"{FILTER} filter-desc=framerate=fps=25 ! {SINK}").run()
        check(not r.errors, f"{fmt}: {r.errors}")
        check(len(r.bufs) == 25, f"{fmt}: got {len(r.bufs)}, want 25")


def t_bitexact_formats():
    """filter-desc=null must reproduce the input bytes exactly, per format.
    640x360 has no row padding in any format, so whole buffers compare."""
    import hashlib
    for fmt in FORMATS:
        pipe = Gst.parse_launch(
            f"videotestsrc num-buffers=10 pattern=smpte ! "
            f"video/x-raw,format={fmt},width=640,height=360,framerate=25/1 ! "
            f"{FILTER} filter-desc=null ! fakesink name=out sync=false")
        sums = {"in": [], "out": []}

        def mk(key):
            def probe(pad, info):
                ok, m = info.get_buffer().map(Gst.MapFlags.READ)
                sums[key].append(hashlib.md5(bytes(m.data)).hexdigest())
                info.get_buffer().unmap(m)
                return Gst.PadProbeReturn.OK
            return probe
        f = pipe.get_by_name("f")
        f.get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, mk("in"))
        f.get_static_pad("src").add_probe(Gst.PadProbeType.BUFFER, mk("out"))
        pipe.set_state(Gst.State.PLAYING)
        msg = pipe.get_bus().timed_pop_filtered(10 * Gst.SECOND, Gst.MessageType.EOS | Gst.MessageType.ERROR)
        pipe.set_state(Gst.State.NULL)
        err = None if msg and msg.type == Gst.MessageType.EOS else (msg.parse_error()[1] if msg else "timeout")
        check(err is None, f"{fmt}: no EOS ({err})")
        check(len(sums["in"]) == 10, f"{fmt}: {len(sums['in'])} input buffers")
        check(sums["in"] == sums["out"], f"{fmt}: output differs from input")


def t_odd_sizes():
    """Odd input and output sizes in every format (chroma rounding, strides)."""
    for fmt in FORMATS:
        r = Run(f"videotestsrc num-buffers=5 ! video/x-raw,format={fmt},width=641,height=361,framerate=25/1 ! "
                f"{FILTER} filter-desc=scale=323:183 ! {SINK}").run()
        check(not r.errors, f"{fmt}: {r.errors}")
        check(len(r.bufs) == 5, f"{fmt}: got {len(r.bufs)}")
        check(r.bufs[0][2][:2] == (323, 183), f"{fmt}: caps {r.bufs[0][2]}")


def t_desc_change_refused_downstream():
    """Like pipeline-controller: fixed 25 fps caps after the filter. A runtime
    filter-desc change to 50 fps must end in a negotiation error, not a hang."""
    r = Run(f"{src()} ! {FILTER} filter-desc=framerate=fps=25 ! "
            f"video/x-raw,framerate=25/1 ! {SINK}")
    state = {"done": False}

    def on_buf(run, n):
        if n == 20 and not state["done"]:
            state["done"] = True
            run.pipe.get_by_name("f").set_property("filter-desc", "framerate=fps=50")
    r.on_buffer = on_buf
    r.run()
    check(r.errors and "TIMEOUT" not in r.errors[0], f"expected an error, got {r.errors or 'none'}")
    check(all(b[2][2] == 25 for b in r.bufs), "a 50 fps buffer was pushed")


def _interlaced_src(n=30, order="top-field-first", pattern="smpte", mode="interleaved"):
    return (f"videotestsrc num-buffers={n} pattern={pattern} ! "
            f"video/x-raw,format=I420,width=320,height=180,framerate=30/1 ! "
            f"capssetter caps=\"video/x-raw,interlace-mode={mode},field-order={order}\"")


def _out_caps_strings(desc_src, filter_desc):
    """Run, return (distinct output caps strings, buffer count, errors)."""
    pipe = Gst.parse_launch(f"{desc_src} ! {FILTER} filter-desc=\"{filter_desc}\" ! fakesink sync=false")
    seen, count = [], [0]

    def probe(pad, info):
        c = pad.get_current_caps().to_string()
        if not seen or seen[-1] != c:
            seen.append(c)
        count[0] += 1
        return Gst.PadProbeReturn.OK
    pipe.get_by_name("f").get_static_pad("src").add_probe(Gst.PadProbeType.BUFFER, probe)
    pipe.set_state(Gst.State.PLAYING)
    msg = pipe.get_bus().timed_pop_filtered(10 * Gst.SECOND, Gst.MessageType.EOS | Gst.MessageType.ERROR)
    pipe.set_state(Gst.State.NULL)
    err = None if msg and msg.type == Gst.MessageType.EOS else (msg.parse_error()[1] if msg else "timeout")
    return seen, count[0], err


def t_interlace_kept_through_framerate():
    """Interlaced input through framerate: output caps stay interleaved/TFF
    (before milestone 2 they were silently relabeled progressive)."""
    seen, n, err = _out_caps_strings(_interlaced_src(), "framerate=fps=25")
    check(not err, err)
    check(n == 25, f"got {n} buffers")
    check(len(seen) == 1 and "interlace-mode=(string)interleaved" in seen[0]
          and "field-order=(string)top-field-first" in seen[0], f"caps {seen}")


def t_interlace_bff_kept():
    seen, n, err = _out_caps_strings(_interlaced_src(order="bottom-field-first"), "null")
    check(not err, err)
    check(len(seen) == 1 and "field-order=(string)bottom-field-first" in seen[0], f"caps {seen}")


def t_deinterlacer_makes_progressive():
    """A deinterlacer in the graph clears the frame flag -> progressive caps."""
    seen, n, err = _out_caps_strings(_interlaced_src(), "yadif")
    check(not err, err)
    check(n == 30, f"got {n} buffers")
    check(seen and "interlace-mode=(string)progressive" in seen[-1], f"caps {seen}")
    check(all("interleaved" not in c for c in seen), f"interleaved caps pushed with buffers: {seen}")


def t_field_flags_reach_ffmpeg():
    """yadif=deint=interlaced only processes frames flagged interlaced: content
    must change for interlaced input and stay bit-identical for progressive."""
    import hashlib

    def sums(src_desc):
        pipe = Gst.parse_launch(f"{src_desc} ! {FILTER} filter-desc=yadif=deint=interlaced ! fakesink sync=false")
        h = {"in": [], "out": []}

        def mk(key):
            def probe(pad, info):
                ok, m = info.get_buffer().map(Gst.MapFlags.READ)
                h[key].append(hashlib.md5(bytes(m.data)).hexdigest())
                info.get_buffer().unmap(m)
                return Gst.PadProbeReturn.OK
            return probe
        f = pipe.get_by_name("f")
        f.get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, mk("in"))
        f.get_static_pad("src").add_probe(Gst.PadProbeType.BUFFER, mk("out"))
        pipe.set_state(Gst.State.PLAYING)
        pipe.get_bus().timed_pop_filtered(10 * Gst.SECOND, Gst.MessageType.EOS | Gst.MessageType.ERROR)
        pipe.set_state(Gst.State.NULL)
        return h
    inter = sums(_interlaced_src(n=10, pattern="snow"))
    prog = sums(f"videotestsrc num-buffers=10 pattern=snow ! video/x-raw,format=I420,width=320,height=180,framerate=30/1")
    check(len(inter["out"]) == 10 and len(prog["out"]) == 10, "frame counts")
    changed = sum(a != b for a, b in zip(inter["in"], inter["out"]))
    check(changed == 10, f"interlaced: only {changed}/10 frames deinterlaced (flags not passed?)")
    check(prog["in"] == prog["out"], "progressive: frames were modified")


def t_alternate_rejected():
    seen, n, err = _out_caps_strings(_interlaced_src(mode="alternate"), "null")
    check(err and n == 0, f"expected an error, got err={err} n={n}")


def t_gap_pushes_held_frames_first():
    """"reverse" holds all frames. A GAP event must not overtake them: the held
    frames come out first, then the gap."""
    pipe = Gst.parse_launch(f"{src()} ! {FILTER} filter-desc=reverse ! fakesink sync=false")
    f = pipe.get_by_name("f")
    seq, state = [], {"n": 0}

    def in_probe(pad, info):
        state["n"] += 1
        if state["n"] == 31:
            b = info.get_buffer()
            pad.send_event(Gst.Event.new_gap(b.pts, b.duration))
            GLib.idle_add(lambda: (pipe.set_state(Gst.State.NULL), loop.quit(), False)[2])
            return Gst.PadProbeReturn.DROP
        return Gst.PadProbeReturn.OK

    def out_probe(pad, info):
        if info.type & Gst.PadProbeType.BUFFER:
            seq.append("buf")
        elif info.get_event().type == Gst.EventType.GAP:
            seq.append("gap")
        return Gst.PadProbeReturn.OK
    f.get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, in_probe)
    f.get_static_pad("src").add_probe(Gst.PadProbeType.BUFFER | Gst.PadProbeType.EVENT_DOWNSTREAM, out_probe)
    loop = GLib.MainLoop()
    GLib.timeout_add_seconds(10, lambda: (loop.quit(), False)[1])
    pipe.set_state(Gst.State.PLAYING)
    loop.run()
    pipe.set_state(Gst.State.NULL)
    check("gap" in seq, f"no gap seen: {seq[:5]}")
    gi_ = seq.index("gap")
    check(gi_ == 30 and seq[:30] == ["buf"] * 30, f"gap at position {gi_}, want 30 (after the 30 held frames)")


def t_prefers_planar_yuv():
    """With a free choice upstream, I420 is negotiated (natively processed by
    most filters), not e.g. GBRA in upstream's own preference order."""
    pipe = Gst.parse_launch(f"videotestsrc num-buffers=1 ! {FILTER} ! fakesink")
    pipe.set_state(Gst.State.PLAYING)
    pipe.get_bus().timed_pop_filtered(10 * Gst.SECOND, Gst.MessageType.EOS | Gst.MessageType.ERROR)
    caps = pipe.get_by_name("f").get_static_pad("sink").get_current_caps()
    pipe.set_state(Gst.State.NULL)
    check(caps and caps.get_structure(0).get_value("format") == "I420", f"negotiated {caps}")


def t_no_ffmpeg_stderr():
    """FFmpeg's log goes through GStreamer debug, not straight to stderr."""
    import os
    import subprocess
    out = subprocess.run(
        ["gst-launch-1.0", "-q", "videotestsrc", "num-buffers=30", "!", "avvideofilter",
         "filter-desc=framerate=fps=25", "!", "fakesink"],
        capture_output=True, text=True, timeout=30, env={**os.environ, "GST_DEBUG": ""})
    check(out.returncode == 0, out.stderr[-300:])
    check("[Parsed_" not in out.stderr and "@ 0x" not in out.stderr, f"stderr: {out.stderr[:300]}")


def _expect_error(desc):
    r = Run(f"{src(n=30)} ! {FILTER} filter-desc=\"{desc}\" ! {SINK}").run()
    check(r.errors and "TIMEOUT" not in r.errors[0], f"expected an error, got {r.errors or 'none'}")
    return r.errors[0]


def t_invalid_desc():
    err = _expect_error("thisfilterdoesnotexist")
    check("No such filter: 'thisfilterdoesnotexist'" in err,
          f"FFmpeg's error detail missing from the element error: {err}")


def t_multi_output_desc():
    _expect_error("split=2[a][b]")


def t_state_cycles():
    pipe = Gst.parse_launch(f"{src()} ! {FILTER} filter-desc=framerate=fps=25 ! fakesink sync=false")
    for _ in range(20):
        pipe.set_state(Gst.State.PLAYING)
        check(pipe.get_state(5 * Gst.SECOND)[0] == Gst.StateChangeReturn.SUCCESS, "PLAYING failed")
        time.sleep(0.02)
        pipe.set_state(Gst.State.READY)
        pipe.get_state(5 * Gst.SECOND)
    pipe.set_state(Gst.State.NULL)


def t_latency_query():
    pipe = Gst.parse_launch(
        f"{src(extra='is-live=true')} ! {FILTER} filter-desc=framerate=fps=25 latency-frames=3 ! fakesink name=out")
    pipe.set_state(Gst.State.PLAYING)
    pipe.get_state(5 * Gst.SECOND)
    q = Gst.Query.new_latency()
    check(pipe.get_by_name("out").get_static_pad("sink").peer_query(q), "latency query failed")
    live, mn, mx = q.parse_latency()
    pipe.set_state(Gst.State.NULL)
    # videotestsrc live reports 1 input frame (33.3 ms) + ours 3 x 40 ms
    check(live and abs(mn - (Gst.SECOND // 30 + 120 * MS)) <= MS, f"min latency {mn}")



def _query_latency(pipe):
    q = Gst.Query.new_latency()
    check(pipe.get_by_name("out").get_static_pad("sink").peer_query(q), "latency query failed")
    return q.parse_latency()


def t_latency_input_frames():
    """latency-input-frames counts input frames (30 fps in -> 33.3 ms each)."""
    pipe = Gst.parse_launch(
        f"{src(extra='is-live=true')} ! {FILTER} filter-desc=framerate=fps=25 latency-input-frames=1 ! fakesink name=out")
    pipe.set_state(Gst.State.PLAYING)
    pipe.get_state(5 * Gst.SECOND)
    live, mn, _ = _query_latency(pipe)
    pipe.set_state(Gst.State.NULL)
    # videotestsrc live reports 1 input frame + ours 1 input frame
    check(live and abs(mn - 2 * Gst.SECOND // 30) <= MS, f"min latency {mn}")


def t_latency_before_negotiation():
    """A live pipeline queries latency on the way to PLAYING, before caps are
    negotiated. The rates then come from the neighbours' fixed caps."""
    # appsrc + capsfilter stand in for a capture source with a fixed mode: it
    # answers latency without data (videotestsrc only once it has negotiated).
    pipe = Gst.parse_launch(
        f"appsrc is-live=true format=time min-latency={Gst.SECOND // 30} "
        f"! video/x-raw,format=I420,width=640,height=360,framerate=30/1 ! {FILTER} "
        f"filter-desc=framerate=fps=25 latency-frames=1 latency-input-frames=1 "
        f"! video/x-raw,framerate=25/1 ! fakesink name=out")
    pipe.set_state(Gst.State.PAUSED)   # live: no data flows in PAUSED
    pipe.get_state(5 * Gst.SECOND)
    check(not pipe.get_by_name("f").get_static_pad("sink").get_current_caps(), "caps already negotiated")
    live, mn, _ = _query_latency(pipe)
    pipe.set_state(Gst.State.NULL)
    # src 33.3 + ours 1 x 40 (out) + 1 x 33.3 (in)
    want = 2 * Gst.SECOND // 30 + 40 * MS
    check(live and abs(mn - want) <= MS, f"min latency {mn}, want {want}")


def t_latency_covers_hold():
    """For framerate, 1 input frame of latency covers how long frames are held:
    an output frame ending at t leaves once the input reaching t' arrives, and
    a live source delivers it at t' (its own latency is 1 input frame), so the
    element must add max(t' - t) + out - in. Checked for up- and down-rates."""
    for fin, fout in [(30, 25), (25, 25), (50, 25), (25, 50), (24, 25), (60, 50)]:
        pipe = Gst.parse_launch(
            f"{src(w=320, h=180, fps=fin, n=90)} ! {FILTER} filter-desc=framerate=fps={fout} "
            f"latency-input-frames=1 ! fakesink name=out sync=false")
        f = pipe.get_by_name("f")
        st = {"in_end": 0}
        need = []

        def pin(pad, info, st=st):
            b = info.get_buffer()
            st["in_end"] = b.pts + b.duration
            return Gst.PadProbeReturn.OK

        def pout(pad, info, st=st, need=need, fin=fin):
            b = info.get_buffer()
            need.append(st["in_end"] - b.pts - Gst.SECOND // fin)
            return Gst.PadProbeReturn.OK
        f.get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, pin)
        f.get_static_pad("src").add_probe(Gst.PadProbeType.BUFFER, pout)
        pipe.set_state(Gst.State.PLAYING)
        msg = pipe.get_bus().timed_pop_filtered(10 * Gst.SECOND, Gst.MessageType.EOS | Gst.MessageType.ERROR)
        check(msg and msg.type == Gst.MessageType.EOS, f"{fin}->{fout}: no EOS")
        _, total, _ = _query_latency(pipe)
        uq = Gst.Query.new_latency()
        check(f.get_static_pad("sink").peer_query(uq), "upstream latency query failed")
        mn = total - uq.parse_latency()[1]   # the element's own part
        pipe.set_state(Gst.State.NULL)
        worst = max(need[:-5])          # the last frames leave on EOS drain
        check(worst <= mn + MS, f"{fin}->{fout}: held {worst / MS:.1f} ms > reported {mn / MS:.1f} ms")
        check(mn - worst <= MS, f"{fin}->{fout}: reported {mn / MS:.1f} ms, only {worst / MS:.1f} needed")


def t_latency_message_on_rate_change():
    """When the input rate changes, the element asks for a latency update."""
    r = Run(f"{src(n=120, extra='is-live=true')} ! {FILTER} filter-desc=framerate=fps=25 "
            f"latency-input-frames=1 ! {SINK}")
    seen = []
    r.pipe.get_bus().connect("message::latency", lambda bus, m: seen.append(m.src.get_name()))
    state = {"done": False}

    def on_buf(run, n):
        if n == 1:
            _query_latency(run.pipe)   # what an app/bin does on the way to PLAYING
        if n == 20 and not state["done"]:
            state["done"] = True
            run.pipe.get_by_name("cf").set_property(
                "caps", Gst.Caps.from_string("video/x-raw,format=I420,width=640,height=360,framerate=50/1"))
    r.on_buffer = on_buf
    r.run()
    check(not r.errors, r.errors)
    check("f" in seen, f"no latency message from the filter (from: {seen})")

def _run_timed(in_fps, out_fps, pts_list, durs):
    """Push I420 frames with exactly these pts/durations through framerate."""
    n, d = in_fps
    r = Run(f"appsrc name=a format=time "
            f"caps=video/x-raw,format=I420,width=64,height=36,framerate={n}/{d} ! "
            f"{FILTER} filter-desc=framerate=fps={out_fps[0]}/{out_fps[1]} ! {SINK}")

    def feed(run):
        a = run.pipe.get_by_name("a")
        for pts, dur in zip(pts_list, durs):
            b = Gst.Buffer.new_wrapped(bytes(64 * 36 * 3 // 2))
            b.pts, b.duration = pts, dur
            a.emit("push-buffer", b)
        a.emit("end-of-stream")
    return r.run(before_play=feed)


def t_output_never_shorter():
    """Output covers the input's time span (within 2 ms) and ends less than one
    output frame after it, for any rate pair, start offset and noise on the
    last frame's duration (1 ns from timestamp rounding, 1 ms from container
    time bases)."""
    pairs = [((30, 1), (25, 1)), ((25, 1), (25, 1)), ((50, 1), (25, 1)), ((25, 1), (50, 1)),
             ((24, 1), (25, 1)), ((60, 1), (50, 1)), ((30000, 1001), (25, 1)), ((25, 1), (30000, 1001))]
    offsets = [0, 1000 * 3600 * Gst.SECOND]
    noises = [0, 1, -1, MS, -MS]           # added to the last frame's duration
    for (fin, fout) in pairs:
        frames = 2 * fin[0] // fin[1]      # ~2 s of input
        step = lambda i: Gst.util_uint64_scale(i, fin[1] * Gst.SECOND, fin[0])
        out_dur = Gst.util_uint64_scale(1, fout[1] * Gst.SECOND, fout[0])
        for off in offsets:
            for noise in noises:
                pts = [off + step(i) for i in range(frames)]
                durs = [step(i + 1) - step(i) for i in range(frames)]
                durs[-1] += noise
                r = _run_timed(fin, fout, pts, durs)
                label = f"{fin[0]}/{fin[1]}->{fout[0]}/{fout[1]} offset={off} noise={noise}"
                check(not r.errors and r.bufs, f"{label}: {r.errors or 'no output'}")
                in_start, in_end = pts[0], pts[-1] + durs[-1]
                out_start = r.bufs[0][0]
                out_end = r.bufs[-1][0] + r.bufs[-1][1]
                check(abs(out_start - in_start) <= 1000, f"{label}: starts at {out_start}, input at {in_start}")
                # framerate ends on the nominal frame interval: a last frame that
                # claims to last longer is not covered by its excess (up to 2 ms)
                check(out_end >= in_end - 2 * MS, f"{label}: output ends {in_end - out_end} ns before the input")
                check(out_end - in_end < out_dur, f"{label}: output {out_end - in_end} ns longer, >= one frame")


def t_coexists_with_system_libav():
    """gst-libav (system FFmpeg) and avvideofilter in one process. With the
    bundled FFmpeg both copies must be loaded side by side (distinct library
    names), not merged into one by SONAME."""
    if not (Gst.ElementFactory.find("avenc_mjpeg") and Gst.ElementFactory.find("avdec_mjpeg")):
        raise Skip("gst-libav not installed")
    r = Run(f"{src(n=60)} ! avenc_mjpeg ! avdec_mjpeg ! videoconvert ! "
            f"video/x-raw,format=I420 ! {FILTER} filter-desc=framerate=fps=25 ! {SINK}").run()
    check(not r.errors, r.errors)
    # 2 s at 25 fps; one more frame when gst-libav's timestamps end a few ns
    # late (see output_never_shorter)
    check(len(r.bufs) in (50, 51), f"{len(r.bufs)} frames, expected 50 (or 51)")
    with open("/proc/self/maps") as f:
        maps = f.read()
    if "_avvf.so" in maps:
        check(re.search(r"/libavutil\.so\.\d+", maps), "system libavutil not loaded next to the bundled one")


TESTS = {k[2:]: v for k, v in globals().items() if k.startswith("t_")}

if __name__ == "__main__":
    names = sys.argv[1:] or list(TESTS)
    failed = skipped = 0
    for name in names:
        t0 = time.monotonic()
        try:
            TESTS[name]()
            print(f"PASS  {name}  ({time.monotonic() - t0:.1f}s)")
        except Skip as e:
            skipped += 1
            print(f"SKIP  {name}: {e}")
        except Fail as e:
            failed += 1
            print(f"FAIL  {name}: {e}")
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"ERROR {name}: {type(e).__name__}: {e}")
    print(f"\n{len(names) - failed - skipped}/{len(names)} passed" + (f", {skipped} skipped" if skipped else ""))
    sys.exit(1 if failed else 0)
