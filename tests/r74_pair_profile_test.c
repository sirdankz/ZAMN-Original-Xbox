#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cosim/r74_pair_profile.h"
int main(void){
 ZamnR74PairStat slots[ZAMN_R74_PAIR_SLOTS]={0};
 ZamnR74PairStat top[ZAMN_R74_PAIR_TOP];
 uint64_t overflow=0;
 for(unsigned i=0;i<64000;i++){
   const uint32_t entry=i%4==0?0x829a6du:(i%4==1?0x81d7f6u:0x81c4a6u);
   r74_pair_stat_add(slots,entry,0x80be8fu,(uint16_t)(i%4),0x38u,i%3!=0,&overflow);
 }
 r74_pair_stat_top(slots,top);
 assert(overflow==0);
 assert(top[0].calls>=top[1].calls && top[1].calls>=top[2].calls);
 assert(top[0].calls>0 && top[0].declined>0);
 for(unsigned i=0;i<ZAMN_R74_PAIR_SLOTS;i++)
   r74_pair_stat_add(slots,0x820000u+i,0x810000u+i,(uint16_t)i,0x44u,1,&overflow);
 assert(overflow>0);
 const uint64_t saved=overflow;
 r74_pair_stat_top(slots,top);
 assert(top[0].calls>=top[1].calls);
 assert(overflow==saved);
 memset(slots,0,sizeof(slots)); overflow=0;
 r74_pair_stat_top(slots,top);
 for(unsigned i=0;i<ZAMN_R74_PAIR_TOP;i++) assert(top[i].calls==0);
 puts("R74 diagnostic pair histogram PASS: 64000 records, top-3 ranking, 16-slot saturation, reset");
}
