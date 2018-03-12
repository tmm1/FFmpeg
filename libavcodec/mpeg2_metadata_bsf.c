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

#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"

#include "bsf.h"
#include "cbs.h"
#include "cbs_mpeg2.h"
#include "mpeg12.h"

enum {
    PASS,
    REMOVE,
    EXTRACT,
};

typedef struct MPEG2MetadataContext {
    const AVClass *class;

    CodedBitstreamContext *cbc;
    CodedBitstreamFragment fragment;

    MPEG2RawExtensionData sequence_display_extension;

    AVRational display_aspect_ratio;

    AVRational frame_rate;

    int video_format;
    int colour_primaries;
    int transfer_characteristics;
    int matrix_coefficients;

    int mpeg1_warned;
    int a53_cc;
} MPEG2MetadataContext;


static int mpeg2_metadata_update_fragment(AVBSFContext *bsf,
                                          CodedBitstreamFragment *frag)
{
    MPEG2MetadataContext             *ctx = bsf->priv_data;
    MPEG2RawSequenceHeader            *sh = NULL;
    MPEG2RawSequenceExtension         *se = NULL;
    MPEG2RawSequenceDisplayExtension *sde = NULL;
    int i, se_pos, add_sde = 0;

    for (i = 0; i < frag->nb_units; i++) {
        if (frag->units[i].type == MPEG2_START_SEQUENCE_HEADER) {
            sh = frag->units[i].content;
        } else if (frag->units[i].type == MPEG2_START_EXTENSION) {
            MPEG2RawExtensionData *ext = frag->units[i].content;
            if (ext->extension_start_code_identifier ==
                MPEG2_EXTENSION_SEQUENCE) {
                se = &ext->data.sequence;
                se_pos = i;
            } else if (ext->extension_start_code_identifier ==
                MPEG2_EXTENSION_SEQUENCE_DISPLAY) {
                sde = &ext->data.sequence_display;
            }
        }
    }

    if (!sh || !se) {
        // No sequence header and sequence extension: not an MPEG-2 video
        // sequence.
        if (sh && !ctx->mpeg1_warned) {
            av_log(bsf, AV_LOG_WARNING, "Stream contains a sequence "
                   "header but not a sequence extension: maybe it's "
                   "actually MPEG-1?\n");
            ctx->mpeg1_warned = 1;
        }
        return 0;
    }

    if (ctx->display_aspect_ratio.num && ctx->display_aspect_ratio.den) {
        int num, den;

        av_reduce(&num, &den, ctx->display_aspect_ratio.num,
                  ctx->display_aspect_ratio.den, 65535);

        if (num == 4 && den == 3)
            sh->aspect_ratio_information = 2;
        else if (num == 16 && den == 9)
            sh->aspect_ratio_information = 3;
        else if (num == 221 && den == 100)
            sh->aspect_ratio_information = 4;
        else
            sh->aspect_ratio_information = 1;
    }

    if (ctx->frame_rate.num && ctx->frame_rate.den) {
        int code, ext_n, ext_d;

        ff_mpeg12_find_best_frame_rate(ctx->frame_rate,
                                       &code, &ext_n, &ext_d, 0);

        sh->frame_rate_code        = code;
        se->frame_rate_extension_n = ext_n;
        se->frame_rate_extension_d = ext_d;
    }

    if (ctx->video_format             >= 0 ||
        ctx->colour_primaries         >= 0 ||
        ctx->transfer_characteristics >= 0 ||
        ctx->matrix_coefficients      >= 0) {
        if (!sde) {
            add_sde = 1;
            ctx->sequence_display_extension.extension_start_code =
                MPEG2_START_EXTENSION;
            ctx->sequence_display_extension.extension_start_code_identifier =
                MPEG2_EXTENSION_SEQUENCE_DISPLAY;
            sde = &ctx->sequence_display_extension.data.sequence_display;

            *sde = (MPEG2RawSequenceDisplayExtension) {
                .video_format = 5,

                .colour_description       = 0,
                .colour_primaries         = 2,
                .transfer_characteristics = 2,
                .matrix_coefficients      = 2,

                .display_horizontal_size =
                    se->horizontal_size_extension << 12 | sh->horizontal_size_value,
                .display_vertical_size =
                    se->vertical_size_extension << 12 | sh->vertical_size_value,
            };
        }

        if (ctx->video_format >= 0)
            sde->video_format = ctx->video_format;

        if (ctx->colour_primaries         >= 0 ||
            ctx->transfer_characteristics >= 0 ||
            ctx->matrix_coefficients      >= 0) {
            sde->colour_description = 1;

            if (ctx->colour_primaries >= 0)
                sde->colour_primaries = ctx->colour_primaries;
            else if (add_sde)
                sde->colour_primaries = 2;

            if (ctx->transfer_characteristics >= 0)
                sde->transfer_characteristics = ctx->transfer_characteristics;
            else if (add_sde)
                sde->transfer_characteristics = 2;

            if (ctx->matrix_coefficients >= 0)
                sde->matrix_coefficients = ctx->matrix_coefficients;
            else if (add_sde)
                sde->matrix_coefficients = 2;
        }
    }

    if (add_sde) {
        int err;

        err = ff_cbs_insert_unit_content(ctx->cbc, frag, se_pos + 1,
                                         MPEG2_START_EXTENSION,
                                         &ctx->sequence_display_extension,
                                         NULL);
        if (err < 0) {
            av_log(bsf, AV_LOG_ERROR, "Failed to insert new sequence "
                   "display extension.\n");
            return err;
        }
    }

    return 0;
}

