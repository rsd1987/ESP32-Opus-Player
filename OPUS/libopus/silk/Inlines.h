/***********************************************************************
Copyright (c) 2006-2011, Skype Limited. All rights reserved.
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
- Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
- Neither the name of Internet Society, IETF or IETF Trust, nor the
names of specific contributors, may be used to endorse or promote
products derived from this software without specific prior written
permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
***********************************************************************/

/*! \file silk_Inlines.h
 *  \brief silk_Inlines.h defines OPUS_INLINE signal processing functions.
 */

#ifndef SILK_FIX_INLINES_H
#define SILK_FIX_INLINES_H

#ifdef  __cplusplus
extern "C"
{
#endif

/* count leading zeros of int32_t64 */
static OPUS_INLINE int32_t silk_CLZ64( int64_t in )
{
    int32_t in_upper;

    in_upper = (int32_t)silk_RSHIFT64(in, 32);
    if (in_upper == 0) {
        /* Search in the lower 32 bits */
        return 32 + silk_CLZ32( (int32_t) in );
    } else {
        /* Search in the upper 32 bits */
        return silk_CLZ32( in_upper );
    }
}

/* get number of leading zeros and fractional part (the bits right after the leading one */
static OPUS_INLINE void silk_CLZ_FRAC(
    int32_t in,            /* I  input                               */
    int32_t *lz,           /* O  number of leading zeros             */
    int32_t *frac_Q7       /* O  the 7 bits right after the leading one */
)
{
    int32_t lzeros = silk_CLZ32(in);

    * lz = lzeros;
    * frac_Q7 = silk_ROR32(in, 24 - lzeros) & 0x7f;
}

/* Approximation of square root                                          */
/* Accuracy: < +/- 10%  for output values > 15                           */
/*           < +/- 2.5% for output values > 120                          */
static OPUS_INLINE int32_t silk_SQRT_APPROX( int32_t x )
{
    int32_t y, lz, frac_Q7;

    if( x <= 0 ) {
        return 0;
    }

    silk_CLZ_FRAC(x, &lz, &frac_Q7);

    if( lz & 1 ) {
        y = 32768;
    } else {
        y = 46214;        /* 46214 = sqrt(2) * 32768 */
    }

    /* get scaling right */
    y >>= silk_RSHIFT(lz, 1);

    /* increment using fractional part of input */
    y = silk_SMLAWB(y, y, silk_SMULBB(213, frac_Q7));

    return y;
}

/* Divide two int32 values and return result as int32 in a given Q-domain */
static OPUS_INLINE int32_t silk_DIV32_varQ(   /* O    returns a good approximation of "(a32 << Qres) / b32" */
    const int32_t     a32,               /* I    numerator (Q0)                  */
    const int32_t     b32,               /* I    denominator (Q0)                */
    const int32_t       Qres               /* I    Q-domain of result (>= 0)       */
)
{
    int32_t   a_headrm, b_headrm, lshift;
    int32_t b32_inv, a32_nrm, b32_nrm, result;

    silk_assert( b32 != 0 );
    silk_assert( Qres >= 0 );

    /* Compute number of bits head room and normalize inputs */
    a_headrm = silk_CLZ32( silk_abs(a32) ) - 1;
    a32_nrm = silk_LSHIFT(a32, a_headrm);                                       /* Q: a_headrm                  */
    b_headrm = silk_CLZ32( silk_abs(b32) ) - 1;
    b32_nrm = silk_LSHIFT(b32, b_headrm);                                       /* Q: b_headrm                  */

    /* Inverse of b32, with 14 bits of precision */
    b32_inv = silk_DIV32_16( silk_int32_MAX >> 2, silk_RSHIFT(b32_nrm, 16) );   /* Q: 29 + 16 - b_headrm        */

    /* First approximation */
    result = silk_SMULWB(a32_nrm, b32_inv);                                     /* Q: 29 + a_headrm - b_headrm  */

    /* Compute residual by subtracting product of denominator and first approximation */
    /* It's OK to overflow because the final value of a32_nrm should always be small */
    a32_nrm = silk_SUB32_ovflw(a32_nrm, silk_LSHIFT_ovflw( silk_SMMUL(b32_nrm, result), 3 ));  /* Q: a_headrm   */

    /* Refinement */
    result = silk_SMLAWB(result, a32_nrm, b32_inv);                             /* Q: 29 + a_headrm - b_headrm  */

    /* Convert to Qres domain */
    lshift = 29 + a_headrm - b_headrm - Qres;
    if( lshift < 0 ) {
        return silk_LSHIFT_SAT32(result, -lshift);
    } else {
        if( lshift < 32){
            return silk_RSHIFT(result, lshift);
        } else {
            /* Avoid undefined result */
            return 0;
        }
    }
}

/* Invert int32 value and return result as int32 in a given Q-domain */
static OPUS_INLINE int32_t silk_INVERSE32_varQ(   /* O    returns a good approximation of "(1 << Qres) / b32" */
    const int32_t     b32,                   /* I    denominator (Q0)                */
    const int32_t       Qres                   /* I    Q-domain of result (> 0)        */
)
{
    int32_t   b_headrm, lshift;
    int32_t b32_inv, b32_nrm, err_Q32, result;

    silk_assert( b32 != 0 );
    silk_assert( Qres > 0 );

    /* Compute number of bits head room and normalize input */
    b_headrm = silk_CLZ32( silk_abs(b32) ) - 1;
    b32_nrm = silk_LSHIFT(b32, b_headrm);                                       /* Q: b_headrm                */

    /* Inverse of b32, with 14 bits of precision */
    b32_inv = silk_DIV32_16( silk_int32_MAX >> 2, silk_RSHIFT(b32_nrm, 16) );   /* Q: 29 + 16 - b_headrm    */

    /* First approximation */
    result = silk_LSHIFT(b32_inv, 16);                                          /* Q: 61 - b_headrm            */

    /* Compute residual by subtracting product of denominator and first approximation from one */
    err_Q32 = silk_LSHIFT( ((int32_t)1<<29) - silk_SMULWB(b32_nrm, b32_inv), 3 );        /* Q32                        */

    /* Refinement */
    result = silk_SMLAWW(result, err_Q32, b32_inv);                             /* Q: 61 - b_headrm            */

    /* Convert to Qres domain */
    lshift = 61 - b_headrm - Qres;
    if( lshift <= 0 ) {
        return silk_LSHIFT_SAT32(result, -lshift);
    } else {
        if( lshift < 32){
            return silk_RSHIFT(result, lshift);
        }else{
            /* Avoid undefined result */
            return 0;
        }
    }
}

#ifdef  __cplusplus
}
#endif

#endif /* SILK_FIX_INLINES_H */
