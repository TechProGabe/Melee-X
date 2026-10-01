/* nv2a.c's pool allocator (texture and vertex pools), on the host:
 * tools/xbox/test_pool.py pastes the allocator in front of this file.
 * Random allocations and frees, then the block list's invariants: blocks in
 * address order and contiguous, no two free neighbours, the free list holds
 * exactly the free blocks, and the used count matches. */
int main(void) {
  Pool pl; static void* ptr[5000]; static uint32_t sz[5000]; int i, it, n=0;
  srand(1);
  assert(pool_init(&pl, 8u<<20));
  for (it=0; it<2000000; it++){
    if (n<5000 && (rand()%3 || !n)) {
      uint32_t want = (rand()%8==0) ? (256*1024 + rand()%(512*1024)) : 1 + rand()%(16*1024);
      void* p = pool_alloc(&pl, want);
      if (p) { ptr[n]=p; sz[n]=(want+127)&~127u; n++; }
    } else { int k=rand()%n; pool_free(&pl, ptr[k]); ptr[k]=ptr[n-1]; sz[k]=sz[n-1]; n--; }
    if (it % 50000 == 0) {
      /* invariants: address order contiguous, used matches, no adjacent free, freelist = free blocks */
      Blk* b; uint32_t off=0, used=0, nf=0, nf2=0; int prevfree=0;
      for (b=pl.blocks; b; b=b->next){ assert(b->off==off); off+=b->size; if(!b->free) used+=b->size; else nf++; assert(!(prevfree&&b->free)); prevfree=b->free; if(b->next) assert(b->next->prev==b);}
      assert(off==pl.bytes); assert(used==pl.used);
      for (b=pl.freelist;b;b=b->fnext){ assert(b->free); nf2++; }
      assert(nf==nf2);
      uint32_t tot=0; for(i=0;i<n;i++) tot+=sz[i]; assert(tot==pl.used);
    }
  }
  for (i=0;i<n;i++) pool_free(&pl, ptr[i]);
  assert(pl.used==0 && pl.blocks && !pl.blocks->next && pl.blocks->free && pl.blocks->size==pl.bytes);
  printf("pool ok\n");
  return 0;
}
