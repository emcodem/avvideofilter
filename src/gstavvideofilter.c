/* avvideofilter: apply an arbitrary libavfilter graph to raw video.
 *
 * Plain GstElement with a chain function (not GstBaseTransform), because the
 * filter graph is N:M — e.g. "framerate=fps=25" consumes 30 frames and emits 25,
 * "minterpolate" holds frames back. Output timestamps come from the frame pts
 * the graph produces; duration from the graph's output frame rate.
 *
 * Graph:  buffer (input caps, time_base 1/1e9) -> <filter-desc> -> format=<in fmt> -> buffersink
 * The trailing format filter pins the output to the input pixel format so the
 * src caps can always be expressed (size and frame rate may change).
 *
 * Milestone 1: copy in/out (no zero-copy), fixed latency via "latency-frames".
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <gst/gst.h>
#include <gst/video/video.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>

GST_DEBUG_CATEGORY_STATIC (avvideofilter_debug);
#define GST_CAT_DEFAULT avvideofilter_debug
/* Same name as gst-libav's category, so non-filter FFmpeg messages (e.g. from
 * avdec_* in the same process) end up where gst-libav would put them. */
GST_DEBUG_CATEGORY_STATIC (libav_debug);

/* FFmpeg error lines logged on this thread since build_graph cleared it; put
 * into the element error (av_strerror alone only says "Filter not found",
 * the preceding log line says which filter). */
static _Thread_local gchar last_av_error[512];

/* av_log callback is process-global. gst-libav installs its own; ours routes
 * filter messages to "avvideofilter" and everything else to "libav", so
 * replacing theirs loses nothing. Re-installed at every graph build in case
 * gst-libav was loaded later and took it over. */
static void
av_log_to_gst (void *avcl, int level, const char *fmt, va_list vl)
{
  const AVClass *cls = avcl ? *(const AVClass **) avcl : NULL;
  GstDebugCategory *cat = (cls && cls->category == AV_CLASS_CATEGORY_FILTER) ?
      avvideofilter_debug : libav_debug;
  GstDebugLevel gl;
  gchar line[1024];
  gint print_prefix = 1;
  gsize len;

  if (level <= AV_LOG_ERROR)
    gl = GST_LEVEL_ERROR;
  else if (level <= AV_LOG_WARNING)
    gl = GST_LEVEL_WARNING;
  else if (level <= AV_LOG_INFO)
    gl = GST_LEVEL_INFO;
  else if (level <= AV_LOG_VERBOSE)
    gl = GST_LEVEL_DEBUG;
  else if (level <= AV_LOG_DEBUG)
    gl = GST_LEVEL_LOG;
  else
    gl = GST_LEVEL_TRACE;

  /* Cheap exit for the common case; errors are always recorded. */
  if (gl > gst_debug_category_get_threshold (cat) && level > AV_LOG_ERROR)
    return;

  av_log_format_line2 (avcl, level, fmt, vl, line, sizeof (line),
      &print_prefix);
  len = strlen (line);
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
    line[--len] = '\0';
  if (len == 0)
    return;
  if (level <= AV_LOG_ERROR) {
    if (last_av_error[0])
      g_strlcat (last_av_error, " | ", sizeof (last_av_error));
    g_strlcat (last_av_error, line, sizeof (last_av_error));
  }
  GST_CAT_LEVEL_LOG (cat, gl, NULL, "%s", line);
}

#define GST_TYPE_AV_VIDEO_FILTER (gst_av_video_filter_get_type ())
G_DECLARE_FINAL_TYPE (GstAvVideoFilter, gst_av_video_filter, GST, AV_VIDEO_FILTER, GstElement)

#define DEFAULT_FILTER_DESC    "null"
#define DEFAULT_LATENCY_FRAMES 0
#define DEFAULT_LATENCY_INPUT_FRAMES 0
#define DEFAULT_THREADS        0

enum
{
  PROP_0,
  PROP_FILTER_DESC,
  PROP_LATENCY_FRAMES,
  PROP_LATENCY_INPUT_FRAMES,
  PROP_THREADS,
};

struct _GstAvVideoFilter
{
  GstElement parent;

  GstPad *sinkpad, *srcpad;

  /* properties (object lock) */
  gchar *filter_desc;
  guint latency_frames;
  guint latency_input_frames;
  gint threads;
  GstClockTime reported_latency;  /* our part of the last LATENCY answer */
  gboolean desc_changed;        /* filter-desc set since the graph was built */

  /* streaming state (graph_lock) */
  GMutex graph_lock;
  AVFilterGraph *graph;
  AVFilterContext *src_ctx, *sink_ctx;
  AVFrame *out_frame;
  gboolean have_in_info;
  GstVideoInfo in_info;
  gboolean have_out_info;
  GstVideoInfo out_info;
  AVRational out_tb;
  GstClockTime last_in_pts;     /* to synthesize pts for untimestamped input */
  gboolean discont;
};

G_DEFINE_TYPE (GstAvVideoFilter, gst_av_video_filter, GST_TYPE_ELEMENT);

/* Formats with an identical memory layout in GStreamer and FFmpeg. The copy
 * code is generic over planes. Planar YUV comes first: most libavfilter video
 * filters (framerate, tmix, ...) process it natively, while for semi-planar,
 * packed and RGB formats libavfilter converts internally (and the trailing
 * format filter converts back), so upstream should prefer planar when it can.
 * Not mappable: v210 (no FFmpeg pixel format), GPU memory (CPU-only element). */
#define VIDEO_FORMATS "{ " \
    "I420, Y42B, Y444, I420_10LE, I422_10LE, Y444_10LE, " \
    "I420_12LE, I422_12LE, Y444_12LE, Y444_16LE, A420, " \
    "GBR, GBR_10LE, GBR_12LE, GBR_16LE, GBRA, GRAY8, GRAY16_LE, " \
    "NV12, NV21, NV16, NV24, P010_10LE, P012_LE, P016_LE, " \
    "UYVY, YUY2, YVYU, Y210, " \
    "RGBA, BGRA, ARGB, ABGR, RGBx, BGRx, xRGB, xBGR, RGB, BGR }"

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (VIDEO_FORMATS)));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (VIDEO_FORMATS)));

