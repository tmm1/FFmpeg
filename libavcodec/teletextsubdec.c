/*
 * Minimal Teletext subtitle decoding for ffmpeg
 * Copyright (c) 2018 Aman Gupta
 * Copyright (c) 2013 Marton Balint
 * Copyright (c) 2005-2010, 2012 Wolfram Gloger
 * Copyright (c) 2007 Vincent Penne, VLC authors and VideoLAN (modules/codec/telx.c)
 * Copyright (c) 2001-2005 dvb.matt, ProjectX java dvb decoder
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
#include "avcodec.h"
#include "libavcodec/ass.h"
#include "libavcodec/dvbtxt.h"
#include "libavutil/opt.h"
#include "libavutil/bprint.h"
#include "libavutil/internal.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/reverse.h"

#define NUM_ROWS 24
#define ROW_SIZE 40
#define NUM_MAGAZINES 9

typedef struct TeletextContext
{
    AVClass *class;
    int64_t pts;
    AVBPrint buffer;
    int readorder;

    uint8_t rows[NUM_ROWS][ROW_SIZE];
    bool active[NUM_MAGAZINES];
    int charset;
} TeletextContext;

/*
 * My doc only mentions 13 national characters, but experiments show there
 * are more, in france for example I already found two more (0x9 and 0xb).
 *
 * Conversion is in this order :
 *
 * 0x23 0x24 0x40 0x5b 0x5c 0x5d 0x5e 0x5f 0x60 0x7b 0x7c 0x7d 0x7e
 * (these are the standard ones)
 * 0x08 0x09 0x0a 0x0b 0x0c 0x0d (apparently a control character) 0x0e 0x0f
 */
static const uint16_t national_charsets[][20] = {
    { 0x00a3, 0x0024, 0x0040, 0x00ab, 0x00bd, 0x00bb, 0x005e, 0x0023,
      0x002d, 0x00bc, 0x00a6, 0x00be, 0x00f7 }, /* english ,000 */

    { 0x00e9, 0x00ef, 0x00e0, 0x00eb, 0x00ea, 0x00f9, 0x00ee, 0x0023,
      0x00e8, 0x00e2, 0x00f4, 0x00fb, 0x00e7, 0, 0x00eb, 0, 0x00ef }, /* french  ,001 */

    { 0x0023, 0x00a4, 0x00c9, 0x00c4, 0x00d6, 0x00c5, 0x00dc, 0x005f,
      0x00e9, 0x00e4, 0x00f6, 0x00e5, 0x00fc }, /* swedish,finnish,hungarian ,010 */

    { 0x0023, 0x016f, 0x010d, 0x0165, 0x017e, 0x00fd, 0x00ed, 0x0159,
      0x00e9, 0x00e1, 0x011b, 0x00fa, 0x0161 }, /* czech,slovak  ,011 */

    { 0x0023, 0x0024, 0x00a7, 0x00c4, 0x00d6, 0x00dc, 0x005e, 0x005f,
      0x00b0, 0x00e4, 0x00f6, 0x00fc, 0x00df }, /* german ,100 */

    { 0x00e7, 0x0024, 0x00a1, 0x00e1, 0x00e9, 0x00ed, 0x00f3, 0x00fa,
      0x00bf, 0x00fc, 0x00f1, 0x00e8, 0x00e0 }, /* portuguese,spanish ,101 */

    { 0x00a3, 0x0024, 0x00e9, 0x00b0, 0x00e7, 0x00bb, 0x005e, 0x0023,
      0x00f9, 0x00e0, 0x00f2, 0x00e8, 0x00ec }, /* italian  ,110 */

    { 0x0023, 0x00a4, 0x0162, 0x00c2, 0x015e, 0x0102, 0x00ce, 0x0131,
      0x0163, 0x00e2, 0x015f, 0x0103, 0x00ee }, /* rumanian ,111 */

    /* I have these tables too, but I don't know how they can be triggered */
    { 0x0023, 0x0024, 0x0160, 0x0117, 0x0119, 0x017d, 0x010d, 0x016b,
      0x0161, 0x0105, 0x0173, 0x017e, 0x012f }, /* lettish,lithuanian ,1000 */

    { 0x0023, 0x0144, 0x0105, 0x005a, 0x015a, 0x0141, 0x0107, 0x00f3,
      0x0119, 0x017c, 0x015b, 0x0142, 0x017a }, /* polish,  1001 */

    { 0x0023, 0x00cb, 0x010c, 0x0106, 0x017d, 0x0110, 0x0160, 0x00eb,
      0x010d, 0x0107, 0x017e, 0x0111, 0x0161 }, /* serbian,croatian,slovenian, 1010 */

    { 0x0023, 0x00f5, 0x0160, 0x00c4, 0x00d6, 0x017e, 0x00dc, 0x00d5,
      0x0161, 0x00e4, 0x00f6, 0x017e, 0x00fc }, /* estonian  ,1011 */

    { 0x0054, 0x011f, 0x0130, 0x015e, 0x00d6, 0x00c7, 0x00dc, 0x011e,
      0x0131, 0x015f, 0x00f6, 0x00e7, 0x00fc }, /* turkish  ,1100 */
};

