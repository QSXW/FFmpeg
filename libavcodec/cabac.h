/*
 * H.26L/H.264/AVC/JVT/14496-10/... encoder/decoder
 * Copyright (c) 2003 Michael Niedermayer <michaelni@gmx.at>
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Context Adaptive Binary Arithmetic Coder.
 */

#ifndef AVCODEC_CABAC_H
#define AVCODEC_CABAC_H

#include <stddef.h>

#include "put_bits.h"

//#undef NDEBUG
#include <assert.h>
#include "libavutil/x86_cpu.h"

#define CABAC_BITS 16
#define CABAC_MASK ((1<<CABAC_BITS)-1)

typedef struct CABACContext{
    int low;
    int range;
    int outstanding_count;
#ifdef STRICT_LIMITS
    int symCount;
#endif
    const uint8_t *bytestream_start;
    const uint8_t *bytestream;
    const uint8_t *bytestream_end;
    PutBitContext pb;
}CABACContext;

extern uint8_t ff_h264_mlps_state[4*64];
extern uint8_t ff_h264_lps_range[4*2*64];  ///< rangeTabLPS
extern uint8_t ff_h264_mps_state[2*64];     ///< transIdxMPS
extern uint8_t ff_h264_lps_state[2*64];     ///< transIdxLPS
extern const uint8_t ff_h264_norm_shift[512];


void ff_init_cabac_encoder(CABACContext *c, uint8_t *buf, int buf_size);
void ff_init_cabac_decoder(CABACContext *c, const uint8_t *buf, int buf_size);
void ff_init_cabac_states(CABACContext *c);


static inline void put_cabac_bit(CABACContext *c, int b){
    put_bits(&c->pb, 1, b);
    for(;c->outstanding_count; c->outstanding_count--){
        put_bits(&c->pb, 1, 1-b);
    }
}

static inline void renorm_cabac_encoder(CABACContext *c){
    while(c->range < 0x100){
        //FIXME optimize
        if(c->low<0x100){
            put_cabac_bit(c, 0);
        }else if(c->low<0x200){
            c->outstanding_count++;
            c->low -= 0x100;
        }else{
            put_cabac_bit(c, 1);
            c->low -= 0x200;
        }

        c->range+= c->range;
        c->low += c->low;
    }
}

#ifdef TEST
static void put_cabac(CABACContext *c, uint8_t * const state, int bit){
    int RangeLPS= ff_h264_lps_range[2*(c->range&0xC0) + *state];

    if(bit == ((*state)&1)){
        c->range -= RangeLPS;
        *state= ff_h264_mps_state[*state];
    }else{
        c->low += c->range - RangeLPS;
        c->range = RangeLPS;
        *state= ff_h264_lps_state[*state];
    }

    renorm_cabac_encoder(c);

#ifdef STRICT_LIMITS
    c->symCount++;
#endif
}

static void put_cabac_static(CABACContext *c, int RangeLPS, int bit){
    assert(c->range > RangeLPS);

    if(!bit){
        c->range -= RangeLPS;
    }else{
        c->low += c->range - RangeLPS;
        c->range = RangeLPS;
    }

    renorm_cabac_encoder(c);

#ifdef STRICT_LIMITS
    c->symCount++;
#endif
}

/**
 * @param bit 0 -> write zero bit, !=0 write one bit
 */
static void put_cabac_bypass(CABACContext *c, int bit){
    c->low += c->low;

    if(bit){
        c->low += c->range;
    }
//FIXME optimize
    if(c->low<0x200){
        put_cabac_bit(c, 0);
    }else if(c->low<0x400){
        c->outstanding_count++;
        c->low -= 0x200;
    }else{
        put_cabac_bit(c, 1);
        c->low -= 0x400;
    }

#ifdef STRICT_LIMITS
    c->symCount++;
#endif
}

/**
 *
 * @return the number of bytes written
 */
static int put_cabac_terminate(CABACContext *c, int bit){
    c->range -= 2;

    if(!bit){
        renorm_cabac_encoder(c);
    }else{
        c->low += c->range;
        c->range= 2;

        renorm_cabac_encoder(c);

        assert(c->low <= 0x1FF);
        put_cabac_bit(c, c->low>>9);
        put_bits(&c->pb, 2, ((c->low>>7)&3)|1);

        flush_put_bits(&c->pb); //FIXME FIXME FIXME XXX wrong
    }

#ifdef STRICT_LIMITS
    c->symCount++;
#endif

    return (put_bits_count(&c->pb)+7)>>3;
}

/**
 * put (truncated) unary binarization.
 */