static const struct
{
  GstVideoFormat gst;
  enum AVPixelFormat av;
} format_map[] = {
  /* planar YUV */
  {GST_VIDEO_FORMAT_I420, AV_PIX_FMT_YUV420P},
  {GST_VIDEO_FORMAT_Y42B, AV_PIX_FMT_YUV422P},
  {GST_VIDEO_FORMAT_Y444, AV_PIX_FMT_YUV444P},
  {GST_VIDEO_FORMAT_I420_10LE, AV_PIX_FMT_YUV420P10LE},
  {GST_VIDEO_FORMAT_I422_10LE, AV_PIX_FMT_YUV422P10LE},
  {GST_VIDEO_FORMAT_Y444_10LE, AV_PIX_FMT_YUV444P10LE},
  {GST_VIDEO_FORMAT_I420_12LE, AV_PIX_FMT_YUV420P12LE},
  {GST_VIDEO_FORMAT_I422_12LE, AV_PIX_FMT_YUV422P12LE},
  {GST_VIDEO_FORMAT_Y444_12LE, AV_PIX_FMT_YUV444P12LE},
  {GST_VIDEO_FORMAT_Y444_16LE, AV_PIX_FMT_YUV444P16LE},
  {GST_VIDEO_FORMAT_A420, AV_PIX_FMT_YUVA420P},
  /* planar RGB, gray */
  {GST_VIDEO_FORMAT_GBR, AV_PIX_FMT_GBRP},
  {GST_VIDEO_FORMAT_GBR_10LE, AV_PIX_FMT_GBRP10LE},
  {GST_VIDEO_FORMAT_GBR_12LE, AV_PIX_FMT_GBRP12LE},
#if GST_CHECK_VERSION(1, 24, 0)
  {GST_VIDEO_FORMAT_GBR_16LE, AV_PIX_FMT_GBRP16LE},
#endif
  {GST_VIDEO_FORMAT_GBRA, AV_PIX_FMT_GBRAP},
  {GST_VIDEO_FORMAT_GRAY8, AV_PIX_FMT_GRAY8},
  {GST_VIDEO_FORMAT_GRAY16_LE, AV_PIX_FMT_GRAY16LE},
  /* semi-planar (hardware decoder output: NV12, P010, P012) */
  {GST_VIDEO_FORMAT_NV12, AV_PIX_FMT_NV12},
  {GST_VIDEO_FORMAT_NV21, AV_PIX_FMT_NV21},
  {GST_VIDEO_FORMAT_NV16, AV_PIX_FMT_NV16},
  {GST_VIDEO_FORMAT_NV24, AV_PIX_FMT_NV24},
  {GST_VIDEO_FORMAT_P010_10LE, AV_PIX_FMT_P010LE},
  {GST_VIDEO_FORMAT_P012_LE, AV_PIX_FMT_P012LE},
  {GST_VIDEO_FORMAT_P016_LE, AV_PIX_FMT_P016LE},
  /* packed YUV */
  {GST_VIDEO_FORMAT_UYVY, AV_PIX_FMT_UYVY422},
  {GST_VIDEO_FORMAT_YUY2, AV_PIX_FMT_YUYV422},
  {GST_VIDEO_FORMAT_YVYU, AV_PIX_FMT_YVYU422},
  {GST_VIDEO_FORMAT_Y210, AV_PIX_FMT_Y210LE},
  /* packed RGB */
  {GST_VIDEO_FORMAT_RGBA, AV_PIX_FMT_RGBA},
  {GST_VIDEO_FORMAT_BGRA, AV_PIX_FMT_BGRA},
  {GST_VIDEO_FORMAT_ARGB, AV_PIX_FMT_ARGB},
  {GST_VIDEO_FORMAT_ABGR, AV_PIX_FMT_ABGR},
  {GST_VIDEO_FORMAT_RGBx, AV_PIX_FMT_RGB0},
  {GST_VIDEO_FORMAT_BGRx, AV_PIX_FMT_BGR0},
  {GST_VIDEO_FORMAT_xRGB, AV_PIX_FMT_0RGB},
  {GST_VIDEO_FORMAT_xBGR, AV_PIX_FMT_0BGR},
  {GST_VIDEO_FORMAT_RGB, AV_PIX_FMT_RGB24},
  {GST_VIDEO_FORMAT_BGR, AV_PIX_FMT_BGR24},
};

static enum AVPixelFormat
gst_to_av_format (GstVideoFormat f)
{
  for (guint i = 0; i < G_N_ELEMENTS (format_map); i++)
    if (format_map[i].gst == f)
      return format_map[i].av;
#if !GST_CHECK_VERSION(1, 24, 0)
  /* GBR_16LE exists since GStreamer 1.24. Built against older headers (the
   * release bundle targets 1.20), resolve it at runtime so the plugin still
   * handles it on a newer GStreamer; an older one never negotiates it. */
  if (f != GST_VIDEO_FORMAT_UNKNOWN && f == gst_video_format_from_string ("GBR_16LE"))
    return AV_PIX_FMT_GBRP16LE;
#endif
  return AV_PIX_FMT_NONE;
}

static const AVRational ns_tb = { 1, GST_SECOND };

/* ── graph lifecycle ─────────────────────────────────────────────────────── */

static void
free_graph (GstAvVideoFilter * self)
{
  avfilter_graph_free (&self->graph);
  self->src_ctx = self->sink_ctx = NULL;
}

