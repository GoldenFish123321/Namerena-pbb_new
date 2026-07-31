// ============================================================================
// enum_verify.cpp — 枚举逻辑穷尽验证
// 对比 consume_seq 的两种实现 (memcpy 链 vs 进位广播) 在同一名字序列上
// 产生的 5 个名字缓冲区 + KSA val 状态是否逐字节一致。
//
// 用法: icpx ... enum_verify.cpp -o enum_verify.exe && enum_verify.exe
// 通过即打印 "ENUM VERIFY: ALL MATCH (N names)"
// ============================================================================
#include "../src/name.hpp"
#include <cstdio>
#include <cstring>

// ---- 与引擎一致的 mode1 参数 (test.config.json) ----
static const char TEAM[] = "test";
static const char PREFIX[] = "test-";   // plen=5
static const int PLEN = 5;
static const int VLEN = 8;
static const int SCL = 4;
static const int CLEN = 10;
static const int NLEN = PLEN + VLEN * SCL;   // 37

static const unsigned char CHARSET_BYTES_U[10 * 4] = {
  0xF0,0x9F,0x99,0x88, 0xF0,0x9F,0x99,0x89, 0xF0,0x9F,0x99,0x8A, 0xF0,0x9F,0x90,0xB5,
  0xF0,0x9F,0x90,0x92, 0xF0,0x9F,0x90,0x9B, 0xF0,0x9F,0x90,0x8D, 0xF0,0x9F,0x90,0x8A,
  0xF0,0x9F,0xA6,0x8E, 0xF0,0x9F,0x90,0x89
};
static const char* CHARSET_BYTES = (const char*)CHARSET_BYTES_U;

#define ENC(dst,ci) do{const char*_s=CHARSET_BYTES+(ci)*SCL;memcpy((dst),_s,SCL);}while(0)

// 上下文: 5 个名字缓冲区 + 5 个 Name (KSA)
struct Ctx {
    char a[NLEN + 8], b[NLEN + 8], c[NLEN + 8], d[NLEN + 8], e[NLEN + 8];
    Name na, nb, nc, nd, ne;
    void init() {
        memset(a, 0, sizeof a); memset(b, 0, sizeof b);
        memset(c, 0, sizeof c); memset(d, 0, sizeof d); memset(e, 0, sizeof e);
        for (int k = 0; k < PLEN; k++) { a[k]=b[k]=c[k]=d[k]=e[k]=PREFIX[k]; }
        na.load_team(TEAM); nb.load_team(TEAM); nc.load_team(TEAM); nd.load_team(TEAM); ne.load_team(TEAM);
        na.PRELEN = nb.PRELEN = nc.PRELEN = nd.PRELEN = ne.PRELEN = PLEN;
        na.load_prefix(a, NLEN); nb.load_prefix(a, NLEN);
        nc.load_prefix(a, NLEN); nd.load_prefix(a, NLEN); ne.load_prefix(a, NLEN);
    }
};

// ---- 变体 A: memcpy 链 (原始) ----
// 前向: b..e 从 a 复制+增量 (KSA 输入: a=G, b=G+1, ..., e=G+4)
static void forwardA(Ctx& x, uint8_t* dig, int epre, int evar) {
    char* a=x.a; char* b=x.b; char* c=x.c; char* d=x.d; char* e=x.e;
    memcpy(b+epre,a+epre,evar*SCL);
    for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(b+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(b+epre+p*SCL,0);}
    memcpy(c+epre,b+epre,evar*SCL);
    for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(c+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(c+epre+p*SCL,0);}
    memcpy(d+epre,c+epre,evar*SCL);
    for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(d+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(d+epre+p*SCL,0);}
    memcpy(e+epre,d+epre,evar*SCL);
    for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(e+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(e+epre+p*SCL,0);}
}
// 尾部: a 从 e 复制+增量 → a=G+5
static void trailA(Ctx& x, uint8_t* dig, int epre, int evar) {
    char* a=x.a; char* e=x.e;
    memcpy(a+epre,e+epre,evar*SCL);
    for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(a+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(a+epre+p*SCL,0);}
}