static const char *color_mappings[8] = {
    "{\\c&H000000&}", // black
    "{\\c&H0000FF&}", // red
    "{\\c&H00FF00&}", // green
    "{\\c&H00FFFF&}", // yellow
    "{\\c&HFF0000&}", // blue
    "{\\c&HFF00FF&}", // magenta
    "{\\c&HFFFF00&}", // cyan
    "{\\c&HFFFFFF&}"  // white
};

static int hamming(int a)
{
    switch (a) {
    case 0xA8: return 0;
    case 0x0B: return 1;
    case 0x26: return 2;
    case 0x85: return 3;
    case 0x92: return 4;
    case 0x31: return 5;
    case 0x1C: return 6;
    case 0xBF: return 7;
    case 0x40: return 8;
    case 0xE3: return 9;
    case 0xCE: return 10;
    case 0x6D: return 11;
    case 0x7A: return 12;
    case 0xD9: return 13;
    case 0xF4: return 14;
    case 0x57: return 15;
    default:   return -1; // decoding error , not yet corrected
    }
}

// ucs-2 --> utf-8
// this is not a general function, but it's enough for what we do here
// the result buffer need to be at least 4 bytes long
static void to_utf8(char *res, uint16_t ch)
{
    if (ch >= 0x80) {
        if (ch >= 0x800) {
            res[0] = (ch >> 12) | 0xE0;
            res[1] = ((ch >> 6) & 0x3F) | 0x80;
            res[2] = (ch & 0x3F) | 0x80;
            res[3] = 0;
        } else {
            res[0] = (ch >> 6) | 0xC0;
            res[1] = (ch & 0x3F) | 0x80;
            res[2] = 0;
        }
    } else {
        res[0] = ch;
        res[1] = 0;
    }
}

static void decode_string(AVCodecContext *avctx, AVBPrint *buf,
                          const uint8_t *packet, int *leading, int *olen)
{
    TeletextContext *ctx = avctx->priv_data;
    int i, len = 0;
    int end_box = 0;
    bool char_seen = false;
    char utf8[7];
    const uint16_t *charset = national_charsets[ctx->charset];
    av_bprint_clear(buf);
    *leading = 0;

    for (i = 0; i < ROW_SIZE; i++) {
        int in = ff_reverse[packet[i]] & 0x7f;
        uint16_t out = 32;

        switch (in) {
        /* special national characters */
        case 0x23: out = charset[0]; break;
        case 0x24: out = charset[1]; break;
        case 0x40: out = charset[2]; break;
        case 0x5b: out = charset[3]; break;
        case 0x5c: out = charset[4]; break;
        case 0x5d: out = charset[5]; break;
        case 0x5e: out = charset[6]; break;
        case 0x5f: out = charset[7]; break;
        case 0x60: out = charset[8]; break;
        case 0x7b: out = charset[9]; break;
        case 0x7c: out = charset[10]; break;
        case 0x7d: out = charset[11]; break;
        case 0x7e: out = charset[12]; break;

        /* control codes */
        case 0xd:
            i++;
            in = ff_reverse[packet[i]] & 0x7f;
            if (in == 0xb)
                in = 7;
            if (in >= 0 && in < 8) {
                av_bprintf(buf, "%s", color_mappings[in]);
            }
            continue;

        case 0xa:
            end_box++;
            if (end_box == 1)
                continue;
            break;

        case 0xb:
            continue;

        /* color codes */
        case 0x0:
        case 0x1:
        case 0x2:
        case 0x3:
        case 0x4:
        case 0x5:
        case 0x6:
        case 0x7:
            av_bprintf(buf, "%s", color_mappings[in]);
            break;

        default:
            /* non documented national range 0x08 - 0x0f */
            if (in >= 0x08 && in <= 0x0f) {
                out = charset[13 + in - 8];
                break;
            }

            /* normal ascii */
            if (in >= 32 && in < 0x7f)
                out = in;
        }

        if (end_box == 2)
            break;

        /* handle undefined national characters */
        if (out == 0)
            out = 32;

        if (out == 32 && !char_seen)
            (*leading)++;
        else if (out != 32)
            char_seen = true;

        /* convert to utf-8 */
        to_utf8(utf8, out);
        av_bprintf(buf, "%s", utf8);

        if (char_seen || out != 32)
            len += strlen(utf8);
    }

    /* remove trailing spaces */
    for (i = buf->len-1; i >= 0 && buf->str[i] == 32; i--)
        ;
    buf->str[i+1] = 0;
    buf->len = i;

    *olen = len;
}