static gboolean
build_graph (GstAvVideoFilter * self)
{
  const GstVideoInfo *in = &self->in_info;
  enum AVPixelFormat pix = gst_to_av_format (GST_VIDEO_INFO_FORMAT (in));
  AVFilterInOut *outputs = NULL, *inputs = NULL;
  gchar *args = NULL, *desc = NULL, *user_desc;
  gint threads, ret;
  gint fps_n = GST_VIDEO_INFO_FPS_N (in), fps_d = GST_VIDEO_INFO_FPS_D (in);
  gint par_n = GST_VIDEO_INFO_PAR_N (in), par_d = GST_VIDEO_INFO_PAR_D (in);

  free_graph (self);
  av_log_set_callback (av_log_to_gst);
  last_av_error[0] = '\0';

  GST_OBJECT_LOCK (self);
  user_desc = g_strdup (self->filter_desc && *self->filter_desc ?
      self->filter_desc : DEFAULT_FILTER_DESC);
  threads = self->threads;
  GST_OBJECT_UNLOCK (self);

  if (fps_n <= 0 || fps_d <= 0) {
    /* variable frame rate: libavfilter wants something; 25 is a harmless hint */
    fps_n = 25;
    fps_d = 1;
  }
  if (par_n <= 0 || par_d <= 0)
    par_n = par_d = 1;

  self->graph = avfilter_graph_alloc ();
  if (!self->graph)
    goto oom;
  if (threads > 0)
    self->graph->nb_threads = threads;

  args = g_strdup_printf ("video_size=%dx%d:pix_fmt=%d:time_base=1/%d:"
      "frame_rate=%d/%d:pixel_aspect=%d/%d",
      GST_VIDEO_INFO_WIDTH (in), GST_VIDEO_INFO_HEIGHT (in), pix, (gint) GST_SECOND,
      fps_n, fps_d, par_n, par_d);

  ret = avfilter_graph_create_filter (&self->src_ctx,
      avfilter_get_by_name ("buffer"), "in", args, NULL, self->graph);
  if (ret < 0)
    goto av_error;
  ret = avfilter_graph_create_filter (&self->sink_ctx,
      avfilter_get_by_name ("buffersink"), "out", NULL, NULL, self->graph);
  if (ret < 0)
    goto av_error;

  outputs = avfilter_inout_alloc ();
  inputs = avfilter_inout_alloc ();
  if (!outputs || !inputs)
    goto oom;
  outputs->name = av_strdup ("in");
  outputs->filter_ctx = self->src_ctx;
  outputs->pad_idx = 0;
  outputs->next = NULL;
  inputs->name = av_strdup ("out");
  inputs->filter_ctx = self->sink_ctx;
  inputs->pad_idx = 0;
  inputs->next = NULL;

  desc = g_strdup_printf ("%s,format=pix_fmts=%s", user_desc,
      av_get_pix_fmt_name (pix));
  ret = avfilter_graph_parse_ptr (self->graph, desc, &inputs, &outputs, NULL);
  if (ret < 0)
    goto av_error;
  ret = avfilter_graph_config (self->graph, NULL);
  if (ret < 0)
    goto av_error;

  /* Derive output caps from the configured sink link. */
  {
    AVRational fr = av_buffersink_get_frame_rate (self->sink_ctx);
    AVRational sar = av_buffersink_get_sample_aspect_ratio (self->sink_ctx);
    gint w = av_buffersink_get_w (self->sink_ctx);
    gint h = av_buffersink_get_h (self->sink_ctx);
    GstVideoInfo *out = &self->out_info;

    self->out_tb = av_buffersink_get_time_base (self->sink_ctx);

    gst_video_info_set_format (out, GST_VIDEO_INFO_FORMAT (in), w, h);
    if (fr.num > 0 && fr.den > 0) {
      GST_VIDEO_INFO_FPS_N (out) = fr.num;
      GST_VIDEO_INFO_FPS_D (out) = fr.den;
    } else {
      GST_VIDEO_INFO_FPS_N (out) = GST_VIDEO_INFO_FPS_N (in);
      GST_VIDEO_INFO_FPS_D (out) = GST_VIDEO_INFO_FPS_D (in);
    }
    if (sar.num > 0 && sar.den > 0) {
      GST_VIDEO_INFO_PAR_N (out) = sar.num;
      GST_VIDEO_INFO_PAR_D (out) = sar.den;
    }
    out->colorimetry = in->colorimetry;
    out->chroma_site = in->chroma_site;
    /* Initial guess; corrected from the first output frame's flags (a
     * deinterlacer in the graph makes the output progressive). */
    GST_VIDEO_INFO_INTERLACE_MODE (out) = GST_VIDEO_INFO_INTERLACE_MODE (in);
    GST_VIDEO_INFO_FIELD_ORDER (out) = GST_VIDEO_INFO_FIELD_ORDER (in);
    self->have_out_info = TRUE;
  }

  GST_INFO_OBJECT (self, "graph '%s' configured: %dx%d@%d/%d -> %dx%d@%d/%d "
      "(out tb %d/%d)", desc,
      GST_VIDEO_INFO_WIDTH (in), GST_VIDEO_INFO_HEIGHT (in), fps_n, fps_d,
      GST_VIDEO_INFO_WIDTH (&self->out_info),
      GST_VIDEO_INFO_HEIGHT (&self->out_info),
      GST_VIDEO_INFO_FPS_N (&self->out_info),
      GST_VIDEO_INFO_FPS_D (&self->out_info), self->out_tb.num,
      self->out_tb.den);

  self->discont = TRUE;
  avfilter_inout_free (&inputs);
  avfilter_inout_free (&outputs);
  g_free (args);
  g_free (desc);
  g_free (user_desc);
  return TRUE;

av_error:
  {
    gchar errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror (ret, errbuf, sizeof (errbuf));
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS,
        ("Failed to set up libavfilter graph"),
        ("filter-desc '%s' (buffer args '%s'): %s%s%s",
            desc ? desc : user_desc, args, errbuf,
            last_av_error[0] ? " | " : "", last_av_error));
    goto fail;
  }
oom:
  GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Out of memory"), (NULL));
fail:
  avfilter_inout_free (&inputs);
  avfilter_inout_free (&outputs);
  free_graph (self);
  g_free (args);
  g_free (desc);
  g_free (user_desc);
  return FALSE;
}

/* ── frame copy ──────────────────────────────────────────────────────────── */

