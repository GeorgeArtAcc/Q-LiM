/* Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE) */
/* Step 36m: random edits (cutting, moving outside the clip, pasting, length) - every result must be valid. */
#include "../src/edit.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned long long rs;
static int R(int n){ rs = rs*6364136223846793005ULL+1442695040888963407ULL; return n>0 ? (int)((rs>>33)%(unsigned)n) : 0; }
int main(int argc,char**argv){
  int seeds = argc>1?atoi(argv[1]):200;
  for (int seed=1; seed<=seeds; ++seed){
    rs = seed;
    int64_t srcl = 1 + R(3) * 100000 + R(500000);
    EditList *e = edit_new(srcl);
    for (int it=0; it<3000; ++it){
      EditList *nl=NULL; int op=R(14);
      int64_t t = e->len > 0 ? (int64_t)R((int)(e->len > 2000000000 ? 2000000000 : e->len)) : 0;
      switch(op){
      case 6: if (e->n) nl = edit_trim(e, R(e->n), R(2), t); break;
      case 7: if (e->n) nl = edit_copy(e, R(e->n), t - 50000); break;
      case 8: if (e->n) nl = edit_set_fades(e, R(e->n), R(30000), R(30000)); break;
      case 0: case 1: nl = edit_split(e, t); break;
      case 2: if (e->n > 1) nl = edit_delete(e, R(e->n)); break;
      case 3: if (e->n) nl = edit_move(e, R(e->n), t + R(200000) - 100000); break;
      case 4: nl = edit_fade(e, t, t + R(100000), R(2) ? 1 : -1); break;
      case 5: if (R(5) == 0) nl = edit_crop(e, t, t + 1000 + R(300000)); break;
      case 9: if (e->n) nl = edit_move(e, R(e->n), (R(2) ? -1 : 1) * (int64_t)R((int)(e->len > 0 ? (e->len > 1000000000 ? 1000000000 : e->len) : 1)) * 2); break;
      case 10: if (e->n) { EditList *cp = edit_extract(e, R(e->n)); nl = cp ? edit_paste(e, cp, t - e->len / 2 + R(1000)) : NULL; edit_free(cp); } break;
      case 11: nl = edit_set_len(e, R(3) == 0 ? 1 + R(1000) : e->len + R(400000) - 100000); break;
      case 12: if (e->n) nl = edit_trim(e, R(e->n), R(2), t * 2 - e->len); break;
      case 13: if (e->n) { EditList *cp = edit_extract(e, R(e->n)); nl = cp ? edit_paste(e, cp, e->len + R(1000)) : NULL; edit_free(cp); } break;
      }
      if (nl && !edit_valid(nl)) {
        char *a=edit_to_text(e), *b=edit_to_text(nl);
        printf("seed %d it %d op %d\nBEFORE len=%lld src=%lld\n%s\nAFTER len=%lld\n%s\n",seed,it,op,(long long)e->len,(long long)e->src_len,a,(long long)nl->len,b); edit_free(nl); edit_free(e); free(a); free(b); return 1; }
      if (nl){ edit_free(e); e=nl; }
    }
    edit_free(e);
  }
  printf("EDIT FUZZ ok\n"); return 0;
}
