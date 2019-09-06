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
function ff_merge8_armv6, export=1
        push            {r4-r9,lr}
1:
        pld             [src1, #64]
        ldm             src1!, {r4-r5}
        pld             [src2, #64]
        ldm             src2!, {r8-r9}
        subs            size, size, #16
        uhadd8          r4, r4, r8
        ldm             src1!, {r6-r7}
        uhadd8          r5, r5, r9
        ldm             src2!, {ip,lr}
        uhadd8          r6, r6, ip
        stm             dest!, {r4-r5}
        uhadd8          r7, r7, lr
        stm             dest!, {r6-r7}
        it              eq
        popeq           {r4-r9,pc}
        b               1b
endfunc

        .align 2
function ff_merge16_armv6, export=1
        push            {r4-r9,lr}
1:
        pld             [src1, #64]
        ldm             src1!, {r4-r5}
        pld             [src2, #64]
        ldm             src2!, {r8-r9}
        subs            size, size, #16
        uhadd16         r4, r4, r8
        ldm             src1!, {r6-r7}
        uhadd16         r5, r5, r9
        ldm             src2!, {ip,lr}
        uhadd16         r6, r6, ip
        stm             dest!, {r4-r5}
        uhadd16         r7, r7, lr
        stm             dest!, {r6-r7}
        it              eq
        popeq           {r4-r9,pc}
        b               1b
endfunc