static int mpeg2_metadata_filter(AVBSFContext *bsf, AVPacket *out)
{
    MPEG2MetadataContext *ctx = bsf->priv_data;
    AVPacket *in = NULL;
    CodedBitstreamFragment *frag = &ctx->fragment;
    int err, i;
    uint8_t *a53_side_data = NULL;
    size_t a53_side_data_size = 0;

    err = ff_bsf_get_packet(bsf, &in);
    if (err < 0)
        goto fail;

    err = ff_cbs_read_packet(ctx->cbc, frag, in);
    if (err < 0) {
        av_log(bsf, AV_LOG_ERROR, "Failed to read packet.\n");
        goto fail;
    }

    err = mpeg2_metadata_update_fragment(bsf, frag);
    if (err < 0) {
        av_log(bsf, AV_LOG_ERROR, "Failed to update frame fragment.\n");
        goto fail;
    }

    if (ctx->a53_cc == REMOVE || ctx->a53_cc == EXTRACT) {
        for (i = 0; i < frag->nb_units; i++) {
            MPEG2RawUserData *ud = NULL;
                uint32_t tag;
                uint8_t type_code, count;

            if (frag->units[i].type != MPEG2_START_USER_DATA)
                continue;
            ud = frag->units[i].content;
            if (ud->user_data_length < 6)
                continue;
            tag = AV_RB32(ud->user_data);
            type_code = ud->user_data[4];
            if (tag != MKBETAG('G', 'A', '9', '4') || type_code != 3)
                continue;

            if (ctx->a53_cc == REMOVE) {
                err = ff_cbs_delete_unit(ctx->cbc, frag, i);
                if (err < 0) {
                    av_log(bsf, AV_LOG_ERROR, "Failed to delete "
                           "A53 CC USER_DATA message.\n");
                    goto fail;
                }
                av_log(bsf, AV_LOG_TRACE, "A53 CC remove!.\n");

                --i;
                break;
            }

            // Extract.
            count = ud->user_data[5] & 0x1f;
            if (3 * count + 8 > ud->user_data_length) {
                av_log(bsf, AV_LOG_ERROR, "Invalid A/53 closed caption "
                       "data: count %d overflows length %zu.\n",
                       count, ud->user_data_length);
                continue;
            }
            av_log(bsf, AV_LOG_TRACE, "A53 CC extract: %zu bytes.\n", ud->user_data_length);

            err = av_reallocp(&a53_side_data,
                              a53_side_data_size + 3 * count);
            if (err)
                goto fail;
            memcpy(a53_side_data + a53_side_data_size,
                   ud->user_data + 7, 3 * count);
            a53_side_data_size += 3 * count;
        }
    }

    err = ff_cbs_write_packet(ctx->cbc, out, frag);
    if (err < 0) {
        av_log(bsf, AV_LOG_ERROR, "Failed to write packet.\n");
        goto fail;
    }

    err = av_packet_copy_props(out, in);
    if (err < 0) {
        av_packet_unref(out);
        goto fail;
    }

    if (a53_side_data) {
        err = av_packet_add_side_data(out, AV_PKT_DATA_A53_CC,
                                      a53_side_data, a53_side_data_size);
        if (err) {
            av_log(bsf, AV_LOG_ERROR, "Failed to attach extracted A/53 "
                   "side data to packet.\n");
            goto fail;
        }
        a53_side_data = NULL;
    }

    err = 0;
fail:
    ff_cbs_fragment_uninit(ctx->cbc, frag);
    av_freep(&a53_side_data);

    av_packet_free(&in);

    return err;
}

