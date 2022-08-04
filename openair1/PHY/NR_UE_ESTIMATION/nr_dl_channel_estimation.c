/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.0  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */


#include <string.h>
#include "SCHED_NR_UE/defs.h"
#include "nr_estimation.h"
#include "PHY/NR_REFSIG/refsig_defs_ue.h"
#include "PHY/NR_REFSIG/nr_refsig.h"
#include "PHY/NR_REFSIG/dmrs_nr.h"
#include "PHY/NR_REFSIG/ptrs_nr.h"
#include "PHY/NR_TRANSPORT/nr_sch_dmrs.h"
#include "PHY/NR_TRANSPORT/nr_transport_proto.h"
#include "filt16a_32.h"
#include "T.h"
#include <openair1/PHY/TOOLS/phy_scope_interface.h>

//#define DEBUG_PDSCH
//#define DEBUG_PDCCH
//#define DEBUG_CH
//#define DEBUG_PRS_CHEST
//#define DEBUG_PRS_PRINTS
#define IDFT_INTERPOL_FACTOR 1
extern short nr_qpsk_mod_table[8];
int k_prime_table[4][12] = {
      {0,1,0,1,0,1,0,1,0,1,0,1},
      {0,2,1,3,0,2,1,3,0,2,1,3},
      {0,3,1,4,2,5,0,3,1,4,2,5},
      {0,6,3,9,1,7,4,10,2,8,5,11}};

static inline int abs32(int x)
{
  return (((int)((short*)&x)[0])*((int)((short*)&x)[0]) + ((int)((short*)&x)[1])*((int)((short*)&x)[1]));
}

