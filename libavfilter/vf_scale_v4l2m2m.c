/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavcodec/v4l2_m2m.h"
#include "libavfilter/bufferqueue.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "scale.h"
#include "video.h"

typedef struct ScaleV4L2Context {
    V4L2m2mPriv v4l2m2m_priv; // must be first, contains AVClass*

    char *w_expr;      // width expression string
    char *h_expr;      // height expression string

    int output_format;
    int output_width, output_height;

    int eof;
    struct FFBufQueue frame_queue;
} ScaleV4L2Context;

static int scale_v4l2_config_output(AVFilterLink *outlink)
{
    AVFilterLink *inlink    = outlink->src->inputs[0];
    AVFilterContext *avctx  = outlink->src;
    ScaleV4L2Context *ctx   = avctx->priv;
    V4L2m2mPriv *priv       = &ctx->v4l2m2m_priv;
    V4L2m2mContext *s       = priv->context;
    V4L2Context *capture, *output;
    int err;

    if ((err = ff_scale_eval_dimensions(ctx,
                                        ctx->w_expr, ctx->h_expr,
                                        inlink, outlink,
                                        &ctx->output_width, &ctx->output_height)) < 0)
        return err;

    if (!ctx->output_width)
        ctx->output_width  = avctx->inputs[0]->w;
    if (!ctx->output_height)
        ctx->output_height = avctx->inputs[0]->h;

    if (inlink->sample_aspect_ratio.num)
        outlink->sample_aspect_ratio = av_mul_q((AVRational){outlink->h * inlink->w, outlink->w * inlink->h}, inlink->sample_aspect_ratio);
    else
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    outlink->w = ctx->output_width;
    outlink->h = ctx->output_height;

    capture = &s->capture;
    output  = &s->output;

    /* dimension settings */
    output->height = avctx->inputs[0]->h;
    output->width = avctx->inputs[0]->w;
    capture->height = ctx->output_height;
    capture->width = ctx->output_width;

    /* output context */
    output->av_codec_id = AV_CODEC_ID_RAWVIDEO;
    output->av_pix_fmt = avctx->inputs[0]->format;
    if (output->av_pix_fmt == AV_PIX_FMT_DRM_PRIME)
        output->sw_pix_fmt = AV_PIX_FMT_NV12;

    /* capture context */
    capture->av_codec_id = AV_CODEC_ID_RAWVIDEO;
    capture->av_pix_fmt = avctx->outputs[0]->format;
    if (capture->av_pix_fmt == AV_PIX_FMT_DRM_PRIME)
        capture->sw_pix_fmt = AV_PIX_FMT_NV12;

    if (output->av_pix_fmt == AV_PIX_FMT_DRM_PRIME ||
        capture->av_pix_fmt == AV_PIX_FMT_DRM_PRIME) {
        if (avctx->hw_device_ctx) {
            s->device_ref = av_buffer_ref(avctx->hw_device_ctx);
        } else {
            s->device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DRM);
            if (!s->device_ref)
                return AVERROR(ENOMEM);

            err = av_hwdevice_ctx_init(s->device_ref);
            if (err < 0) {
                av_buffer_unref(&s->device_ref);
                return err;
            }
        }
    }

    err = ff_v4l2_m2m_codec_init(priv);
    if (err) {
        av_log(avctx, AV_LOG_ERROR, "can't configure encoder\n");
        return err;
    }

    return 0;
}

static int scale_v4l2_dequeue(AVFilterContext *avctx, int timeout)
{
    ScaleV4L2Context *ctx   = avctx->priv;
    AVFilterLink *outlink   = avctx->outputs[0];
    AVFrame *input_frame    = NULL;
    AVFrame *output_frame   = NULL;
    V4L2m2mPriv *priv       = &ctx->v4l2m2m_priv;
    V4L2m2mContext *s       = priv->context;
    int err;
    V4L2Context *capture;

    if (!ctx->frame_queue.available)
        return ctx->eof ? AVERROR_EOF : AVERROR(EAGAIN);

    if (outlink->format == AV_PIX_FMT_DRM_PRIME)
        output_frame = av_frame_alloc();
    else
        output_frame = ff_get_video_buffer(outlink, ctx->output_width,
                                           ctx->output_height);
    if (!output_frame) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    capture = &s->capture;

    err = ff_v4l2_context_dequeue_frame(capture, output_frame, timeout);
    if (err < 0)
        goto fail;

    input_frame = ff_bufqueue_get(&ctx->frame_queue);
    err = av_frame_copy_props(output_frame, input_frame);
    if (err < 0)
        goto fail;

    av_frame_free(&input_frame);

    return ff_filter_frame(outlink, output_frame);

fail:
    av_frame_free(&input_frame);
    av_frame_free(&output_frame);
    return err;
}