static gint
plane_height (enum AVPixelFormat pix, gint plane, gint h)
{
  const AVPixFmtDescriptor *d = av_pix_fmt_desc_get (pix);
  return (plane == 1 || plane == 2) ? AV_CEIL_RSHIFT (h, d->log2_chroma_h) : h;
}

static void
copy_planes (enum AVPixelFormat pix, gint w, gint h, guint8 * dst[],
    const gint dst_stride[], guint8 * const src[], const gint src_stride[])
{
  gint bw[4];
  av_image_fill_linesizes (bw, pix, w);
  for (gint p = 0; p < 4 && bw[p] > 0; p++)
    av_image_copy_plane (dst[p], dst_stride[p], src[p], src_stride[p], bw[p],
        plane_height (pix, p, h));
}

static AVFrame *
frame_from_buffer (GstAvVideoFilter * self, GstBuffer * buf)
{
  const GstVideoInfo *in = &self->in_info;
  enum AVPixelFormat pix = gst_to_av_format (GST_VIDEO_INFO_FORMAT (in));
  GstVideoFrame vf;
  AVFrame *f;
  guint8 *src[4] = { NULL };
  gint src_stride[4] = { 0 };

  if (!gst_video_frame_map (&vf, (GstVideoInfo *) in, buf, GST_MAP_READ))
    return NULL;

  f = av_frame_alloc ();
  f->format = pix;
  f->width = GST_VIDEO_INFO_WIDTH (in);
  f->height = GST_VIDEO_INFO_HEIGHT (in);
  if (av_frame_get_buffer (f, 0) < 0) {
    av_frame_free (&f);
    gst_video_frame_unmap (&vf);
    return NULL;
  }
  for (guint p = 0; p < GST_VIDEO_FRAME_N_PLANES (&vf); p++) {
    src[p] = GST_VIDEO_FRAME_PLANE_DATA (&vf, p);
    src_stride[p] = GST_VIDEO_FRAME_PLANE_STRIDE (&vf, p);
  }
  copy_planes (pix, f->width, f->height, f->data, f->linesize, src, src_stride);
  gst_video_frame_unmap (&vf);
  return f;
}

static GstBuffer *
buffer_from_frame (GstAvVideoFilter * self, const AVFrame * f)
{
  GstVideoInfo *out = &self->out_info;
  GstBuffer *buf = gst_buffer_new_allocate (NULL, GST_VIDEO_INFO_SIZE (out),
      NULL);
  GstVideoFrame vf;
  guint8 *dst[4] = { NULL };
  gint dst_stride[4] = { 0 };

  if (!buf || !gst_video_frame_map (&vf, out, buf, GST_MAP_WRITE)) {
    if (buf)
      gst_buffer_unref (buf);
    return NULL;
  }
  for (guint p = 0; p < GST_VIDEO_FRAME_N_PLANES (&vf); p++) {
    dst[p] = GST_VIDEO_FRAME_PLANE_DATA (&vf, p);
    dst_stride[p] = GST_VIDEO_FRAME_PLANE_STRIDE (&vf, p);
  }
  copy_planes (f->format, f->width, f->height, dst, dst_stride,
      (guint8 * const *) f->data, f->linesize);
  gst_video_frame_unmap (&vf);
  return buf;
}

/* ── data path ───────────────────────────────────────────────────────────── */

static GstClockTime
out_frame_duration (GstAvVideoFilter * self)
{
  if (!self->have_out_info || GST_VIDEO_INFO_FPS_N (&self->out_info) <= 0)
    return GST_CLOCK_TIME_NONE;
  return gst_util_uint64_scale_int (GST_SECOND,
      GST_VIDEO_INFO_FPS_D (&self->out_info),
      GST_VIDEO_INFO_FPS_N (&self->out_info));
}

static GstClockTime
in_frame_duration (GstAvVideoFilter * self)
{
  if (!self->have_in_info || GST_VIDEO_INFO_FPS_N (&self->in_info) <= 0)
    return GST_CLOCK_TIME_NONE;
  return gst_util_uint64_scale_int (GST_SECOND,
      GST_VIDEO_INFO_FPS_D (&self->in_info),
      GST_VIDEO_INFO_FPS_N (&self->in_info));
}

/* Frame duration from the peer's caps, if they fix a framerate. A live
 * pipeline queries latency on the way to PLAYING, usually before any caps
 * are negotiated; the neighbours' caps (e.g. a capsfilter with the channel
 * rate downstream, a capture card with a fixed mode upstream) still tell. */
static GstClockTime
peer_frame_duration (GstPad * pad)
{
  GstCaps *caps = gst_pad_peer_query_caps (pad, NULL);
  GstClockTime d = GST_CLOCK_TIME_NONE;
  gint n, dd;

  if (caps && !gst_caps_is_empty (caps) && !gst_caps_is_any (caps)) {
    GstStructure *st = gst_caps_get_structure (caps, 0);
    const GValue *v = gst_structure_get_value (st, "framerate");
    gboolean same = TRUE;
    guint i;

    /* only a single fixed framerate across all structures counts */
    for (i = 1; i < gst_caps_get_size (caps) && v; i++) {
      const GValue *o =
          gst_structure_get_value (gst_caps_get_structure (caps, i),
          "framerate");
      same = same && o && gst_value_compare (v, o) == GST_VALUE_EQUAL;
    }
    if (v && same && GST_VALUE_HOLDS_FRACTION (v)) {
      n = gst_value_get_fraction_numerator (v);
      dd = gst_value_get_fraction_denominator (v);
      if (n > 0 && dd > 0)
        d = gst_util_uint64_scale_int (GST_SECOND, dd, n);
    }
  }
  if (caps)
    gst_caps_unref (caps);
  return d;
}

/* Our latency: latency-frames output frames + latency-input-frames input
 * frames. Frame durations come from the negotiated caps, else from the
 * neighbours' fixed caps. An unknown input rate falls back to the output
 * rate (exact for same-rate sources). Must not be called with graph_lock. */