int nr_prs_channel_estimation(uint8_t gNB_id,
                              uint8_t rsc_id,
                              uint8_t rep_num,
                              PHY_VARS_NR_UE *ue,
                              UE_nr_rxtx_proc_t *proc,
                              NR_DL_FRAME_PARMS *frame_params)
{
  int32_t **rxdataF      = ue->common_vars.common_vars_rx_data_per_thread[proc->thread_id].rxdataF;
  prs_data_t *prs_cfg    = &ue->prs_vars[gNB_id]->prs_resource[rsc_id].prs_cfg;
  prs_meas_t **prs_meas  = ue->prs_vars[gNB_id]->prs_resource[rsc_id].prs_meas;
  int32_t **prs_chestF   = ue->prs_vars[gNB_id]->prs_resource[rsc_id].prs_ch_estimates;
  int32_t **prs_chestT   = ue->prs_vars[gNB_id]->prs_resource[rsc_id].prs_ch_estimates_time;
  int slot_prs           = (proc->nr_slot_rx - rep_num*prs_cfg->PRSResourceTimeGap + frame_params->slots_per_frame)%frame_params->slots_per_frame;
  uint32_t **nr_gold_prs = ue->nr_gold_prs[gNB_id][rsc_id][slot_prs];
  
  uint8_t rxAnt = 0, idx = 0;
  int16_t *rxF, *pil, *fl, *fm, *fmm, *fml, *fmr, *fr, mod_prs[NR_MAX_PRS_LENGTH<<1];
  int16_t ch[2] = {0}, noiseFig[2] = {0};
  int16_t k_prime = 0, k = 0, re_offset = 0, first_half = 0, second_half = 0;
  int16_t *ch_tmp  = (int16_t *)malloc16_clear(frame_params->ofdm_symbol_size*sizeof(int32_t));
  AssertFatal((ch_tmp != NULL), "[%s] channel estimate buffer initialization failed!!", __FUNCTION__);
  int16_t *chF_interpol  = (int16_t *)malloc16_clear(IDFT_INTERPOL_FACTOR*frame_params->ofdm_symbol_size*sizeof(int32_t));
  AssertFatal((chF_interpol != NULL), "[%s] channel estimate buffer initialization failed!!", __FUNCTION__);
  int16_t *chT_interpol  = (int16_t *)malloc16_clear(IDFT_INTERPOL_FACTOR*frame_params->ofdm_symbol_size*sizeof(int32_t));
  AssertFatal((chT_interpol != NULL), "[%s] channel estimate buffer initialization failed!!", __FUNCTION__);
  int32_t ch_pwr = 0, snr = 0;
#ifdef DEBUG_PRS_CHEST
  char filename[64] = {0}, varname[64] = {0};
#endif
  int16_t *ch_init     = ch_tmp;
  int16_t scale_factor = (1.0f/(float)(prs_cfg->NumPRSSymbols))*(1<<15);
  int16_t num_pilots   = (12/prs_cfg->CombSize)*prs_cfg->NumRB;
  int16_t start_offset = (IDFT_INTERPOL_FACTOR-1)*frame_params->ofdm_symbol_size>>1;

  for(int l = prs_cfg->SymbolStart; l < prs_cfg->SymbolStart+prs_cfg->NumPRSSymbols; l++)
  {
    int symInd = l-prs_cfg->SymbolStart;
    if (prs_cfg->CombSize == 2) {
      k_prime = k_prime_table[0][symInd];
    }
    else if (prs_cfg->CombSize == 4){
      k_prime = k_prime_table[1][symInd];
    }
    else if (prs_cfg->CombSize == 6){
      k_prime = k_prime_table[2][symInd];
    }
    else if (prs_cfg->CombSize == 12){
      k_prime = k_prime_table[3][symInd];
    }
   
#ifdef DEBUG_PRS_PRINTS 
    printf("[gNB %d][rsc %d] PRS config l %d k_prime %d:\nprs_cfg->SymbolStart %d\nprs_cfg->NumPRSSymbols %d\nprs_cfg->NumRB %d\nprs_cfg->CombSize %d\n", gNB_id, rsc_id, l, k_prime, prs_cfg->SymbolStart, prs_cfg->NumPRSSymbols, prs_cfg->NumRB, prs_cfg->CombSize);
#endif
    // Pilots generation and modulation
    for (int m = 0; m < num_pilots; m++) 
    {
      idx = (((nr_gold_prs[l][(m<<1)>>5])>>((m<<1)&0x1f))&3);
      mod_prs[m<<1]     = nr_qpsk_mod_table[idx<<1];
      mod_prs[(m<<1)+1] = nr_qpsk_mod_table[(idx<<1) + 1];
    } 
     
    for (rxAnt=0; rxAnt < frame_params->nb_antennas_rx; rxAnt++)
    {
      snr = 0;
      
      // calculate RE offset
      k = re_offset = (prs_cfg->REOffset+k_prime) % prs_cfg->CombSize + prs_cfg->RBOffset*12 + frame_params->first_carrier_offset;

      // Channel estimation and interpolation
      pil       = (int16_t *)&mod_prs[0];
      rxF       = (int16_t *)&rxdataF[rxAnt][l*frame_params->ofdm_symbol_size + k];
      //memset(ch_tmp,0,sizeof(int32_t)*(ue->frame_parms.ofdm_symbol_size));
      
      if(prs_cfg->CombSize == 2)
      {
        // Choose the interpolation filters
        switch (k_prime) {
          case 0:
            fl  = filt8_l0;
            fml = filt8_m0;
            fmm = filt8_mm0;
            fmr = filt8_mr0;
            fm  = filt8_m0;
            fr  = filt8_r0;
            break;

          case 1:
            fl  = filt8_l1;
            fmm = filt8_mm1;
            fml = filt8_ml1;
            fmr = fmm;
            fm  = filt8_m1;
            fr  = filt8_r1;
            break;

          default:
            LOG_I(PHY, "%s: ERROR!! Invalid k_prime=%d for PRS comb_size %d, symbol %d\n",__FUNCTION__, k_prime, prs_cfg->CombSize, l);
            return(-1);
            break;
        }

        //Start pilot
        ch[0] = (int16_t)(((int32_t)rxF[0]*pil[0] + (int32_t)rxF[1]*pil[1])>>15);
        ch[1] = (int16_t)(((int32_t)rxF[1]*pil[0] - (int32_t)rxF[0]*pil[1])>>15);
        multadd_real_vector_complex_scalar(fl,
		    	       ch,
		    	       ch_tmp,
		    	       8);

        //SNR estimation
        noiseFig[0] = rxF[0] - (int16_t)(((int32_t)ch[0]*pil[0] - (int32_t)ch[1]*pil[1])>>15);
        noiseFig[1] = rxF[1] - (int16_t)(((int32_t)ch[1]*pil[0] + (int32_t)ch[0]*pil[1])>>15);
        snr += 10*log10(abs32(*((int*)rxF)) - abs32(*((int*)noiseFig))) - 10*log10(abs32(*((int*)noiseFig)));
#ifdef DEBUG_PRS_PRINTS
        printf("[Rx %d] pilot %3d, SNR %+2d dB: rxF - > (%+3d, %+3d) addr %p  ch -> (%+3d, %+3d), pil -> (%+d, %+d) \n", rxAnt, 0, snr, rxF[0],rxF[1],&rxF[0],ch[0],ch[1],pil[0],pil[1]);
#endif
        pil +=2;
        k   = (k+prs_cfg->CombSize) % frame_params->ofdm_symbol_size;
        rxF = (int16_t *)&rxdataF[rxAnt][l*frame_params->ofdm_symbol_size + k];
        
        //Middle pilots
        for(int pIdx = 1; pIdx < num_pilots-1; pIdx+=2)
        {
          ch[0] = (int16_t)(((int32_t)rxF[0]*pil[0] + (int32_t)rxF[1]*pil[1])>>15);
          ch[1] = (int16_t)(((int32_t)rxF[1]*pil[0] - (int32_t)rxF[0]*pil[1])>>15);
          if(pIdx == 1) // 2nd pilot
          {
            multadd_real_vector_complex_scalar(fml,
		        	       ch,
		        	       ch_tmp,
		        	       8);
          }
          else
          {
            multadd_real_vector_complex_scalar(fm,
		        	       ch,
		        	       ch_tmp,
		        	       8);
          }
          
          //SNR estimation
          noiseFig[0] = rxF[0] - (int16_t)(((int32_t)ch[0]*pil[0] - (int32_t)ch[1]*pil[1])>>15);
          noiseFig[1] = rxF[1] - (int16_t)(((int32_t)ch[1]*pil[0] + (int32_t)ch[0]*pil[1])>>15);
          snr += 10*log10(abs32(*((int*)rxF)) - abs32(*((int*)noiseFig))) - 10*log10(abs32(*((int*)noiseFig)));
#ifdef DEBUG_PRS_PRINTS
          printf("[Rx %d] pilot %3d, SNR %+2d dB: rxF - > (%+3d, %+3d) addr %p  ch -> (%+3d, %+3d), pil -> (%+d, %+d) \n", rxAnt, pIdx, snr/(pIdx+1), rxF[0],rxF[1],&rxF[0],ch[0],ch[1],pil[0],pil[1]);
#endif
          pil +=2;
          k   = (k+prs_cfg->CombSize) % frame_params->ofdm_symbol_size;
          rxF = (int16_t *)&rxdataF[rxAnt][l*frame_params->ofdm_symbol_size + k];

          ch[0] = (int16_t)(((int32_t)rxF[0]*pil[0] + (int32_t)rxF[1]*pil[1])>>15);
          ch[1] = (int16_t)(((int32_t)rxF[1]*pil[0] - (int32_t)rxF[0]*pil[1])>>15);
          if(pIdx == (num_pilots-3)) // 2nd last pilot
          {
            multadd_real_vector_complex_scalar(fmr,
		        	       ch,
		        	       ch_tmp,
		        	       8);
          }
          else
          {
            multadd_real_vector_complex_scalar(fmm,
		        	       ch,
		        	       ch_tmp,
		        	       8);
          }
          
          //SNR estimation
          noiseFig[0] = rxF[0] - (int16_t)(((int32_t)ch[0]*pil[0] - (int32_t)ch[1]*pil[1])>>15);
          noiseFig[1] = rxF[1] - (int16_t)(((int32_t)ch[1]*pil[0] + (int32_t)ch[0]*pil[1])>>15);
          snr += 10*log10(abs32(*((int*)rxF)) - abs32(*((int*)noiseFig))) - 10*log10(abs32(*((int*)noiseFig)));
#ifdef DEBUG_PRS_PRINTS
          printf("[Rx %d] pilot %3d, SNR %+2d dB: rxF - > (%+3d, %+3d) addr %p  ch -> (%+3d, %+3d), pil -> (%+d, %+d) \n", rxAnt, pIdx+1, snr/(pIdx+2), rxF[0],rxF[1],&rxF[0],ch[0],ch[1],pil[0],pil[1]);
#endif
          pil +=2;
          k   = (k+prs_cfg->CombSize) % frame_params->ofdm_symbol_size;
          rxF = (int16_t *)&rxdataF[rxAnt][l*frame_params->ofdm_symbol_size + k];
          ch_tmp +=8;
        }

        //End pilot
        ch[0] = (int16_t)(((int32_t)rxF[0]*pil[0] + (int32_t)rxF[1]*pil[1])>>15);
        ch[1] = (int16_t)(((int32_t)rxF[1]*pil[0] - (int32_t)rxF[0]*pil[1])>>15);
        multadd_real_vector_complex_scalar(fr,
	      	       ch,
	      	       ch_tmp,
	      	       8);

          //SNR estimation
          noiseFig[0] = rxF[0] - (int16_t)(((int32_t)ch[0]*pil[0] - (int32_t)ch[1]*pil[1])>>15);
          noiseFig[1] = rxF[1] - (int16_t)(((int32_t)ch[1]*pil[0] + (int32_t)ch[0]*pil[1])>>15);
          snr += 10*log10(abs32(*((int*)rxF)) - abs32(*((int*)noiseFig))) - 10*log10(abs32(*((int*)noiseFig)));
#ifdef DEBUG_PRS_PRINTS
          printf("[Rx %d] pilot %3d, SNR %+2d dB: rxF - > (%+3d, %+3d) addr %p  ch -> (%+3d, %+3d), pil -> (%+d, %+d) \n", rxAnt, num_pilots-1, snr/num_pilots, rxF[0],rxF[1],&rxF[0],ch[0],ch[1],pil[0],pil[1]);
#endif
          // average out the SNR computed
          snr = snr/num_pilots;
          prs_meas[rxAnt]->snr = snr;
      }
      else if(prs_cfg->CombSize == 4)
      {
        // Choose the interpolation filters
        switch (k_prime) {
          case 0:
            fl  = filt16a_l0;
            fml = filt16a_mm0;
            fmm = filt16a_mm0;
            fmr = filt16a_m0;
            fm  = filt16a_m0;
            fr  = filt16a_r0;
            break;

          case 1:
            fl  = filt16a_l1;
            fml = filt16a_ml1;
            fmm = filt16a_mm1;
            fmr = filt16a_mr1;
            fm  = filt16a_m1;
            fr  = filt16a_r1;
            break;

          case 2:
            fl  = filt16a_l2;
            fml = filt16a_ml2;
            fmm = filt16a_mm2;
            fmr = filt16a_mr2;
            fm  = filt16a_m2;
            fr  = filt16a_r2;
            break;

          case 3:
            fl  = filt16a_l3;
            fml = filt16a_ml3;
            fmm = filt16a_mm3;
            fmr = filt16a_mm3;
            fm  = filt16a_m3;
            fr  = filt16a_r3;
            break;

          default:
            LOG_I(PHY, "%s: ERROR!! Invalid k_prime=%d for PRS comb_size %d, symbol %d\n",__FUNCTION__, k_prime, prs_cfg->CombSize, l);
            return(-1);
            break;
        }

        //Start pilot
        ch[0] = (int16_t)(((int32_t)rxF[0]*pil[0] + (int32_t)rxF[1]*pil[1])>>15);
        ch[1] = (int16_t)(((int32_t)rxF[1]*pil[0] - (int32_t)rxF[0]*pil[1])>>15);
        multadd_real_vector_complex_scalar(fl,
	      	       ch,
	      	       ch_tmp,
	      	       16);
        
        //SNR estimation
        noiseFig[0] = rxF[0] - (int16_t)(((int32_t)ch[0]*pil[0] - (int32_t)ch[1]*pil[1])>>15);
        noiseFig[1] = rxF[1] - (int16_t)(((int32_t)ch[1]*pil[0] + (int32_t)ch[0]*pil[1])>>15);
        snr += 10*log10(abs32(*((int*)rxF)) - abs32(*((int*)noiseFig))) - 10*log10(abs32(*((int*)noiseFig)));
#ifdef DEBUG_PRS_PRINTS
        printf("[Rx %d] pilot %3d, SNR %+2d dB: rxF - > (%+3d, %+3d) addr %p  ch -> (%+3d, %+3d), pil -> (%+d, %+d) \n", rxAnt, 0, snr, rxF[0],rxF[1],&rxF[0],ch[0],ch[1],pil[0],pil[1]);
#endif
        pil +=2;
        k   = (k+prs_cfg->CombSize) % frame_params->ofdm_symbol_size;
        rxF = (int16_t *)&rxdataF[rxAnt][l*frame_params->ofdm_symbol_size + k];

        ch[0] = (int16_t)(((int32_t)rxF[0]*pil[0] + (int32_t)rxF[1]*pil[1])>>15);
        ch[1] = (int16_t)(((int32_t)rxF[1]*pil[0] - (int32_t)rxF[0]*pil[1])>>15);
        multadd_real_vector_complex_scalar(fml,
	      	       ch,
	      	       ch_tmp,
	      	       16);

        //SNR estimation
        noiseFig[0] = rxF[0] - (int16_t)(((int32_t)ch[0]*pil[0] - (int32_t)ch[1]*pil[1])>>15);
        noiseFig[1] = rxF[1] - (int16_t)(((int32_t)ch[1]*pil[0] + (int32_t)ch[0]*pil[1])>>15);
        snr += 10*log10(abs32(*((int*)rxF)) - abs32(*((int*)noiseFig))) - 10*log10(abs32(*((int*)noiseFig)));
#ifdef DEBUG_PRS_PRINTS
        printf("[Rx %d] pilot %3d, SNR %+2d dB: rxF - > (%+3d, %+3d) addr %p  ch -> (%+3d, %+3d), pil -> (%+d, %+d) \n", rxAnt, 1, snr/2, rxF[0],rxF[1],&rxF[0],ch[0],ch[1],pil[0],pil[1]);
#endif
        pil +=2;
        k   = (k+prs_cfg->CombSize) % frame_params->ofdm_symbol_size;
        rxF = (int16_t *)&rxdataF[rxAnt][l*frame_params->ofdm_symbol_size + k];
        ch_tmp +=8;

        //Middle pilots
        for(int pIdx = 2; pIdx < num_pilots-2; pIdx++)
        {
          ch[0] = (int16_t)(((int32_t)rxF[0]*pil[0] + (int32_t)rxF[1]*pil[1])>>15);
          ch[1] = (int16_t)(((int32_t)rxF[1]*pil[0] - (int32_t)rxF[0]*pil[1])>>15);
          multadd_real_vector_complex_scalar(fmm,
	      	         ch,
	      	         ch_tmp,
	      	         16);
        
          //SNR estimation
          noiseFig[0] = rxF[0] - (int16_t)(((int32_t)ch[0]*pil[0] - (int32_t)ch[1]*pil[1])>>15);
          noiseFig[1] = rxF[1] - (int16_t)(((int32_t)ch[1]*pil[0] + (int32_t)ch[0]*pil[1])>>15);
          snr += 10*log10(abs32(*((int*)rxF)) - abs32(*((int*)noiseFig))) - 10*log10(abs32(*((int*)noiseFig)));
#ifdef DEBUG_PRS_PRINTS
          printf("[Rx %d] pilot %3d, SNR %+2d dB: rxF - > (%+3d, %+3d) addr %p  ch -> (%+3d, %+3d), pil -> (%+d, %+d) \n", rxAnt, pIdx, snr/(pIdx+1), rxF[0],rxF[1],&rxF[0],ch[0],ch[1],pil[0],pil[1]);
#endif
          pil +=2;
          k   = (k+prs_cfg->CombSize) % frame_params->ofdm_symbol_size;
          rxF = (int16_t *)&rxdataF[rxAnt][l*frame_params->ofdm_symbol_size + k];
          ch_tmp +=8;
        }

        //End pilot
        ch[0] = (int16_t)(((int32_t)rxF[0]*pil[0] + (int32_t)rxF[1]*pil[1])>>15);
        ch[1] = (int16_t)(((int32_t)rxF[1]*pil[0] - (int32_t)rxF[0]*pil[1])>>15);
        multadd_real_vector_complex_scalar(fmr,
	      	       ch,
	      	       ch_tmp,
	      	       16);
        
        //SNR estimation
        noiseFig[0] = rxF[0] - (int16_t)(((int32_t)ch[0]*pil[0] - (int32_t)ch[1]*pil[1])>>15);
        noiseFig[1] = rxF[1] - (int16_t)(((int32_t)ch[1]*pil[0] + (int32_t)ch[0]*pil[1])>>15);
        snr += 10*log10(abs32(*((int*)rxF)) - abs32(*((int*)noiseFig))) - 10*log10(abs32(*((int*)noiseFig)));
#ifdef DEBUG_PRS_PRINTS
        printf("[Rx %d] pilot %3d, SNR %+2d dB: rxF - > (%+3d, %+3d) addr %p  ch -> (%+3d, %+3d), pil -> (%+d, %+d) \n", rxAnt, num_pilots-2, snr/(num_pilots-1), rxF[0],rxF[1],&rxF[0],ch[0],ch[1],pil[0],pil[1]);
#endif
        pil +=2;
        k   = (k+prs_cfg->CombSize) % frame_params->ofdm_symbol_size;
        rxF = (int16_t *)&rxdataF[rxAnt][l*frame_params->ofdm_symbol_size + k];

        ch[0] = (int16_t)(((int32_t)rxF[0]*pil[0] + (int32_t)rxF[1]*pil[1])>>15);
        ch[1] = (int16_t)(((int32_t)rxF[1]*pil[0] - (int32_t)rxF[0]*pil[1])>>15);
        multadd_real_vector_complex_scalar(fr,
	      	       ch,
	      	       ch_tmp,
	      	       16);
        
        //SNR estimation
        noiseFig[0] = rxF[0] - (int16_t)(((int32_t)ch[0]*pil[0] - (int32_t)ch[1]*pil[1])>>15);
        noiseFig[1] = rxF[1] - (int16_t)(((int32_t)ch[1]*pil[0] + (int32_t)ch[0]*pil[1])>>15);
        snr += 10*log10(abs32(*((int*)rxF)) - abs32(*((int*)noiseFig))) - 10*log10(abs32(*((int*)noiseFig)));
#ifdef DEBUG_PRS_PRINTS
        printf("[Rx %d] pilot %3d, SNR %+2d dB: rxF - > (%+3d, %+3d) addr %p  ch -> (%+3d, %+3d), pil -> (%+d, +%d) \n", rxAnt, num_pilots-1, snr/num_pilots, rxF[0],rxF[1],&rxF[0],ch[0],ch[1],pil[0],pil[1]);
#endif
          // average out the SNR computed
          snr = snr/num_pilots;
          prs_meas[rxAnt]->snr = snr;
      }
      else
      {
        AssertFatal((prs_cfg->CombSize == 2)||(prs_cfg->CombSize == 4), "[%s] DL PRS CombSize other than 2 and 4 are NOT supported currently. Exiting!!!", __FUNCTION__);
      }

      //reset channel pointer
      ch_tmp = ch_init;
    } // for rxAnt
  } //for l
  
  for (rxAnt=0; rxAnt < frame_params->nb_antennas_rx; rxAnt++)
  {
    // scale by averaging factor 1/NumPrsSymbols
    multadd_complex_vector_real_scalar(ch_tmp,
                                       scale_factor,
                                       ch_tmp,
                                       1,
                                       frame_params->ofdm_symbol_size);

#ifdef DEBUG_PRS_PRINTS
    for (int rb = 0; rb < prs_cfg->NumRB; rb++)
    {
      printf("================================================================\n");
      printf("\t\t\t[gNB %d][Rx %d][RB %d]\n", gNB_id, rxAnt, rb);
      printf("================================================================\n");
      idx = (12*rb)<<1;
      printf("%4d %4d  %4d %4d  %4d %4d  %4d %4d  %4d %4d  %4d %4d\n", ch_tmp[idx], ch_tmp[idx+1], ch_tmp[idx+2], ch_tmp[idx+3], ch_tmp[idx+4], ch_tmp[idx+5], ch_tmp[idx+6], ch_tmp[idx+7], ch_tmp[idx+8], ch_tmp[idx+9], ch_tmp[idx+10], ch_tmp[idx+11]);
      printf("%4d %4d  %4d %4d  %4d %4d  %4d %4d  %4d %4d  %4d %4d\n", ch_tmp[idx+12], ch_tmp[idx+13], ch_tmp[idx+14], ch_tmp[idx+15], ch_tmp[idx+16], ch_tmp[idx+17], ch_tmp[idx+18], ch_tmp[idx+19], ch_tmp[idx+20], ch_tmp[idx+21], ch_tmp[idx+22], ch_tmp[idx+23]);
      printf("\n");
    }
#endif

    // Place PRS channel estimates in FFT shifted format
    first_half  = frame_params->ofdm_symbol_size - re_offset;
    second_half = (prs_cfg->NumRB*12) - first_half;
    if(first_half > 0)
      memcpy((int16_t *)&prs_chestF[rxAnt][re_offset], &ch_tmp[0], first_half*sizeof(int32_t));
    if(second_half > 0)
      memcpy((int16_t *)&prs_chestF[rxAnt][0], &ch_tmp[(first_half<<1)], second_half*sizeof(int32_t));

    memcpy((int16_t *)&chF_interpol[start_offset+re_offset], &ch_tmp[0], first_half*sizeof(int32_t));
    memcpy((int16_t *)&chF_interpol[start_offset], &ch_tmp[(first_half<<1)], second_half*sizeof(int32_t));

    // Time domain IMPULSE response
    idft_size_idx_t idftsizeidx;
    switch (IDFT_INTERPOL_FACTOR*frame_params->ofdm_symbol_size) {
    case 128:
      idftsizeidx = IDFT_128;
      break;

    case 256:
      idftsizeidx = IDFT_256;
      break;

    case 512:
      idftsizeidx = IDFT_512;
      break;

    case 768:
      idftsizeidx = IDFT_768;
      break;

    case 1024:
      idftsizeidx = IDFT_1024;
      break;

    case 1536:
      idftsizeidx = IDFT_1536;
      break;

    case 2048:
      idftsizeidx = IDFT_2048;
      break;

    case 3072:
      idftsizeidx = IDFT_3072;
      break;

    case 4096:
      idftsizeidx = IDFT_4096;
      break;

    case 8192:
      idftsizeidx = IDFT_8192;
      break;

    case 12288:
      idftsizeidx = IDFT_12288;
      break;

    case 16384:
      idftsizeidx = IDFT_16384;
      break;

    case 24576:
      idftsizeidx = IDFT_24576;
      break;

    case 32768:
      idftsizeidx = IDFT_32768;
      break;

    case 49152:
      idftsizeidx = IDFT_49152;
      break;

    case 65536:
      idftsizeidx = IDFT_65536;
      break;

    default:
      LOG_I(PHY, "%s: unsupported ofdm symbol size \n", __FUNCTION__);
      assert(0);
    }

    idft(idftsizeidx,
         (int16_t *)&chF_interpol[0],
         (int16_t *)&chT_interpol[0],1);

    // rearrange impulse response
    memcpy((int16_t *)&prs_chestT[rxAnt][0], &chT_interpol[IDFT_INTERPOL_FACTOR*frame_params->ofdm_symbol_size], (frame_params->ofdm_symbol_size>>1)*sizeof(int32_t));
    memcpy((int16_t *)&prs_chestT[rxAnt][(frame_params->ofdm_symbol_size>>1)], &chT_interpol[start_offset], (frame_params->ofdm_symbol_size>>1)*sizeof(int32_t));

    // peak estimator
    peak_estimator(&prs_chestT[rxAnt][0],
                   frame_params->ofdm_symbol_size,
                   &prs_meas[rxAnt]->dl_toa,
                   &ch_pwr);

    //prs measurements
    prs_meas[rxAnt]->dl_toa    -= (frame_params->ofdm_symbol_size>>1);
    prs_meas[rxAnt]->gNB_id     = gNB_id;
    prs_meas[rxAnt]->sfn        = proc->frame_rx;
    prs_meas[rxAnt]->slot       = proc->nr_slot_rx;
    prs_meas[rxAnt]->rxAnt_idx  = rxAnt;
    prs_meas[rxAnt]->dl_aoa     = 0;
    LOG_I(PHY, "[gNB %d][rsc %d][Rx %d][sfn %d][slot %d] DL PRS ToA ==> %d / %d samples, peak channel power %.1f dBm, SNR %+2d dB\n", gNB_id, rsc_id, rxAnt, proc->frame_rx, proc->nr_slot_rx, prs_meas[rxAnt]->dl_toa, frame_params->ofdm_symbol_size, 10*log10(ch_pwr/frame_params->ofdm_symbol_size)-30, prs_meas[rxAnt]->snr);

#ifdef DEBUG_PRS_CHEST
    sprintf(filename, "%s%i%s", "PRSpilot_", rxAnt, ".m");
    LOG_M(filename, "prs_loc", &mod_prs[0], prs_cfg->NumRB*(12/prs_cfg->CombSize),1,1);
    sprintf(filename, "%s%i%s", "rxSigF_", rxAnt, ".m");
    sprintf(varname, "%s%i", "rxF_", rxAnt);
    LOG_M(filename, varname, &rxdataF[rxAnt][0], prs_cfg->NumPRSSymbols*frame_params->ofdm_symbol_size,1,1);
    sprintf(filename, "%s%i%s", "prsChestF_", rxAnt, ".m");
    sprintf(varname, "%s%i", "prsChF_", rxAnt);
    LOG_M(filename, varname, &prs_chestF[rxAnt][0], frame_params->ofdm_symbol_size,1,1);
    sprintf(filename, "%s%i%s", "prsChestT_", rxAnt, ".m");
    sprintf(varname, "%s%i", "prsChT_", rxAnt);
    LOG_M(filename, varname, &prs_chestT[rxAnt][0], frame_params->ofdm_symbol_size,1,1);
#endif

    // T tracer dump
    T(T_UE_PHY_INPUT_SIGNAL, T_INT(gNB_id),
      T_INT(proc->frame_rx), T_INT(proc->nr_slot_rx),
      T_INT(rxAnt), T_BUFFER(&rxdataF[rxAnt][0], frame_params->samples_per_slot_wCP*sizeof(int32_t)));

    T(T_UE_PHY_DL_CHANNEL_ESTIMATE_FREQ, T_INT(gNB_id), T_INT(rsc_id),
      T_INT(proc->frame_rx), T_INT(proc->nr_slot_rx),
      T_INT(rxAnt), T_BUFFER(&prs_chestF[rxAnt][0], frame_params->ofdm_symbol_size*sizeof(int32_t)));

    T(T_UE_PHY_DL_CHANNEL_ESTIMATE, T_INT(gNB_id), T_INT(rsc_id),
      T_INT(proc->frame_rx), T_INT(proc->nr_slot_rx),
      T_INT(rxAnt), T_BUFFER(&prs_chestT[rxAnt][0], frame_params->ofdm_symbol_size*sizeof(int32_t)));
  }

  free(ch_tmp);
  free(chF_interpol);
  free(chT_interpol);
  return(0);
}


