/*
 * Android MediaCodec Wrapper
 *
 * Copyright (c) 2015-2016 Matthieu Bouron <matthieu.bouron stupeflix.com>
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

#ifndef AVCODEC_MEDIACODEC_WRAPPER_H
#define AVCODEC_MEDIACODEC_WRAPPER_H

#include <stdint.h>
#include <sys/types.h>

#include "avcodec.h"

#define FF_MEDIACODEC_USE_NDK 0

#if FF_MEDIACODEC_USE_NDK
#include <android/native_window_jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#endif

/**
 * The following API around MediaCodec and MediaFormat is based on the
 * NDK one provided by Google since Android 5.0.
 *
 * Differences from the NDK API:
 *
 * Buffers returned by ff_AMediaFormat_toString and ff_AMediaFormat_getString
 * are newly allocated buffer and must be freed by the user after use.
 *
 * The MediaCrypto API is not implemented.
 *
 * ff_AMediaCodec_infoTryAgainLater, ff_AMediaCodec_infoOutputBuffersChanged,
 * ff_AMediaCodec_infoOutputFormatChanged, ff_AMediaCodec_cleanOutputBuffers
 * ff_AMediaCodec_getName and ff_AMediaCodec_getBufferFlagEndOfStream are not
 * part of the original NDK API and are convenience functions to hide JNI
 * implementation.
 *
 * The API around MediaCodecList is not part of the NDK (and is lacking as
 * we still need to retrieve the codec name to work around faulty decoders
 * and encoders).
 *
 * For documentation, please refers to NdkMediaCodec.h NdkMediaFormat.h and
 * http://developer.android.com/reference/android/media/MediaCodec.html.
 *
 */

int ff_Build_SDK_INT(AVCodecContext *avctx);

int ff_AMediaCodecProfile_getProfileFromAVCodecContext(AVCodecContext *avctx);

char *ff_AMediaCodecList_getCodecNameByType(const char *mime, int profile, int encoder, void *log_ctx);

#if FF_MEDIACODEC_USE_NDK

typedef AMediaFormat FFAMediaFormat;
typedef AMediaCodec FFAMediaCodec;
typedef AMediaCodecCryptoInfo FFAMediaCodecCryptoInfo;
typedef AMediaCodecBufferInfo FFAMediaCodecBufferInfo;

#define ff_AMediaFormat_new AMediaFormat_new
#define ff_AMediaFormat_delete AMediaFormat_delete

static av_always_inline char *ff_AMediaFormat_toString(FFAMediaFormat *format) {
    const char *str = AMediaFormat_toString(format);
    return av_strdup(str);
}

#define ff_AMediaFormat_getInt32 AMediaFormat_getInt32
#define ff_AMediaFormat_getInt64 AMediaFormat_getInt64
#define ff_AMediaFormat_getFloat AMediaFormat_getFloat
#define ff_AMediaFormat_getBuffer AMediaFormat_getBuffer
static av_always_inline int ff_AMediaFormat_getString(FFAMediaFormat *format, const char *name, const char **out) {
    const char *str = NULL;
    int ret = AMediaFormat_getString(format, name, &str);
    if (ret)
        *out = av_strdup(str);
    else
        *out = NULL;
    return ret;
}

#define ff_AMediaFormat_setInt32 AMediaFormat_setInt32
#define ff_AMediaFormat_setInt64 AMediaFormat_setInt64
#define ff_AMediaFormat_setFloat AMediaFormat_setFloat
#define ff_AMediaFormat_setString AMediaFormat_setString
#define ff_AMediaFormat_setBuffer AMediaFormat_setBuffer

#define ff_AMediaCodec_getName AMediaCodec_getName

#define ff_AMediaCodec_createCodecByName AMediaCodec_createCodecByName
#define ff_AMediaCodec_createDecoderByType AMediaCodec_createDecoderByType
#define ff_AMediaCodec_createEncoderByType AMediaCodec_createEncoderByType

#define ff_AMediaCodec_configure AMediaCodec_configure
#define ff_AMediaCodec_start AMediaCodec_start
#define ff_AMediaCodec_stop AMediaCodec_stop
#define ff_AMediaCodec_flush AMediaCodec_flush
#define ff_AMediaCodec_delete AMediaCodec_delete

#define ff_AMediaCodec_getInputBuffer AMediaCodec_getInputBuffer
#define ff_AMediaCodec_getOutputBuffer AMediaCodec_getOutputBuffer

#define ff_AMediaCodec_dequeueInputBuffer AMediaCodec_dequeueInputBuffer
#define ff_AMediaCodec_queueInputBuffer AMediaCodec_queueInputBuffer

#define ff_AMediaCodec_dequeueOutputBuffer AMediaCodec_dequeueOutputBuffer
#define ff_AMediaCodec_getOutputFormat AMediaCodec_getOutputFormat

#define ff_AMediaCodec_releaseOutputBuffer AMediaCodec_releaseOutputBuffer
#define ff_AMediaCodec_releaseOutputBufferAtTime AMediaCodec_releaseOutputBufferAtTime

#define ff_AMediaCodec_infoTryAgainLater(codec, index) (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
#define ff_AMediaCodec_infoOutputBuffersChanged(codec, index) (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
#define ff_AMediaCodec_infoOutputFormatChanged(codec, index) (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)

