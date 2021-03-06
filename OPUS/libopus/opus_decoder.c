/* Copyright (c) 2010 Xiph.Org Foundation, Skype Limited
   Written by Jean-Marc Valin and Koen Vos */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

//#ifdef HAVE_CONFIG_H
# include "config.h"
//#endif

#include <stdarg.h>
#include "celt/celt.h"
#include "opus.h"
#include "celt/entdec.h"
#include "celt/modes.h"
#include "silk/API.h"
#include "celt/stack_alloc.h"
#include "celt/float_cast.h"
#include "opus_private.h"
#include "celt/os_support.h"
#include "silk/structs.h"
#include "silk/define.h"
#include "celt/mathops.h"
#include "celt/cpu_support.h"

struct OpusDecoder {
   int          celt_dec_offset;
   int          silk_dec_offset;
   int          channels;
   int32_t   Fs;          /** Sampling rate (at the API level) */
   silk_DecControlStruct DecControl;
   int          decode_gain;
   int          arch;

   /* Everything beyond this point gets cleared on a reset */
#define OPUS_DECODER_RESET_START stream_channels
   int          stream_channels;

   int          bandwidth;
   int          mode;
   int          prev_mode;
   int          frame_size;
   int          prev_redundancy;
   int          last_packet_duration;


   uint32_t  rangeFinal;
};


#define VALIDATE_OPUS_DECODER(st)


int opus_decoder_get_size(int channels)
{
   int silkDecSizeBytes, celtDecSizeBytes;
   int ret;
   if (channels<1 || channels > 2)
      return 0;
   ret = silk_Get_Decoder_Size( &silkDecSizeBytes );
   if(ret)
      return 0;
   silkDecSizeBytes = align(silkDecSizeBytes);
   celtDecSizeBytes = celt_decoder_get_size(channels);
   return align(sizeof(OpusDecoder))+silkDecSizeBytes+celtDecSizeBytes;
}

int opus_decoder_init(OpusDecoder *st, int32_t Fs, int channels)
{
   void *silk_dec;
   CELTDecoder *celt_dec;
   int ret, silkDecSizeBytes;

   if ((Fs!=48000&&Fs!=24000&&Fs!=16000&&Fs!=12000&&Fs!=8000)
    || (channels!=1&&channels!=2))
      return OPUS_BAD_ARG;

   OPUS_CLEAR((char*)st, opus_decoder_get_size(channels));
   /* Initialize SILK decoder */
   ret = silk_Get_Decoder_Size(&silkDecSizeBytes);
   if (ret)
      return OPUS_INTERNAL_ERROR;

   silkDecSizeBytes = align(silkDecSizeBytes);
   st->silk_dec_offset = align(sizeof(OpusDecoder));
   st->celt_dec_offset = st->silk_dec_offset+silkDecSizeBytes;
   silk_dec = (char*)st+st->silk_dec_offset;
   celt_dec = (CELTDecoder*)((char*)st+st->celt_dec_offset);
   st->stream_channels = st->channels = channels;

   st->Fs = Fs;
   st->DecControl.API_sampleRate = st->Fs;
   st->DecControl.nChannelsAPI      = st->channels;

   /* Reset decoder */
   ret = silk_InitDecoder( silk_dec );
   if(ret)return OPUS_INTERNAL_ERROR;

   /* Initialize CELT decoder */
   ret = celt_decoder_init(celt_dec, Fs, channels);
   if(ret!=OPUS_OK)return OPUS_INTERNAL_ERROR;

   celt_decoder_ctl(celt_dec, CELT_SET_SIGNALLING(0));

   st->prev_mode = 0;
   st->frame_size = Fs/400;
   st->arch = opus_select_arch();
   return OPUS_OK;
}