static GstClockTime
compute_latency (GstAvVideoFilter * self)
{
  guint frames, in_frames;
  GstClockTime out_d, in_d, ours = 0;

  GST_OBJECT_LOCK (self);
  frames = self->latency_frames;
  in_frames = self->latency_input_frames;
  GST_OBJECT_UNLOCK (self);
  if (frames == 0 && in_frames == 0)
    return 0;

  g_mutex_lock (&self->graph_lock);
  out_d = out_frame_duration (self);
  in_d = in_frame_duration (self);
  g_mutex_unlock (&self->graph_lock);
  if (!GST_CLOCK_TIME_IS_VALID (out_d))
    out_d = peer_frame_duration (self->srcpad);
  if (in_frames > 0 && !GST_CLOCK_TIME_IS_VALID (in_d))
    in_d = peer_frame_duration (self->sinkpad);
  if (!GST_CLOCK_TIME_IS_VALID (in_d))
    in_d = out_d;

  if (frames > 0 && GST_CLOCK_TIME_IS_VALID (out_d))
    ours += frames * out_d;
  if (in_frames > 0 && GST_CLOCK_TIME_IS_VALID (in_d))
    ours += in_frames * in_d;
  return ours;
}

/* After a caps change: if our latency now differs from what was last
 * reported, ask the application to redistribute it (gst_bin_recalculate_latency
 * on GST_MESSAGE_LATENCY). Must not be called with graph_lock. */
static void
maybe_post_latency (GstAvVideoFilter * self)
{
  GstClockTime ours = compute_latency (self), prev;

  GST_OBJECT_LOCK (self);
  prev = self->reported_latency;
  GST_OBJECT_UNLOCK (self);
  if (GST_CLOCK_TIME_IS_VALID (prev) && prev != ours) {
    GST_INFO_OBJECT (self, "latency changed %" GST_TIME_FORMAT " -> %"
        GST_TIME_FORMAT, GST_TIME_ARGS (prev), GST_TIME_ARGS (ours));
    gst_element_post_message (GST_ELEMENT (self),
        gst_message_new_latency (GST_OBJECT (self)));
  }
}

/* Make the src caps' interlace info match an output frame. Mixed input stays
 * mixed (per-buffer flags carry it). Otherwise the frame flags decide: e.g.
 * yadif turns interleaved input into progressive output. Called with
 * graph_lock held; released around the caps push. */
static GstFlowReturn
update_out_interlace (GstAvVideoFilter * self, gboolean interlaced,
    gboolean tff)
{
  GstVideoInfo *out = &self->out_info;
  GstVideoInterlaceMode mode;
  GstVideoFieldOrder order;
  GstCaps *caps;
  gboolean ok;

  if (GST_VIDEO_INFO_INTERLACE_MODE (out) == GST_VIDEO_INTERLACE_MODE_MIXED)
    return GST_FLOW_OK;
  mode = interlaced ? GST_VIDEO_INTERLACE_MODE_INTERLEAVED :
      GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;
  order = !interlaced ? GST_VIDEO_FIELD_ORDER_UNKNOWN :
      tff ? GST_VIDEO_FIELD_ORDER_TOP_FIELD_FIRST :
      GST_VIDEO_FIELD_ORDER_BOTTOM_FIELD_FIRST;
  if (GST_VIDEO_INFO_INTERLACE_MODE (out) == mode &&
      (!interlaced || GST_VIDEO_INFO_FIELD_ORDER (out) == order))
    return GST_FLOW_OK;

  GST_VIDEO_INFO_INTERLACE_MODE (out) = mode;
  GST_VIDEO_INFO_FIELD_ORDER (out) = order;
  caps = gst_video_info_to_caps (out);
  GST_DEBUG_OBJECT (self, "output interlacing changed: %" GST_PTR_FORMAT, caps);
  g_mutex_unlock (&self->graph_lock);
  ok = gst_pad_push_event (self->srcpad, gst_event_new_caps (caps));
  g_mutex_lock (&self->graph_lock);
  gst_caps_unref (caps);
  if (!ok) {
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
        ("downstream refused the output interlace mode"));
    return GST_FLOW_NOT_NEGOTIATED;
  }
  return GST_FLOW_OK;
}

/* Pull everything the graph has ready and push it. Called with graph_lock
 * held; the lock is released around gst_pad_push. */
static GstFlowReturn
push_ready_frames (GstAvVideoFilter * self)
{
  GstFlowReturn flow = GST_FLOW_OK;

  while (self->graph && flow == GST_FLOW_OK) {
    GstBuffer *buf;
    gint ret = av_buffersink_get_frame (self->sink_ctx, self->out_frame);

    if (ret == AVERROR (EAGAIN) || ret == AVERROR_EOF)
      break;
    if (ret < 0) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED, (NULL),
          ("av_buffersink_get_frame failed: %d", ret));
      return GST_FLOW_ERROR;
    }

    {
      gboolean interlaced =
          (self->out_frame->flags & AV_FRAME_FLAG_INTERLACED) != 0;
      gboolean tff =
          (self->out_frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0;

      flow = update_out_interlace (self, interlaced, tff);
      if (flow != GST_FLOW_OK || !self->graph) {
        av_frame_unref (self->out_frame);
        return flow;
      }
    }

    buf = buffer_from_frame (self, self->out_frame);
    if (!buf) {
      av_frame_unref (self->out_frame);
      GST_ELEMENT_ERROR (self, RESOURCE, FAILED, (NULL),
          ("Failed to allocate output buffer"));
      return GST_FLOW_ERROR;
    }
    if (self->out_frame->pts != AV_NOPTS_VALUE) {
      gint64 pts = av_rescale_q (self->out_frame->pts, self->out_tb, ns_tb);
      GST_BUFFER_PTS (buf) = pts >= 0 ? (GstClockTime) pts : 0;
    }
    GST_BUFFER_DURATION (buf) = out_frame_duration (self);
    if (self->out_frame->flags & AV_FRAME_FLAG_INTERLACED) {
      GST_BUFFER_FLAG_SET (buf, GST_VIDEO_BUFFER_FLAG_INTERLACED);
      if (self->out_frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST)
        GST_BUFFER_FLAG_SET (buf, GST_VIDEO_BUFFER_FLAG_TFF);
    }
    if (self->discont) {
      GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
      self->discont = FALSE;
    }
    av_frame_unref (self->out_frame);

    GST_LOG_OBJECT (self, "pushing %" GST_PTR_FORMAT, buf);
    g_mutex_unlock (&self->graph_lock);
    flow = gst_pad_push (self->srcpad, buf);
    g_mutex_lock (&self->graph_lock);
  }
  return flow;
}