static int teletext_init_decoder(AVCodecContext *avctx)
{
    TeletextContext *ctx = avctx->priv_data;
    av_bprint_init(&ctx->buffer, 0, AV_BPRINT_SIZE_UNLIMITED);
    ctx->pts = AV_NOPTS_VALUE;
    return ff_ass_subtitle_header(avctx,
        "Monospace",
        ASS_DEFAULT_FONT_SIZE,
        ASS_DEFAULT_COLOR,
        ASS_DEFAULT_BACK_COLOR,
        ASS_DEFAULT_BOLD,
        ASS_DEFAULT_ITALIC,
        ASS_DEFAULT_UNDERLINE,
        3,
        ASS_DEFAULT_ALIGNMENT);
}

static int teletext_close_decoder(AVCodecContext *avctx)
{
    TeletextContext *ctx = avctx->priv_data;
    ctx->pts = AV_NOPTS_VALUE;
    av_bprint_finalize(&ctx->buffer, NULL);
    return 0;
}

static void teletext_flush(AVCodecContext *avctx)
{
    TeletextContext *ctx = avctx->priv_data;
    int i;
    ctx->pts = AV_NOPTS_VALUE;
    if (!(avctx->flags2 & AV_CODEC_FLAG2_RO_FLUSH_NOOP))
        ctx->readorder = 0;
    for (i = 0; i < NUM_ROWS; i++)
        ctx->rows[i][0] = 0;
    av_bprint_clear(&ctx->buffer);
}

