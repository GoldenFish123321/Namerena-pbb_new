// det_test.cpp — 测试 load_name_quint_shared_key 的确定性
#include "../src/name.hpp"
#include <cstdio>
#include <cstring>

static const char TEAM[] = "test";
static const char PREFIX[] = "test-";
static const int PLEN = 5, VLEN = 8, SCL = 4, CLEN = 10, NLEN = PLEN + VLEN*SCL;
static const unsigned char CS[10*4] = {
  0xF0,0x9F,0x99,0x88, 0xF0,0x9F,0x99,0x89, 0xF0,0x9F,0x99,0x8A, 0xF0,0x9F,0x90,0xB5,
  0xF0,0x9F,0x90,0x92, 0xF0,0x9F,0x90,0x9B, 0xF0,0x9F,0x90,0x8D, 0xF0,0x9F,0x90,0x8A,
  0xF0,0x9F,0xA6,0x8E, 0xF0,0x9F,0x90,0x89 };
#define ENC(dst,ci) memcpy((dst),(const char*)CS+(ci)*SCL,SCL)

int main(){
    char a[NLEN+8],b[NLEN+8],c[NLEN+8],d[NLEN+8],e[NLEN+8];
    memset(a,0,sizeof a);memset(b,0,sizeof b);memset(c,0,sizeof c);memset(d,0,sizeof d);memset(e,0,sizeof e);
    for(int k=0;k<PLEN;k++) a[k]=b[k]=c[k]=d[k]=e[k]=PREFIX[k];
    // a=5, b=1, c=2, d=3, e=4 (最后一位)
    {uint64_t nw=5;for(int p=VLEN-1;p>=0;p--){ENC(a+PLEN+p*SCL,nw%CLEN);nw/=CLEN;}}
    {uint64_t nw=1;for(int p=VLEN-1;p>=0;p--){ENC(b+PLEN+p*SCL,nw%CLEN);nw/=CLEN;}}
    {uint64_t nw=2;for(int p=VLEN-1;p>=0;p--){ENC(c+PLEN+p*SCL,nw%CLEN);nw/=CLEN;}}
    {uint64_t nw=3;for(int p=VLEN-1;p>=0;p--){ENC(d+PLEN+p*SCL,nw%CLEN);nw/=CLEN;}}
    {uint64_t nw=4;for(int p=VLEN-1;p>=0;p--){ENC(e+PLEN+p*SCL,nw%CLEN);nw/=CLEN;}}
    Name na,nb,nc,nd,ne;
    na.load_team(TEAM); nb.load_team(TEAM); nc.load_team(TEAM); nd.load_team(TEAM); ne.load_team(TEAM);
    na.PRELEN=nb.PRELEN=nc.PRELEN=nd.PRELEN=ne.PRELEN=PLEN;
    na.load_prefix(a,NLEN); nb.load_prefix(a,NLEN); nc.load_prefix(a,NLEN); nd.load_prefix(a,NLEN); ne.load_prefix(a,NLEN);
    int vary_start = NLEN - SCL;
    // 第一次
    na.load_name_quint_shared_key(a,b,c,d,e,NLEN,vary_start,nb,nc,nd,ne);
    unsigned char v1[5][256];
    memcpy(v1[0],na.val,256);memcpy(v1[1],nb.val,256);memcpy(v1[2],nc.val,256);memcpy(v1[3],nd.val,256);memcpy(v1[4],ne.val,256);
    // 第二次 (重置)
    na.PRELEN=nb.PRELEN=nc.PRELEN=nd.PRELEN=ne.PRELEN=PLEN;
    na.load_prefix(a,NLEN); nb.load_prefix(a,NLEN); nc.load_prefix(a,NLEN); nd.load_prefix(a,NLEN); ne.load_prefix(a,NLEN);
    na.load_name_quint_shared_key(a,b,c,d,e,NLEN,vary_start,nb,nc,nd,ne);
    int err=0;
    for(int k=0;k<5;k++) if(memcmp(v1[k], (k==0?na.val:k==1?nb.val:k==2?nc.val:k==3?nd.val:ne.val),256)) err++;
    printf("determinism errors: %d\n", err);
    // 打印第一遍 val[0..7]
    printf("run1 name0 val[0..7]: %u %u %u %u %u %u %u %u\n", v1[0][0],v1[0][1],v1[0][2],v1[0][3],v1[0][4],v1[0][5],v1[0][6],v1[0][7]);
    return 0;
}