OpusDecoder *opus_decoder_create(int32_t Fs, int channels, int *error)
{
   int ret;
   OpusDecoder *st;
   if ((Fs!=48000&&Fs!=24000&&Fs!=16000&&Fs!=12000&&Fs!=8000)
    || (channels!=1&&channels!=2))
   {
      if (error)
         *error = OPUS_BAD_ARG;
      return NULL;
   }
   st = (OpusDecoder *)opus_alloc(opus_decoder_get_size(channels));
   if (st == NULL)
   {
      if (error)
         *error = OPUS_ALLOC_FAIL;
      return NULL;
   }
   ret = opus_decoder_init(st, Fs, channels);
   if (error)
      *error = ret;
   if (ret != OPUS_OK)
   {
      opus_free(st);
      st = NULL;
   }
   return st;
}

static void smooth_fade(const opus_val16 *in1, const opus_val16 *in2,
      opus_val16 *out, int overlap, int channels,
      const opus_val16 *window, int32_t Fs)
{
   int i, c;
   int inc = 48000/Fs;
   for (c=0;c<channels;c++)
   {
      for (i=0;i<overlap;i++)
      {
         opus_val16 w = MULT16_16_Q15(window[i*inc], window[i*inc]);
         out[i*channels+c] = SHR32(MAC16_16(MULT16_16(w,in2[i*channels+c]),
                                   Q15ONE-w, in1[i*channels+c]), 15);
      }
   }
}

static int opus_packet_get_mode(const unsigned char *data)
{
   int mode;
   if (data[0]&0x80)
   {
      mode = MODE_CELT_ONLY;
   } else if ((data[0]&0x60) == 0x60)
   {
      mode = MODE_HYBRID;
   } else {
      mode = MODE_SILK_ONLY;
   }
   return mode;
}