static int scale_v4l2_filter_frame(AVFilterLink *inlink, AVFrame *input_frame)
{
    AVFilterContext *avctx  = inlink->dst;
    ScaleV4L2Context *ctx   = avctx->priv;
    V4L2m2mPriv *priv       = &ctx->v4l2m2m_priv;
    V4L2m2mContext *s       = priv->context;
    V4L2Context *capture, *output;
    int err;

    av_log(avctx, AV_LOG_DEBUG, "Filter input: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(input_frame->format),
           input_frame->width, input_frame->height, input_frame->pts);

    capture = &s->capture;
    output  = &s->output;

    err = ff_v4l2_context_enqueue_frame(output, input_frame);
    if (err < 0)
        return err;
    ff_bufqueue_add(avctx, &ctx->frame_queue, input_frame);

    if (!output->streamon) {
        err = ff_v4l2_context_set_status(output, VIDIOC_STREAMON);
        if (err) {
            av_log(avctx, AV_LOG_ERROR, "VIDIOC_STREAMON failed on output context: %s\n", strerror(errno));
            return err;
        }
    }
    if (!capture->streamon) {
        err = ff_v4l2_context_set_status(capture, VIDIOC_STREAMON);
        if (err) {
            av_log(avctx, AV_LOG_ERROR, "VIDIOC_STREAMON failed on capture context: %s\n", strerror(errno));
            return err;
        }
    }

    err = scale_v4l2_dequeue(avctx, 0);
    if (err == AVERROR(EAGAIN))
        return 0;

    return err;
}

static int scale_v4l2_request_frame(AVFilterLink *outlink)
{
    AVFilterContext *avctx  = outlink->src;
    ScaleV4L2Context *ctx   = avctx->priv;
    V4L2m2mPriv *priv       = &ctx->v4l2m2m_priv;
    V4L2m2mContext *s       = priv->context;
    int err, timeout = 0;

    /* if feeding in dmabuf, wait to receive a frame so we can
     * free the underlying buffer and return it to the decoder.
     */
    if (s->output.av_pix_fmt == AV_PIX_FMT_DRM_PRIME)
        timeout = -1;

    err = scale_v4l2_dequeue(avctx, timeout);
    if (err != AVERROR(EAGAIN))
        return err;

    err = ff_request_frame(outlink->src->inputs[0]);
    if (err == AVERROR_EOF) {
        ctx->eof = 1;
        s->draining = 1;
        err = scale_v4l2_dequeue(avctx, -1);
    }

    return err;
}

static int scale_v4l2_query_formats(AVFilterContext *avctx)
{
    ScaleV4L2Context *ctx = avctx->priv;
    static const enum AVPixelFormat hw_pixel_formats[] = {
        AV_PIX_FMT_DRM_PRIME,
        AV_PIX_FMT_NONE,
    };
    static const enum AVPixelFormat pixel_formats[] = {
        AV_PIX_FMT_DRM_PRIME,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_NONE,
    };
    int ret;

    if (ctx->output_format == AV_PIX_FMT_DRM_PRIME) {
        if ((ret = ff_formats_ref(ff_make_format_list(pixel_formats),
                                  &avctx->inputs[0]->out_formats)) < 0)
            return ret;
        if ((ret = ff_formats_ref(ff_make_format_list(hw_pixel_formats),
                                  &avctx->outputs[0]->in_formats)) < 0)
            return ret;
    } else {
        if ((ret = ff_set_common_formats(avctx, ff_make_format_list(pixel_formats))) < 0)
            return ret;
    }

    return 0;
}

static av_cold int scale_v4l2_init(AVFilterContext *avctx)
{
    ScaleV4L2Context *ctx = avctx->priv;
    V4L2m2mContext *s;
    int ret;

    ret = ff_v4l2_m2m_create_context(&ctx->v4l2m2m_priv, &s);
    if (ret < 0)
        return ret;
    s->filterctx = avctx;

    return 0;
}

static av_cold void scale_v4l2_uninit(AVFilterContext *avctx)
{
    ScaleV4L2Context *ctx = avctx->priv;
    V4L2m2mPriv *priv     = &ctx->v4l2m2m_priv;

    ff_v4l2_m2m_codec_end(priv);
    ff_bufqueue_discard_all(&ctx->frame_queue);
}

#define OFFSET(x) offsetof(ScaleV4L2Context, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption scale_v4l2m2m_options[] = {
    { "w", "Output video width",
      OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, .flags = FLAGS },
    { "h", "Output video height",
      OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, .flags = FLAGS },
    { "format", "Optional format conversion with scaling",
      OFFSET(output_format), AV_OPT_TYPE_PIXEL_FMT, {.i64 = AV_PIX_FMT_NONE}, AV_PIX_FMT_NONE, INT_MAX, .flags = FLAGS },

#undef OFFSET
#define OFFSET(x) offsetof(V4L2m2mPriv, x)
    V4L_M2M_DEFAULT_OPTS(6, 6),
    { NULL },
};

AVFILTER_DEFINE_CLASS(scale_v4l2m2m);

static const AVFilterPad scale_v4l2_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &scale_v4l2_filter_frame,
    },
    { NULL }
};

static const AVFilterPad scale_v4l2_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props  = &scale_v4l2_config_output,
        .request_frame = &scale_v4l2_request_frame,
    },
    { NULL }
};

AVFilter ff_vf_scale_v4l2m2m = {
    .name          = "scale_v4l2m2m",
    .description   = NULL_IF_CONFIG_SMALL("Scale using V4L2 M2M device."),
    .priv_size     = sizeof(ScaleV4L2Context),
    .init          = &scale_v4l2_init,
    .uninit        = &scale_v4l2_uninit,
    .query_formats = &scale_v4l2_query_formats,
    .inputs        = scale_v4l2_inputs,
    .outputs       = scale_v4l2_outputs,
    .priv_class    = &scale_v4l2m2m_class,
};