/* Send EOF into the graph, push what comes out, then drop the graph. */
static GstFlowReturn
drain_graph (GstAvVideoFilter * self)
{
  GstFlowReturn flow;

  if (!self->graph)
    return GST_FLOW_OK;
  if (av_buffersrc_add_frame_flags (self->src_ctx, NULL, 0) < 0)
    GST_WARNING_OBJECT (self, "failed to signal EOF to the graph");
  flow = push_ready_frames (self);
  free_graph (self);
  return flow;
}

/* Make sure a graph matching the current filter-desc exists. A filter-desc
 * change drains the old graph first (its held frames go out under the old
 * caps). If the rebuilt graph's output differs, new src caps are pushed.
 * Called with graph_lock held. */
static GstFlowReturn
ensure_graph (GstAvVideoFilter * self)
{
  GstVideoInfo prev_out;
  gboolean had_out, desc_changed;

  GST_OBJECT_LOCK (self);
  desc_changed = self->desc_changed;
  self->desc_changed = FALSE;
  GST_OBJECT_UNLOCK (self);

  if (desc_changed && self->graph) {
    GstFlowReturn flow = drain_graph (self);
    if (flow != GST_FLOW_OK)
      return flow;
  }
  if (self->graph)
    return GST_FLOW_OK;

  prev_out = self->out_info;
  had_out = self->have_out_info;
  if (!build_graph (self))
    return GST_FLOW_ERROR;

  if (!had_out || !gst_video_info_is_equal (&prev_out, &self->out_info)) {
    GstCaps *caps = gst_video_info_to_caps (&self->out_info);
    gboolean ok;

    GST_DEBUG_OBJECT (self, "output changed, new src caps %" GST_PTR_FORMAT,
        caps);
    g_mutex_unlock (&self->graph_lock);
    ok = gst_pad_push_event (self->srcpad, gst_event_new_caps (caps));
    g_mutex_lock (&self->graph_lock);
    gst_caps_unref (caps);
    if (!ok) {
      GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
          ("downstream refused the new output caps"));
      return GST_FLOW_NOT_NEGOTIATED;
    }
  }
  return GST_FLOW_OK;
}

/* GStreamer interlace info -> AVFrame flags, so deinterlacers in the graph
 * (yadif, bwdif with deint=interlaced, ...) see what is interlaced. */
static void
set_frame_interlace_flags (GstAvVideoFilter * self, AVFrame * f,
    GstBuffer * buf)
{
  GstVideoInterlaceMode mode = GST_VIDEO_INFO_INTERLACE_MODE (&self->in_info);
  gboolean interlaced = mode == GST_VIDEO_INTERLACE_MODE_INTERLEAVED ||
      (mode == GST_VIDEO_INTERLACE_MODE_MIXED &&
      GST_BUFFER_FLAG_IS_SET (buf, GST_VIDEO_BUFFER_FLAG_INTERLACED));
  gboolean tff = GST_BUFFER_FLAG_IS_SET (buf, GST_VIDEO_BUFFER_FLAG_TFF) ||
      (mode == GST_VIDEO_INTERLACE_MODE_INTERLEAVED &&
      GST_VIDEO_INFO_FIELD_ORDER (&self->in_info) ==
      GST_VIDEO_FIELD_ORDER_TOP_FIELD_FIRST);

  if (interlaced) {
    f->flags |= AV_FRAME_FLAG_INTERLACED;
    if (tff)
      f->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
  }
}

static GstFlowReturn
gst_av_video_filter_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstAvVideoFilter *self = GST_AV_VIDEO_FILTER (parent);
  GstFlowReturn flow;
  AVFrame *f;
  GstClockTime pts;
  gint ret;

  g_mutex_lock (&self->graph_lock);
  if (!self->have_in_info) {
    g_mutex_unlock (&self->graph_lock);
    gst_buffer_unref (buf);
    return GST_FLOW_NOT_NEGOTIATED;
  }
  flow = ensure_graph (self);
  if (flow != GST_FLOW_OK) {
    g_mutex_unlock (&self->graph_lock);
    gst_buffer_unref (buf);
    return flow;
  }

  pts = GST_BUFFER_PTS (buf);
  if (!GST_CLOCK_TIME_IS_VALID (pts)) {
    GstClockTime d = GST_BUFFER_DURATION_IS_VALID (buf) ?
        GST_BUFFER_DURATION (buf) : gst_util_uint64_scale_int (GST_SECOND,
        MAX (GST_VIDEO_INFO_FPS_D (&self->in_info), 1),
        MAX (GST_VIDEO_INFO_FPS_N (&self->in_info), 1));
    pts = GST_CLOCK_TIME_IS_VALID (self->last_in_pts) ?
        self->last_in_pts + d : 0;
  }
  self->last_in_pts = pts;

  f = frame_from_buffer (self, buf);
  if (!f) {
    g_mutex_unlock (&self->graph_lock);
    GST_ELEMENT_ERROR (self, STREAM, FAILED, (NULL),
        ("Failed to map input buffer"));
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }
  f->pts = (int64_t) pts;
  if (GST_BUFFER_DURATION_IS_VALID (buf))
    f->duration = (int64_t) GST_BUFFER_DURATION (buf);
  f->sample_aspect_ratio = (AVRational) {
  GST_VIDEO_INFO_PAR_N (&self->in_info), GST_VIDEO_INFO_PAR_D (&self->in_info)};
  set_frame_interlace_flags (self, f, buf);
  gst_buffer_unref (buf);

  ret = av_buffersrc_add_frame_flags (self->src_ctx, f, 0);
  av_frame_free (&f);
  if (ret < 0) {
    g_mutex_unlock (&self->graph_lock);
    GST_ELEMENT_ERROR (self, STREAM, FAILED, (NULL),
        ("av_buffersrc_add_frame failed: %d", ret));
    return GST_FLOW_ERROR;
  }

  flow = push_ready_frames (self);
  g_mutex_unlock (&self->graph_lock);
  return flow;
}

