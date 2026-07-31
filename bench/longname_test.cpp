// longname_test.cpp — 验证长名字 (nlen>256) 时共享前缀单链是否越界
// shared_key vs 逐名字参考
#include "../src/name.hpp"
#include <cstdio>
#include <cstring>

static const char TEAM[] = "test";
static const char PREFIX[] = "test-";
static const int PLEN = 5, SCL = 4, CLEN = 10;
static const int SLEN = 0;
static const unsigned char CS[10*4] = {
  0xF0,0x9F,0x99,0x88, 0xF0,0x9F,0x99,0x89, 0xF0,0x9F,0x99,0x8A, 0xF0,0x9F,0x90,0xB5,
  0xF0,0x9F,0x90,0x92, 0xF0,0x9F,0x90,0x9B, 0xF0,0x9F,0x90,0x8D, 0xF0,0x9F,0x90,0x8A,
  0xF0,0x9F,0xA6,0x8E, 0xF0,0x9F,0x90,0x89 };
#define ENC(dst,ci) memcpy((dst),(const char*)CS+(ci)*SCL,SCL)

int main(){
    const int VLEN = 80;   // 80*4=320 字节变量区, nlen=325
    const int NLEN = PLEN + VLEN*SCL + SLEN;   // 325
    if (NLEN+1 > 512) { printf("NLEN too big\n"); return 1; }
    char* a = new char[NLEN+8]; char* b = new char[NLEN+8];
    char* c = new char[NLEN+8]; char* d = new char[NLEN+8]; char* e = new char[NLEN+8];
    memset(a,0,NLEN+8);memset(b,0,NLEN+8);memset(c,0,NLEN+8);memset(d,0,NLEN+8);memset(e,0,NLEN+8);
    for (int k=0;k<PLEN;k++) a[k]=b[k]=c[k]=d[k]=e[k]=PREFIX[k];
    uint64_t v[5] = {0,1,2,3,4};
    for (int i=0;i<5;i++) {
        char* buf = i==0?a:i==1?b:i==2?c:i==3?d:e;
        uint64_t nw = v[i];
        for (int p=VLEN-1;p>=0;p--){ ENC(buf+PLEN+p*SCL, nw%CLEN); nw/=CLEN; }
        buf[NLEN]=0;
    }
    Name na1,nb1,nc1,nd1,ne1, na2,nb2,nc2,nd2,ne2;
    for (auto* p : {&na1,&nb1,&nc1,&nd1,&ne1,&na2,&nb2,&nc2,&nd2,&ne2}) { p->load_team(TEAM); p->PRELEN = PLEN; }
    na1.load_prefix(a,NLEN);nb1.load_prefix(a,NLEN);nc1.load_prefix(a,NLEN);nd1.load_prefix(a,NLEN);ne1.load_prefix(a,NLEN);
    na2.load_prefix(a,NLEN);nb2.load_prefix(a,NLEN);nc2.load_prefix(a,NLEN);nd2.load_prefix(a,NLEN);ne2.load_prefix(a,NLEN);
    int vary_start = PLEN + (VLEN-1)*SCL;   // 321 (修复后)
    printf("NLEN=%d vary_start=%d, i_pre=%d\n", NLEN, vary_start, (int)na1.i_pre);
    na1.load_name_quint_shared_key(a,b,c,d,e,NLEN,vary_start,nb1,nc1,nd1,ne1);
    // 逐名字参考
    na2.load_name(a,NLEN);nb2.load_name(b,NLEN);nc2.load_name(c,NLEN);nd2.load_name(d,NLEN);ne2.load_name(e,NLEN);
    Name* pa[5]={&na1,&nb1,&nc1,&nd1,&ne1};
    Name* pb[5]={&na2,&nb2,&nc2,&nd2,&ne2};
    for(int k=0;k<5;k++){
        int d_=0; for(int i=0;i<256;i++) if(pa[k]->val[i]!=pb[k]->val[i]) d_++;
        printf("  shared_key vs 逐名字 val 差异: %c = %d/256\n", "abcde"[k], d_);
    }
    // 检查 Name 结构被越界写坏 (prefix_loaded/val_base 等)
    printf("  na1.prefix_loaded=%d (应为1) val_base[0]=%d saved_val[0]=%d\n",
        na1.prefix_loaded?1:0, na1.val_base[0], na1.saved_val[0]);
    delete[] a;delete[] b;delete[] c;delete[] d;delete[] e;
    return 0;
}
