// suffix_test2.cpp — 单字节字符集下验证非空后缀 shared_key 是否正确
// 字符 'a'..'j' (首字节即不同), scl=1
#include "../src/name.hpp"
#include <cstdio>
#include <cstring>

static const char TEAM[] = "test";
static const char PREFIX[] = "test-";
static const int PLEN = 5, VLEN = 8, SCL = 1, CLEN = 10;
static const int SLEN = 2;   // 非空后缀 "xy"
static const int NLEN = PLEN + VLEN*SCL + SLEN;   // 15
static const char* CS = "abcdefghij";   // 单字节字符集, 首字节不同
#define ENC(dst,ci) memcpy((dst), CS+(ci)*SCL, SCL)

static void build_names(char* a, char* b, char* c, char* d, char* e, uint64_t base) {
    for (int k=0;k<PLEN;k++) a[k]=b[k]=c[k]=d[k]=e[k]=PREFIX[k];
    uint64_t v[5] = {base, base+1, base+2, base+3, base+4};
    for (int i=0;i<5;i++) {
        char* buf = i==0?a:i==1?b:i==2?c:i==3?d:e;
        uint64_t nw = v[i];
        for (int p=VLEN-1;p>=0;p--){ ENC(buf+PLEN+p*SCL, nw%CLEN); nw/=CLEN; }
        buf[PLEN+VLEN*SCL] = 'x'; buf[PLEN+VLEN*SCL+1] = 'y';
        buf[NLEN] = 0;
    }
}

static int count_diff(Name& x, Name& y) {
    int d=0; for(int i=0;i<256;i++) if(x.val[i]!=y.val[i]) d++; return d;
}

int main(){
    for (uint64_t base : {0ull, 5ull, 20ull}) {
        char a[NLEN+4],b[NLEN+4],c[NLEN+4],d[NLEN+4],e[NLEN+4];
        build_names(a,b,c,d,e, base);
        Name na1,nb1,nc1,nd1,ne1, na2,nb2,nc2,nd2,ne2, na3,nb3,nc3,nd3,ne3;
        for (auto* p : {&na1,&nb1,&nc1,&nd1,&ne1,&na2,&nb2,&nc2,&nd2,&ne2,&na3,&nb3,&nc3,&nd3,&ne3}) {
            p->load_team(TEAM); p->PRELEN = PLEN;
        }
        auto lp=[&](Name& na,Name& nb,Name& nc,Name& nd,Name& ne){
            na.load_prefix(a,NLEN);nb.load_prefix(a,NLEN);nc.load_prefix(a,NLEN);nd.load_prefix(a,NLEN);ne.load_prefix(a,NLEN);
        };
        lp(na1,nb1,nc1,nd1,ne1); lp(na2,nb2,nc2,nd2,ne2); lp(na3,nb3,nc3,nd3,ne3);
        int vary_start_old = NLEN - SCL;              // 旧算法 (nlen-scl, 错误)
        int vary_start_new = PLEN+(VLEN-1)*SCL;       // 新算法 (epre+(evar-1)*scl, 正确)
        na1.load_name_quint_shared_key(a,b,c,d,e,NLEN,vary_start_old,nb1,nc1,nd1,ne1);   // 旧 vary_start
        na2.load_name_quint_shared_key(a,b,c,d,e,NLEN,vary_start_new,nb2,nc2,nd2,ne2);   // 新 vary_start
        na3.load_name_quint(a,b,c,d,e,NLEN,nb3,nc3,nd3,ne3);   // 自检测 (正确参考)
        printf("base=%llu vary_start_old=%d vary_start_new=%d 真实差异起点=%d\n",
            (unsigned long long)base, vary_start_old, vary_start_new, PLEN+(VLEN-1)*SCL);
        printf("  旧vary_start vs 自检测: a=%d b=%d c=%d d=%d e=%d\n",
            count_diff(na1,na3),count_diff(nb1,nb3),count_diff(nc1,nc3),count_diff(nd1,nd3),count_diff(ne1,ne3));
        printf("  新vary_start vs 自检测: a=%d b=%d c=%d d=%d e=%d\n",
            count_diff(na2,na3),count_diff(nb2,nb3),count_diff(nc2,nc3),count_diff(nd2,nd3),count_diff(ne2,ne3));
    }
    return 0;
}