static void put_cabac_u(CABACContext *c, uint8_t * state, int v, int max, int max_index, int truncated){
    int i;

    assert(v <= max);

#if 1
    for(i=0; i<v; i++){
        put_cabac(c, state, 1);
        if(i < max_index) state++;
    }
    if(truncated==0 || v<max)
        put_cabac(c, state, 0);
#else
    if(v <= max_index){
        for(i=0; i<v; i++){
            put_cabac(c, state+i, 1);
        }
        if(truncated==0 || v<max)
            put_cabac(c, state+i, 0);
    }else{
        for(i=0; i<=max_index; i++){
            put_cabac(c, state+i, 1);
        }
        for(; i<v; i++){
            put_cabac(c, state+max_index, 1);
        }
        if(truncated==0 || v<max)
            put_cabac(c, state+max_index, 0);
    }
#endif
}

/**
 * put unary exp golomb k-th order binarization.
 */
static void put_cabac_ueg(CABACContext *c, uint8_t * state, int v, int max, int is_signed, int k, int max_index){
    int i;

    if(v==0)
        put_cabac(c, state, 0);
    else{
        const int sign= v < 0;

        if(is_signed) v= FFABS(v);

        if(v<max){
            for(i=0; i<v; i++){
                put_cabac(c, state, 1);
                if(i < max_index) state++;
            }

            put_cabac(c, state, 0);
        }else{
            int m= 1<<k;

            for(i=0; i<max; i++){
                put_cabac(c, state, 1);
                if(i < max_index) state++;
            }

            v -= max;
            while(v >= m){ //FIXME optimize
                put_cabac_bypass(c, 1);
                v-= m;
                m+= m;
            }
            put_cabac_bypass(c, 0);
            while(m>>=1){
                put_cabac_bypass(c, v&m);
            }
        }

        if(is_signed)
            put_cabac_bypass(c, sign);
    }
}
#endif /* TEST */

static void refill(CABACContext *c){
#if CABAC_BITS == 16
        c->low+= (c->bytestream[0]<<9) + (c->bytestream[1]<<1);
#else
        c->low+= c->bytestream[0]<<1;
#endif
    c->low -= CABAC_MASK;
    c->bytestream+= CABAC_BITS/8;
}

#if ! ( ARCH_X86 && HAVE_7REGS && HAVE_EBX_AVAILABLE && !defined(BROKEN_RELOCATIONS) )
static void refill2(CABACContext *c){
    int i, x;

    x= c->low ^ (c->low-1);
    i= 7 - ff_h264_norm_shift[x>>(CABAC_BITS-1)];

    x= -CABAC_MASK;

#if CABAC_BITS == 16
        x+= (c->bytestream[0]<<9) + (c->bytestream[1]<<1);
#else
        x+= c->bytestream[0]<<1;
#endif

    c->low += x<<i;
    c->bytestream+= CABAC_BITS/8;
}
#endif

static inline void renorm_cabac_decoder(CABACContext *c){
    while(c->range < 0x100){
        c->range+= c->range;
        c->low+= c->low;
        if(!(c->low & CABAC_MASK))
            refill(c);
    }
}

static inline void renorm_cabac_decoder_once(CABACContext *c){
    int shift= (uint32_t)(c->range - 0x100)>>31;
    c->range<<= shift;
    c->low  <<= shift;
    if(!(c->low & CABAC_MASK))
        refill(c);
}