static int mpeg2_metadata_init(AVBSFContext *bsf)
{
    MPEG2MetadataContext *ctx = bsf->priv_data;
    CodedBitstreamFragment *frag = &ctx->fragment;
    int err;

    err = ff_cbs_init(&ctx->cbc, AV_CODEC_ID_MPEG2VIDEO, bsf);
    if (err < 0)
        return err;

    if (bsf->par_in->extradata) {
        err = ff_cbs_read_extradata(ctx->cbc, frag, bsf->par_in);
        if (err < 0) {
            av_log(bsf, AV_LOG_ERROR, "Failed to read extradata.\n");
            goto fail;
        }

        err = mpeg2_metadata_update_fragment(bsf, frag);
        if (err < 0) {
            av_log(bsf, AV_LOG_ERROR, "Failed to update metadata fragment.\n");
            goto fail;
        }

        err = ff_cbs_write_extradata(ctx->cbc, bsf->par_out, frag);
        if (err < 0) {
            av_log(bsf, AV_LOG_ERROR, "Failed to write extradata.\n");
            goto fail;
        }
    }

    err = 0;
fail:
    ff_cbs_fragment_uninit(ctx->cbc, frag);
    return err;
}

static void mpeg2_metadata_close(AVBSFContext *bsf)
{
    MPEG2MetadataContext *ctx = bsf->priv_data;
    ff_cbs_close(&ctx->cbc);
}

#define OFFSET(x) offsetof(MPEG2MetadataContext, x)
static const AVOption mpeg2_metadata_options[] = {
    { "display_aspect_ratio", "Set display aspect ratio (table 6-3)",
        OFFSET(display_aspect_ratio), AV_OPT_TYPE_RATIONAL,
        { .dbl = 0.0 }, 0, 65535 },

    { "frame_rate", "Set frame rate",
        OFFSET(frame_rate), AV_OPT_TYPE_RATIONAL,
        { .dbl = 0.0 }, 0, UINT_MAX },

    { "video_format", "Set video format (table 6-6)",
        OFFSET(video_format), AV_OPT_TYPE_INT,
        { .i64 = -1 }, -1, 7 },
    { "colour_primaries", "Set colour primaries (table 6-7)",
        OFFSET(colour_primaries), AV_OPT_TYPE_INT,
        { .i64 = -1 }, -1, 255 },
    { "transfer_characteristics", "Set transfer characteristics (table 6-8)",
        OFFSET(transfer_characteristics), AV_OPT_TYPE_INT,
        { .i64 = -1 }, -1, 255 },
    { "matrix_coefficients", "Set matrix coefficients (table 6-9)",
        OFFSET(matrix_coefficients), AV_OPT_TYPE_INT,
        { .i64 = -1 }, -1, 255 },

    { "a53_cc", "A/53 Closed Captions in SEI NAL units",
        OFFSET(a53_cc), AV_OPT_TYPE_INT,
        { .i64 = PASS }, PASS, EXTRACT, 0, "a53_cc" },
    { "pass",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = PASS    }, .unit = "a53_cc" },
    { "remove",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = REMOVE  }, .unit = "a53_cc" },
    { "extract", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = EXTRACT }, .unit = "a53_cc" },

    { NULL }
};

static const AVClass mpeg2_metadata_class = {
    .class_name = "mpeg2_metadata_bsf",
    .item_name  = av_default_item_name,
    .option     = mpeg2_metadata_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const enum AVCodecID mpeg2_metadata_codec_ids[] = {
    AV_CODEC_ID_MPEG2VIDEO, AV_CODEC_ID_NONE,
};

const AVBitStreamFilter ff_mpeg2_metadata_bsf = {
    .name           = "mpeg2_metadata",
    .priv_data_size = sizeof(MPEG2MetadataContext),
    .priv_class     = &mpeg2_metadata_class,
    .init           = &mpeg2_metadata_init,
    .close          = &mpeg2_metadata_close,
    .filter         = &mpeg2_metadata_filter,
    .codec_ids      = mpeg2_metadata_codec_ids,
};