static int opus_decode_frame(OpusDecoder *st, const unsigned char *data,
      int32_t len, opus_val16 *pcm, int frame_size, int decode_fec)
{
   void *silk_dec;
   CELTDecoder *celt_dec;
   int i, silk_ret=0, celt_ret=0;
   ec_dec dec;
   int32_t silk_frame_size;
   int pcm_silk_size;
   VARDECL(int16_t, pcm_silk);
   int pcm_transition_silk_size;
   VARDECL(opus_val16, pcm_transition_silk);
   int pcm_transition_celt_size;
   VARDECL(opus_val16, pcm_transition_celt);
   opus_val16 *pcm_transition=NULL;
   int redundant_audio_size;
   VARDECL(opus_val16, redundant_audio);

   int audiosize;
   int mode;
   int bandwidth;
   int transition=0;
   int start_band;
   int redundancy=0;
   int redundancy_bytes = 0;
   int celt_to_silk=0;
   int c;
   int F2_5, F5, F10, F20;
   const opus_val16 *window;
   uint32_t redundant_rng = 0;
   int celt_accum;
   ALLOC_STACK;

   silk_dec = (char*)st+st->silk_dec_offset;
   celt_dec = (CELTDecoder*)((char*)st+st->celt_dec_offset);
   F20 = st->Fs/50;
   F10 = F20>>1;
   F5 = F10>>1;
   F2_5 = F5>>1;
   if (frame_size < F2_5)
   {
      RESTORE_STACK;
      return OPUS_BUFFER_TOO_SMALL;
   }
   /* Limit frame_size to avoid excessive stack allocations. */
   frame_size = IMIN(frame_size, st->Fs/25*3);
   /* Payloads of 1 (2 including ToC) or 0 trigger the PLC/DTX */
   if (len<=1)
   {
      data = NULL;
      /* In that case, don't conceal more than what the ToC says */
      frame_size = IMIN(frame_size, st->frame_size);
   }
   if (data != NULL)
   {
      audiosize = st->frame_size;
      mode = st->mode;
      bandwidth = st->bandwidth;
      ec_dec_init(&dec,(unsigned char*)data,len);
   } else {
      audiosize = frame_size;
      mode = st->prev_mode;
      bandwidth = 0;

      if (mode == 0)
      {
         /* If we haven't got any packet yet, all we can do is return zeros */
         for (i=0;i<audiosize*st->channels;i++)
            pcm[i] = 0;
         RESTORE_STACK;
         return audiosize;
      }

      /* Avoids trying to run the PLC on sizes other than 2.5 (CELT), 5 (CELT),
         10, or 20 (e.g. 12.5 or 30 ms). */
      if (audiosize > F20)
      {
         do {
            int ret = opus_decode_frame(st, NULL, 0, pcm, IMIN(audiosize, F20), 0);
            if (ret<0)
            {
               RESTORE_STACK;
               return ret;
            }
            pcm += ret*st->channels;
            audiosize -= ret;
         } while (audiosize > 0);
         RESTORE_STACK;
         return frame_size;
      } else if (audiosize < F20)
      {
         if (audiosize > F10)
            audiosize = F10;
         else if (mode != MODE_SILK_ONLY && audiosize > F5 && audiosize < F10)
            audiosize = F5;
      }
   }

   /* In fixed-point, we can tell CELT to do the accumulation on top of the
      SILK PCM buffer. This saves some stack space. */

   celt_accum = (mode != MODE_CELT_ONLY) && (frame_size >= F10);


   pcm_transition_silk_size = ALLOC_NONE;
   pcm_transition_celt_size = ALLOC_NONE;
   if (data!=NULL && st->prev_mode > 0 && (
       (mode == MODE_CELT_ONLY && st->prev_mode != MODE_CELT_ONLY && !st->prev_redundancy)
    || (mode != MODE_CELT_ONLY && st->prev_mode == MODE_CELT_ONLY) )
      )
   {
      transition = 1;
      /* Decide where to allocate the stack memory for pcm_transition */
      if (mode == MODE_CELT_ONLY)
         pcm_transition_celt_size = F5*st->channels;
      else
         pcm_transition_silk_size = F5*st->channels;
   }
   ALLOC(pcm_transition_celt, pcm_transition_celt_size, opus_val16);
   if (transition && mode == MODE_CELT_ONLY)
   {
      pcm_transition = pcm_transition_celt;
      opus_decode_frame(st, NULL, 0, pcm_transition, IMIN(F5, audiosize), 0);
   }
   if (audiosize > frame_size)
   {
      /*fprintf(stderr, "PCM buffer too small: %d vs %d (mode = %d)\n", audiosize, frame_size, mode);*/
      RESTORE_STACK;
      return OPUS_BAD_ARG;
   } else {
      frame_size = audiosize;
   }

   /* Don't allocate any memory when in CELT-only mode */
   pcm_silk_size = (mode != MODE_CELT_ONLY && !celt_accum) ? IMAX(F10, frame_size)*st->channels : ALLOC_NONE;
   ALLOC(pcm_silk, pcm_silk_size, int16_t);

   /* SILK processing */
   if (mode != MODE_CELT_ONLY)
   {
      int lost_flag, decoded_samples;
      int16_t *pcm_ptr;
      if (celt_accum)
         pcm_ptr = pcm;
      else
         pcm_ptr = pcm_silk;

      if (st->prev_mode==MODE_CELT_ONLY)
         silk_InitDecoder( silk_dec );

      /* The SILK PLC cannot produce frames of less than 10 ms */
      st->DecControl.payloadSize_ms = IMAX(10, 1000 * audiosize / st->Fs);

      if (data != NULL)
      {
        st->DecControl.nChannelsInternal = st->stream_channels;
        if( mode == MODE_SILK_ONLY ) {
           if( bandwidth == OPUS_BANDWIDTH_NARROWBAND ) {
              st->DecControl.internalSampleRate = 8000;
           } else if( bandwidth == OPUS_BANDWIDTH_MEDIUMBAND ) {
              st->DecControl.internalSampleRate = 12000;
           } else if( bandwidth == OPUS_BANDWIDTH_WIDEBAND ) {
              st->DecControl.internalSampleRate = 16000;
           } else {
              st->DecControl.internalSampleRate = 16000;
              celt_assert( 0 );
           }
        } else {
           /* Hybrid mode */
           st->DecControl.internalSampleRate = 16000;
        }
     }

     lost_flag = data == NULL ? 1 : 2 * decode_fec;
     decoded_samples = 0;
     do {
        /* Call SILK decoder */
        int first_frame = decoded_samples == 0;
        silk_ret = silk_Decode( silk_dec, &st->DecControl,
                                lost_flag, first_frame, &dec, pcm_ptr, &silk_frame_size, st->arch );
        if( silk_ret ) {
           if (lost_flag) {
              /* PLC failure should not be fatal */
              silk_frame_size = frame_size;
              for (i=0;i<frame_size*st->channels;i++)
                 pcm_ptr[i] = 0;
           } else {
             RESTORE_STACK;
             return OPUS_INTERNAL_ERROR;
           }
        }
        pcm_ptr += silk_frame_size * st->channels;
        decoded_samples += silk_frame_size;
      } while( decoded_samples < frame_size );
   }

   start_band = 0;
   if (!decode_fec && mode != MODE_CELT_ONLY && data != NULL
    && ec_tell(&dec)+17+20*(st->mode == MODE_HYBRID) <= 8*len)
   {
      /* Check if we have a redundant 0-8 kHz band */
      if (mode == MODE_HYBRID)
         redundancy = ec_dec_bit_logp(&dec, 12);
      else
         redundancy = 1;
      if (redundancy)
      {
         celt_to_silk = ec_dec_bit_logp(&dec, 1);
         /* redundancy_bytes will be at least two, in the non-hybrid
            case due to the ec_tell() check above */
         redundancy_bytes = mode==MODE_HYBRID ?
               (int32_t)ec_dec_uint(&dec, 256)+2 :
               len-((ec_tell(&dec)+7)>>3);
         len -= redundancy_bytes;
         /* This is a sanity check. It should never happen for a valid
            packet, so the exact behaviour is not normative. */
         if (len*8 < ec_tell(&dec))
         {
            len = 0;
            redundancy_bytes = 0;
            redundancy = 0;
         }
         /* Shrink decoder because of raw bits */
         dec.storage -= redundancy_bytes;
      }
   }
   if (mode != MODE_CELT_ONLY)
      start_band = 17;

   if (redundancy)
   {
      transition = 0;
      pcm_transition_silk_size=ALLOC_NONE;
   }

   ALLOC(pcm_transition_silk, pcm_transition_silk_size, opus_val16);

   if (transition && mode != MODE_CELT_ONLY)
   {
      pcm_transition = pcm_transition_silk;
      opus_decode_frame(st, NULL, 0, pcm_transition, IMIN(F5, audiosize), 0);
   }


   if (bandwidth)
   {
      int endband=21;

      switch(bandwidth)
      {
      case OPUS_BANDWIDTH_NARROWBAND:
         endband = 13;
         break;
      case OPUS_BANDWIDTH_MEDIUMBAND:
      case OPUS_BANDWIDTH_WIDEBAND:
         endband = 17;
         break;
      case OPUS_BANDWIDTH_SUPERWIDEBAND:
         endband = 19;
         break;
      case OPUS_BANDWIDTH_FULLBAND:
         endband = 21;
         break;
      default:
         celt_assert(0);
         break;
      }
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_SET_END_BAND(endband)));
   }
   MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_SET_CHANNELS(st->stream_channels)));

   /* Only allocation memory for redundancy if/when needed */
   redundant_audio_size = redundancy ? F5*st->channels : ALLOC_NONE;
   ALLOC(redundant_audio, redundant_audio_size, opus_val16);

   /* 5 ms redundant frame for CELT->SILK*/
   if (redundancy && celt_to_silk)
   {
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_SET_START_BAND(0)));
      celt_decode_with_ec(celt_dec, data+len, redundancy_bytes,
                          redundant_audio, F5, NULL, 0);
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, OPUS_GET_FINAL_RANGE(&redundant_rng)));
   }

   /* MUST be after PLC */
   MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_SET_START_BAND(start_band)));

   if (mode != MODE_SILK_ONLY)
   {
      int celt_frame_size = IMIN(F20, frame_size);
      /* Make sure to discard any previous CELT state */
      if (mode != st->prev_mode && st->prev_mode > 0 && !st->prev_redundancy)
         MUST_SUCCEED(celt_decoder_ctl(celt_dec, OPUS_RESET_STATE));
      /* Decode CELT */
      celt_ret = celt_decode_with_ec(celt_dec, decode_fec ? NULL : data,
                                     len, pcm, celt_frame_size, &dec, celt_accum);
   } else {
      unsigned char silence[2] = {0xFF, 0xFF};
      if (!celt_accum)
      {
         for (i=0;i<frame_size*st->channels;i++)
            pcm[i] = 0;
      }
      /* For hybrid -> SILK transitions, we let the CELT MDCT
         do a fade-out by decoding a silence frame */
      if (st->prev_mode == MODE_HYBRID && !(redundancy && celt_to_silk && st->prev_redundancy) )
      {
         MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_SET_START_BAND(0)));
         celt_decode_with_ec(celt_dec, silence, 2, pcm, F2_5, NULL, celt_accum);
      }
   }

   if (mode != MODE_CELT_ONLY && !celt_accum)
   {
      for (i=0;i<frame_size*st->channels;i++)
         pcm[i] = SAT16(ADD32(pcm[i], pcm_silk[i]));
   }

   {
      const CELTMode *celt_mode;
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_GET_MODE(&celt_mode)));
      window = celt_mode->window;
   }

   /* 5 ms redundant frame for SILK->CELT */
   if (redundancy && !celt_to_silk)
   {
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, OPUS_RESET_STATE));
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_SET_START_BAND(0)));

      celt_decode_with_ec(celt_dec, data+len, redundancy_bytes, redundant_audio, F5, NULL, 0);
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, OPUS_GET_FINAL_RANGE(&redundant_rng)));
      smooth_fade(pcm+st->channels*(frame_size-F2_5), redundant_audio+st->channels*F2_5,
                  pcm+st->channels*(frame_size-F2_5), F2_5, st->channels, window, st->Fs);
   }
   if (redundancy && celt_to_silk)
   {
      for (c=0;c<st->channels;c++)
      {
         for (i=0;i<F2_5;i++)
            pcm[st->channels*i+c] = redundant_audio[st->channels*i+c];
      }
      smooth_fade(redundant_audio+st->channels*F2_5, pcm+st->channels*F2_5,
                  pcm+st->channels*F2_5, F2_5, st->channels, window, st->Fs);
   }
   if (transition)
   {
      if (audiosize >= F5)
      {
         for (i=0;i<st->channels*F2_5;i++)
            pcm[i] = pcm_transition[i];
         smooth_fade(pcm_transition+st->channels*F2_5, pcm+st->channels*F2_5,
                     pcm+st->channels*F2_5, F2_5,
                     st->channels, window, st->Fs);
      } else {
         /* Not enough time to do a clean transition, but we do it anyway
            This will not preserve amplitude perfectly and may introduce
            a bit of temporal aliasing, but it shouldn't be too bad and
            that's pretty much the best we can do. In any case, generating this
            transition it pretty silly in the first place */
         smooth_fade(pcm_transition, pcm,
                     pcm, F2_5,
                     st->channels, window, st->Fs);
      }
   }

   if(st->decode_gain)
   {
      opus_val32 gain;
      gain = celt_exp2(MULT16_16_P15(QCONST16(6.48814081e-4f, 25), st->decode_gain));
      for (i=0;i<frame_size*st->channels;i++)
      {
         opus_val32 x;
         x = MULT16_32_P16(pcm[i],gain);
         pcm[i] = SATURATE(x, 32767);
      }
   }

   if (len <= 1)
      st->rangeFinal = 0;
   else
      st->rangeFinal = dec.rng ^ redundant_rng;

   st->prev_mode = mode;
   st->prev_redundancy = redundancy && !celt_to_silk;

   if (celt_ret>=0)
   {
      if (OPUS_CHECK_ARRAY(pcm, audiosize*st->channels))
         OPUS_PRINT_INT(audiosize);
   }

   RESTORE_STACK;
   return celt_ret < 0 ? celt_ret : audiosize;

}