/* ── events / queries ────────────────────────────────────────────────────── */

static gboolean
gst_av_video_filter_set_caps (GstAvVideoFilter * self, GstCaps * caps)
{
  GstVideoInfo info;
  GstCaps *out_caps;
  gboolean ok;

  if (!gst_video_info_from_caps (&info, caps))
    return FALSE;
  if (GST_VIDEO_INFO_INTERLACE_MODE (&info) ==
      GST_VIDEO_INTERLACE_MODE_ALTERNATE) {
    GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL),
        ("interlace-mode=alternate (one field per buffer) is not supported"));
    return FALSE;
  }

  g_mutex_lock (&self->graph_lock);
  if (self->have_in_info && gst_video_info_is_equal (&info, &self->in_info)
      && self->have_out_info) {
    g_mutex_unlock (&self->graph_lock);
    return TRUE;
  }
  /* Push what the old graph still holds under the old caps, then rebuild. */
  drain_graph (self);
  self->in_info = info;
  self->have_in_info = TRUE;
  self->last_in_pts = GST_CLOCK_TIME_NONE;
  GST_OBJECT_LOCK (self);
  self->desc_changed = FALSE;   /* build_graph reads the current filter-desc */
  GST_OBJECT_UNLOCK (self);
  if (!build_graph (self)) {
    self->have_in_info = FALSE;
    g_mutex_unlock (&self->graph_lock);
    return FALSE;
  }
  out_caps = gst_video_info_to_caps (&self->out_info);
  g_mutex_unlock (&self->graph_lock);

  GST_DEBUG_OBJECT (self, "src caps %" GST_PTR_FORMAT, out_caps);
  ok = gst_pad_push_event (self->srcpad, gst_event_new_caps (out_caps));
  gst_caps_unref (out_caps);
  if (ok)
    maybe_post_latency (self);
  return ok;
}

static gboolean
gst_av_video_filter_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstAvVideoFilter *self = GST_AV_VIDEO_FILTER (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:{
      GstCaps *caps;
      gboolean ok;
      gst_event_parse_caps (event, &caps);
      ok = gst_av_video_filter_set_caps (self, caps);
      gst_event_unref (event);
      return ok;
    }
    case GST_EVENT_SEGMENT:{
      const GstSegment *seg;
      gst_event_parse_segment (event, &seg);
      if (seg->format != GST_FORMAT_TIME) {
        GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL),
            ("only TIME segments are supported, got %s",
                gst_format_get_name (seg->format)));
        gst_event_unref (event);
        return FALSE;
      }
      break;
    }
    case GST_EVENT_FLUSH_STOP:
      /* Discard everything buffered inside the graph; rebuilt on next buffer. */
      g_mutex_lock (&self->graph_lock);
      free_graph (self);
      self->last_in_pts = GST_CLOCK_TIME_NONE;
      g_mutex_unlock (&self->graph_lock);
      break;
    case GST_EVENT_EOS:
      g_mutex_lock (&self->graph_lock);
      drain_graph (self);
      g_mutex_unlock (&self->graph_lock);
      break;
    case GST_EVENT_GAP:
      /* Frames still held in the graph are older than the gap: push them
       * first so output stays in order. The graph restarts after the gap. */
      g_mutex_lock (&self->graph_lock);
      drain_graph (self);
      self->last_in_pts = GST_CLOCK_TIME_NONE;
      g_mutex_unlock (&self->graph_lock);
      break;
    default:
      break;
  }
  return gst_pad_event_default (pad, parent, event);
}

static gboolean
gst_av_video_filter_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:{
      /* Size/fps may change across the graph, so don't proxy downstream caps.
       * Keep our template's format order (planar YUV first) so upstream picks
       * a format libavfilter filters process natively when it has a choice. */
      GstCaps *filter, *caps = gst_pad_get_pad_template_caps (pad);
      gst_query_parse_caps (query, &filter);
      if (filter) {
        GstCaps *tmp = gst_caps_intersect_full (caps, filter,
            GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref (caps);
        caps = tmp;
      }
      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);
      return TRUE;
    }
    case GST_QUERY_ACCEPT_CAPS:{
      GstCaps *caps, *templ = gst_pad_get_pad_template_caps (pad);
      gst_query_parse_accept_caps (query, &caps);
      gst_query_set_accept_caps_result (query,
          gst_caps_is_subset (caps, templ));
      gst_caps_unref (templ);
      return TRUE;
    }
    case GST_QUERY_ALLOCATION:
      /* We copy the input; don't forward downstream's allocation needs. */
      return FALSE;
    default:
      return gst_pad_query_default (pad, parent, query);
  }
}

static gboolean
gst_av_video_filter_src_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  GstAvVideoFilter *self = GST_AV_VIDEO_FILTER (parent);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:{
      GstCaps *filter, *caps = gst_pad_get_current_caps (pad);
      if (!caps)
        caps = gst_pad_get_pad_template_caps (pad);
      gst_query_parse_caps (query, &filter);
      if (filter) {
        GstCaps *tmp = gst_caps_intersect_full (filter, caps,
            GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref (caps);
        caps = tmp;
      }
      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);
      return TRUE;
    }
    case GST_QUERY_LATENCY:{
      gboolean live;
      GstClockTime min, max, ours;

      if (!gst_pad_peer_query (self->sinkpad, query))
        return FALSE;
      gst_query_parse_latency (query, &live, &min, &max);

      ours = compute_latency (self);
      GST_OBJECT_LOCK (self);
      self->reported_latency = ours;
      GST_OBJECT_UNLOCK (self);

      min += ours;
      if (GST_CLOCK_TIME_IS_VALID (max))
        max += ours;
      GST_DEBUG_OBJECT (self, "latency: adding %" GST_TIME_FORMAT
          " -> min %" GST_TIME_FORMAT, GST_TIME_ARGS (ours),
          GST_TIME_ARGS (min));
      gst_query_set_latency (query, live, min, max);
      return TRUE;
    }
    default:
      return gst_pad_query_default (pad, parent, query);
  }
}