// ---- 变体 B: 进位广播 (新) ----
static void forwardB(Ctx& x, uint8_t* dig, int epre, int evar, int carry_pos, int& cpos) {
    char* a=x.a; char* b=x.b; char* c=x.c; char* d=x.d; char* e=x.e;
    if(carry_pos>=0){
        for(int p=carry_pos;p<evar-1;p++){
            memcpy(b+epre+p*SCL,a+epre+p*SCL,SCL);
            memcpy(c+epre+p*SCL,a+epre+p*SCL,SCL);
            memcpy(d+epre+p*SCL,a+epre+p*SCL,SCL);
            memcpy(e+epre+p*SCL,a+epre+p*SCL,SCL);
        }
    }
    cpos=-1;
    #define INC_B(DST, T1, T2, T3) do{ \
        for(int p=evar-1;p>=0;p--){ \
            if(++dig[p]<(unsigned)CLEN){ \
                ENC(DST+epre+p*SCL,dig[p]); \
                if(p<evar-1){ if(cpos<0||p<cpos)cpos=p; ENC(T1+epre+p*SCL,dig[p]); ENC(T2+epre+p*SCL,dig[p]); ENC(T3+epre+p*SCL,dig[p]); } \
                break; \
            } \
            dig[p]=0; ENC(DST+epre+p*SCL,0); \
            if(p<evar-1){ if(cpos<0||p<cpos)cpos=p; ENC(T1+epre+p*SCL,0); ENC(T2+epre+p*SCL,0); ENC(T3+epre+p*SCL,0); } \
        } \
    }while(0)
    INC_B(b, c, d, e);
    INC_B(c, d, e, e);
    INC_B(d, e, e, e);
    INC_B(e, e, e, e);
    #undef INC_B
}
static void trailB(Ctx& x, uint8_t* dig, int epre, int evar, int cpos, int& carry_pos) {
    char* a=x.a; char* b=x.b; char* c=x.c; char* d=x.d; char* e=x.e;
    if(cpos>=0)
        memcpy(a+epre+cpos*SCL,e+epre+cpos*SCL,(evar-1-cpos)*SCL);
    #define INC_A(DST) do{ \
        for(int p=evar-1;p>=0;p--){ \
            if(++dig[p]<(unsigned)CLEN){ \
                ENC(DST+epre+p*SCL,dig[p]); \
                if(p<evar-1){ if(cpos<0||p<cpos)cpos=p; ENC(b+epre+p*SCL,dig[p]); ENC(c+epre+p*SCL,dig[p]); ENC(d+epre+p*SCL,dig[p]); ENC(e+epre+p*SCL,dig[p]); } \
                break; \
            } \
            dig[p]=0; ENC(DST+epre+p*SCL,0); \
            if(p<evar-1){ if(cpos<0||p<cpos)cpos=p; ENC(b+epre+p*SCL,0); ENC(c+epre+p*SCL,0); ENC(d+epre+p*SCL,0); ENC(e+epre+p*SCL,0); } \
        } \
    }while(0)
    INC_A(a);
    carry_pos = cpos;
    #undef INC_A
}

static bool cmp_ctx(Ctx& A, Ctx& B) {
    const char* names[5]={"a","b","c","d","e"};
    char* bufA[5]={A.a,A.b,A.c,A.d,A.e};
    char* bufB[5]={B.a,B.b,B.c,B.d,B.e};
    for(int k=0;k<5;k++){
        for(int i=0;i<NLEN+8;i++){
            if(bufA[k][i]!=bufB[k][i]){
                printf("  BUFFER diff: %s[%d] A=%d B=%d\n", names[k], i, (unsigned char)bufA[k][i], (unsigned char)bufB[k][i]);
                return false;
            }
        }
    }
    return true;
}

// 对一组 5 名字运行 KSA 并比较 val
static bool cmp_ksa(Ctx& A, Ctx& B, int nlen, int vary_start, bool can_shared, const char* tag) {
    if (can_shared) {
        A.na.load_name_quint_shared_key(A.a,A.b,A.c,A.d,A.e,nlen,vary_start,A.nb,A.nc,A.nd,A.ne);
        B.na.load_name_quint_shared_key(B.a,B.b,B.c,B.d,B.e,nlen,vary_start,B.nb,B.nc,B.nd,B.ne);
    } else {
        A.na.load_name_quint(A.a,A.b,A.c,A.d,A.e,nlen,A.nb,A.nc,A.nd,A.ne);
        B.na.load_name_quint(B.a,B.b,B.c,B.d,B.e,nlen,B.nb,B.nc,B.nd,B.ne);
    }
    Name* pa[5]={&A.na,&A.nb,&A.nc,&A.nd,&A.ne};
    Name* pb[5]={&B.na,&B.nb,&B.nc,&B.nd,&B.ne};
    for(int k=0;k<5;k++){
        for(int i=0;i<256;i++){
            if(pa[k]->val[i]!=pb[k]->val[i]){
                printf("KSA mismatch %s: name%d val[%d] A=%u B=%u\n", tag, k, i, pa[k]->val[i], pb[k]->val[i]);
                printf("  A state: s_pre=%u i_pre=%u j_pre=%u | B state: s_pre=%u i_pre=%u j_pre=%u\n",
                       A.na.s_pre, A.na.i_pre, A.na.j_pre, B.na.s_pre, B.na.i_pre, B.na.j_pre);
                printf("  buf37: A.a[37]=%d A.b[37]=%d | B.a[37]=%d B.b[37]=%d\n",
                       A.a[37], A.b[37], B.a[37], B.b[37]);
                printf("  A.a[33..36]=%d,%d,%d,%d B.a[33..36]=%d,%d,%d,%d\n",
                       A.a[33],A.a[34],A.a[35],A.a[36],B.a[33],B.a[34],B.a[35],B.a[36]);
                return false;
            }
        }
    }
    return true;
}