int opus_decode_native(OpusDecoder *st, const unsigned char *data,
      int32_t len, opus_val16 *pcm, int frame_size, int decode_fec,
      int self_delimited, int32_t *packet_offset, int soft_clip)
{
//   log_i("len %i frame_size %i decode_fec %i self_delimited %i, packet_offset %i", len, frame_size, decode_fec, self_delimited, *packet_offset);
   int i, nb_samples;
   int count, offset;
   unsigned char toc;
   int packet_frame_size, packet_bandwidth, packet_mode, packet_stream_channels;
   /* 48 x 2.5 ms = 120 ms */
   int16_t size[48];
   VALIDATE_OPUS_DECODER(st);
   if (decode_fec<0 || decode_fec>1)
      return OPUS_BAD_ARG;
   /* For FEC/PLC, frame_size has to be to have a multiple of 2.5 ms */
   if ((decode_fec || len==0 || data==NULL) && frame_size%(st->Fs/400)!=0){
       log_i("OPUS_BAD_ARG %i", i);
       return OPUS_BAD_ARG;
   }

   if (len==0 || data==NULL)
   {
      int pcm_count=0;
      do {
         int ret;
         ret = opus_decode_frame(st, NULL, 0, pcm+pcm_count*st->channels, frame_size-pcm_count, 0);
         if (ret<0){
            log_i("ret %i", ret);
            return ret;
         }
         pcm_count += ret;
      } while (pcm_count < frame_size);
      celt_assert(pcm_count == frame_size);
      if (OPUS_CHECK_ARRAY(pcm, pcm_count*st->channels))
         OPUS_PRINT_INT(pcm_count);
      st->last_packet_duration = pcm_count;
      log_i("pcm_count %i", pcm_count);
      return pcm_count;
   } else if (len<0){
      log_i("OPUS_BAD_ARG %i", OPUS_BAD_ARG);
      return OPUS_BAD_ARG;
   }
   packet_mode = opus_packet_get_mode(data);
   packet_bandwidth = opus_packet_get_bandwidth(data);
   packet_frame_size = opus_packet_get_samples_per_frame(data, st->Fs);
   packet_stream_channels = opus_packet_get_nb_channels(data);

   count = opus_packet_parse_impl(data, len, self_delimited, &toc, NULL,
                                  size, &offset, packet_offset);
   if (count<0){
       log_i("count %i", count);
          return count;
   }
   data += offset;

   if (decode_fec)
   {
      int duration_copy;
      int ret;
      /* If no FEC can be present, run the PLC (recursive call) */
      if (frame_size < packet_frame_size || packet_mode == MODE_CELT_ONLY || st->mode == MODE_CELT_ONLY){
          ret = opus_decode_native(st, NULL, 0, pcm, frame_size, 0, 0, NULL, soft_clip);
          log_i("ret %i", ret);
          return ret;
      }
      /* Otherwise, run the PLC on everything except the size for which we might have FEC */
      duration_copy = st->last_packet_duration;
      if (frame_size-packet_frame_size!=0)
      {
         ret = opus_decode_native(st, NULL, 0, pcm, frame_size-packet_frame_size, 0, 0, NULL, soft_clip);
         if (ret<0)
         {
            st->last_packet_duration = duration_copy;
            log_i("ret %i", ret);
            return ret;
         }
         celt_assert(ret==frame_size-packet_frame_size);
      }
      /* Complete with FEC */
      st->mode = packet_mode;
      st->bandwidth = packet_bandwidth;
      st->frame_size = packet_frame_size;
      st->stream_channels = packet_stream_channels;
      ret = opus_decode_frame(st, data, size[0], pcm+st->channels*(frame_size-packet_frame_size),
            packet_frame_size, 1);
      if (ret<0){
          log_i("ret %i", ret);
          return ret;
      }

      else {
         if (OPUS_CHECK_ARRAY(pcm, frame_size*st->channels))
            OPUS_PRINT_INT(frame_size);
         st->last_packet_duration = frame_size;
         log_i("frame_size %i", frame_size);
         return frame_size;
      }
   }

   if (count*packet_frame_size > frame_size){
       log_i("OPUS_BUFFER_TOO_SMALL %i", OPUS_BUFFER_TOO_SMALL);
        return OPUS_BUFFER_TOO_SMALL;
   }



   /* Update the state as the last step to avoid updating it on an invalid packet */
   st->mode = packet_mode;
   st->bandwidth = packet_bandwidth;
   st->frame_size = packet_frame_size;
   st->stream_channels = packet_stream_channels;

   nb_samples=0;
   for (i=0;i<count;i++)
   {
      int ret;
      ret = opus_decode_frame(st, data, size[i], pcm+nb_samples*st->channels, frame_size-nb_samples, 0);
      if (ret<0){
          log_i("ret %i", ret);
          return ret;
      }

      celt_assert(ret==packet_frame_size);
      data += size[i];
      nb_samples += ret;
   }
   st->last_packet_duration = nb_samples;
   if (OPUS_CHECK_ARRAY(pcm, nb_samples*st->channels))
      OPUS_PRINT_INT(nb_samples);
//   log_i("len %i, nb_samples %i", len, nb_samples);
   return nb_samples;
}


