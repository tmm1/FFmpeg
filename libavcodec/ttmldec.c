/*
 * Minimal TTML subtitle decoding for ffmpeg
 * Copyright (c) 2021 Aman Gupta
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdbool.h>
#include <libxml/parser.h>
#include "avcodec.h"
#include "libavcodec/ass.h"
#include "libavcodec/dvbtxt.h"
#include "libavutil/opt.h"
#include "libavutil/bprint.h"
#include "libavutil/internal.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/reverse.h"

typedef struct TTMLContext
{
    AVClass *class;
} TTMLContext;

static const AVClass ttml_class = {
    .class_name = "ttml",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_ttml_decoder = {
    .name      = "ttml",
    .long_name = NULL_IF_CONFIG_SMALL("Minimal TTML subtitle decoder"),
    .type      = AVMEDIA_TYPE_SUBTITLE,
    .id        = AV_CODEC_ID_TTML,
    .priv_data_size = sizeof(TTMLContext),
    //.init      = ttml_init_decoder,
    //.close     = ttml_close_decoder,
    //.decode    = ttml_decode_frame,
    //.flush     = ttml_flush,
    .capabilities = AV_CODEC_CAP_DELAY,
    .priv_class= &ttml_class,
};
