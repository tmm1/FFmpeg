/*
 * Copyright (C) 2018 Philip Langdale <philipl@overt.org>
 *               2020 Aman Karmani <aman@tmm1.net>
 *
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

#include "internal.h"
#include "yadif.h"
#include <libavutil/avassert.h>
#include <libavutil/hwcontext.h>

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

extern char vf_deinterlace_videotoolbox_metallib[];
extern unsigned int vf_deinterlace_videotoolbox_metallib_len;

typedef struct DeintMetalContext {
    YADIFContext yadif;

    AVBufferRef       *device_ref;
    AVBufferRef       *input_frames_ref;
    AVHWFramesContext *input_frames;

    id<MTLDevice> mtlDevice;
    id<MTLLibrary> mtlLibrary;
    id<MTLCommandQueue> mtlQueue;
    id<MTLComputePipelineState> mtlPipeline;
    id<MTLFunction> mtlFunction;
    id<MTLBuffer> mtlParamsBuffer;

    CVMetalTextureCacheRef textureCache;
} DeintMetalContext;

struct mtlYadifParams {
    uint channels;
    uint parity;
    uint tff;
    bool is_second_field;
    bool skip_spatial_check;
    int field_mode;
};

static void call_kernel(AVFilterContext *ctx,
                        id<MTLTexture> dst,
                        id<MTLTexture> prev,
                        id<MTLTexture> cur,
                        id<MTLTexture> next,
                        int channels,
                        int parity,
                        int tff)
{
    DeintMetalContext *s = ctx->priv;
    id<MTLCommandBuffer> buffer = s->mtlQueue.commandBuffer;
    id<MTLComputeCommandEncoder> encoder = buffer.computeCommandEncoder;
    struct mtlYadifParams *params = (struct mtlYadifParams *)s->mtlParamsBuffer.contents;
    *params = (struct mtlYadifParams){
        .channels = channels,
        .parity = parity,
        .tff = tff,
        .is_second_field = !(parity ^ tff),
        .skip_spatial_check = s->yadif.mode&2,
        .field_mode = s->yadif.current_field
    };

    [encoder setComputePipelineState:s->mtlPipeline];
    [encoder setTexture:dst  atIndex:0];
    [encoder setTexture:prev atIndex:1];
    [encoder setTexture:cur  atIndex:2];
    [encoder setTexture:next atIndex:3];
    [encoder setBuffer:s->mtlParamsBuffer offset:0 atIndex:4];

    NSUInteger w = s->mtlPipeline.threadExecutionWidth;
    NSUInteger h = s->mtlPipeline.maxTotalThreadsPerThreadgroup / w;
    MTLSize threadsPerThreadgroup = MTLSizeMake(w, h, 1);
    BOOL fallback = YES;
    if (@available(macOS 10.15, iOS 11, tvOS 14.5, *)) {
        if ([s->mtlDevice supportsFamily:MTLGPUFamilyCommon3]) {
            MTLSize threadsPerGrid = MTLSizeMake(dst.width, dst.height, 1);
            [encoder dispatchThreads:threadsPerGrid threadsPerThreadgroup:threadsPerThreadgroup];
            fallback = NO;
        }
    }
    if (fallback) {
        MTLSize threadgroups = MTLSizeMake((dst.width + w - 1) / w,
                                           (dst.height + h - 1) / h,
                                           1);
        [encoder dispatchThreadgroups:threadgroups threadsPerThreadgroup:threadsPerThreadgroup];
    }

    [encoder endEncoding];

    [buffer commit];
    [buffer waitUntilCompleted];

    [encoder release];
    encoder = nil;
    [buffer release];
    buffer = nil;
}

static CVMetalTextureRef pixbuf_to_texture(AVFilterContext *ctx,
                                           CVPixelBufferRef pixbuf,
                                           int plane,
                                           MTLPixelFormat format)
{
    DeintMetalContext *s = ctx->priv;
    CVMetalTextureRef tex = NULL;
    CVReturn ret;

    ret = CVMetalTextureCacheCreateTextureFromImage(
        NULL,
        s->textureCache,
        pixbuf,
        NULL,
        format,
        CVPixelBufferGetWidthOfPlane(pixbuf, plane),
        CVPixelBufferGetHeightOfPlane(pixbuf, plane),
        plane,
        &tex
    );
    if (ret != kCVReturnSuccess) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create CVMetalTexture from image: %d\n", ret);
        return NULL;
    }

    return tex;
}

static void filter(AVFilterContext *ctx, AVFrame *dst,
                   int parity, int tff)
{
    DeintMetalContext *s = ctx->priv;
    YADIFContext *y = &s->yadif;
    int i;

    for (i = 0; i < y->csp->nb_components; i++) {
        int pixel_size, channels;
        const AVComponentDescriptor *comp = &y->csp->comp[i];
        CVMetalTextureRef prev, cur, next, dest;
        id<MTLTexture> tex_prev, tex_cur, tex_next, tex_dest;
        MTLPixelFormat format;

        if (comp->plane < i) {
            // We process planes as a whole, so don't reprocess
            // them for additional components
            continue;
        }

        pixel_size = (comp->depth + comp->shift) / 8;
        channels = comp->step / pixel_size;
        if (pixel_size > 2 || channels > 2) {
            av_log(ctx, AV_LOG_ERROR, "Unsupported pixel format: %s\n", y->csp->name);
            goto exit;
        }
        switch (pixel_size) {
        case 1:
            format = channels == 1 ? MTLPixelFormatR8Unorm : MTLPixelFormatRG8Unorm;
            break;
        case 2:
            format = channels == 1 ? MTLPixelFormatR16Unorm : MTLPixelFormatRG16Unorm;
            break;
        default:
            av_log(ctx, AV_LOG_ERROR, "Unsupported pixel format: %s\n", y->csp->name);
            goto exit;
        }
        av_log(ctx, AV_LOG_TRACE,
               "Deinterlacing plane %d: pixel_size: %d channels: %d\n",
               comp->plane, pixel_size, channels);

        prev = pixbuf_to_texture(ctx, (CVPixelBufferRef)y->prev->data[3], i, format);
        cur  = pixbuf_to_texture(ctx, (CVPixelBufferRef)y->cur->data[3], i, format);
        next = pixbuf_to_texture(ctx, (CVPixelBufferRef)y->next->data[3], i, format);
        dest = pixbuf_to_texture(ctx, (CVPixelBufferRef)dst->data[3], i, format);

        tex_prev = CVMetalTextureGetTexture(prev);
        tex_cur  = CVMetalTextureGetTexture(cur);
        tex_next = CVMetalTextureGetTexture(next);
        tex_dest = CVMetalTextureGetTexture(dest);

        call_kernel(ctx, tex_dest, tex_prev, tex_cur, tex_next,
                         channels, parity, tff);

        CFRelease(prev);
        CFRelease(cur);
        CFRelease(next);
        CFRelease(dest);
    }

    CVBufferPropagateAttachments((CVPixelBufferRef)y->cur->data[3], (CVPixelBufferRef)dst->data[3]);

    if (y->current_field == YADIF_FIELD_END) {
        y->current_field = YADIF_FIELD_NORMAL;
    }

exit:
    return;
}

static av_cold int deint_videotoolbox_init(AVFilterContext *ctx)
{
    DeintMetalContext *s = ctx->priv;
    NSError *err = nil;
    CVReturn ret;

    s->mtlDevice = MTLCreateSystemDefaultDevice();
    if (!s->mtlDevice) {
        av_log(ctx, AV_LOG_ERROR, "Unable to find Metal device\n");
        return AVERROR_EXTERNAL;
    }

    av_log(ctx, AV_LOG_INFO, "Using Metal device: %s\n", s->mtlDevice.name.UTF8String);

    dispatch_data_t libData = dispatch_data_create(
        vf_deinterlace_videotoolbox_metallib,
        vf_deinterlace_videotoolbox_metallib_len,
        nil,
        nil);
    s->mtlLibrary = [s->mtlDevice
        newLibraryWithData:libData
        error:&err];
    dispatch_release(libData);
    libData = nil;
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Failed to load Metal library: %s\n", err.description.UTF8String);
        return AVERROR_EXTERNAL;
    }
    s->mtlFunction = [s->mtlLibrary newFunctionWithName:@"deint"];

    s->mtlQueue = s->mtlDevice.newCommandQueue;
    if (!s->mtlQueue) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create Metal command queue!\n");
        return AVERROR_EXTERNAL;
    }

    s->mtlPipeline = [s->mtlDevice
        newComputePipelineStateWithFunction:s->mtlFunction
        error:&err];
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create Metal compute pipeline: %s\n", err.description.UTF8String);
        return AVERROR_EXTERNAL;
    }

    s->mtlParamsBuffer = [s->mtlDevice
        newBufferWithLength:sizeof(struct mtlYadifParams)
        options:MTLResourceStorageModeShared];

    ret = CVMetalTextureCacheCreate(
        NULL,
        NULL,
        s->mtlDevice,
        NULL,
        &s->textureCache
    );
    if (ret != kCVReturnSuccess) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create CVMetalTextureCache: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static av_cold void deint_videotoolbox_uninit(AVFilterContext *ctx)
{
    DeintMetalContext *s = ctx->priv;
    YADIFContext *y = &s->yadif;

    av_frame_free(&y->prev);
    av_frame_free(&y->cur);
    av_frame_free(&y->next);

    av_buffer_unref(&s->device_ref);
    av_buffer_unref(&s->input_frames_ref);
    s->input_frames = NULL;

    [s->mtlParamsBuffer release];
    [s->mtlFunction release];
    [s->mtlPipeline release];
    [s->mtlQueue release];
    [s->mtlLibrary release];
    [s->mtlDevice release];

    s->mtlParamsBuffer = nil;
    s->mtlFunction = nil;
    s->mtlPipeline = nil;
    s->mtlQueue = nil;
    s->mtlLibrary = nil;
    s->mtlDevice = nil;

    if (s->textureCache) {
        CFRelease(s->textureCache);
        s->textureCache = NULL;
    }
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    DeintMetalContext *s  = ctx->priv;

    if (!inlink->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "A hardware frames reference is "
               "required to associate the processing device.\n");
        return AVERROR(EINVAL);
    }

    s->input_frames_ref = av_buffer_ref(inlink->hw_frames_ctx);
    if (!s->input_frames_ref) {
        av_log(ctx, AV_LOG_ERROR, "A input frames reference create "
               "failed.\n");
        return AVERROR(ENOMEM);
    }
    s->input_frames = (AVHWFramesContext*)s->input_frames_ref->data;

    return 0;
}

static int config_output(AVFilterLink *link)
{
    AVHWFramesContext *output_frames;
    AVFilterContext *ctx = link->src;
    DeintMetalContext *s = ctx->priv;
    YADIFContext *y = &s->yadif;
    int ret = 0;

    av_assert0(s->input_frames);
    s->device_ref = av_buffer_ref(s->input_frames->device_ref);
    if (!s->device_ref) {
        av_log(ctx, AV_LOG_ERROR, "A device reference create "
               "failed.\n");
        return AVERROR(ENOMEM);
    }

    link->hw_frames_ctx = av_hwframe_ctx_alloc(s->device_ref);
    if (!link->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create HW frame context "
               "for output.\n");
        ret = AVERROR(ENOMEM);
        goto exit;
    }

    output_frames = (AVHWFramesContext*)link->hw_frames_ctx->data;

    output_frames->format    = AV_PIX_FMT_VIDEOTOOLBOX;
    output_frames->sw_format = s->input_frames->sw_format;
    output_frames->width     = ctx->inputs[0]->w;
    output_frames->height    = ctx->inputs[0]->h;

    ret = ff_filter_init_hw_frames(ctx, link, 10);
    if (ret < 0)
        goto exit;

    ret = av_hwframe_ctx_init(link->hw_frames_ctx);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to initialise VideoToolbox frame "
               "context for output: %d\n", ret);
        goto exit;
    }

    link->time_base.num = ctx->inputs[0]->time_base.num;
    link->time_base.den = ctx->inputs[0]->time_base.den * 2;
    link->w             = ctx->inputs[0]->w;
    link->h             = ctx->inputs[0]->h;

    if(y->mode & 1)
        link->frame_rate = av_mul_q(ctx->inputs[0]->frame_rate,
                                    (AVRational){2, 1});

    if (link->w < 3 || link->h < 3) {
        av_log(ctx, AV_LOG_ERROR, "Video of less than 3 columns or lines is not supported\n");
        ret = AVERROR(EINVAL);
        goto exit;
    }

    y->csp = av_pix_fmt_desc_get(output_frames->sw_format);
    y->filter = filter;

exit:
    return ret;
}

#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
#define CONST(name, help, val, unit) { name, help, 0, AV_OPT_TYPE_CONST, {.i64=val}, INT_MIN, INT_MAX, FLAGS, unit }

static const AVOption deinterlace_videotoolbox_options[] = {
    #define OFFSET(x) offsetof(YADIFContext, x)
    { "mode",   "specify the interlacing mode", OFFSET(mode), AV_OPT_TYPE_INT, {.i64=YADIF_MODE_SEND_FRAME}, 0, 3, FLAGS, "mode"},
    CONST("send_frame",           "send one frame for each frame",                                     YADIF_MODE_SEND_FRAME,           "mode"),
    CONST("send_field",           "send one frame for each field",                                     YADIF_MODE_SEND_FIELD,           "mode"),
    CONST("send_frame_nospatial", "send one frame for each frame, but skip spatial interlacing check", YADIF_MODE_SEND_FRAME_NOSPATIAL, "mode"),
    CONST("send_field_nospatial", "send one frame for each field, but skip spatial interlacing check", YADIF_MODE_SEND_FIELD_NOSPATIAL, "mode"),

    { "parity", "specify the assumed picture field parity", OFFSET(parity), AV_OPT_TYPE_INT, {.i64=YADIF_PARITY_AUTO}, -1, 1, FLAGS, "parity" },
    CONST("tff",  "assume top field first",    YADIF_PARITY_TFF,  "parity"),
    CONST("bff",  "assume bottom field first", YADIF_PARITY_BFF,  "parity"),
    CONST("auto", "auto detect parity",        YADIF_PARITY_AUTO, "parity"),

    { "deint", "specify which frames to deinterlace", OFFSET(deint), AV_OPT_TYPE_INT, {.i64=YADIF_DEINT_ALL}, 0, 1, FLAGS, "deint" },
    CONST("all",        "deinterlace all frames",                       YADIF_DEINT_ALL,        "deint"),
    CONST("interlaced", "only deinterlace frames marked as interlaced", YADIF_DEINT_INTERLACED, "deint"),
    #undef OFFSET

    { NULL }
};

AVFILTER_DEFINE_CLASS(deinterlace_videotoolbox);

static const AVFilterPad deint_videotoolbox_inputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .filter_frame  = ff_yadif_filter_frame,
        .config_props  = config_input,
    },
};

static const AVFilterPad deint_videotoolbox_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .request_frame = ff_yadif_request_frame,
        .config_props  = config_output,
    },
};

AVFilter ff_vf_deinterlace_videotoolbox = {
    .name           = "deinterlace_videotoolbox",
    .description    = NULL_IF_CONFIG_SMALL("Deinterlace VideoToolbox frames with Metal compute"),
    .priv_size      = sizeof(DeintMetalContext),
    .priv_class     = &deinterlace_videotoolbox_class,
    .init           = deint_videotoolbox_init,
    .uninit         = deint_videotoolbox_uninit,
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_VIDEOTOOLBOX),
    FILTER_INPUTS(deint_videotoolbox_inputs),
    FILTER_OUTPUTS(deint_videotoolbox_outputs),
    .flags          = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
