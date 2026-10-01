/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A disc struct with bit-fields and plain scalars, initialized statically
 * with brace elision (ityaku.c's ItemAttr). */
#include <stdint.h>
#include <stdio.h>
#ifdef MELEE_DISC_LOWERING
#define DISC __attribute__((annotate("melee_disc")))
#else
#define DISC __attribute__((scalar_storage_order("big-endian")))
#endif
struct DISC E {float top,bottom;};
struct DISC A {uint8_t h:1;uint8_t k:4;uint8_t m:3;uint8_t x3;float f;int32_t s;struct E e;float scale;int32_t gfx;};
static struct A g={1,6,1,7,2.5f,-2,2.0f,3.0f,1.0f,0x83D60};
static struct A arr[2]={{0,3,2,0,1.0f,5,{4.0f,-1.0f},0.5f,-1},{1,0,0,9,-1.0f,0,0,0,8.0f,7}};
int main(void){printf("%u %u %u %u %.3f %d %.3f %.3f %.3f %x\n",g.h,g.k,g.m,g.x3,g.f,g.s,g.e.top,g.e.bottom,g.scale,g.gfx);
for(int j=0;j<2;j++)printf("%u %u %.3f %d %.3f %.3f %d\n",arr[j].k,arr[j].x3,arr[j].f,arr[j].s,arr[j].e.top,arr[j].scale,arr[j].gfx);
for(unsigned i=0;i<sizeof(g);i++)printf("%02x",((unsigned char*)&g)[i]);puts("");
for(unsigned i=0;i<sizeof(arr);i++)printf("%02x",((unsigned char*)arr)[i]);puts("");}