static int capture_screen(AVCodecContext *avctx)
{
    TeletextContext *ctx = avctx->priv_data;
    int i, j;
    int tab = 0;
    int num_rows = 0;
    bool align_center = false, align_right = false;
    bool maybe_center = false;
    char prev_line[128] = {0};
    AVBPrint line;
    av_bprint_init(&line, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprint_clear(&ctx->buffer);

    for (i = 0; i < NUM_ROWS; i++) {
        int leading, len;
        int spaces = 0;
        const uint8_t *row = ctx->rows[i];
        if (row[0] == 0)
            continue;
        num_rows++;

        av_bprint_clear(&line);
        decode_string(avctx, &line, row, &leading, &len);

        // av_log(avctx, AV_LOG_DEBUG, "line[%d]: '%s' [leading=%d, len=%d]\n",
        //        i, line.str, leading, len);

        for (j = 0; j < line.len; j++) {
            if (line.str[j] == ' ')
                spaces++;
            else
                break;
        }

        if (!tab || spaces < tab)
            tab = spaces;

        if (leading > 0 && leading + len > 35)
            align_right = true;
        else if (leading > 0 && leading*2 + len <= 36)
            align_center = true;
        else if (leading == 0 && len == 34)
            maybe_center = true;
    }

    if (num_rows == 1 && maybe_center)
        align_center = true;

    for (i = 0; i < NUM_ROWS; i++) {
        int leading, len;
        int x, y, alignment = 7;
        const uint8_t *row = ctx->rows[i];
        bool char_seen = false;
        if (row[0] == 0)
             continue;

        av_bprint_clear(&line);
        decode_string(avctx, &line, row, &leading, &len);

        len = FFMIN(sizeof(prev_line), line.len+1);
        if (strncmp(prev_line, line.str, len) == 0)
            continue;
        strncpy(prev_line, line.str, len);

        /* skip leading space */
        j = 0;
        while (line.str[j] == ' ' && j < tab)
            j++;

        x = ASS_DEFAULT_PLAYRESX * (0.1 + (0.80 / 34) * j);
        y = ASS_DEFAULT_PLAYRESY * (0.1 + (0.80 / 25) * i);

        if (align_center) {
            x = ASS_DEFAULT_PLAYRESX * 0.5;
            alignment = 8;
        } else if (align_right) {
            x = ASS_DEFAULT_PLAYRESX * 0.9;
            alignment = 9;
        }

        av_bprintf(&ctx->buffer, "{\\an%d}{\\pos(%d,%d)}", alignment, x, y);
        for (; j <= line.len; j++) {
            if (line.str[j] == 32 && !char_seen && !align_center && !align_right)
                av_bprintf(&ctx->buffer, "\\h");
            else {
                av_bprintf(&ctx->buffer, "%c", line.str[j]);
                char_seen = true;
            }
        }
        av_bprintf(&ctx->buffer, "\\N");
    }

    av_bprint_finalize(&line, NULL);
    if (!av_bprint_is_complete(&ctx->buffer))
        return AVERROR(ENOMEM);
    return 0;
}

static int teletext_decode_frame(AVCodecContext *avctx, void *data, int *got_sub, AVPacket *pkt)
{
    TeletextContext *ctx = avctx->priv_data;
    AVSubtitle *sub = data;
    int ret = 0;
    int offset = 0;
    bool updated = false, erased = false;

    if (avctx->pkt_timebase.num && pkt->pts != AV_NOPTS_VALUE)
        ctx->pts = av_rescale_q(pkt->pts, avctx->pkt_timebase, AV_TIME_BASE_Q);

    if (pkt->size) {
        const int full_pes_size = pkt->size + 45; /* PES header is 45 bytes */

        // We allow unreasonably big packets, even if the standard only allows a max size of 1472
        if (full_pes_size < 184 || full_pes_size > 65504 || full_pes_size % 184 != 0)
            return AVERROR_INVALIDDATA;

        if (!ff_data_identifier_is_teletext(*pkt->data))
            return pkt->size;

        for (offset = 1; offset + 46 <= pkt->size; offset += 46) {
            int mpag, row, magazine;
            uint8_t *packet = pkt->data + offset;
            if (packet[0] == 0xFF)
                continue;

            mpag = (hamming(packet[4]) << 4) | hamming(packet[5]);
            if (mpag < 0)
                continue;

            row = 0xFF & ff_reverse[mpag];
            magazine = 7 & row;
            if (magazine == 0)
                magazine = 8;
            row >>= 3;

            if (row == 0) { // row 0: flags and header line
                int flag = 0;
                int i, page, f_charset;
                bool f_erase, f_subtitle, f_inhibit, f_suppress, f_update, f_news;

                for (i = 0; i < 6; i++)
                    flag |= (0xF & (ff_reverse[hamming(packet[8+i])] >> 4)) << (4*i);

                page = (0xF0 &  ff_reverse[hamming(packet[7])]      ) |
                       (0x0F & (ff_reverse[hamming(packet[6])] >> 4));

                f_erase    = 1 & (flag>>7);
                f_news     = 1 & (flag>>15);
                f_subtitle = 1 & (flag>>15);
                f_suppress = 1 & (flag>>16);
                f_update   = 1 & (flag>>17);
                f_inhibit  = 1 & (flag>>19);
                f_charset  = 7 & (flag>>21);

                ctx->active[magazine] = f_subtitle;
                if (!ctx->active[magazine])
                    continue;

                ctx->charset = f_charset;

                // av_log(avctx, AV_LOG_DEBUG, "magazine=%d page=%d erase=%d subtitle=%d news=%d charset=%d inhibit=%d suppress=%d update=%d\n",
                //       magazine, page, f_erase, f_subtitle, f_news, f_charset, f_inhibit, f_suppress, f_update);

                if (f_erase) {
                    for (i = 0; i < NUM_ROWS; i++)
                        ctx->rows[i][0] = 0;
                    erased = true;
                    continue;
                }

            } else if (row < 24) { // row 1-23: normal lines
                if (!ctx->active[magazine])
                    continue;
                if (memcmp(ctx->rows[row], packet+6, ROW_SIZE) != 0) {
                    updated |= true;
                    memcpy(ctx->rows[row], packet+6, ROW_SIZE);
                }

                // int j;
                // av_log(avctx, AV_LOG_DEBUG, "row[%d]: ", row);
                // for (j = 0; j < ROW_SIZE; j++) {
                //     int in = ff_reverse[ctx->rows[row][j]] & 0x7f;
                //     av_log(avctx, AV_LOG_DEBUG, "0x%x ", in);
                // }
                // av_log(avctx, AV_LOG_DEBUG, "\n");
            }
        }
    }

    if (updated || (erased && ctx->buffer.len)) {
        ret = capture_screen(avctx);
        if (ret < 0)
            return ret;
        ret = ff_ass_add_rect(sub, ctx->buffer.str, ctx->readorder++, 0, NULL, NULL);
        if (ret < 0)
            return ret;
        sub->end_display_time = -1;
    }

    *got_sub = sub->num_rects > 0;
    return offset;
}

static const AVClass teletext_class = {
    .class_name = "teletextsub",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_teletextsub_decoder = {
    .name      = "teletext_subtitle",
    .long_name = NULL_IF_CONFIG_SMALL("Minimal DVB teletext subtitle decoder"),
    .type      = AVMEDIA_TYPE_SUBTITLE,
    .id        = AV_CODEC_ID_DVB_TELETEXT,
    .priv_data_size = sizeof(TeletextContext),
    .init      = teletext_init_decoder,
    .close     = teletext_close_decoder,
    .decode    = teletext_decode_frame,
    .capabilities = AV_CODEC_CAP_DELAY,
    .flush     = teletext_flush,
    .priv_class= &teletext_class,
};
