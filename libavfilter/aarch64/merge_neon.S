/*
 * Copyright (c) 2009-2016 Rémi Denis-Courmont, Janne Grunau, VLC authors
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

#include "libavutil/aarch64/asm.S"

#define dest x0
#define src1 x1
#define src2 x2
#define size x3

        .align 2
        // NOTE: Offset and pitch must be multiple of 16-bytes.
function ff_merge8_neon, export=1
        ands            x5, size, #~63
        b.eq            2f
        mov             x10, #64
        add             x11, src1, #32
        add             x12, src2, #32
1:
        ld1             {v0.16b,v1.16b}, [src1], x10
        ld1             {v4.16b,v5.16b}, [src2], x10
        ld1             {v2.16b,v3.16b}, [x11], x10
        uhadd           v0.16b, v0.16b, v4.16b
        ld1             {v6.16b,v7.16b}, [x12], x10
        subs            x5, x5, #64
        uhadd           v1.16b, v1.16b, v5.16b
        uhadd           v2.16b, v2.16b, v6.16b
        uhadd           v3.16b, v3.16b, v7.16b
        st1             {v0.16b,v1.16b}, [dest], #32
        st1             {v2.16b,v3.16b}, [dest], #32
        b.gt            1b
2:
        tbz             size, #5,  3f
        ld1             {v0.16b,v1.16b}, [src1], #32
        ld1             {v4.16b,v5.16b}, [src2], #32
        uhadd           v0.16b, v0.16b, v4.16b
        uhadd           v1.16b, v1.16b, v5.16b
        st1             {v0.16b,v1.16b}, [dest], #32
3:
        tbz             size, #4, 4f
        ld1             {v0.16b}, [src1]
        ld1             {v4.16b}, [src2]
        uhadd           v0.16b, v0.16b, v4.16b
        st1             {v0.16b}, [dest]
4:
        ret
endfunc

        .align 2
function ff_merge16_neon, export=1
        ands            x5, size, #~63
        b.eq            2f
1:
        ld1             {v0.8h,v1.8h}, [src1], #32
        ld1             {v4.8h,v5.8h}, [src2], #32
        ld1             {v2.8h,v3.8h}, [src1], #32
        uhadd           v0.8h, v0.8h, v4.8h
        ld1             {v6.8h,v7.8h}, [src2], #32
        uhadd           v1.8h, v1.8h, v5.8h
        uhadd           v2.8h, v2.8h, v6.8h
        uhadd           v3.8h, v3.8h, v7.8h
        st1             {v0.8h,v1.8h}, [dest], #32
        st1             {v2.8h,v3.8h}, [dest], #32
        subs            x5, x5, #64
        b.gt            1b
2:
        tbz             size, #5, 3f
        ld1             {v0.8h,v1.8h}, [src1], #32
        ld1             {v4.8h,v5.8h}, [src2], #32
        uhadd           v0.8h, v0.8h, v4.8h
        uhadd           v1.8h, v1.8h, v5.8h
        st1             {v0.8h,v1.8h}, [dest], #32
3:
        tbz             size, #4,  4f
        ld1             {v0.8h}, [src1]
        ld1             {v4.8h}, [src2]
        uhadd           v0.8h, v0.8h,v4.8h
        st1             {v0.8h}, [dest]
4:
        ret
endfunc