#define ff_AMediaCodec_getBufferFlagCodecConfig(codec) AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG
#define ff_AMediaCodec_getBufferFlagEndOfStream(codec) AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM
#define ff_AMediaCodec_getBufferFlagKeyFrame(codec) 1

#define ff_AMediaCodec_getConfigureFlagEncode(codec) AMEDIACODEC_CONFIGURE_FLAG_ENCODE

static av_always_inline int ff_AMediaCodec_cleanOutputBuffers(FFAMediaCodec *codec) {
    return AVERROR(EINVAL);
}

#else

struct FFAMediaFormat;
typedef struct FFAMediaFormat FFAMediaFormat;

FFAMediaFormat *ff_AMediaFormat_new(void);
int ff_AMediaFormat_delete(FFAMediaFormat* format);

char* ff_AMediaFormat_toString(FFAMediaFormat* format);

int ff_AMediaFormat_getInt32(FFAMediaFormat* format, const char *name, int32_t *out);
int ff_AMediaFormat_getInt64(FFAMediaFormat* format, const char *name, int64_t *out);
int ff_AMediaFormat_getFloat(FFAMediaFormat* format, const char *name, float *out);
int ff_AMediaFormat_getBuffer(FFAMediaFormat* format, const char *name, void** data, size_t *size);
int ff_AMediaFormat_getString(FFAMediaFormat* format, const char *name, const char **out);

void ff_AMediaFormat_setInt32(FFAMediaFormat* format, const char* name, int32_t value);
void ff_AMediaFormat_setInt64(FFAMediaFormat* format, const char* name, int64_t value);
void ff_AMediaFormat_setFloat(FFAMediaFormat* format, const char* name, float value);
void ff_AMediaFormat_setString(FFAMediaFormat* format, const char* name, const char* value);
void ff_AMediaFormat_setBuffer(FFAMediaFormat* format, const char* name, void* data, size_t size);

struct FFAMediaCodec;
typedef struct FFAMediaCodec FFAMediaCodec;
typedef struct FFAMediaCodecCryptoInfo FFAMediaCodecCryptoInfo;

struct FFAMediaCodecBufferInfo {
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
};
typedef struct FFAMediaCodecBufferInfo FFAMediaCodecBufferInfo;

char *ff_AMediaCodec_getName(FFAMediaCodec *codec);

FFAMediaCodec* ff_AMediaCodec_createCodecByName(const char *name);
FFAMediaCodec* ff_AMediaCodec_createDecoderByType(const char *mime_type);
FFAMediaCodec* ff_AMediaCodec_createEncoderByType(const char *mime_type);

int ff_AMediaCodec_configure(FFAMediaCodec* codec, const FFAMediaFormat* format, void* surface, void *crypto, uint32_t flags);
int ff_AMediaCodec_start(FFAMediaCodec* codec);
int ff_AMediaCodec_stop(FFAMediaCodec* codec);
int ff_AMediaCodec_flush(FFAMediaCodec* codec);
int ff_AMediaCodec_delete(FFAMediaCodec* codec);

uint8_t* ff_AMediaCodec_getInputBuffer(FFAMediaCodec* codec, size_t idx, size_t *out_size);
uint8_t* ff_AMediaCodec_getOutputBuffer(FFAMediaCodec* codec, size_t idx, size_t *out_size);

ssize_t ff_AMediaCodec_dequeueInputBuffer(FFAMediaCodec* codec, int64_t timeoutUs);
int ff_AMediaCodec_queueInputBuffer(FFAMediaCodec* codec, size_t idx, off_t offset, size_t size, uint64_t time, uint32_t flags);

ssize_t ff_AMediaCodec_dequeueOutputBuffer(FFAMediaCodec* codec, FFAMediaCodecBufferInfo *info, int64_t timeoutUs);
FFAMediaFormat* ff_AMediaCodec_getOutputFormat(FFAMediaCodec* codec);

int ff_AMediaCodec_releaseOutputBuffer(FFAMediaCodec* codec, size_t idx, int render);
int ff_AMediaCodec_releaseOutputBufferAtTime(FFAMediaCodec *codec, size_t idx, int64_t timestampNs);

int ff_AMediaCodec_infoTryAgainLater(FFAMediaCodec *codec, ssize_t idx);
int ff_AMediaCodec_infoOutputBuffersChanged(FFAMediaCodec *codec, ssize_t idx);
int ff_AMediaCodec_infoOutputFormatChanged(FFAMediaCodec *codec, ssize_t indx);

int ff_AMediaCodec_getBufferFlagCodecConfig (FFAMediaCodec *codec);
int ff_AMediaCodec_getBufferFlagEndOfStream(FFAMediaCodec *codec);
int ff_AMediaCodec_getBufferFlagKeyFrame(FFAMediaCodec *codec);

int ff_AMediaCodec_getConfigureFlagEncode(FFAMediaCodec *codec);

int ff_AMediaCodec_cleanOutputBuffers(FFAMediaCodec *codec);

#endif /* FF_MEDIACODEC_USE_NDK */

#endif /* AVCODEC_MEDIACODEC_WRAPPER_H */
