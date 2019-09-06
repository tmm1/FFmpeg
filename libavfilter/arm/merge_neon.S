/*
 * Copyright (c) 2009-2012 Rémi Denis-Courmont, VLC authors
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

#include "libavutil/arm/asm.S"

#define dest r0
#define src1 r1
#define src2 r2
#define size r3

        .align 2
        @ NOTE: Offset and pitch must be multiple of 16-bytes.
function ff_merge8_neon, export=1
        cmp             size, #64
        blo             2f
1:
        pld             [src1, #64]
        vld1.u8         {q0-q1}, [src1,:128]!
        pld             [src2, #64]
        vld1.u8         {q8-q9}, [src2,:128]!
        vhadd.u8        q0, q0, q8
        sub             size, size, #64
        vld1.u8         {q2-q3}, [src1,:128]!
        vhadd.u8        q1, q1, q9
        vld1.u8         {q10-q11}, [src2,:128]!
        vhadd.u8        q2, q2, q10
        cmp             size, #64
        vhadd.u8        q3, q3, q11
        vst1.u8         {q0-q1}, [dest,:128]!
        vst1.u8         {q2-q3}, [dest,:128]!
        bhs             1b
2:
        cmp             size, #32
        blo             3f
        vld1.u8         {q0-q1}, [src1,:128]!
        sub             size, size, #32
        vld1.u8         {q8-q9}, [src2,:128]!
        vhadd.u8        q0, q0, q8
        vhadd.u8        q1, q1, q9
        vst1.u8         {q0-q1}, [dest,:128]!
3:
        cmp             size, #16
        it              lo
        bxlo            lr
        vld1.u8         {q0}, [src1,:128]!
        sub             size, size, #16
        vld1.u8         {q8}, [src2,:128]!
        vhadd.u8        q0, q0, q8
        vst1.u8         {q0}, [dest,:128]!
        bx              lr
endfunc

        .align 2
function ff_merge16_neon, export=1
        cmp             size, #64
        blo             2f
1:
        pld             [src1, #64]
        vld1.u16        {q0-q1}, [src1,:128]!
        pld             [src2, #64]
        vld1.u16        {q8-q9}, [src2,:128]!
        vhadd.u16       q0, q0, q8
        sub             size, size, #64
        vld1.u16        {q2-q3}, [src1,:128]!
        vhadd.u16       q1, q1, q9
        vld1.u16        {q10-q11}, [src2,:128]!
        vhadd.u16       q2, q2, q10
        cmp             size, #64
        vhadd.u16       q3, q3, q11
        vst1.u16        {q0-q1}, [dest,:128]!
        vst1.u16        {q2-q3}, [dest,:128]!
        bhs             1b
2:
        cmp             size, #32
        blo             3f
        vld1.u16        {q0-q1}, [src1,:128]!
        sub             size, size, #32
        vld1.u16        {q8-q9}, [src2,:128]!
        vhadd.u16       q0, q0, q8
        vhadd.u16       q1, q1, q9
        vst1.u16        {q0-q1}, [dest,:128]!
3:
        cmp             size, #16
        it              lo
        bxlo            lr
        vld1.u16        {q0}, [src1,:128]!
        sub             size, size, #16
        vld1.u16        {q8}, [src2,:128]!
        vhadd.u16       q0, q0, q8
        vst1.u16        {q0}, [dest,:128]!
        bx              lr
endfunc