/* Generic function to find the peak of channel estimation buffer */
void peak_estimator(int32_t *buffer, int32_t buf_len, int32_t *peak_idx, int32_t *peak_val)
{
  int32_t max_val = 0, max_idx = 0, abs_val = 0;
  for(int k = 0; k < buf_len; k++)
  {
    abs_val = abs32(buffer[k]);
    if(abs_val > max_val)
    {
      max_val = abs_val;
      max_idx = k;
    }
  }
  *peak_val = max_val;
  *peak_idx = max_idx;
}

int nr_pbch_dmrs_correlation(PHY_VARS_NR_UE *ue,
                             UE_nr_rxtx_proc_t *proc,
                             uint8_t gNB_id,
                             unsigned char Ns,
                             unsigned char symbol,
                             int dmrss,
                             NR_UE_SSB *current_ssb)
{
  int pilot[200] __attribute__((aligned(16)));
  unsigned short k;
  unsigned int pilot_cnt;
  int16_t ch[2],*pil,*rxF;
  int symbol_offset;


  uint8_t nushift;
  uint8_t ssb_index=current_ssb->i_ssb;
  uint8_t n_hf=current_ssb->n_hf;
  int **rxdataF=ue->common_vars.common_vars_rx_data_per_thread[proc->thread_id].rxdataF;

  nushift =  ue->frame_parms.Nid_cell%4;
  ue->frame_parms.nushift = nushift;
  unsigned int  ssb_offset = ue->frame_parms.first_carrier_offset + ue->frame_parms.ssb_start_subcarrier;
  if (ssb_offset>= ue->frame_parms.ofdm_symbol_size) ssb_offset-=ue->frame_parms.ofdm_symbol_size;

  AssertFatal(dmrss >= 0 && dmrss < 3,
	      "symbol %d is illegal for PBCH DM-RS \n",
	      dmrss);

  symbol_offset = ue->frame_parms.ofdm_symbol_size*symbol;


  k = nushift;

#ifdef DEBUG_CH
  printf("PBCH DMRS Correlation : ThreadId %d, gNB_id %d , OFDM size %d, Ncp=%d, Ns=%d, k=%d symbol %d\n",proc->thread_id, gNB_id,ue->frame_parms.ofdm_symbol_size,
         ue->frame_parms.Ncp,Ns,k, symbol);
#endif

  // generate pilot
  nr_pbch_dmrs_rx(dmrss,ue->nr_gold_pbch[n_hf][ssb_index], &pilot[0]);

  for (int aarx=0; aarx<ue->frame_parms.nb_antennas_rx; aarx++) {

    int re_offset = ssb_offset;
    pil   = (int16_t *)&pilot[0];
    rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];

#ifdef DEBUG_CH
    printf("pbch ch est pilot addr %p RB_DL %d\n",&pilot[0], ue->frame_parms.N_RB_DL);
    printf("k %d, first_carrier %d\n",k,ue->frame_parms.first_carrier_offset);
    printf("rxF addr %p\n", rxF);
#endif
    //if ((ue->frame_parms.N_RB_DL&1)==0) {

    // Treat first 2 pilots specially (left edge)
    ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
    ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

    current_ssb->c_re += ch[0];
    current_ssb->c_im += ch[1];

#ifdef DEBUG_CH
    printf("ch 0 %d\n",((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1]));
    printf("pilot 0 : rxF - > (%d,%d) addr %p  ch -> (%d,%d), pil -> (%d,%d) \n",rxF[0],rxF[1],&rxF[0],ch[0],ch[1],pil[0],pil[1]);
#endif

    pil += 2;
    re_offset = (re_offset+4) % ue->frame_parms.ofdm_symbol_size;
    rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];


    ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
    ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

    current_ssb->c_re += ch[0];
    current_ssb->c_im += ch[1];