static bool run_range(uint64_t L, uint64_t R, long long& nnames) {
    const int epre = PLEN, evar = VLEN;
    const int vary_start = NLEN - SCL;
    Ctx A, B;
    A.init(); B.init();
    // 复刻 consume_mode1: dig=L, 缓冲区 a..e = L..L+4 (引擎预填全部 5 个)
    uint8_t dA[16], dB[16];
    {uint64_t now=L;for(int p=evar-1;p>=0;p--){dA[p]=now%CLEN;now/=CLEN;}}
    memcpy(dB, dA, sizeof dA);
    char* bufsA[5]={A.a,A.b,A.c,A.d,A.e};
    char* bufsB[5]={B.a,B.b,B.c,B.d,B.e};
    for(int k=0;k<5;k++){uint64_t nw=L+k;for(int p=evar-1;p>=0;p--){ENC(bufsA[k]+epre+p*SCL,nw%CLEN);ENC(bufsB[k]+epre+p*SCL,nw%CLEN);nw/=CLEN;}}
    int carryB=-1;
    uint64_t i;
    for(i=L;i+4<R;i+=5){
        bool canA = dA[evar-1]+4 < (unsigned)CLEN;
        bool canB = dB[evar-1]+4 < (unsigned)CLEN;
        if (canA != canB) { printf("can_shared mismatch at %llu\n", (unsigned long long)i); return false; }
        int cposB;
        forwardA(A, dA, epre, evar);
        forwardB(B, dB, epre, evar, carryB, cposB);
        // KSA 输入时刻: a=G, b=G+1..e=G+4 — 两变体必须完全一致
        if (!cmp_ctx(A, B)) { printf("BUFFER mismatch at group %llu (forward)\n", (unsigned long long)i); return false; }
        if (!cmp_ksa(A, B, NLEN, vary_start, canA, "g")) { printf("  at group %llu\n", (unsigned long long)i); return false; }
        if (memcmp(dA, dB, sizeof dA)) { printf("dig mismatch at group %llu\n", (unsigned long long)i); return false; }
        // 尾部 (a-INC): 两变体各自推进到下一组
        trailA(A, dA, epre, evar);
        trailB(B, dB, epre, evar, cposB, carryB);
        nnames += 5;
    }
    // tail
    for(; i<R; i++){
        if(i+1<R){
            for(int p=evar-1;p>=0;p--){if(++dA[p]<(unsigned)CLEN){ENC(A.a+epre+p*SCL,dA[p]);break;}dA[p]=0;ENC(A.a+epre+p*SCL,0);}
            for(int p=evar-1;p>=0;p--){if(++dB[p]<(unsigned)CLEN){ENC(B.a+epre+p*SCL,dB[p]);break;}dB[p]=0;ENC(B.a+epre+p*SCL,0);}
        }
        if (memcmp(A.a, B.a, NLEN)) { printf("TAIL buffer mismatch at %llu\n", (unsigned long long)i); return false; }
        nnames++;
    }
    return true;
}

int main() {
    long long total = 0;
    struct Rg { uint64_t L, R; };
    Rg rgs[] = {
        {0, 2000000},
        {99999800ull, 100000050ull},   // 跨 8 位全进位
        {9876540, 9876700},
        {123456, 123700},
        {99999980ull, 100000005ull},   // 跨全进位 + 尾部
        {50000000, 50000300},
        {100, 100013},                 // 带尾部
        {1234567890ull, 1234568000ull},// 大值 (9位, 跨位)
    };
    for (auto& r : rgs) {
        if (!run_range(r.L, r.R, total)) { printf("FAIL at [%llu,%llu)\n", (unsigned long long)r.L, (unsigned long long)r.R); return 1; }
        printf("range [%llu,%llu) OK\n", (unsigned long long)r.L, (unsigned long long)r.R);
    }
    printf("ENUM VERIFY: ALL MATCH (%lld names)\n", total);
    return 0;
}