int opus_decode(OpusDecoder *st, const unsigned char *data,
      int32_t len, opus_val16 *pcm, int frame_size, int decode_fec)
{
   log_i("len %i frame_size %i decode_fec %i", len, frame_size, decode_fec);
   if(frame_size<=0)
      return OPUS_BAD_ARG;
   return opus_decode_native(st, data, len, pcm, frame_size, decode_fec, 0, NULL, 0);
}



int opus_decoder_ctl(OpusDecoder *st, int request, ...)
{
   int ret = OPUS_OK;
   va_list ap;
   void *silk_dec;
   CELTDecoder *celt_dec;

   silk_dec = (char*)st+st->silk_dec_offset;
   celt_dec = (CELTDecoder*)((char*)st+st->celt_dec_offset);


   va_start(ap, request);

   switch (request)
   {
   case OPUS_GET_BANDWIDTH_REQUEST:
   {
      int32_t *value = va_arg(ap, int32_t*);
      if (!value)
      {
         goto bad_arg;
      }
      *value = st->bandwidth;
   }
   break;
   case OPUS_GET_FINAL_RANGE_REQUEST:
   {
      uint32_t *value = va_arg(ap, uint32_t*);
      if (!value)
      {
         goto bad_arg;
      }
      *value = st->rangeFinal;
   }
   break;
   case OPUS_RESET_STATE:
   {
      OPUS_CLEAR((char*)&st->OPUS_DECODER_RESET_START,
            sizeof(OpusDecoder)-
            ((char*)&st->OPUS_DECODER_RESET_START - (char*)st));

      celt_decoder_ctl(celt_dec, OPUS_RESET_STATE);
      silk_InitDecoder( silk_dec );
      st->stream_channels = st->channels;
      st->frame_size = st->Fs/400;
   }
   break;
   case OPUS_GET_SAMPLE_RATE_REQUEST:
   {
      int32_t *value = va_arg(ap, int32_t*);
      if (!value)
      {
         goto bad_arg;
      }
      *value = st->Fs;
   }
   break;
   case OPUS_GET_PITCH_REQUEST:
   {
      int32_t *value = va_arg(ap, int32_t*);
      if (!value)
      {
         goto bad_arg;
      }
      if (st->prev_mode == MODE_CELT_ONLY)
         ret = celt_decoder_ctl(celt_dec, OPUS_GET_PITCH(value));
      else
         *value = st->DecControl.prevPitchLag;
   }
   break;
   case OPUS_GET_GAIN_REQUEST:
   {
      int32_t *value = va_arg(ap, int32_t*);
      if (!value)
      {
         goto bad_arg;
      }
      *value = st->decode_gain;
   }
   break;
   case OPUS_SET_GAIN_REQUEST:
   {
       int32_t value = va_arg(ap, int32_t);
       if (value<-32768 || value>32767)
       {
          goto bad_arg;
       }
       st->decode_gain = value;
   }
   break;
   case OPUS_GET_LAST_PACKET_DURATION_REQUEST:
   {
      int32_t *value = va_arg(ap, int32_t*);
      if (!value)
      {
         goto bad_arg;
      }
      *value = st->last_packet_duration;
   }
   break;
   case OPUS_SET_PHASE_INVERSION_DISABLED_REQUEST:
   {
       int32_t value = va_arg(ap, int32_t);
       if(value<0 || value>1)
       {
          goto bad_arg;
       }
       ret = celt_decoder_ctl(celt_dec, OPUS_SET_PHASE_INVERSION_DISABLED(value));
   }
   break;
   case OPUS_GET_PHASE_INVERSION_DISABLED_REQUEST:
   {
       int32_t *value = va_arg(ap, int32_t*);
       if (!value)
       {
          goto bad_arg;
       }
       ret = celt_decoder_ctl(celt_dec, OPUS_GET_PHASE_INVERSION_DISABLED(value));
   }
   break;
   default:
      /*fprintf(stderr, "unknown opus_decoder_ctl() request: %d", request);*/
      ret = OPUS_UNIMPLEMENTED;
      break;
   }

   va_end(ap);
   return ret;
bad_arg:
   va_end(ap);
   return OPUS_BAD_ARG;
}