#ifdef DEBUG_CH
    printf("pilot 1 : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif

    pil += 2;
    re_offset = (re_offset+4) % ue->frame_parms.ofdm_symbol_size;
    rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];

    current_ssb->c_re += ch[0];
    current_ssb->c_im += ch[1];

#ifdef DEBUG_CH
    printf("pilot 2 : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif

    pil += 2;
    re_offset = (re_offset+4) % ue->frame_parms.ofdm_symbol_size;
    rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];

    for (pilot_cnt=3; pilot_cnt<(3*20); pilot_cnt += 3) {

      //	if (pilot_cnt == 30)
      //	  rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k)];

      // in 2nd symbol, skip middle  REs (48 with DMRS,  144 for SSS, and another 48 with DMRS) 
      if (dmrss == 1 && pilot_cnt == 12) {
	pilot_cnt=48;
	re_offset = (re_offset+144) % ue->frame_parms.ofdm_symbol_size;
	rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];
      }
      ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
      
      current_ssb->c_re += ch[0];
      current_ssb->c_im += ch[1];

#ifdef DEBUG_CH
      printf("pilot %u : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt,rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif

      pil += 2;
      re_offset = (re_offset+4) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];
        
  
      ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

      current_ssb->c_re += ch[0];
      current_ssb->c_im += ch[1];

#ifdef DEBUG_CH
      printf("pilot %u : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt+1,rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif
      pil += 2;
      re_offset = (re_offset+4) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];
        

      ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

      current_ssb->c_re += ch[0];
      current_ssb->c_im += ch[1];

#ifdef DEBUG_CH
      printf("pilot %u : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt+2,rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif

      pil += 2;
      re_offset = (re_offset+4) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];

    }


    //}

  }
  return(0);
}


int nr_pbch_channel_estimation(PHY_VARS_NR_UE *ue,
                               int estimateSz,
                               struct complex16 dl_ch_estimates [][estimateSz],
                               struct complex16 dl_ch_estimates_time [][ue->frame_parms.ofdm_symbol_size],
                               UE_nr_rxtx_proc_t *proc,
                               uint8_t gNB_id,
                               unsigned char Ns,
                               unsigned char symbol,
                               int dmrss,
                               uint8_t ssb_index,
                               uint8_t n_hf)
{
  int pilot[200] __attribute__((aligned(16)));
  unsigned short k;
  unsigned int pilot_cnt;
  int16_t *pil,*rxF,*dl_ch,*fl,*fm,*fr;
  int ch_offset,symbol_offset;
  //int slot_pbch;

  uint8_t nushift;
   int **rxdataF=ue->common_vars.common_vars_rx_data_per_thread[proc->thread_id].rxdataF;

  nushift =  ue->frame_parms.Nid_cell%4;
  ue->frame_parms.nushift = nushift;
  unsigned int  ssb_offset = ue->frame_parms.first_carrier_offset + ue->frame_parms.ssb_start_subcarrier;
  if (ssb_offset>= ue->frame_parms.ofdm_symbol_size) ssb_offset-=ue->frame_parms.ofdm_symbol_size;

  ch_offset     = ue->frame_parms.ofdm_symbol_size*symbol;

  AssertFatal(dmrss >= 0 && dmrss < 3,
	      "symbol %d is illegal for PBCH DM-RS \n",
	      dmrss);

  symbol_offset = ue->frame_parms.ofdm_symbol_size*symbol;


  k = nushift;

#ifdef DEBUG_CH
  printf("PBCH Channel Estimation : ThreadId %d, gNB_id %d ch_offset %d, OFDM size %d, Ncp=%d, Ns=%d, k=%d symbol %d\n",proc->thread_id, gNB_id,ch_offset,ue->frame_parms.ofdm_symbol_size,
         ue->frame_parms.Ncp,Ns,k, symbol);
#endif

  switch (k) {
  case 0:
    fl = filt16a_l0;
    fm = filt16a_m0;
    fr = filt16a_r0;
    break;

  case 1:
    fl = filt16a_l1;
    fm = filt16a_m1;
    fr = filt16a_r1;
    break;

  case 2:
    fl = filt16a_l2;
    fm = filt16a_m2;
    fr = filt16a_r2;
    break;

  case 3:
    fl = filt16a_l3;
    fm = filt16a_m3;
    fr = filt16a_r3;
    break;

  default:
    msg("pbch_channel_estimation: k=%d -> ERROR\n",k);
    return(-1);
    break;
  }

  idft_size_idx_t idftsizeidx;
  
  switch (ue->frame_parms.ofdm_symbol_size) {
  case 128:
    idftsizeidx = IDFT_128;
    break;
    
  case 256:
    idftsizeidx = IDFT_256;
    break;
    
  case 512:
    idftsizeidx = IDFT_512;
    break;
    
  case 768:
    idftsizeidx = IDFT_768;
    break;

  case 1024:
    idftsizeidx = IDFT_1024;
    break;
    
  case 1536:
    idftsizeidx = IDFT_1536;
    break;
    
  case 2048:
    idftsizeidx = IDFT_2048;
    break;
    
  case 3072:
    idftsizeidx = IDFT_3072;
    break;
    
  case 4096:
    idftsizeidx = IDFT_4096;
    break;
    
  default:
    printf("unsupported ofdm symbol size \n");
    assert(0);
  }
  
  // generate pilot
  nr_pbch_dmrs_rx(dmrss,ue->nr_gold_pbch[n_hf][ssb_index], &pilot[0]);

  for (int aarx=0; aarx<ue->frame_parms.nb_antennas_rx; aarx++) {

    int re_offset = ssb_offset;
    pil   = (int16_t *)&pilot[0];
    rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];
    dl_ch = (int16_t *)&dl_ch_estimates[aarx][ch_offset];

    memset(dl_ch,0,sizeof(struct complex16)*(ue->frame_parms.ofdm_symbol_size));

#ifdef DEBUG_CH
    printf("pbch ch est pilot addr %p RB_DL %d\n",&pilot[0], ue->frame_parms.N_RB_DL);
    printf("k %d, first_carrier %d\n",k,ue->frame_parms.first_carrier_offset);
    printf("rxF addr %p\n", rxF);
    printf("dl_ch addr %p\n",dl_ch);
#endif

    // Treat first 2 pilots specially (left edge)
    int16_t ch[2];
    ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
    ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

#ifdef DEBUG_CH
    printf("ch 0 %d\n",((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1]));
    printf("pilot 0 : rxF - > (%d,%d) addr %p  ch -> (%d,%d), pil -> (%d,%d) \n",rxF[0],rxF[1],&rxF[0],ch[0],ch[1],pil[0],pil[1]);
#endif
    multadd_real_vector_complex_scalar(fl,
				       ch,
				       dl_ch,
				       16);
    pil += 2;
    re_offset = (re_offset+4) % ue->frame_parms.ofdm_symbol_size;
    rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];

    //for (int i= 0; i<8; i++)
    //printf("dl_ch addr %p %d\n", dl_ch+i, *(dl_ch+i));

    ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
    ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);


#ifdef DEBUG_CH
    printf("pilot 1 : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif
    multadd_real_vector_complex_scalar(fm,
				       ch,
				       dl_ch,
				       16);
    pil += 2;
    re_offset = (re_offset+4) % ue->frame_parms.ofdm_symbol_size;
    rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];

    ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
    ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

#ifdef DEBUG_CH
    printf("pilot 2 : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif

    multadd_real_vector_complex_scalar(fr,
				       ch,
				       dl_ch,
				       16);
    pil += 2;
    re_offset = (re_offset+4) % ue->frame_parms.ofdm_symbol_size;
    rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];
    dl_ch += 24;

    for (pilot_cnt=3; pilot_cnt<(3*20); pilot_cnt += 3) {

      //	if (pilot_cnt == 30)
      //	  rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k)];

      // in 2nd symbol, skip middle  REs (48 with DMRS,  144 for SSS, and another 48 with DMRS) 
      if (dmrss == 1 && pilot_cnt == 12) {
        pilot_cnt=48;
        re_offset = (re_offset+144) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];
        dl_ch += 288;
      }
      ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