static av_always_inline int get_cabac_inline(CABACContext *c, uint8_t * const state){
    //FIXME gcc generates duplicate load/stores for c->low and c->range
#if ARCH_X86 && HAVE_7REGS && HAVE_EBX_AVAILABLE && !defined(BROKEN_RELOCATIONS)
    int bit;

#if HAVE_FAST_CMOV
#define BRANCHLESS_GET_CABAC_UPDATE(ret, cabac, statep, low, lowword, range, tmp, tmpbyte)\
        "mov    "tmp"       , %%ecx                                     \n\t"\
        "shl    $17         , "tmp"                                     \n\t"\
        "cmp    "low"       , "tmp"                                     \n\t"\
        "cmova  %%ecx       , "range"                                   \n\t"\
        "sbb    %%ecx       , %%ecx                                     \n\t"\
        "and    %%ecx       , "tmp"                                     \n\t"\
        "sub    "tmp"       , "low"                                     \n\t"\
        "xor    %%ecx       , "ret"                                     \n\t"
#else /* HAVE_FAST_CMOV */
#define BRANCHLESS_GET_CABAC_UPDATE(ret, cabac, statep, low, lowword, range, tmp, tmpbyte)\
        "mov    "tmp"       , %%ecx                                     \n\t"\
        "shl    $17         , "tmp"                                     \n\t"\
        "sub    "low"       , "tmp"                                     \n\t"\
        "sar    $31         , "tmp"                                     \n\t" /*lps_mask*/\
        "sub    %%ecx       , "range"                                   \n\t" /*RangeLPS - range*/\
        "and    "tmp"       , "range"                                   \n\t" /*(RangeLPS - range)&lps_mask*/\
        "add    %%ecx       , "range"                                   \n\t" /*new range*/\
        "shl    $17         , %%ecx                                     \n\t"\
        "and    "tmp"       , %%ecx                                     \n\t"\
        "sub    %%ecx       , "low"                                     \n\t"\
        "xor    "tmp"       , "ret"                                     \n\t"
#endif /* HAVE_FAST_CMOV */


#define BRANCHLESS_GET_CABAC(ret, cabac, statep, low, lowword, range, tmp, tmpbyte, byte) \
        "movzbl "statep"    , "ret"                                     \n\t"\
        "mov    "range"     , "tmp"                                     \n\t"\
        "and    $0xC0       , "range"                                   \n\t"\
        "movzbl "MANGLE(ff_h264_lps_range)"("ret", "range", 2), "range" \n\t"\
        "sub    "range"     , "tmp"                                     \n\t"\
        BRANCHLESS_GET_CABAC_UPDATE(ret, cabac, statep, low, lowword, range, tmp, tmpbyte)\
        "movzbl " MANGLE(ff_h264_norm_shift) "("range"), %%ecx          \n\t"\
        "shl    %%cl        , "range"                                   \n\t"\
        "movzbl "MANGLE(ff_h264_mlps_state)"+128("ret"), "tmp"          \n\t"\
        "mov    "tmpbyte"   , "statep"                                  \n\t"\
        "shl    %%cl        , "low"                                     \n\t"\
        "test   "lowword"   , "lowword"                                 \n\t"\
        " jnz   1f                                                      \n\t"\
        "mov "byte"("cabac"), %%"REG_c"                                 \n\t"\
        "movzwl (%%"REG_c")     , "tmp"                                 \n\t"\
        "bswap  "tmp"                                                   \n\t"\
        "shr    $15         , "tmp"                                     \n\t"\
        "sub    $0xFFFF     , "tmp"                                     \n\t"\
        "add    $2          , %%"REG_c"                                 \n\t"\
        "mov    %%"REG_c"   , "byte    "("cabac")                       \n\t"\
        "lea    -1("low")   , %%ecx                                     \n\t"\
        "xor    "low"       , %%ecx                                     \n\t"\
        "shr    $15         , %%ecx                                     \n\t"\
        "movzbl " MANGLE(ff_h264_norm_shift) "(%%ecx), %%ecx            \n\t"\
        "neg    %%ecx                                                   \n\t"\
        "add    $7          , %%ecx                                     \n\t"\
        "shl    %%cl        , "tmp"                                     \n\t"\
        "add    "tmp"       , "low"                                     \n\t"\
        "1:                                                             \n\t"

    __asm__ volatile(
        "movl %a3(%2), %%esi            \n\t"
        "movl %a4(%2), %%ebx            \n\t"
        BRANCHLESS_GET_CABAC("%0", "%2", "(%1)", "%%ebx", "%%bx", "%%esi", "%%edx", "%%dl", "%a5")
        "movl %%esi, %a3(%2)            \n\t"
        "movl %%ebx, %a4(%2)            \n\t"

        :"=&a"(bit)
        :"r"(state), "r"(c),
         "i"(offsetof(CABACContext, range)), "i"(offsetof(CABACContext, low)),
         "i"(offsetof(CABACContext, bytestream))
        : "%"REG_c, "%ebx", "%edx", "%esi", "memory"
    );
    bit&=1;
#else /* ARCH_X86 && HAVE_7REGS && HAVE_EBX_AVAILABLE && !defined(BROKEN_RELOCATIONS) */
    int s = *state;
    int RangeLPS= ff_h264_lps_range[2*(c->range&0xC0) + s];
    int bit, lps_mask;

    c->range -= RangeLPS;
    lps_mask= ((c->range<<(CABAC_BITS+1)) - c->low)>>31;

    c->low -= (c->range<<(CABAC_BITS+1)) & lps_mask;
    c->range += (RangeLPS - c->range) & lps_mask;

    s^=lps_mask;
    *state= (ff_h264_mlps_state+128)[s];
    bit= s&1;

    lps_mask= ff_h264_norm_shift[c->range];
    c->range<<= lps_mask;
    c->low  <<= lps_mask;
    if(!(c->low & CABAC_MASK))
        refill2(c);
#endif /* ARCH_X86 && HAVE_7REGS && HAVE_EBX_AVAILABLE && !defined(BROKEN_RELOCATIONS) */
    return bit;
}