void opus_decoder_destroy(OpusDecoder *st)
{
   opus_free(st);
}


int opus_packet_get_bandwidth(const unsigned char *data)
{
   int bandwidth;
   if (data[0]&0x80)
   {
      bandwidth = OPUS_BANDWIDTH_MEDIUMBAND + ((data[0]>>5)&0x3);
      if (bandwidth == OPUS_BANDWIDTH_MEDIUMBAND)
         bandwidth = OPUS_BANDWIDTH_NARROWBAND;
   } else if ((data[0]&0x60) == 0x60)
   {
      bandwidth = (data[0]&0x10) ? OPUS_BANDWIDTH_FULLBAND :
                                   OPUS_BANDWIDTH_SUPERWIDEBAND;
   } else {
      bandwidth = OPUS_BANDWIDTH_NARROWBAND + ((data[0]>>5)&0x3);
   }
   return bandwidth;
}

int opus_packet_get_nb_channels(const unsigned char *data)
{
   return (data[0]&0x4) ? 2 : 1;
}

int opus_packet_get_nb_frames(const unsigned char packet[], int32_t len)
{
   int count;
   if (len<1)
      return OPUS_BAD_ARG;
   count = packet[0]&0x3;
   if (count==0)
      return 1;
   else if (count!=3)
      return 2;
   else if (len<2)
      return OPUS_INVALID_PACKET;
   else
      return packet[1]&0x3F;
}

int opus_packet_get_nb_samples(const unsigned char packet[], int32_t len,
      int32_t Fs)
{
   int samples;
   int count = opus_packet_get_nb_frames(packet, len);

   if (count<0)
      return count;

   samples = count*opus_packet_get_samples_per_frame(packet, Fs);
   /* Can't have more than 120 ms */
   if (samples*25 > Fs*3)
      return OPUS_INVALID_PACKET;
   else
      return samples;
}

int opus_decoder_get_nb_samples(const OpusDecoder *dec,
      const unsigned char packet[], int32_t len)
{
   return opus_packet_get_nb_samples(packet, len, dec->Fs);
}