#ifdef DEBUG_CH
      printf("pilot %u : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt,rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif
      multadd_real_vector_complex_scalar(fl,
					 ch,
					 dl_ch,
					 16);

      //for (int i= 0; i<8; i++)
      //            printf("pilot_cnt %d dl_ch %d %d\n", pilot_cnt, dl_ch+i, *(dl_ch+i));

      pil += 2;
      re_offset = (re_offset+4) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];
        
  
      ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

#ifdef DEBUG_CH
      printf("pilot %u : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt+1,rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif
      multadd_real_vector_complex_scalar(fm,
					 ch,
					 dl_ch,
					 16);
      pil += 2;
      re_offset = (re_offset+4) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];

      ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

#ifdef DEBUG_CH
      printf("pilot %u : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt+2,rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif

      multadd_real_vector_complex_scalar(fr,
					 ch,
					 dl_ch,
					 16);
      pil += 2;
      re_offset = (re_offset+4) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+re_offset)];
      dl_ch += 24;

    }

    if( dmrss == 2) // update time statistics for last PBCH symbol
    {
      // do ifft of channel estimate
      LOG_D(PHY,"Channel Impulse Computation Slot %d Symbol %d ch_offset %d\n", Ns, symbol, ch_offset);
      idft(idftsizeidx,
	   (int16_t*) &dl_ch_estimates[aarx][ch_offset],
	   (int16_t*) dl_ch_estimates_time[aarx],
	   1);
    }
  }

  if (dmrss == 2)
    UEscopeCopy(ue, pbchDlChEstimateTime, (void*)dl_ch_estimates_time, sizeof(struct complex16), ue->frame_parms.nb_antennas_rx, idftsizeidx);

  return(0);
}

int nr_pdcch_channel_estimation(PHY_VARS_NR_UE *ue,
                                UE_nr_rxtx_proc_t *proc,
                                uint8_t gNB_id,
                                unsigned char Ns,
                                unsigned char symbol,
                                unsigned short scrambling_id,
                                unsigned short coreset_start_subcarrier,
                                unsigned short nb_rb_coreset,
                                int32_t pdcch_est_size,
                                int32_t pdcch_dl_ch_estimates[][pdcch_est_size])
{

  unsigned char aarx;
  unsigned short k;
  unsigned int pilot_cnt;
  int16_t ch[2],*pil,*rxF,*dl_ch,*fl,*fm,*fr;
  int ch_offset,symbol_offset;

  int **rxdataF=ue->common_vars.common_vars_rx_data_per_thread[proc->thread_id].rxdataF;

  ch_offset     = ue->frame_parms.ofdm_symbol_size*symbol;

  symbol_offset = ue->frame_parms.ofdm_symbol_size*symbol;


#ifdef DEBUG_PDCCH
  printf("PDCCH Channel Estimation : ThreadId %d, gNB_id %d ch_offset %d, OFDM size %d, Ncp=%d, Ns=%d, symbol %d\n",
         proc->thread_id, gNB_id,ch_offset,ue->frame_parms.ofdm_symbol_size,ue->frame_parms.Ncp,Ns,symbol);
#endif

  fl = filt16a_l1;
  fm = filt16a_m1;
  fr = filt16a_r1;

  // checking if re-initialization of scrambling IDs is needed (should be done here but scrambling ID for PDCCH is not taken from RRC)
  if (scrambling_id != ue->scramblingID_pdcch){
    ue->scramblingID_pdcch = scrambling_id;
    nr_gold_pdcch(ue,ue->scramblingID_pdcch);
  }

  // generate pilot
  int pilot[nb_rb_coreset * 3] __attribute__((aligned(16))); 
  nr_pdcch_dmrs_rx(ue,gNB_id,Ns,ue->nr_gold_pdcch[gNB_id][Ns][symbol], &pilot[0],2000,nb_rb_coreset);


  for (aarx=0; aarx<ue->frame_parms.nb_antennas_rx; aarx++) {

    k = coreset_start_subcarrier;
    pil   = (int16_t *)&pilot[0];
    rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+k+1)];
    dl_ch = (int16_t *)&pdcch_dl_ch_estimates[aarx][ch_offset];

    memset(dl_ch,0,4*(ue->frame_parms.ofdm_symbol_size));

#ifdef DEBUG_PDCCH
    printf("pdcch ch est pilot addr %p RB_DL %d\n",&pilot[0], ue->frame_parms.N_RB_DL);
    printf("k %d, first_carrier %d\n",k,ue->frame_parms.first_carrier_offset);
    printf("rxF addr %p\n", rxF);

    printf("dl_ch addr %p\n",dl_ch);
#endif
    //    if ((ue->frame_parms.N_RB_DL&1)==0) {
    // Treat first 2 pilots specially (left edge)
    ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
    ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
#ifdef DEBUG_PDCCH
    printf("ch 0 %d\n",((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1]));
    printf("pilot 0 : rxF - > (%d,%d) addr %p  ch -> (%d,%d), pil -> (%d,%d) \n",rxF[0],rxF[1],&rxF[0],ch[0],ch[1],pil[0],pil[1]);
#endif
    multadd_real_vector_complex_scalar(fl,
				       ch,
				       dl_ch,
				       16);
    pil += 2;
    rxF += 8;
    k   += 4;

    if (k >= ue->frame_parms.ofdm_symbol_size) {
      k  -= ue->frame_parms.ofdm_symbol_size;
      rxF = (int16_t *)&rxdataF[aarx][(symbol_offset+k+1)];
    }

    ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
    ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