static GstStateChangeReturn
gst_av_video_filter_change_state (GstElement * element,
    GstStateChange transition)
{
  GstAvVideoFilter *self = GST_AV_VIDEO_FILTER (element);
  GstStateChangeReturn ret =
      GST_ELEMENT_CLASS (gst_av_video_filter_parent_class)->change_state
      (element, transition);

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    g_mutex_lock (&self->graph_lock);
    free_graph (self);
    self->have_in_info = self->have_out_info = FALSE;
    self->last_in_pts = GST_CLOCK_TIME_NONE;
    g_mutex_unlock (&self->graph_lock);
  }
  return ret;
}

/* ── GObject boilerplate ─────────────────────────────────────────────────── */

static void
gst_av_video_filter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAvVideoFilter *self = GST_AV_VIDEO_FILTER (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_FILTER_DESC:
      g_free (self->filter_desc);
      self->filter_desc = g_value_dup_string (value);
      self->desc_changed = TRUE;
      break;
    case PROP_LATENCY_FRAMES:
      self->latency_frames = g_value_get_uint (value);
      break;
    case PROP_LATENCY_INPUT_FRAMES:
      self->latency_input_frames = g_value_get_uint (value);
      break;
    case PROP_THREADS:
      self->threads = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_av_video_filter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstAvVideoFilter *self = GST_AV_VIDEO_FILTER (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_FILTER_DESC:
      g_value_set_string (value, self->filter_desc);
      break;
    case PROP_LATENCY_FRAMES:
      g_value_set_uint (value, self->latency_frames);
      break;
    case PROP_LATENCY_INPUT_FRAMES:
      g_value_set_uint (value, self->latency_input_frames);
      break;
    case PROP_THREADS:
      g_value_set_int (value, self->threads);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_av_video_filter_finalize (GObject * object)
{
  GstAvVideoFilter *self = GST_AV_VIDEO_FILTER (object);

  free_graph (self);
  av_frame_free (&self->out_frame);
  g_free (self->filter_desc);
  g_mutex_clear (&self->graph_lock);
  G_OBJECT_CLASS (gst_av_video_filter_parent_class)->finalize (object);
}

static void
gst_av_video_filter_class_init (GstAvVideoFilterClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->set_property = gst_av_video_filter_set_property;
  gobject_class->get_property = gst_av_video_filter_get_property;
  gobject_class->finalize = gst_av_video_filter_finalize;

  g_object_class_install_property (gobject_class, PROP_FILTER_DESC,
      g_param_spec_string ("filter-desc", "Filter description",
          "libavfilter graph description for a single-input, single-output "
          "video chain, e.g. \"framerate=fps=25\" (changes apply at the next frame)",
          DEFAULT_FILTER_DESC, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_LATENCY_FRAMES,
      g_param_spec_uint ("latency-frames", "Latency frames",
          "Latency reported in the LATENCY query, in output frames",
          0, 1000, DEFAULT_LATENCY_FRAMES,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_LATENCY_INPUT_FRAMES,
      g_param_spec_uint ("latency-input-frames", "Latency input frames",
          "Latency reported in the LATENCY query, in input frames (added to "
          "latency-frames). framerate needs exactly 1 for any rate pair",
          0, 1000, DEFAULT_LATENCY_INPUT_FRAMES,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_THREADS,
      g_param_spec_int ("threads", "Threads",
          "Number of filter graph threads (0 = libavfilter default)",
          0, 256, DEFAULT_THREADS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_set_static_metadata (element_class,
      "libav video filter graph", "Filter/Effect/Video",
      "Applies an arbitrary libavfilter video graph (N:M frames)",
      "pipeline-controller");

  element_class->change_state = gst_av_video_filter_change_state;
}

static void
gst_av_video_filter_init (GstAvVideoFilter * self)
{
  self->sinkpad = gst_pad_new_from_static_template (&sink_template, "sink");
  gst_pad_set_chain_function (self->sinkpad, gst_av_video_filter_chain);
  gst_pad_set_event_function (self->sinkpad, gst_av_video_filter_sink_event);
  gst_pad_set_query_function (self->sinkpad, gst_av_video_filter_sink_query);
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);

  self->srcpad = gst_pad_new_from_static_template (&src_template, "src");
  gst_pad_set_query_function (self->srcpad, gst_av_video_filter_src_query);
  gst_pad_use_fixed_caps (self->srcpad);
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);

  self->filter_desc = g_strdup (DEFAULT_FILTER_DESC);
  self->latency_frames = DEFAULT_LATENCY_FRAMES;
  self->latency_input_frames = DEFAULT_LATENCY_INPUT_FRAMES;
  self->reported_latency = GST_CLOCK_TIME_NONE;
  self->threads = DEFAULT_THREADS;
  self->out_frame = av_frame_alloc ();
  self->last_in_pts = GST_CLOCK_TIME_NONE;
  g_mutex_init (&self->graph_lock);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (avvideofilter_debug, "avvideofilter", 0,
      "libavfilter graph element");
  GST_DEBUG_CATEGORY_INIT (libav_debug, "libav", 0, "libav messages");
  av_log_set_callback (av_log_to_gst);
  return gst_element_register (plugin, "avvideofilter", GST_RANK_NONE,
      GST_TYPE_AV_VIDEO_FILTER);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, avvideofilter,
    "libavfilter graph element", plugin_init, VERSION, GST_LICENSE,
    GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
