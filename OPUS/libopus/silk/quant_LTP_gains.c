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
#include "tuning_parameters.h"

void silk_quant_LTP_gains(
    int16_t                  B_Q14[ MAX_NB_SUBFR * LTP_ORDER ],          /* O    Quantized LTP gains             */
    int8_t                   cbk_index[ MAX_NB_SUBFR ],                  /* O    Codebook Index                  */
    int8_t                   *periodicity_index,                         /* O    Periodicity Index               */
    int32_t                  *sum_log_gain_Q7,                           /* I/O  Cumulative max prediction gain  */
    int32_t                    *pred_gain_dB_Q7,                           /* O    LTP prediction gain             */
    const int32_t            XX_Q17[ MAX_NB_SUBFR*LTP_ORDER*LTP_ORDER ], /* I    Correlation matrix in Q18       */
    const int32_t            xX_Q17[ MAX_NB_SUBFR*LTP_ORDER ],           /* I    Correlation vector in Q18       */
    const int32_t              subfr_len,                                  /* I    Number of samples per subframe  */
    const int32_t              nb_subfr,                                   /* I    Number of subframes             */
    int                         arch                                        /* I    Run-time architecture           */
)
{
    int32_t             j, k, cbk_size;
    int8_t            temp_idx[ MAX_NB_SUBFR ];
    const uint8_t     *cl_ptr_Q5;
    const int8_t      *cbk_ptr_Q7;
    const uint8_t     *cbk_gain_ptr_Q7;
    const int32_t     *XX_Q17_ptr, *xX_Q17_ptr;
    int32_t           res_nrg_Q15_subfr, res_nrg_Q15, rate_dist_Q7_subfr, rate_dist_Q7, min_rate_dist_Q7;
    int32_t           sum_log_gain_tmp_Q7, best_sum_log_gain_Q7, max_gain_Q7;
    int32_t             gain_Q7;

    /***************************************************/
    /* iterate over different codebooks with different */
    /* rates/distortions, and choose best */
    /***************************************************/
    min_rate_dist_Q7 = silk_int32_MAX;
    best_sum_log_gain_Q7 = 0;
    for( k = 0; k < 3; k++ ) {
        /* Safety margin for pitch gain control, to take into account factors
           such as state rescaling/rewhitening. */
        int32_t gain_safety = SILK_FIX_CONST( 0.4, 7 );

        cl_ptr_Q5  = silk_LTP_gain_BITS_Q5_ptrs[ k ];
        cbk_ptr_Q7 = silk_LTP_vq_ptrs_Q7[        k ];
        cbk_gain_ptr_Q7 = silk_LTP_vq_gain_ptrs_Q7[ k ];
        cbk_size   = silk_LTP_vq_sizes[          k ];

        /* Set up pointers to first subframe */
        XX_Q17_ptr = XX_Q17;
        xX_Q17_ptr = xX_Q17;

        res_nrg_Q15 = 0;
        rate_dist_Q7 = 0;
        sum_log_gain_tmp_Q7 = *sum_log_gain_Q7;
        for( j = 0; j < nb_subfr; j++ ) {
            max_gain_Q7 = silk_log2lin( ( SILK_FIX_CONST( MAX_SUM_LOG_GAIN_DB / 6.0, 7 ) - sum_log_gain_tmp_Q7 )
                                        + SILK_FIX_CONST( 7, 7 ) ) - gain_safety;
            silk_VQ_WMat_EC(
                &temp_idx[ j ],         /* O    index of best codebook vector                           */
                &res_nrg_Q15_subfr,     /* O    residual energy                                         */
                &rate_dist_Q7_subfr,    /* O    best weighted quantization error + mu * rate            */
                &gain_Q7,               /* O    sum of absolute LTP coefficients                        */
                XX_Q17_ptr,             /* I    correlation matrix                                      */
                xX_Q17_ptr,             /* I    correlation vector                                      */
                cbk_ptr_Q7,             /* I    codebook                                                */
                cbk_gain_ptr_Q7,        /* I    codebook effective gains                                */
                cl_ptr_Q5,              /* I    code length for each codebook vector                    */
                subfr_len,              /* I    number of samples per subframe                          */
                max_gain_Q7,            /* I    maximum sum of absolute LTP coefficients                */
                cbk_size,               /* I    number of vectors in codebook                           */
                arch                    /* I    Run-time architecture                                   */
            );

            res_nrg_Q15  = silk_ADD_POS_SAT32( res_nrg_Q15, res_nrg_Q15_subfr );
            rate_dist_Q7 = silk_ADD_POS_SAT32( rate_dist_Q7, rate_dist_Q7_subfr );
            sum_log_gain_tmp_Q7 = silk_max(0, sum_log_gain_tmp_Q7
                                + silk_lin2log( gain_safety + gain_Q7 ) - SILK_FIX_CONST( 7, 7 ));

            XX_Q17_ptr += LTP_ORDER * LTP_ORDER;
            xX_Q17_ptr += LTP_ORDER;
        }

        if( rate_dist_Q7 <= min_rate_dist_Q7 ) {
            min_rate_dist_Q7 = rate_dist_Q7;
            *periodicity_index = (int8_t)k;
            silk_memcpy( cbk_index, temp_idx, nb_subfr * sizeof( int8_t ) );
            best_sum_log_gain_Q7 = sum_log_gain_tmp_Q7;
        }
    }

    cbk_ptr_Q7 = silk_LTP_vq_ptrs_Q7[ *periodicity_index ];
    for( j = 0; j < nb_subfr; j++ ) {
        for( k = 0; k < LTP_ORDER; k++ ) {
            B_Q14[ j * LTP_ORDER + k ] = silk_LSHIFT( cbk_ptr_Q7[ cbk_index[ j ] * LTP_ORDER + k ], 7 );
        }
    }

    if( nb_subfr == 2 ) {
        res_nrg_Q15 = silk_RSHIFT32( res_nrg_Q15, 1 );
    } else {
        res_nrg_Q15 = silk_RSHIFT32( res_nrg_Q15, 2 );
    }

    *sum_log_gain_Q7 = best_sum_log_gain_Q7;
    *pred_gain_dB_Q7 = (int32_t)silk_SMULBB( -3, silk_lin2log( res_nrg_Q15 ) - ( 15 << 7 ) );
}