static int av_noinline av_unused get_cabac_noinline(CABACContext *c, uint8_t * const state){
    return get_cabac_inline(c,state);
}

static int av_unused get_cabac(CABACContext *c, uint8_t * const state){
    return get_cabac_inline(c,state);
}

static int av_unused get_cabac_bypass(CABACContext *c){
    int range;
    c->low += c->low;

    if(!(c->low & CABAC_MASK))
        refill(c);

    range= c->range<<(CABAC_BITS+1);
    if(c->low < range){
        return 0;
    }else{
        c->low -= range;
        return 1;
    }
}


static av_always_inline int get_cabac_bypass_sign(CABACContext *c, int val){
#if ARCH_X86 && HAVE_EBX_AVAILABLE
    __asm__ volatile(
        "movl %a2(%1), %%ebx                    \n\t"
        "movl %a3(%1), %%eax                    \n\t"
        "shl $17, %%ebx                         \n\t"
        "add %%eax, %%eax                       \n\t"
        "sub %%ebx, %%eax                       \n\t"
        "cltd                                   \n\t"
        "and %%edx, %%ebx                       \n\t"
        "add %%ebx, %%eax                       \n\t"
        "xor %%edx, %%ecx                       \n\t"
        "sub %%edx, %%ecx                       \n\t"
        "test %%ax, %%ax                        \n\t"
        " jnz 1f                                \n\t"
        "mov  %a4(%1), %%"REG_b"                \n\t"
        "subl $0xFFFF, %%eax                    \n\t"
        "movzwl (%%"REG_b"), %%edx              \n\t"
        "bswap %%edx                            \n\t"
        "shrl $15, %%edx                        \n\t"
        "add  $2, %%"REG_b"                     \n\t"
        "addl %%edx, %%eax                      \n\t"
        "mov  %%"REG_b", %a4(%1)                \n\t"
        "1:                                     \n\t"
        "movl %%eax, %a3(%1)                    \n\t"

        :"+c"(val)
        :"r"(c),
         "i"(offsetof(CABACContext, range)), "i"(offsetof(CABACContext, low)),
         "i"(offsetof(CABACContext, bytestream))
        : "%eax", "%"REG_b, "%edx", "memory"
    );
    return val;
#else
    int range, mask;
    c->low += c->low;

    if(!(c->low & CABAC_MASK))
        refill(c);

    range= c->range<<(CABAC_BITS+1);
    c->low -= range;
    mask= c->low >> 31;
    range &= mask;
    c->low += range;
    return (val^mask)-mask;
#endif
}

/**
 *
 * @return the number of bytes read or 0 if no end
 */
static int av_unused get_cabac_terminate(CABACContext *c){
    c->range -= 2;
    if(c->low < c->range<<(CABAC_BITS+1)){
        renorm_cabac_decoder_once(c);
        return 0;
    }else{
        return c->bytestream - c->bytestream_start;
    }
}

#if 0
/**
 * Get (truncated) unary binarization.
 */
static int get_cabac_u(CABACContext *c, uint8_t * state, int max, int max_index, int truncated){
    int i;

    for(i=0; i<max; i++){
        if(get_cabac(c, state)==0)
            return i;

        if(i< max_index) state++;
    }

    return truncated ? max : -1;
}

/**
 * get unary exp golomb k-th order binarization.
 */
static int get_cabac_ueg(CABACContext *c, uint8_t * state, int max, int is_signed, int k, int max_index){
    int i, v;
    int m= 1<<k;

    if(get_cabac(c, state)==0)
        return 0;

    if(0 < max_index) state++;

    for(i=1; i<max; i++){
        if(get_cabac(c, state)==0){
            if(is_signed && get_cabac_bypass(c)){
                return -i;
            }else
                return i;
        }

        if(i < max_index) state++;
    }

    while(get_cabac_bypass(c)){
        i+= m;
        m+= m;
    }

    v=0;
    while(m>>=1){
        v+= v + get_cabac_bypass(c);
    }
    i += v;

    if(is_signed && get_cabac_bypass(c)){
        return -i;
    }else
        return i;
}
#endif /* 0 */

#endif /* AVCODEC_CABAC_H */
