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

#include "main.h"
#include "../celt/stack_alloc.h"

/* Convert Left/Right stereo signal to adaptive Mid/Side representation */
void silk_stereo_LR_to_MS(
    stereo_enc_state            *state,                         /* I/O  State                                       */
    int16_t                  x1[],                           /* I/O  Left input signal, becomes mid signal       */
    int16_t                  x2[],                           /* I/O  Right input signal, becomes side signal     */
    int8_t                   ix[ 2 ][ 3 ],                   /* O    Quantization indices                        */
    int8_t                   *mid_only_flag,                 /* O    Flag: only mid signal coded                 */
    int32_t                  mid_side_rates_bps[],           /* O    Bitrates for mid and side signals           */
    int32_t                  total_rate_bps,                 /* I    Total bitrate                               */
    int32_t                    prev_speech_act_Q8,             /* I    Speech activity level in previous frame     */
    int32_t                    toMono,                         /* I    Last frame before a stereo->mono transition */
    int32_t                    fs_kHz,                         /* I    Sample rate (kHz)                           */
    int32_t                    frame_length                    /* I    Number of samples                           */
)
{
    int32_t   n, is10msFrame, denom_Q16, delta0_Q13, delta1_Q13;
    int32_t sum, diff, smooth_coef_Q16, pred_Q13[ 2 ], pred0_Q13, pred1_Q13;
    int32_t LP_ratio_Q14, HP_ratio_Q14, frac_Q16, frac_3_Q16, min_mid_rate_bps, width_Q14, w_Q24, deltaw_Q24;
    VARDECL( int16_t, side );
    VARDECL( int16_t, LP_mid );
    VARDECL( int16_t, HP_mid );
    VARDECL( int16_t, LP_side );
    VARDECL( int16_t, HP_side );
    int16_t *mid = &x1[ -2 ];
    SAVE_STACK;

    ALLOC( side, frame_length + 2, int16_t );
    /* Convert to basic mid/side signals */
    for( n = 0; n < frame_length + 2; n++ ) {
        sum  = x1[ n - 2 ] + (int32_t)x2[ n - 2 ];
        diff = x1[ n - 2 ] - (int32_t)x2[ n - 2 ];
        mid[  n ] = (int16_t)silk_RSHIFT_ROUND( sum, 1 );
        side[ n ] = (int16_t)silk_SAT16( silk_RSHIFT_ROUND( diff, 1 ) );
    }

    /* Buffering */
    silk_memcpy( mid,  state->sMid,  2 * sizeof( int16_t ) );
    silk_memcpy( side, state->sSide, 2 * sizeof( int16_t ) );
    silk_memcpy( state->sMid,  &mid[  frame_length ], 2 * sizeof( int16_t ) );
    silk_memcpy( state->sSide, &side[ frame_length ], 2 * sizeof( int16_t ) );

    /* LP and HP filter mid signal */
    ALLOC( LP_mid, frame_length, int16_t );
    ALLOC( HP_mid, frame_length, int16_t );
    for( n = 0; n < frame_length; n++ ) {
        sum = silk_RSHIFT_ROUND( silk_ADD_LSHIFT( mid[ n ] + (int32_t)mid[ n + 2 ], mid[ n + 1 ], 1 ), 2 );
        LP_mid[ n ] = sum;
        HP_mid[ n ] = mid[ n + 1 ] - sum;
    }

    /* LP and HP filter side signal */
    ALLOC( LP_side, frame_length, int16_t );
    ALLOC( HP_side, frame_length, int16_t );
    for( n = 0; n < frame_length; n++ ) {
        sum = silk_RSHIFT_ROUND( silk_ADD_LSHIFT( side[ n ] + (int32_t)side[ n + 2 ], side[ n + 1 ], 1 ), 2 );
        LP_side[ n ] = sum;
        HP_side[ n ] = side[ n + 1 ] - sum;
    }

    /* Find energies and predictors */
    is10msFrame = frame_length == 10 * fs_kHz;
    smooth_coef_Q16 = is10msFrame ?
        SILK_FIX_CONST( STEREO_RATIO_SMOOTH_COEF / 2, 16 ) :
        SILK_FIX_CONST( STEREO_RATIO_SMOOTH_COEF,     16 );
    smooth_coef_Q16 = silk_SMULWB( silk_SMULBB( prev_speech_act_Q8, prev_speech_act_Q8 ), smooth_coef_Q16 );

    pred_Q13[ 0 ] = silk_stereo_find_predictor( &LP_ratio_Q14, LP_mid, LP_side, &state->mid_side_amp_Q0[ 0 ], frame_length, smooth_coef_Q16 );
    pred_Q13[ 1 ] = silk_stereo_find_predictor( &HP_ratio_Q14, HP_mid, HP_side, &state->mid_side_amp_Q0[ 2 ], frame_length, smooth_coef_Q16 );
    /* Ratio of the norms of residual and mid signals */
    frac_Q16 = silk_SMLABB( HP_ratio_Q14, LP_ratio_Q14, 3 );
    frac_Q16 = silk_min( frac_Q16, SILK_FIX_CONST( 1, 16 ) );

    /* Determine bitrate distribution between mid and side, and possibly reduce stereo width */
    total_rate_bps -= is10msFrame ? 1200 : 600;      /* Subtract approximate bitrate for coding stereo parameters */
    if( total_rate_bps < 1 ) {
        total_rate_bps = 1;
    }
    min_mid_rate_bps = silk_SMLABB( 2000, fs_kHz, 600 );
    silk_assert( min_mid_rate_bps < 32767 );
    /* Default bitrate distribution: 8 parts for Mid and (5+3*frac) parts for Side. so: mid_rate = ( 8 / ( 13 + 3 * frac ) ) * total_ rate */
    frac_3_Q16 = silk_MUL( 3, frac_Q16 );
    mid_side_rates_bps[ 0 ] = silk_DIV32_varQ( total_rate_bps, SILK_FIX_CONST( 8 + 5, 16 ) + frac_3_Q16, 16+3 );
    /* If Mid bitrate below minimum, reduce stereo width */
    if( mid_side_rates_bps[ 0 ] < min_mid_rate_bps ) {
        mid_side_rates_bps[ 0 ] = min_mid_rate_bps;
        mid_side_rates_bps[ 1 ] = total_rate_bps - mid_side_rates_bps[ 0 ];
        /* width = 4 * ( 2 * side_rate - min_rate ) / ( ( 1 + 3 * frac ) * min_rate ) */
        width_Q14 = silk_DIV32_varQ( silk_LSHIFT( mid_side_rates_bps[ 1 ], 1 ) - min_mid_rate_bps,
            silk_SMULWB( SILK_FIX_CONST( 1, 16 ) + frac_3_Q16, min_mid_rate_bps ), 14+2 );
        width_Q14 = silk_LIMIT( width_Q14, 0, SILK_FIX_CONST( 1, 14 ) );
    } else {
        mid_side_rates_bps[ 1 ] = total_rate_bps - mid_side_rates_bps[ 0 ];
        width_Q14 = SILK_FIX_CONST( 1, 14 );
    }

    /* Smoother */
    state->smth_width_Q14 = (int16_t)silk_SMLAWB( state->smth_width_Q14, width_Q14 - state->smth_width_Q14, smooth_coef_Q16 );

    /* At very low bitrates or for inputs that are nearly amplitude panned, switch to panned-mono coding */
    *mid_only_flag = 0;
    if( toMono ) {
        /* Last frame before stereo->mono transition; collapse stereo width */
        width_Q14 = 0;
        pred_Q13[ 0 ] = 0;
        pred_Q13[ 1 ] = 0;
        silk_stereo_quant_pred( pred_Q13, ix );
    } else if( state->width_prev_Q14 == 0 &&
        ( 8 * total_rate_bps < 13 * min_mid_rate_bps || silk_SMULWB( frac_Q16, state->smth_width_Q14 ) < SILK_FIX_CONST( 0.05, 14 ) ) )
    {
        /* Code as panned-mono; previous frame already had zero width */
        /* Scale down and quantize predictors */
        pred_Q13[ 0 ] = silk_RSHIFT( silk_SMULBB( state->smth_width_Q14, pred_Q13[ 0 ] ), 14 );
        pred_Q13[ 1 ] = silk_RSHIFT( silk_SMULBB( state->smth_width_Q14, pred_Q13[ 1 ] ), 14 );
        silk_stereo_quant_pred( pred_Q13, ix );
        /* Collapse stereo width */
        width_Q14 = 0;
        pred_Q13[ 0 ] = 0;
        pred_Q13[ 1 ] = 0;
        mid_side_rates_bps[ 0 ] = total_rate_bps;
        mid_side_rates_bps[ 1 ] = 0;
        *mid_only_flag = 1;
    } else if( state->width_prev_Q14 != 0 &&
        ( 8 * total_rate_bps < 11 * min_mid_rate_bps || silk_SMULWB( frac_Q16, state->smth_width_Q14 ) < SILK_FIX_CONST( 0.02, 14 ) ) )
    {
        /* Transition to zero-width stereo */
        /* Scale down and quantize predictors */
        pred_Q13[ 0 ] = silk_RSHIFT( silk_SMULBB( state->smth_width_Q14, pred_Q13[ 0 ] ), 14 );
        pred_Q13[ 1 ] = silk_RSHIFT( silk_SMULBB( state->smth_width_Q14, pred_Q13[ 1 ] ), 14 );
        silk_stereo_quant_pred( pred_Q13, ix );
        /* Collapse stereo width */
        width_Q14 = 0;
        pred_Q13[ 0 ] = 0;
        pred_Q13[ 1 ] = 0;
    } else if( state->smth_width_Q14 > SILK_FIX_CONST( 0.95, 14 ) ) {
        /* Full-width stereo coding */
        silk_stereo_quant_pred( pred_Q13, ix );
        width_Q14 = SILK_FIX_CONST( 1, 14 );
    } else {
        /* Reduced-width stereo coding; scale down and quantize predictors */
        pred_Q13[ 0 ] = silk_RSHIFT( silk_SMULBB( state->smth_width_Q14, pred_Q13[ 0 ] ), 14 );
        pred_Q13[ 1 ] = silk_RSHIFT( silk_SMULBB( state->smth_width_Q14, pred_Q13[ 1 ] ), 14 );
        silk_stereo_quant_pred( pred_Q13, ix );
        width_Q14 = state->smth_width_Q14;
    }

    /* Make sure to keep on encoding until the tapered output has been transmitted */
    if( *mid_only_flag == 1 ) {
        state->silent_side_len += frame_length - STEREO_INTERP_LEN_MS * fs_kHz;
        if( state->silent_side_len < LA_SHAPE_MS * fs_kHz ) {
            *mid_only_flag = 0;
        } else {
            /* Limit to avoid wrapping around */
            state->silent_side_len = 10000;
        }
    } else {
        state->silent_side_len = 0;
    }

    if( *mid_only_flag == 0 && mid_side_rates_bps[ 1 ] < 1 ) {
        mid_side_rates_bps[ 1 ] = 1;
        mid_side_rates_bps[ 0 ] = silk_max_int( 1, total_rate_bps - mid_side_rates_bps[ 1 ]);
    }

    /* Interpolate predictors and subtract prediction from side channel */
    pred0_Q13  = -state->pred_prev_Q13[ 0 ];
    pred1_Q13  = -state->pred_prev_Q13[ 1 ];
    w_Q24      =  silk_LSHIFT( state->width_prev_Q14, 10 );
    denom_Q16  = silk_DIV32_16( (int32_t)1 << 16, STEREO_INTERP_LEN_MS * fs_kHz );
    delta0_Q13 = -silk_RSHIFT_ROUND( silk_SMULBB( pred_Q13[ 0 ] - state->pred_prev_Q13[ 0 ], denom_Q16 ), 16 );
    delta1_Q13 = -silk_RSHIFT_ROUND( silk_SMULBB( pred_Q13[ 1 ] - state->pred_prev_Q13[ 1 ], denom_Q16 ), 16 );
    deltaw_Q24 =  silk_LSHIFT( silk_SMULWB( width_Q14 - state->width_prev_Q14, denom_Q16 ), 10 );
    for( n = 0; n < STEREO_INTERP_LEN_MS * fs_kHz; n++ ) {
        pred0_Q13 += delta0_Q13;
        pred1_Q13 += delta1_Q13;
        w_Q24   += deltaw_Q24;
        sum = silk_LSHIFT( silk_ADD_LSHIFT( mid[ n ] + (int32_t)mid[ n + 2 ], mid[ n + 1 ], 1 ), 9 );    /* Q11 */
        sum = silk_SMLAWB( silk_SMULWB( w_Q24, side[ n + 1 ] ), sum, pred0_Q13 );               /* Q8  */
        sum = silk_SMLAWB( sum, silk_LSHIFT( (int32_t)mid[ n + 1 ], 11 ), pred1_Q13 );       /* Q8  */
        x2[ n - 1 ] = (int16_t)silk_SAT16( silk_RSHIFT_ROUND( sum, 8 ) );
    }

    pred0_Q13 = -pred_Q13[ 0 ];
    pred1_Q13 = -pred_Q13[ 1 ];
    w_Q24     =  silk_LSHIFT( width_Q14, 10 );
    for( n = STEREO_INTERP_LEN_MS * fs_kHz; n < frame_length; n++ ) {
        sum = silk_LSHIFT( silk_ADD_LSHIFT( mid[ n ] + (int32_t)mid[ n + 2 ], mid[ n + 1 ], 1 ), 9 );    /* Q11 */
        sum = silk_SMLAWB( silk_SMULWB( w_Q24, side[ n + 1 ] ), sum, pred0_Q13 );               /* Q8  */
        sum = silk_SMLAWB( sum, silk_LSHIFT( (int32_t)mid[ n + 1 ], 11 ), pred1_Q13 );       /* Q8  */
        x2[ n - 1 ] = (int16_t)silk_SAT16( silk_RSHIFT_ROUND( sum, 8 ) );
    }
    state->pred_prev_Q13[ 0 ] = (int16_t)pred_Q13[ 0 ];
    state->pred_prev_Q13[ 1 ] = (int16_t)pred_Q13[ 1 ];
    state->width_prev_Q14     = (int16_t)width_Q14;
    RESTORE_STACK;
}
