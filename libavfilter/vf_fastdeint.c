/*
 * Copyright (C) 2015 Aman Gupta <aman@tmm1.net>
 *               2000-2011 VLC authors and VideoLAN
 *
 * Author: Sam Hocevar <sam@zoy.org>
 *         Damien Lucas <nitrox@videolan.org>
 *         Laurent Aimar <fenrir@videolan.org>
 *         Sigmund Augdal Helberg <sigmunau@videolan.org>
 *
 * These algorithms are derived from the VLC project's
 * modules/video_filter/deinterlace/algo_basic.c
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

#include "libavutil/avassert.h"
#include "libavutil/cpu.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/timestamp.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

enum Mode {
  MODE_DISCARD,
  MODE_MEAN,
  MODE_BLEND,
  MODE_BOB,
  MODE_LINEAR,
  MODE_MAX,
};

typedef void (*merge_fn)(void *dst, const void *src1, const void *src2, size_t len);

typedef struct FastDeintContext {
    const AVClass *class;
    merge_fn merge;
    int merge_size;
    int merge_aligned;
    AVFrame *cur, *next;
    enum Mode mode;
    int eof;
} FastDeintContext;

static void merge8_c(uint8_t *dst, const uint8_t *src1, const uint8_t *src2, size_t bytes)
{
    for (; bytes > 0; bytes--)
        *dst++ = ( *src1++ + *src2++ ) >> 1;
}

static void merge16_c(uint16_t *dst, const uint16_t *src1, const uint16_t *src2, size_t bytes)
{
    for (size_t words = bytes / 2; words > 0; words--)
        *dst++ = ( *src1++ + *src2++ ) >> 1;
}

static void merge8_unaligned(FastDeintContext *s, uint8_t *dst, const uint8_t *src1, const uint8_t *src2, size_t bytes)
{
    if (s->merge_aligned) {
        size_t remainder = bytes % 16;
        if (remainder > 0) {
            merge8_c(dst, src1, src2, remainder);
            bytes -= remainder;
            dst += remainder;
            src1 += remainder;
            src2 += remainder;
        }
    }
    s->merge(dst, src1, src2, bytes);
}

static void merge16_unaligned(FastDeintContext *s, uint16_t *dst, const uint16_t *src1, const uint16_t *src2, size_t bytes)
{
    if (s->merge_aligned) {
        size_t words = bytes / 2;
        size_t remainder = words % 8;
        if (remainder > 0) {
            merge16_c(dst, src1, src2, remainder);
            words -= remainder;
            dst += remainder;
            src1 += remainder;
            src2 += remainder;
        }
    }
    s->merge(dst, src1, src2, bytes);
}

static void merge_unaligned(FastDeintContext *s, void *dst, const void *src1, const void *src2, size_t bytes)
{
    if (s->merge_size == 16)
        merge16_unaligned(s, dst, src1, src2, bytes);
    else
        merge8_unaligned(s, dst, src1, src2, bytes);
}

#if HAVE_SSE2_INLINE && defined(__x86_64__)
static void merge8_sse2(uint8_t *dst, const uint8_t *src1, const uint8_t *src2, size_t bytes)
{
    for(; bytes > 0 && ((uintptr_t)src1 & 15); bytes--)
        *dst++ = ( *src1++ + *src2++ ) >> 1;

    for (; bytes >= 16; bytes -= 16) {
        __asm__  __volatile__( "movdqu %2,%%xmm1;"
                               "pavgb %1, %%xmm1;"
                               "movdqu %%xmm1, %0" :"=m" (*dst):
                                                 "m" (*src1),
                                                 "m" (*src2) : "xmm1" );
        dst += 16;
        src1 += 16;
        src2 += 16;
    }

    if (bytes > 0) {
        merge8_c(dst, src1, src2, bytes);
    }
}
static void merge16_sse2(uint16_t *dst, const uint16_t *src1, const uint16_t *src2, size_t bytes)
{
    size_t words = bytes / 2;

    for(; words > 0 && ((uintptr_t)src1 & 15); words--)
        *dst++ = ( *src1++ + *src2++ ) >> 1;

    for (; words >= 8; words -= 8) {
        __asm__  __volatile__( "movdqu %2,%%xmm1;"
                               "pavgw %1, %%xmm1;"
                               "movdqu %%xmm1, %0" :"=m" (*dst):
                                                 "m" (*src1),
                                                 "m" (*src2) : "xmm1" );
        dst += 8;
        src1 += 8;
        src2 += 8;
    }

    if (words > 0) {
        merge16_c(dst, src1, src2, words * 2);
    }
}
#define merge8 merge8_sse2
#define merge16 merge16_sse2
#else
#define merge8 merge8_c
#define merge16 merge16_c
#endif

static void render_image_single(FastDeintContext *s, AVFrame *out, AVFrame *frame)
{
    int i, planes_nb = 0;
    enum Mode mode = s->mode;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(out->format);

    for (i = 0; i < desc->nb_components; i++)
        planes_nb = FFMAX(planes_nb, desc->comp[i].plane + 1);

    for (i = 0; i < planes_nb; i++) {
        int height, bwidth;
        int dst_linesize, src_linesize;
        const uint8_t *src;
        uint8_t *dst;

        bwidth = av_image_get_linesize(out->format, out->width, i);
        if (bwidth < 0) {
            av_log(s, AV_LOG_ERROR, "av_image_get_linesize failed\n");
            return;
        }

        height = out->height;
        if (i == 1 || i == 2) {
            height = FF_CEIL_RSHIFT(out->height, desc->log2_chroma_h);
        }

        src = frame->data[i];
        dst = out->data[i];
        dst_linesize = out->linesize[i];
        src_linesize = frame->linesize[i];

        if (mode == MODE_BLEND) {
            // Copy first line
            memcpy(dst, src, bwidth);
            dst += dst_linesize;
            height--;
        }

        // Merge remaining lines
        for (; height > 0; height--) {
            if (mode == MODE_DISCARD)
                memcpy(dst, src, bwidth);
            else
                merge_unaligned(s, dst, src, src + src_linesize, bwidth);
            dst += dst_linesize;
            src += src_linesize;
            if (mode == MODE_MEAN || mode == MODE_DISCARD) {
                src += src_linesize;
                height--;
            }
        }
    }
    if (mode != MODE_DISCARD)
        emms_c();
}

static void render_image_doubler(FastDeintContext *s, AVFrame *out, AVFrame *frame, int field)
{
    int i, planes_nb = 0;
    enum Mode mode = s->mode;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(out->format);

    for (i = 0; i < desc->nb_components; i++)
        planes_nb = FFMAX(planes_nb, desc->comp[i].plane + 1);

    for (i = 0; i < planes_nb; i++) {
        int height, bwidth;
        int dst_linesize, src_linesize;
        const uint8_t *src;
        uint8_t *dst;

        bwidth = av_image_get_linesize(out->format, out->width, i);
        if (bwidth < 0) {
            av_log(s, AV_LOG_ERROR, "av_image_get_linesize failed\n");
            return;
        }
        height = out->height;
        if (i == 1 || i == 2) {
            height = FF_CEIL_RSHIFT(out->height, desc->log2_chroma_h);
        }

        src = frame->data[i];
        dst = out->data[i];
        src_linesize = frame->linesize[i];
        dst_linesize = out->linesize[i];

        // For BOTTOM field we need to add the first line
        if (field == 1) {
            memcpy(dst, src, bwidth);
            dst += dst_linesize;
            src += src_linesize;
            height--;
        }

        height -= 2;

        for (; height > 0; height-=2) {
            memcpy(dst, src, bwidth);
            dst += dst_linesize;

            if (mode == MODE_LINEAR)
                merge_unaligned(s, dst, src, src + 2 * src_linesize, bwidth);
            else
                memcpy(dst, src, bwidth);
            dst += dst_linesize;

            src += src_linesize * 2;
        }

        memcpy(dst, src, bwidth);

        // For TOP field we need to add the last line
        if (field == 0)
        {
            dst += dst_linesize;
            src += src_linesize;
            memcpy(dst, src, bwidth);
        }
    }
    if (mode == MODE_LINEAR)
        emms_c();
}

static int filter_frame_single(AVFilterLink *link, AVFrame *frame)
{
    AVFilterContext *ctx = link->dst;
    AVFrame *out;
    FastDeintContext *s = ctx->priv;

    if (!frame->interlaced_frame) {
        return ff_filter_frame(ctx->outputs[0], frame);
    }

    out = ff_get_video_buffer(ctx->outputs[0], link->w, link->h);
    if (!out) {
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }

    av_frame_copy_props(out, frame);
    out->interlaced_frame = 0;
    render_image_single(s, out, frame);

    av_frame_free(&frame);
    return ff_filter_frame(ctx->outputs[0], out);
}

static AVFrame *copy_frame(AVFilterLink *link, AVFrame *frame)
{
    AVFilterContext *ctx = link->dst;
    AVFrame *out;

    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX)
        out = av_frame_alloc();
    else
        out = ff_get_video_buffer(ctx->outputs[0], link->w, link->h);

    if (!out)
        return NULL;

    av_frame_copy_props(out, frame);
    return out;
}

static int filter_frame_double(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext *ctx = link->dst;
    FastDeintContext *s = ctx->priv;
    AVFrame *frame, *out, *out2;
    int tff, ret;

    s->cur = s->next;
    s->next = in;

    if (!s->cur) {
        return 0;
    }

    frame = s->cur;

    if (!frame->interlaced_frame) {
        if (frame->pts != AV_NOPTS_VALUE)
            frame->pts *= 2;
        s->cur = NULL;
        return ff_filter_frame(ctx->outputs[0], frame);
    }

    tff = frame->top_field_first;
    out = copy_frame(link, frame);
    if (!out) {
        av_frame_free(&frame);
        s->cur = NULL;
        return AVERROR(ENOMEM);
    }

    out->interlaced_frame = 0;
    if (out->pts != AV_NOPTS_VALUE)
        out->pts = out->pts * 2;
    render_image_doubler(s, out, frame, !tff);

    ret = ff_filter_frame(ctx->outputs[0], out);
    if (ret < 0) {
        av_frame_free(&frame);
        s->cur = NULL;
        return ret;
    }

    out2 = copy_frame(link, frame);
    if (!out2) {
        av_frame_free(&frame);
        s->cur = NULL;
        return AVERROR(ENOMEM);
    }

    out2->interlaced_frame = 0;
    av_frame_remove_side_data(out2, AV_FRAME_DATA_A53_CC);
    if (out2->pts != AV_NOPTS_VALUE) {
        out2->pts = frame->pts + s->next->pts;
    }
    render_image_doubler(s, out2, frame, tff);

    av_frame_free(&frame);
    s->cur = NULL;

    return ff_filter_frame(ctx->outputs[0], out2);
}

static int filter_frame(AVFilterLink *link, AVFrame *frame)
{
    AVFilterContext *ctx = link->dst;
    FastDeintContext *s = ctx->priv;

    av_assert0(frame);

    if (s->mode == MODE_LINEAR || s->mode == MODE_BOB) {
        return filter_frame_double(link, frame);
    } else {
        return filter_frame_single(link, frame);
    }
}

static av_cold void uninit(AVFilterContext *ctx)
{
    FastDeintContext *s = ctx->priv;
    av_frame_free(&s->cur);
    av_frame_free(&s->next);
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_YUV411P,
        AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_YUVJ420P,
        AV_PIX_FMT_YUVJ422P,
        AV_PIX_FMT_YUVJ444P,
        AV_PIX_FMT_GRAY16,
        AV_PIX_FMT_YUV440P,
        AV_PIX_FMT_YUVJ440P,
        AV_PIX_FMT_YUV420P9,
        AV_PIX_FMT_YUV422P9,
        AV_PIX_FMT_YUV444P9,
        AV_PIX_FMT_YUV420P10,
        AV_PIX_FMT_YUV422P10,
        AV_PIX_FMT_YUV444P10,
        AV_PIX_FMT_YUV420P12,
        AV_PIX_FMT_YUV422P12,
        AV_PIX_FMT_YUV444P12,
        AV_PIX_FMT_YUV420P14,
        AV_PIX_FMT_YUV422P14,
        AV_PIX_FMT_YUV444P14,
        AV_PIX_FMT_YUV420P16,
        AV_PIX_FMT_YUV422P16,
        AV_PIX_FMT_YUV444P16,
        AV_PIX_FMT_YUVA420P,
        AV_PIX_FMT_YUVA422P,
        AV_PIX_FMT_YUVA444P,
        AV_PIX_FMT_GBRP,
        AV_PIX_FMT_GBRP9,
        AV_PIX_FMT_GBRP10,
        AV_PIX_FMT_GBRP12,
        AV_PIX_FMT_GBRP14,
        AV_PIX_FMT_GBRP16,
        AV_PIX_FMT_GBRAP,
        AV_PIX_FMT_NONE
    };

    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

#if ARCH_ARM
#include "libavutil/arm/cpu.h"
#endif
#if ARCH_AARCH64
#include "libavutil/aarch64/cpu.h"
#endif
#if ARCH_AARCH64 || ARCH_ARM
void ff_merge8_neon(uint8_t *dst, const uint8_t *src1, const uint8_t *src2, size_t bytes);
void ff_merge16_neon(uint16_t *dst, const uint16_t *src1, const uint16_t *src2, size_t bytes);
void ff_merge8_armv6(uint8_t *dst, const uint8_t *src1, const uint8_t *src2, size_t bytes);
void ff_merge16_armv6(uint16_t *dst, const uint16_t *src1, const uint16_t *src2, size_t bytes);
#endif

static int config_props(AVFilterLink *link)
{
    AVFilterContext *ctx = link->src;
    FastDeintContext *s = ctx->priv;
    const AVPixFmtDescriptor *pix;
#if ARCH_AARCH64 || ARCH_ARM
    int cpu_flags = av_get_cpu_flags();
#endif

    link->w = link->src->inputs[0]->w;
    link->h = link->src->inputs[0]->h;
    link->time_base  = link->src->inputs[0]->time_base;
    link->frame_rate = link->src->inputs[0]->frame_rate;
    link->sample_aspect_ratio = link->src->inputs[0]->sample_aspect_ratio;

    if (s->mode == MODE_MEAN || s->mode == MODE_DISCARD) {
        link->h /= 2;
        link->sample_aspect_ratio = av_mul_q(link->sample_aspect_ratio, av_make_q(1, 2));
    }
    if (s->mode == MODE_LINEAR || s->mode == MODE_BOB) {
        link->time_base  = av_mul_q(link->time_base,  av_make_q(1, 2));
        link->frame_rate = av_mul_q(link->frame_rate, av_make_q(2, 1));
    }

    pix = av_pix_fmt_desc_get(link->format);
    s->merge_size = (pix->comp[0].depth > 8) ? 16 : 8;
    s->merge = s->merge_size == 16 ? (merge_fn)merge16 : (merge_fn)merge8;

#if ARCH_ARM
    if (have_armv6(cpu_flags)) {
        s->merge = s->merge_size == 16 ? (merge_fn)ff_merge16_armv6 : (merge_fn)ff_merge8_armv6;
        s->merge_aligned = 1;
    }
#endif
#if ARCH_AARCH64 || ARCH_ARM
    if (have_neon(cpu_flags)) {
        s->merge = s->merge_size == 16 ? (merge_fn)ff_merge16_neon : (merge_fn)ff_merge8_neon;
        s->merge_aligned = 1;
    }
#endif

    return 0;
}

static int request_frame(AVFilterLink *link)
{
    AVFilterContext *ctx = link->src;
    FastDeintContext *s = ctx->priv;
    int ret;

    if (s->eof)
        return AVERROR_EOF;

    ret = ff_request_frame(ctx->inputs[0]);

    if (ret == AVERROR_EOF && s->cur) {
        AVFrame *next = av_frame_clone(s->next);
        if (!next)
            return AVERROR(ENOMEM);

        next->pts = s->next->pts * 2 - s->cur->pts;
        filter_frame(ctx->inputs[0], next);
        s->eof = 1;
    } else if (ret < 0) {
        return ret;
    }

    return 0;
}

#define OFFSET(x) offsetof(FastDeintContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

#define CONST(name, help, val, unit) { name, help, 0, AV_OPT_TYPE_CONST, {.i64=val}, INT_MIN, INT_MAX, FLAGS, unit }

static const AVOption fastdeint_options[] = {
    { "mode", "specify the deinterlacing mode", OFFSET(mode), AV_OPT_TYPE_INT, {.i64=MODE_BLEND}, 0, MODE_MAX-1, FLAGS, "mode" },
    CONST("discard", "discard bottom frame", MODE_DISCARD, "mode"),
    CONST("mean", "half resolution blender", MODE_MEAN, "mode"),
    CONST("blend", "full resolution blender", MODE_BLEND, "mode"),
    CONST("bob", "bob doubler", MODE_BOB, "mode"),
    CONST("linear", "bob doubler with linear interpolation", MODE_LINEAR, "mode"),

    { NULL }
};

AVFILTER_DEFINE_CLASS(fastdeint);

static const AVFilterPad fastdeint_inputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .filter_frame  = filter_frame,
    },
    { NULL }
};

static const AVFilterPad fastdeint_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_props,
        .request_frame = request_frame
    },
    { NULL }
};

AVFilter ff_vf_fastdeint = {
    .name          = "fastdeint",
    .description   = NULL_IF_CONFIG_SMALL("fast deinterlacing algorithms"),
    .priv_size     = sizeof(FastDeintContext),
    .priv_class    = &fastdeint_class,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = fastdeint_inputs,
    .outputs       = fastdeint_outputs,
};