#ifdef DEBUG_PDCCH
    printf("pilot 1 : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif
    multadd_real_vector_complex_scalar(fm,
				       ch,
				       dl_ch,
				       16);
    pil += 2;
    rxF += 8;
    k   += 4;

    if (k >= ue->frame_parms.ofdm_symbol_size) {
      k  -= ue->frame_parms.ofdm_symbol_size;
      rxF = (int16_t *)&rxdataF[aarx][(symbol_offset+k+1)];
    }

    ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
    ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

#ifdef DEBUG_PDCCH
    printf("pilot 2 : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif

    multadd_real_vector_complex_scalar(fr,
				       ch,
				       dl_ch,
				       16);
#ifdef DEBUG_PDCCH
    for (int m =0; m<12; m++)
      printf("data :  dl_ch -> (%d,%d)\n",dl_ch[0+2*m],dl_ch[1+2*m]);
#endif
    dl_ch += 24;

    pil += 2;
    rxF += 8;
    k   += 4;

    for (pilot_cnt=3; pilot_cnt<(3*nb_rb_coreset); pilot_cnt += 3) {

      if (k >= ue->frame_parms.ofdm_symbol_size) {
        k  -= ue->frame_parms.ofdm_symbol_size;
        rxF = (int16_t *)&rxdataF[aarx][(symbol_offset+k+1)];
      }

      ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
#ifdef DEBUG_PDCCH
      printf("pilot %u : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt,rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif
      multadd_real_vector_complex_scalar(fl,
					 ch,
					 dl_ch,
					 16);

      //for (int i= 0; i<8; i++)
      //            printf("pilot_cnt %d dl_ch %d %d\n", pilot_cnt, dl_ch+i, *(dl_ch+i));

      pil += 2;
      rxF += 8;
      k   += 4;

      if (k >= ue->frame_parms.ofdm_symbol_size) {
        k  -= ue->frame_parms.ofdm_symbol_size;
        rxF = (int16_t *)&rxdataF[aarx][(symbol_offset+k+1)];
      }

      ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
#ifdef DEBUG_PDCCH
      printf("pilot %u : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt+1,rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif
      multadd_real_vector_complex_scalar(fm,
					 ch,
					 dl_ch,
					 16);
      pil += 2;
      rxF += 8;
      k   += 4;

      if (k >= ue->frame_parms.ofdm_symbol_size) {
        k  -= ue->frame_parms.ofdm_symbol_size;
        rxF = (int16_t *)&rxdataF[aarx][(symbol_offset+k+1)];
      }

      ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

#ifdef DEBUG_PDCCH
      printf("pilot %u : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt+2,rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif

      multadd_real_vector_complex_scalar(fr,
					 ch,
					 dl_ch,
					 16);
#ifdef DEBUG_PDCCH
    for (int m =0; m<12; m++)
      printf("data :  dl_ch -> (%d,%d)\n",dl_ch[0+2*m],dl_ch[1+2*m]);
#endif
      dl_ch += 24;

      pil += 2;
      rxF += 8;
      k   += 4;
    }


    //}

  }

  return(0);
}

int nr_pdsch_channel_estimation(PHY_VARS_NR_UE *ue,
                                UE_nr_rxtx_proc_t *proc,
                                uint8_t gNB_id,
                                bool is_SI,
                                unsigned char Ns,
                                unsigned short p,
                                unsigned char symbol,
                                unsigned char nscid,
                                unsigned short scrambling_id,
                                unsigned short BWPStart,
                                uint8_t config_type,
                                unsigned short bwp_start_subcarrier,
                                unsigned short nb_rb_pdsch)
{
  int pilot[3280] __attribute__((aligned(16)));
  unsigned char aarx;
  unsigned short k;
  unsigned int pilot_cnt;
  int16_t ch_l[2],ch_r[2],ch[2],*pil,*rxF,*dl_ch;
  int16_t *fl=NULL,*fm=NULL,*fr=NULL,*fml=NULL,*fmr=NULL,*fmm=NULL,*fdcl=NULL,*fdcr=NULL,*fdclh=NULL,*fdcrh=NULL, *frl=NULL, *frr=NULL;
  int ch_offset,symbol_offset;

  uint8_t nushift;
  int **dl_ch_estimates  =ue->pdsch_vars[proc->thread_id][gNB_id]->dl_ch_estimates;
  int **rxdataF=ue->common_vars.common_vars_rx_data_per_thread[proc->thread_id].rxdataF;

  ch_offset     = ue->frame_parms.ofdm_symbol_size*symbol;

  symbol_offset = ue->frame_parms.ofdm_symbol_size*symbol;

  k = bwp_start_subcarrier;
  int re_offset = k;

#ifdef DEBUG_CH
  printf("PDSCH Channel Estimation : ThreadId %d, gNB_id %d ch_offset %d, symbol_offset %d OFDM size %d, Ncp=%d, Ns=%d, k=%d symbol %d\n",proc->thread_id, gNB_id,ch_offset,symbol_offset,ue->frame_parms.ofdm_symbol_size,
         ue->frame_parms.Ncp,Ns,k, symbol);
#endif

  // generate pilot for gNB port number 1000+p
  uint16_t rb_offset = (bwp_start_subcarrier - ue->frame_parms.first_carrier_offset) / 12;
  if (is_SI) {
    rb_offset -= BWPStart;
  }
  int8_t delta = get_delta(p, config_type);

  // checking if re-initialization of scrambling IDs is needed
  if (scrambling_id != ue->scramblingID_dlsch[nscid]){
    ue->scramblingID_dlsch[nscid] = scrambling_id;
    nr_gold_pdsch(ue, nscid, scrambling_id);
  }

  nr_pdsch_dmrs_rx(ue, Ns, ue->nr_gold_pdsch[gNB_id][Ns][symbol][0], &pilot[0], 1000+p, 0, nb_rb_pdsch+rb_offset, config_type);

  if (config_type == NFAPI_NR_DMRS_TYPE1){
    nushift = (p>>1)&1;
    if (p<4) ue->frame_parms.nushift = nushift;
    switch (delta) {

    case 0://port 0,1
      fl = filt8_l0;//left interpolation Filter for DMRS config. 1
      fm = filt8_m0;//left middle interpolation Filter
      fr = filt8_r0;//right interpolation Filter
      fmm = filt8_mm0;;//middle middle interpolation Filter
      fml = filt8_m0;//left middle interpolation Filter
      fmr = filt8_mr0;//middle right interpolation Filter
      fdcl = filt8_dcl0;//left DC interpolation Filter (even RB)
      fdcr = filt8_dcr0;//right DC interpolation Filter (even RB)
      fdclh = filt8_dcl0_h;//left DC interpolation Filter (odd RB)
      fdcrh = filt8_dcr0_h;//right DC interpolation Filter (odd RB)
      frl = NULL;
      frr = NULL;
      break;

    case 1://port2,3
      fl = filt8_l1;
      fm = filt8_m1;
      fr = filt8_r1;
      fmm = filt8_mm1;
      fml = filt8_ml1;
      fmr = filt8_m1;
      fdcl = filt8_dcl1;
      fdcr = filt8_dcr1;
      fdclh = filt8_dcl1_h;
      fdcrh = filt8_dcr1_h;
      frl = NULL;
      frr = NULL;
      break;

    default:
      LOG_E(PHY,"pdsch_channel_estimation: nushift=%d -> ERROR\n",nushift);
      return -1;
      break;
    }
  } else {//NFAPI_NR_DMRS_TYPE2
    nushift = delta;
    if (p<6) ue->frame_parms.nushift = nushift;
    switch (delta) {
    case 0://port 0,1
      fl = filt8_l2;//left interpolation Filter should be fml
      fr = filt8_r2;//right interpolation Filter should be fmr
      fm = filt8_l2;
      fmm = filt8_r2;
      fml = filt8_ml2;
      fmr = filt8_mr2;
      frl = filt8_rl2;
      frr = filt8_rm2;
      fdcl = filt8_dcl1;
      fdcr = filt8_dcr1;
      fdclh = filt8_dcl1_h;
      fdcrh = filt8_dcr1_h;
      break;

    case 2://port2,3
      fl = filt8_l3;
      fm = filt8_m2;
      fr = filt8_r3;
      fmm = filt8_mm2;
      fml = filt8_l2;
      fmr = filt8_r2;
      frl = filt8_rl3;
      frr = filt8_rr3;
      fdcl = NULL;
      fdcr = NULL;
      fdclh = NULL;
      fdcrh = NULL;
      break;

    default:
      LOG_E(PHY,"pdsch_channel_estimation: nushift=%d -> ERROR\n",nushift);
      return -1;
      break;
    }
  }

  for (aarx=0; aarx<ue->frame_parms.nb_antennas_rx; aarx++) {
    pil   = (int16_t *)&pilot[rb_offset*((config_type == NFAPI_NR_DMRS_TYPE1) ? 6:4)];
    k     = k % ue->frame_parms.ofdm_symbol_size;
    re_offset = k;
    rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+re_offset+nushift)];
    dl_ch = (int16_t *)&dl_ch_estimates[p*ue->frame_parms.nb_antennas_rx+aarx][ch_offset];

    memset(dl_ch,0,4*(ue->frame_parms.ofdm_symbol_size));
#ifdef DEBUG_PDSCH
    printf("ch est pilot addr %p RB_DL %d\n",&pilot[0], ue->frame_parms.N_RB_DL);
    printf("k %d, first_carrier %d\n",k,ue->frame_parms.first_carrier_offset);
    printf("rxF addr %p p %d\n", rxF,p);
    printf("dl_ch addr %p nushift %d\n",dl_ch,nushift);
#endif
    if (config_type == NFAPI_NR_DMRS_TYPE1 && ue->prb_interpolation == 0) {

      // Treat first 2 pilots specially (left edge)
      ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
#ifdef DEBUG_PDSCH
      printf("ch 0 %d\n",((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1]));
      printf("pilot 0 : rxF - > (%d,%d) addr %p  ch -> (%d,%d), pil -> (%d,%d) \n",rxF[0],rxF[1],&rxF[0],ch[0],ch[1],pil[0],pil[1]);
      printf("data 0 : rxF - > (%d,%d) addr %p  ch -> (%d,%d), pil -> (%d,%d) \n",rxF[2],rxF[3],&rxF[2],ch[0],ch[1],pil[0],pil[1]);
#endif
      multadd_real_vector_complex_scalar(fl,
                                         ch,
                                         dl_ch,
                                         8);
      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
      //for (int i= 0; i<8; i++)
      //printf("dl_ch addr %p %d\n", dl_ch+i, *(dl_ch+i));

      ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
#ifdef DEBUG_PDSCH
      printf("pilot 1 : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif
      multadd_real_vector_complex_scalar(fml,
                                         ch,
                                         dl_ch,
                                         8);
      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
      //printf("dl_ch addr %p\n",dl_ch);
      
      ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
#ifdef DEBUG_PDSCH
      printf("pilot 2 : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif
      multadd_real_vector_complex_scalar(fmm,
                                         ch,
                                         dl_ch,
                                         8);
                                         
      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
      dl_ch += 8;

      for (pilot_cnt=3; pilot_cnt<(6*nb_rb_pdsch-3); pilot_cnt += 2) {

        ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
        ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
#ifdef DEBUG_PDSCH
	printf("pilot %u : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt,rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif
        multadd_real_vector_complex_scalar(fm,
                                           ch,
                                           dl_ch,
                                           8);

        pil += 2;
        re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
      
        ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
        ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
#ifdef DEBUG_PDSCH
	printf("pilot %u : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt+1,rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif
        multadd_real_vector_complex_scalar(fmm,
                                           ch,
                                           dl_ch,
                                           8);
        pil += 2;
        re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
        dl_ch += 8;

      }
      
      // Treat first 2 pilots specially (right edge)
      ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
#ifdef DEBUG_PDSCH
      printf("pilot %u : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt,rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif
      multadd_real_vector_complex_scalar(fm,
                                         ch,
                                         dl_ch,
                                         8);
                                         
      //for (int i= 0; i<8; i++)
      //printf("dl_ch addr %p %d\n", dl_ch+i, *(dl_ch+i));

      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
             
      ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
#ifdef DEBUG_PDSCH
      printf("ch 0 %d\n",((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1]));
      printf("pilot %u: rxF - > (%d,%d) addr %p  ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt+1,rxF[0],rxF[1],&rxF[0],ch[0],ch[1],pil[0],pil[1]);
#endif
      multadd_real_vector_complex_scalar(fmr,
                                         ch,
                                         dl_ch,
                                         8);
                                         
      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
      dl_ch += 8;
      
      ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
#ifdef DEBUG_PDSCH
      printf("pilot %u: rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt+2,rxF[0],rxF[1],ch[0],ch[1],pil[0],pil[1]);
#endif
      multadd_real_vector_complex_scalar(fr,
                                         ch,
                                         dl_ch,
                                         8);
    
      // check if PRB crosses DC and improve estimates around DC
      if ((bwp_start_subcarrier < ue->frame_parms.ofdm_symbol_size) && (bwp_start_subcarrier+nb_rb_pdsch*12 >= ue->frame_parms.ofdm_symbol_size)) {
        dl_ch = (int16_t *)&dl_ch_estimates[aarx][ch_offset];
        uint16_t idxDC = 2*(ue->frame_parms.ofdm_symbol_size - bwp_start_subcarrier);
        uint16_t idxPil = idxDC/2;
        re_offset = k;
        pil = (int16_t *)&pilot[rb_offset*((config_type == NFAPI_NR_DMRS_TYPE1) ? 6:4)];
        pil += (idxPil-2);
        dl_ch += (idxDC-4);
        dl_ch = memset(dl_ch, 0, sizeof(int16_t)*10);
        re_offset = (re_offset+idxDC/2-2) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
        ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
        ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
          
        // for proper allignment of SIMD vectors
        if((ue->frame_parms.N_RB_DL&1) == 0) {
              
          multadd_real_vector_complex_scalar(fdcl,
                     ch,
                     dl_ch-4,
                     8);
              
          pil += 4;
          re_offset = (re_offset+4) % ue->frame_parms.ofdm_symbol_size;
          rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
          ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
          ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
              
          multadd_real_vector_complex_scalar(fdcr,
                     ch,
                     dl_ch-4,
                     8);
        } else {

          multadd_real_vector_complex_scalar(fdclh,
                     ch,
                     dl_ch,
                     8);
              
          pil += 4;
          re_offset = (re_offset+4) % ue->frame_parms.ofdm_symbol_size;
          rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
          ch[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
          ch[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);
              
          multadd_real_vector_complex_scalar(fdcrh,
                     ch,
                     dl_ch,
                     8);
        }
      }
    } else if (config_type == NFAPI_NR_DMRS_TYPE2 && ue->prb_interpolation == 0){ //pdsch_dmrs_type2  |dmrs_r,dmrs_l,0,0,0,0,dmrs_r,dmrs_l,0,0,0,0|

      // Treat first 4 pilots specially (left edge)
      ch_l[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch_l[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

#ifdef DEBUG_PDSCH
      printf("ch 0 %d\n",((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1]));
      printf("pilot 0 : rxF - > (%d,%d) addr %p  ch -> (%d,%d), pil -> (%d,%d) \n",rxF[0],rxF[1],&rxF[0],ch_l[0],ch_l[1],pil[0],pil[1]);
#endif

      pil += 2;
      re_offset = (re_offset+1) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
      ch_r[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch_r[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

      ch[0] = (ch_l[0]+ch_r[0])>>1;
      ch[1] = (ch_l[1]+ch_r[1])>>1;

      dl_ch[(0+2*nushift)] = ch[0];
      dl_ch[(1+2*nushift)] = ch[1];
      dl_ch[2+2*nushift] = ch[0];
      dl_ch[3+2*nushift] = ch[1];

      multadd_real_vector_complex_scalar(fl,
                                         ch,
                                         dl_ch,
                                         8);

      pil += 2;
      re_offset = (re_offset+5) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
      ch_l[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch_l[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

      pil += 2;
      re_offset = (re_offset+1) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
      ch_r[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
      ch_r[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

      ch[0] = (ch_l[0]+ch_r[0])>>1;
      ch[1] = (ch_l[1]+ch_r[1])>>1;

      multadd_real_vector_complex_scalar(fr,
                                         ch,
                                         dl_ch,
                                         8);

      dl_ch += 12;
      dl_ch[0+2*nushift] = ch[0];
      dl_ch[1+2*nushift] = ch[1];
      dl_ch[2+2*nushift] = ch[0];
      dl_ch[3+2*nushift] = ch[1];
      dl_ch += 4;

      for (pilot_cnt=4; pilot_cnt<4*nb_rb_pdsch; pilot_cnt += 4) {

        multadd_real_vector_complex_scalar(fml,
                                           ch,
                                           dl_ch,
                                           8);
        pil += 2;
        re_offset = (re_offset+5) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
        ch_l[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
        ch_l[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

#ifdef DEBUG_PDSCH
        printf("pilot %u : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt,rxF[0],rxF[1],ch_l[0],ch_l[1],pil[0],pil[1]);
#endif

        pil += 2;
        re_offset = (re_offset+1) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
        ch_r[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
        ch_r[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

        ch[0] = (ch_l[0]+ch_r[0])>>1;
        ch[1] = (ch_l[1]+ch_r[1])>>1;

#ifdef DEBUG_PDSCH
        printf("pilot %u : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt+1,rxF[0],rxF[1],ch_r[0],ch_r[1],pil[0],pil[1]);
#endif

        multadd_real_vector_complex_scalar(fmr,
                                           ch,
                                           dl_ch,
                                           8);

        dl_ch += 8;
        dl_ch[0+2*nushift] = ch[0];
        dl_ch[1+2*nushift] = ch[1];
        dl_ch[2+2*nushift] = ch[0];
        dl_ch[3+2*nushift] = ch[1];

        multadd_real_vector_complex_scalar(fm,
                                           ch,
                                           dl_ch,
                                           8);

        pil += 2;
        re_offset = (re_offset+5) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
        ch_l[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
        ch_l[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

        pil += 2;
        re_offset = (re_offset+1) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];
        ch_r[0] = (int16_t)(((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15);
        ch_r[1] = (int16_t)(((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15);

#ifdef DEBUG_PDSCH
        printf("pilot %u : rxF - > (%d,%d) ch -> (%d,%d), pil -> (%d,%d) \n",pilot_cnt+1,rxF[0],rxF[1],ch_r[0],ch_r[1],pil[0],pil[1]);
#endif

        ch[0] = (ch_l[0]+ch_r[0])>>1;
        ch[1] = (ch_l[1]+ch_r[1])>>1;

        multadd_real_vector_complex_scalar(fmm,
                                           ch,
                                           dl_ch,
                                           8);

        dl_ch += 12;
        dl_ch[0+2*nushift] = ch[0];
        dl_ch[1+2*nushift] = ch[1];
        dl_ch[2+2*nushift] = ch[0];
        dl_ch[3+2*nushift] = ch[1];
        dl_ch += 4;
      }

      // Treat last 2 pilots specially (right edge)
      // dl_ch-2+nushift<<1
      multadd_real_vector_complex_scalar(frl,
                                         dl_ch-2+2*nushift,
                                         dl_ch,
                                         8);

      multadd_real_vector_complex_scalar(frr,
                                         dl_ch-14+2*nushift,/*14*/
                                         dl_ch,
                                         8);

      // check if PRB crosses DC and improve estimates around DC
      if ((bwp_start_subcarrier < ue->frame_parms.ofdm_symbol_size) && (bwp_start_subcarrier+nb_rb_pdsch*12 >= ue->frame_parms.ofdm_symbol_size) && (p<2)) {

        dl_ch = (int16_t *)&dl_ch_estimates[p*ue->frame_parms.nb_antennas_rx+aarx][ch_offset];
        uint16_t idxDC = 2*(ue->frame_parms.ofdm_symbol_size - bwp_start_subcarrier);
        dl_ch += (idxDC-8);
        dl_ch = memset(dl_ch, 0, sizeof(int16_t)*20);

        dl_ch -= 2;

        ch_r[0] = dl_ch[0];
        ch_r[1]= dl_ch[1] ;
        dl_ch += 22;
        ch_l[0] = dl_ch[0];
        ch_l[1]= dl_ch[1] ;

        // for proper allignment of SIMD vectors
        if((ue->frame_parms.N_RB_DL&1) == 0) {
          dl_ch -= 20;
          //Interpolate fdcrl1 with ch_r
          multadd_real_vector_complex_scalar(filt8_dcrl1,
                                             ch_r,
                                             dl_ch,
                                             8);
          //Interpolate fdclh1 with ch_l
          multadd_real_vector_complex_scalar(filt8_dclh1,
                                             ch_l,
                                             dl_ch,
                                             8);
          dl_ch += 16;
          //Interpolate fdcrh1 with ch_r
          multadd_real_vector_complex_scalar(filt8_dcrh1,
                                             ch_r,
                                             dl_ch,
                                             8);
          //Interpolate fdcll1 with ch_l
          multadd_real_vector_complex_scalar(filt8_dcll1,
                                             ch_l,
                                             dl_ch,
                                             8);
        } else {
          dl_ch -= 28;
          //Interpolate fdcrl1 with ch_r
          multadd_real_vector_complex_scalar(filt8_dcrl2,
                                             ch_r,
                                             dl_ch,
                                             8);
          //Interpolate fdclh1 with ch_l
          multadd_real_vector_complex_scalar(filt8_dclh2,
                                             ch_l,
                                             dl_ch,
                                             8);
          dl_ch += 16;
          //Interpolate fdcrh1 with ch_r
          multadd_real_vector_complex_scalar(filt8_dcrh2,
                                             ch_r,
                                             dl_ch,
                                             8);
          //Interpolate fdcll1 with ch_l
          multadd_real_vector_complex_scalar(filt8_dcll2,
                                             ch_l,
                                             dl_ch,
                                             8);
        }
      }
    }
    else if (config_type == NFAPI_NR_DMRS_TYPE1) { // this is case without frequency-domain linear interpolation, just take average of LS channel estimates of 6 DMRS REs and use a common value for the whole PRB
      int32_t ch_0, ch_1;
      
      ch_0 = ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 = ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch[0] = ch_0 / 6;
      ch[1] = ch_1 / 6;

      multadd_real_vector_complex_scalar(filt8_avlip0,
                                         ch,
                                         dl_ch,
                                         8);

      dl_ch += 16;
      multadd_real_vector_complex_scalar(filt8_avlip1,
                                         ch,
                                         dl_ch,
                                         8);

      dl_ch += 16;
      multadd_real_vector_complex_scalar(filt8_avlip2,
                                         ch,
                                         dl_ch,
                                         8);
      dl_ch -= 24;

      for (pilot_cnt=6; pilot_cnt<6*(nb_rb_pdsch-1); pilot_cnt += 6) {

        ch_0 = ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
        ch_1 = ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

        pil += 2;
        re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

        ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
        ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

        pil += 2;
        re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

        ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
        ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

        pil += 2;
        re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

        ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
        ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

        pil += 2;
        re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

        ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
        ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

        pil += 2;
        re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

        ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
        ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

        pil += 2;
        re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

        ch[0] = ch_0 / 6;
        ch[1] = ch_1 / 6;
        dl_ch[6] += (ch[0] * 1365)>>15; // 1/12*16384
        dl_ch[7] += (ch[1] * 1365)>>15; // 1/12*16384

        dl_ch += 8;
        multadd_real_vector_complex_scalar(filt8_avlip3,
                                           ch,
                                           dl_ch,
                                           8);

        dl_ch += 16;
        multadd_real_vector_complex_scalar(filt8_avlip4,
                                           ch,
                                           dl_ch,
                                           8);

        dl_ch += 16;
        multadd_real_vector_complex_scalar(filt8_avlip5,
                                           ch,
                                           dl_ch,
                                           8);
        dl_ch -= 16;
      }
      ch_0 = ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 = ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+2) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch[0] = ch_0 / 6;
      ch[1] = ch_1 / 6;

      dl_ch[6] += (ch[0] * 1365)>>15; // 1/12*16384
      dl_ch[7] += (ch[1] * 1365)>>15; // 1/12*16384

      dl_ch += 8;
      multadd_real_vector_complex_scalar(filt8_avlip3,
                                         ch,
                                         dl_ch,
                                         8);

      dl_ch += 16;
      multadd_real_vector_complex_scalar(filt8_avlip6,
                                         ch,
                                         dl_ch,
                                         8);
    }
    else  { // this is case without frequency-domain linear interpolation, just take average of LS channel estimates of 4 DMRS REs and use a common value for the whole PRB
      int32_t ch_0, ch_1;

      ch_0 = ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 = ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+1) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+5) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+1) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+5) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch[0] = ch_0 / 4;
      ch[1] = ch_1 / 4;

      multadd_real_vector_complex_scalar(filt8_avlip0,
                                         ch,
                                         dl_ch,
                                         8);

      dl_ch += 16;
      multadd_real_vector_complex_scalar(filt8_avlip1,
                                         ch,
                                         dl_ch,
                                         8);

      dl_ch += 16;
      multadd_real_vector_complex_scalar(filt8_avlip2,
                                         ch,
                                         dl_ch,
                                         8);
      dl_ch -= 24;

      for (pilot_cnt=4; pilot_cnt<4*(nb_rb_pdsch-1); pilot_cnt += 4) {
        int32_t ch_0, ch_1;

        ch_0 = ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
        ch_1 = ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

        pil += 2;
        re_offset = (re_offset+1) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

        ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
        ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

        pil += 2;
        re_offset = (re_offset+5) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

        ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
        ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

        pil += 2;
        re_offset = (re_offset+1) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

        ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
        ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

        pil += 2;
        re_offset = (re_offset+5) % ue->frame_parms.ofdm_symbol_size;
        rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

        ch[0] = ch_0 / 4;
        ch[1] = ch_1 / 4;

        dl_ch[6] += (ch[0] * 1365)>>15; // 1/12*16384
        dl_ch[7] += (ch[1] * 1365)>>15; // 1/12*16384

        dl_ch += 8;
        multadd_real_vector_complex_scalar(filt8_avlip3,
                                           ch,
                                           dl_ch,
                                           8);

        dl_ch += 16;
        multadd_real_vector_complex_scalar(filt8_avlip4,
                                           ch,
                                           dl_ch,
                                           8);

        dl_ch += 16;
        multadd_real_vector_complex_scalar(filt8_avlip5,
                                           ch,
                                           dl_ch,
                                           8);
        dl_ch -= 16;
      }

      ch_0 = ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 = ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+1) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+5) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+1) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch_0 += ((int32_t)pil[0]*rxF[0] - (int32_t)pil[1]*rxF[1])>>15;
      ch_1 += ((int32_t)pil[0]*rxF[1] + (int32_t)pil[1]*rxF[0])>>15;

      pil += 2;
      re_offset = (re_offset+5) % ue->frame_parms.ofdm_symbol_size;
      rxF   = (int16_t *)&rxdataF[aarx][(symbol_offset+nushift+re_offset)];

      ch[0] = ch_0 / 4;
      ch[1] = ch_1 / 4;

      dl_ch[6] += (ch[0] * 1365)>>15; // 1/12*16384
      dl_ch[7] += (ch[1] * 1365)>>15; // 1/12*16384

      dl_ch += 8;
      multadd_real_vector_complex_scalar(filt8_avlip3,
                                         ch,
                                         dl_ch,
                                         8);

      dl_ch += 16;
      multadd_real_vector_complex_scalar(filt8_avlip6,
                                         ch,
                                         dl_ch,
                                         8);
    }
#ifdef DEBUG_PDSCH
    dl_ch = (int16_t *)&dl_ch_estimates[p*ue->frame_parms.nb_antennas_rx+aarx][ch_offset];
    for(uint16_t idxP=0; idxP<ceil((float)nb_rb_pdsch*12/8); idxP++) {
      for(uint8_t idxI=0; idxI<16; idxI += 2) {
        printf("%d\t%d\t",dl_ch[idxP*16+idxI],dl_ch[idxP*16+idxI+1]);
      }
      printf("%d\n",idxP);
    }
#endif
  }
  return(0);
}

/*******************************************************************
 *
 * NAME :         nr_pdsch_ptrs_processing
 *
 * PARAMETERS :   PHY_VARS_NR_UE    : ue data structure
 *                NR_UE_PDSCH       : pdsch_vars pointer
 *                NR_DL_FRAME_PARMS : frame_parms pointer
 *                NR_DL_UE_HARQ_t   : dlsch0_harq pointer
 *                NR_DL_UE_HARQ_t   : dlsch1_harq pointer
 *                uint8_t           : gNB_id,
 *                uint8_t           : nr_slot_rx,
 *                unsigned char     : symbol,
 *                uint32_t          : nb_re_pdsch,
 *                uint16_t          : rnti
 *                RX_type_t         : rx_type
 * RETURN : Nothing
 *
 * DESCRIPTION :
 *  If ptrs is enabled process the symbol accordingly
 *  1) Estimate common phase error per PTRS symbol
 *  2) Interpolate PTRS estimated value in TD after all PTRS symbols
 *  3) Compensate signal with PTRS estimation for slot
 *********************************************************************/
void nr_pdsch_ptrs_processing(PHY_VARS_NR_UE *ue,
                              NR_UE_PDSCH **pdsch_vars,
                              NR_DL_FRAME_PARMS *frame_parms,
                              NR_DL_UE_HARQ_t *dlsch0_harq,
                              NR_DL_UE_HARQ_t *dlsch1_harq,
                              uint8_t gNB_id,
                              uint8_t nr_slot_rx,
                              unsigned char symbol,
                              uint32_t nb_re_pdsch,
                              uint16_t rnti,
                              RX_type_t rx_type)
{
  //#define DEBUG_DL_PTRS 1
  int16_t *phase_per_symbol = NULL;
  int32_t *ptrs_re_symbol = NULL;
  int8_t   ret = 0;
  /* harq specific variables */
  uint8_t  symbInSlot       = 0;
  uint16_t *startSymbIndex  = NULL;
  uint16_t *nbSymb          = NULL;
  uint8_t  *L_ptrs          = NULL;
  uint8_t  *K_ptrs          = NULL;
  uint16_t *dmrsSymbPos     = NULL;
  uint16_t *ptrsSymbPos     = NULL;
  uint8_t  *ptrsSymbIdx     = NULL;
  uint8_t  *ptrsReOffset    = NULL;
  uint8_t  *dmrsConfigType  = NULL;
  uint16_t *nb_rb           = NULL;

  if(dlsch0_harq->status == ACTIVE) {
    symbInSlot      = dlsch0_harq->start_symbol + dlsch0_harq->nb_symbols;
    startSymbIndex  = &dlsch0_harq->start_symbol;
    nbSymb          = &dlsch0_harq->nb_symbols;
    L_ptrs          = &dlsch0_harq->PTRSTimeDensity;
    K_ptrs          = &dlsch0_harq->PTRSFreqDensity;
    dmrsSymbPos     = &dlsch0_harq->dlDmrsSymbPos;
    ptrsSymbPos     = &dlsch0_harq->ptrs_symbols;
    ptrsSymbIdx     = &dlsch0_harq->ptrs_symbol_index;
    ptrsReOffset    = &dlsch0_harq->PTRSReOffset;
    dmrsConfigType  = &dlsch0_harq->dmrsConfigType;
    nb_rb           = &dlsch0_harq->nb_rb;
  }
  if(dlsch1_harq) {
    symbInSlot      = dlsch1_harq->start_symbol + dlsch0_harq->nb_symbols;
    startSymbIndex  = &dlsch1_harq->start_symbol;
    nbSymb          = &dlsch1_harq->nb_symbols;
    L_ptrs          = &dlsch1_harq->PTRSTimeDensity;
    K_ptrs          = &dlsch1_harq->PTRSFreqDensity;
    dmrsSymbPos     = &dlsch1_harq->dlDmrsSymbPos;
    ptrsSymbPos     = &dlsch1_harq->ptrs_symbols;
    ptrsSymbIdx     = &dlsch1_harq->ptrs_symbol_index;
    ptrsReOffset    = &dlsch1_harq->PTRSReOffset;
    dmrsConfigType  = &dlsch1_harq->dmrsConfigType;
    nb_rb           = &dlsch1_harq->nb_rb;
  }
  /* loop over antennas */
  for (int aarx=0; aarx<frame_parms->nb_antennas_rx; aarx++) {
    phase_per_symbol = (int16_t*)pdsch_vars[gNB_id]->ptrs_phase_per_slot[aarx];
    ptrs_re_symbol = (int32_t*)pdsch_vars[gNB_id]->ptrs_re_per_slot[aarx];
    ptrs_re_symbol[symbol] = 0;
    phase_per_symbol[(2*symbol)+1] = 0; // Imag
    /* set DMRS estimates to 0 angle with magnitude 1 */
    if(is_dmrs_symbol(symbol,*dmrsSymbPos)) {
      /* set DMRS real estimation to 32767 */
      phase_per_symbol[2*symbol]=(int16_t)((1<<15)-1); // 32767
#ifdef DEBUG_DL_PTRS
      printf("[PHY][PTRS]: DMRS Symbol %d -> %4d + j*%4d\n", symbol, phase_per_symbol[2*symbol],phase_per_symbol[(2*symbol)+1]);
#endif
    }
    else { // real ptrs value is set to 0
      phase_per_symbol[2*symbol] = 0; // Real
    }

    if(dlsch0_harq->status == ACTIVE) {
      if(symbol == *startSymbIndex) {
        *ptrsSymbPos = 0;
        set_ptrs_symb_idx(ptrsSymbPos,
                          *nbSymb,
                          *startSymbIndex,
                          1<< *L_ptrs,
                          *dmrsSymbPos);
      }
      /* if not PTRS symbol set current ptrs symbol index to zero*/
      *ptrsSymbIdx = 0;
      /* Check if current symbol contains PTRS */
      if(is_ptrs_symbol(symbol, *ptrsSymbPos)) {
        *ptrsSymbIdx = symbol;
        /*------------------------------------------------------------------------------------------------------- */
        /* 1) Estimate common phase error per PTRS symbol                                                                */
        /*------------------------------------------------------------------------------------------------------- */
        nr_ptrs_cpe_estimation(*K_ptrs,*ptrsReOffset,*dmrsConfigType,*nb_rb,
                               rnti,
                               (int16_t *)&pdsch_vars[gNB_id]->dl_ch_ptrs_estimates_ext[aarx][symbol*nb_re_pdsch],
                               nr_slot_rx,
                               symbol,frame_parms->ofdm_symbol_size,
                               (int16_t*)&pdsch_vars[gNB_id]->rxdataF_comp0[aarx][(symbol * nb_re_pdsch)],
                               ue->nr_gold_pdsch[gNB_id][nr_slot_rx][symbol][0],
                               &phase_per_symbol[2* symbol],
                               &ptrs_re_symbol[symbol]);
      }
    }// HARQ 0

    /* For last OFDM symbol at each antenna perform interpolation and compensation for the slot*/
    if(symbol == (symbInSlot -1)) {
      /*------------------------------------------------------------------------------------------------------- */
      /* 2) Interpolate PTRS estimated value in TD */
      /*------------------------------------------------------------------------------------------------------- */
      /* If L-PTRS is > 0 then we need interpolation */
      if(*L_ptrs > 0) {
        ret = nr_ptrs_process_slot(*dmrsSymbPos, *ptrsSymbPos, phase_per_symbol, *startSymbIndex, *nbSymb);
        if(ret != 0) {
          LOG_W(PHY,"[PTRS] Compensation is skipped due to error in PTRS slot processing !!\n");
        }
      }
#ifdef DEBUG_DL_PTRS
      LOG_M("ptrsEst.m","est",pdsch_vars[gNB_id]->ptrs_phase_per_slot[aarx],frame_parms->symbols_per_slot,1,1 );
      LOG_M("rxdataF_bf_ptrs_comp.m","bf_ptrs_cmp",
            &pdsch_vars[gNB_id]->rxdataF_comp0[aarx][(*startSymbIndex) * NR_NB_SC_PER_RB * (*nb_rb) ],
            (*nb_rb) * NR_NB_SC_PER_RB * (*nbSymb),1,1);
#endif
      /*------------------------------------------------------------------------------------------------------- */
      /* 3) Compensated DMRS based estimated signal with PTRS estimation                                        */
      /*--------------------------------------------------------------------------------------------------------*/
      for(uint8_t i = *startSymbIndex; i< symbInSlot ;i++) {
        /* DMRS Symbol has 0 phase so no need to rotate the respective symbol */
        /* Skip rotation if the slot processing is wrong */
        if((!is_dmrs_symbol(i,*dmrsSymbPos)) && (ret == 0)) {
#ifdef DEBUG_DL_PTRS
          printf("[PHY][DL][PTRS]: Rotate Symbol %2d with  %d + j* %d\n", i, phase_per_symbol[2* i],phase_per_symbol[(2* i) +1]);
#endif
          rotate_cpx_vector((int16_t*)&pdsch_vars[gNB_id]->rxdataF_comp0[aarx][(i * (*nb_rb) * NR_NB_SC_PER_RB)],
                            &phase_per_symbol[2* i],
                            (int16_t*)&pdsch_vars[gNB_id]->rxdataF_comp0[aarx][(i * (*nb_rb) * NR_NB_SC_PER_RB)],
                            ((*nb_rb) * NR_NB_SC_PER_RB), 15);
        }// if not DMRS Symbol
      }// symbol loop
    }// last symbol check
  }//Antenna loop
}//main function
