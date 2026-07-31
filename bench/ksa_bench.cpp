// ============================================================================
// ksa_bench.cpp — 微基准: 隔离测速核心热路径 (KSA + 属性过滤 + V 检查)
//
// 复刻 engine.hpp 中 mode 1 + PAIR_WIDTH=5 的 consume 路径, 不含 Python/队列/IO。
//
// 实验开关 (编译期):
//   -DEXP0  完整路径 (基线)
//   -DEXP1  跳过 KSA 第二遍
//   -DEXP2  跳过属性过滤 finish (仅 KSA)
//   -DEXP3  跳过 KSA 运算体 (循环仍在)
//   -DEXP8  跳过 val 的 memcpy
//   -DEXP9  跳过变量部分 ENC/memcpy
//   -DEXP10 非共享路径也用本地 ksa_quint (vary_start=0)
//   -DEXP11 ksa_quint 为真空操作 (纯循环控制)
//   -DEXP6  32 位累加器
// ============================================================================
#include "../src/engine.hpp"
#include <chrono>

// ---- 与引擎一致的模式1参数 (test.config.json) ----
static const char TEAM[] = "test";
static const char PREFIX[] = "test-";   // plen=5
static const int PLEN = 5;
static const int VLEN = 8;
static const int SCL = 4;
static const int CLEN = 10;
static const int NLEN = PLEN + VLEN * SCL;   // 37

// 字符集: 10 个 emoji (与 test.config.json custom_values 一致)
static const unsigned char CHARSET_BYTES_U[10 * 4] = {
  0xF0,0x9F,0x99,0x88, 0xF0,0x9F,0x99,0x89, 0xF0,0x9F,0x99,0x8A, 0xF0,0x9F,0x90,0xB5,
  0xF0,0x9F,0x90,0x92, 0xF0,0x9F,0x90,0x9B, 0xF0,0x9F,0x90,0x8D, 0xF0,0x9F,0x90,0x8A,
  0xF0,0x9F,0xA6,0x8E, 0xF0,0x9F,0x90,0x89
};
static const char* CHARSET_BYTES = (const char*)CHARSET_BYTES_U;

#define ENC(dst,ci) do{const char*_s=CHARSET_BYTES+(ci)*SCL;if(SCL==4)memcpy((dst),_s,4);else if(SCL==1)*(dst)=*_s;else if(SCL==2)memcpy((dst),_s,2);else if(SCL==3)memcpy((dst),_s,3);else memcpy((dst),_s,SCL);}while(0)

// ===== 本地 KSA quint (5 链交错 + 共享 key) — 复刻 load_name_quint_shared_key =====
static void ksa_quint(char* a, char* b, char* c, char* d, char* e, int nlen, int vary_start,
                      Name& na, Name& nb, Name& nc, Name& nd, Name& ne) {
    na.q_len = -1; nb.q_len = -1; nc.q_len = -1; nd.q_len = -1; ne.q_len = -1;
#if defined(EXP11)
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)nlen;(void)vary_start;
    return;
#endif
#if !defined(EXP8)
    memcpy(na.val, na.prefix_loaded ? na.saved_val : na.val_base2, sizeof na.val);
    memcpy(nb.val, nb.prefix_loaded ? nb.saved_val : nb.val_base2, sizeof nb.val);
    memcpy(nc.val, nc.prefix_loaded ? nc.saved_val : nc.val_base2, sizeof nc.val);
    memcpy(nd.val, nd.prefix_loaded ? nd.saved_val : nd.val_base2, sizeof nd.val);
    memcpy(ne.val, ne.prefix_loaded ? ne.saved_val : ne.val_base2, sizeof ne.val);
#endif
#if defined(EXP6)
    uint32_t sa = na.s_pre, sb = nb.s_pre, sc = nc.s_pre, sd = nd.s_pre, se = ne.s_pre;
    const uint32_t MASK = 0xFFu;
#else
    u8_t sa = na.s_pre, sb = nb.s_pre, sc = nc.s_pre, sd = nd.s_pre, se = ne.s_pre;
    const uint32_t MASK = 0xFFu;
#endif
    const char* __restrict nm_a = a; const char* __restrict nm_b = b;
    const char* __restrict nm_c = c; const char* __restrict nm_d = d;
    const char* __restrict nm_e = e;
    u8_t* __restrict va = na.val; u8_t* __restrict vb = nb.val;
    u8_t* __restrict vc = nc.val; u8_t* __restrict vd = nd.val;
    u8_t* __restrict ve = ne.val;
    const int kN = N;
    // ---- 第一遍 KSA ----
    for (int i = na.i_pre, j = na.j_pre; i < kN; i++, j++) {
#if defined(EXP3)
        (void)0;
#else
      if (j >= vary_start) {
#if defined(EXP6)
        sa = (sa + nm_a[j] + va[i]) & MASK; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
        sb = (sb + nm_b[j] + vb[i]) & MASK; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
        sc = (sc + nm_c[j] + vc[i]) & MASK; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
        sd = (sd + nm_d[j] + vd[i]) & MASK; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
        se = (se + nm_e[j] + ve[i]) & MASK; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
#else
        sa += nm_a[j] + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
        sb += nm_b[j] + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
        sc += nm_c[j] + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
        sd += nm_d[j] + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
        se += nm_e[j] + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
#endif
      } else {
        u8_t kb = nm_a[j];
#if defined(EXP6)
        sa = (sa + kb + va[i]) & MASK; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
        sb = (sb + kb + vb[i]) & MASK; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
        sc = (sc + kb + vc[i]) & MASK; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
        sd = (sd + kb + vd[i]) & MASK; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
        se = (se + kb + ve[i]) & MASK; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
#else
        sa += kb + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
        sb += kb + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
        sc += kb + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
        sd += kb + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
        se += kb + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
#endif
      }
#endif
      if (j == nlen) j = -1;
    }
#if defined(EXP1)
    (void)0;
#else
    // ---- 第二遍 KSA ----
    sa = 0; sb = 0; sc = 0; sd = 0; se = 0;
    for (int i = 0, j = nlen; i < kN; i++, j++) {
#if defined(EXP3)
        (void)0;
#else
      if (j >= vary_start) {
#if defined(EXP6)
        sa = (sa + nm_a[j] + va[i]) & MASK; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
        sb = (sb + nm_b[j] + vb[i]) & MASK; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
        sc = (sc + nm_c[j] + vc[i]) & MASK; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
        sd = (sd + nm_d[j] + vd[i]) & MASK; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
        se = (se + nm_e[j] + ve[i]) & MASK; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
#else
        sa += nm_a[j] + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
        sb += nm_b[j] + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
        sc += nm_c[j] + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
        sd += nm_d[j] + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
        se += nm_e[j] + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
#endif
      } else {
        u8_t kb = nm_a[j];
#if defined(EXP6)
        sa = (sa + kb + va[i]) & MASK; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
        sb = (sb + kb + vb[i]) & MASK; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
        sc = (sc + kb + vc[i]) & MASK; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
        sd = (sd + kb + vd[i]) & MASK; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
        se = (se + kb + ve[i]) & MASK; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
#else
        sa += kb + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
        sb += kb + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
        sc += kb + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
        sd += kb + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
        se += kb + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
#endif
      }
#endif
      if (j == nlen) j = -1;
    }
#endif
    na._ksa_done = true; nb._ksa_done = true; nc._ksa_done = true;
    nd._ksa_done = true; ne._ksa_done = true;
}

#if defined(EXP13)
// 前置声明
static void finish_variant(Name& na);

// ===== 6 链 KSA =====
static void ksa_sext(char* a, char* b, char* c, char* d, char* e, char* f, int nlen, int vary_start,
                     Name& na, Name& nb, Name& nc, Name& nd, Name& ne, Name& nf) {
    na.q_len=-1; nb.q_len=-1; nc.q_len=-1; nd.q_len=-1; ne.q_len=-1; nf.q_len=-1;
    memcpy(na.val, na.prefix_loaded ? na.saved_val : na.val_base2, sizeof na.val);
    memcpy(nb.val, nb.prefix_loaded ? nb.saved_val : nb.val_base2, sizeof nb.val);
    memcpy(nc.val, nc.prefix_loaded ? nc.saved_val : nc.val_base2, sizeof nc.val);
    memcpy(nd.val, nd.prefix_loaded ? nd.saved_val : nd.val_base2, sizeof nd.val);
    memcpy(ne.val, ne.prefix_loaded ? ne.saved_val : ne.val_base2, sizeof ne.val);
    memcpy(nf.val, nf.prefix_loaded ? nf.saved_val : nf.val_base2, sizeof nf.val);
    u8_t sa=na.s_pre, sb=nb.s_pre, sc=nc.s_pre, sd=nd.s_pre, se=ne.s_pre, sf=nf.s_pre;
    const char* __restrict nm_a=a; const char* __restrict nm_b=b; const char* __restrict nm_c=c;
    const char* __restrict nm_d=d; const char* __restrict nm_e=e; const char* __restrict nm_f=f;
    u8_t* __restrict va=na.val; u8_t* __restrict vb=nb.val; u8_t* __restrict vc=nc.val;
    u8_t* __restrict vd=nd.val; u8_t* __restrict ve=ne.val; u8_t* __restrict vf=nf.val;
    const int kN=N;
    for (int i = na.i_pre, j = na.j_pre; i < kN; i++, j++) {
      if (j >= vary_start) {
        sa += nm_a[j] + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
        sb += nm_b[j] + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
        sc += nm_c[j] + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
        sd += nm_d[j] + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
        se += nm_e[j] + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
        sf += nm_f[j] + vf[i]; { u8_t t = vf[i]; vf[i] = vf[sf]; vf[sf] = t; }
      } else {
        u8_t kb = nm_a[j];
        sa += kb + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
        sb += kb + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
        sc += kb + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
        sd += kb + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
        se += kb + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
        sf += kb + vf[i]; { u8_t t = vf[i]; vf[i] = vf[sf]; vf[sf] = t; }
      }
      if (j == nlen) j = -1;
    }
    sa=0; sb=0; sc=0; sd=0; se=0; sf=0;
    for (int i = 0, j = nlen; i < kN; i++, j++) {
      if (j >= vary_start) {
        sa += nm_a[j] + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
        sb += nm_b[j] + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
        sc += nm_c[j] + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
        sd += nm_d[j] + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
        se += nm_e[j] + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
        sf += nm_f[j] + vf[i]; { u8_t t = vf[i]; vf[i] = vf[sf]; vf[sf] = t; }
      } else {
        u8_t kb = nm_a[j];
        sa += kb + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
        sb += kb + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
        sc += kb + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
        sd += kb + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
        se += kb + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
        sf += kb + vf[i]; { u8_t t = vf[i]; vf[i] = vf[sf]; vf[sf] = t; }
      }
      if (j == nlen) j = -1;
    }
    na._ksa_done=nb._ksa_done=nc._ksa_done=nd._ksa_done=ne._ksa_done=nf._ksa_done=true;
}

// ---- 6 链 consume_seq ----
static void consume_seq6(char* a,int nlen,Name& na,char* b,Name& nb,char* c,Name& nc,char* d,Name& nd,char* e,Name& ne,char* f,Name& nf,
                         int epre,int evar,uint64_t L,uint64_t R,uint64_t& names_done){
    int vary_start = nlen - SCL;
    uint8_t dig[16];
    {uint64_t now=L;for(int p=evar-1;p>=0;p--){dig[p]=now%CLEN;ENC(a+epre+p*SCL,dig[p]);now/=CLEN;}}
    for(uint64_t i=L;i+5<R;i+=6){
        memcpy(b+epre,a+epre,evar*SCL);
        for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(b+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(b+epre+p*SCL,0);}
        memcpy(c+epre,b+epre,evar*SCL);
        for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(c+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(c+epre+p*SCL,0);}
        memcpy(d+epre,c+epre,evar*SCL);
        for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(d+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(d+epre+p*SCL,0);}
        memcpy(e+epre,d+epre,evar*SCL);
        for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(e+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(e+epre+p*SCL,0);}
        memcpy(f+epre,e+epre,evar*SCL);
        for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(f+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(f+epre+p*SCL,0);}
        ksa_sext(a,b,c,d,e,f,nlen,vary_start,na,nb,nc,nd,ne,nf);
        finish_variant(na); finish_variant(nb); finish_variant(nc);
        finish_variant(nd); finish_variant(ne); finish_variant(nf);
        names_done += 6;
        memcpy(a+epre,f+epre,evar*SCL);
        for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(a+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(a+epre+p*SCL,0);}
    }
    for(uint64_t i=L+((R-L)/6)*6;i<R;i++){
        na._ksa_done=false; na.load_name(a,nlen);
        finish_variant(na);
        names_done += 1;
        if(i+1<R){for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(a+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(a+epre+p*SCL,0);}}
    }
}

static void consume_mode1_6(char* a,int nlen,Name& na,char* b,Name& nb,char* c,Name& nc,char* d,Name& nd,char* e,Name& ne,char* f,Name& nf,int plen,int vlen,uint64_t L,uint64_t R,uint64_t& names_done){
    na.PRELEN=plen;na.load_prefix(a,nlen);
    nb.PRELEN=plen;nb.load_prefix(a,nlen);
    nc.PRELEN=plen;nc.load_prefix(a,nlen);
    nd.PRELEN=plen;nd.load_prefix(a,nlen);
    ne.PRELEN=plen;ne.load_prefix(a,nlen);
    nf.PRELEN=plen;nf.load_prefix(a,nlen);
    uint8_t dl[16],dr[16];uint64_t now;
    now=L;for(int dd=vlen-1;dd>=0;dd--){dl[dd]=now%CLEN;now/=CLEN;}
    now=R-1;for(int dd=vlen-1;dd>=0;dd--){dr[dd]=now%CLEN;now/=CLEN;}
    int up=0;while(up<vlen&&dl[up]==dr[up])up++;
    now=L;for(int dd=vlen-1;dd>=0;dd--){int ci=now%CLEN;ENC(a+plen+dd*SCL,ci);now/=CLEN;}
    now=L+1;for(int dd=vlen-1;dd>=0;dd--){int ci=now%CLEN;ENC(b+plen+dd*SCL,ci);now/=CLEN;}
    now=L+2;for(int dd=vlen-1;dd>=0;dd--){int ci=now%CLEN;ENC(c+plen+dd*SCL,ci);now/=CLEN;}
    now=L+3;for(int dd=vlen-1;dd>=0;dd--){int ci=now%CLEN;ENC(d+plen+dd*SCL,ci);now/=CLEN;}
    now=L+4;for(int dd=vlen-1;dd>=0;dd--){int ci=now%CLEN;ENC(e+plen+dd*SCL,ci);now/=CLEN;}
    now=L+5;for(int dd=vlen-1;dd>=0;dd--){int ci=now%CLEN;ENC(f+plen+dd*SCL,ci);now/=CLEN;}
    int epre=plen+up*SCL,evar=vlen-up;
    consume_seq6(a,nlen,na,b,nb,c,nc,d,nd,e,ne,f,nf,epre,evar,L,R,names_done);
}
#endif

#if defined(EXP14)
// ===== 2× 手动展开的 5 链 KSA (软件流水线辅助调度) =====
// 处理 2 个 i 迭代, 让编译器提前调度独立的 val[i] 加载以隐藏依赖延迟
static void ksa_quint_unroll2(char* a, char* b, char* c, char* d, char* e, int nlen, int vary_start,
                      Name& na, Name& nb, Name& nc, Name& nd, Name& ne) {
    na.q_len = -1; nb.q_len = -1; nc.q_len = -1; nd.q_len = -1; ne.q_len = -1;
    memcpy(na.val, na.prefix_loaded ? na.saved_val : na.val_base2, sizeof na.val);
    memcpy(nb.val, nb.prefix_loaded ? nb.saved_val : nb.val_base2, sizeof nb.val);
    memcpy(nc.val, nc.prefix_loaded ? nc.saved_val : nc.val_base2, sizeof nc.val);
    memcpy(nd.val, nd.prefix_loaded ? nd.saved_val : nd.val_base2, sizeof nd.val);
    memcpy(ne.val, ne.prefix_loaded ? ne.saved_val : ne.val_base2, sizeof ne.val);
    const char* __restrict nm_a = a; const char* __restrict nm_b = b;
    const char* __restrict nm_c = c; const char* __restrict nm_d = d;
    const char* __restrict nm_e = e;
    u8_t* __restrict va0 = na.val; u8_t* __restrict va1 = nb.val;
    u8_t* __restrict va2 = nc.val; u8_t* __restrict va3 = nd.val;
    u8_t* __restrict va4 = ne.val;
    const int kN = N;
    // ---- 第一遍 (2× unroll) ----
    {
        int i = na.i_pre, j = na.j_pre;
        u8_t sa = na.s_pre, sb = na.s_pre, sc = na.s_pre, sd = na.s_pre, se = na.s_pre;
        for (; i + 1 < kN; i += 2) {
            // 迭代 i (j)
            if (j >= vary_start) {
                sa += nm_a[j] + va0[i]; { u8_t t = va0[i]; va0[i] = va0[sa]; va0[sa] = t; }
                sb += nm_b[j] + va1[i]; { u8_t t = va1[i]; va1[i] = va1[sb]; va1[sb] = t; }
                sc += nm_c[j] + va2[i]; { u8_t t = va2[i]; va2[i] = va2[sc]; va2[sc] = t; }
                sd += nm_d[j] + va3[i]; { u8_t t = va3[i]; va3[i] = va3[sd]; va3[sd] = t; }
                se += nm_e[j] + va4[i]; { u8_t t = va4[i]; va4[i] = va4[se]; va4[se] = t; }
            } else {
                u8_t kb = nm_a[j];
                sa += kb + va0[i]; { u8_t t = va0[i]; va0[i] = va0[sa]; va0[sa] = t; }
                sb += kb + va1[i]; { u8_t t = va1[i]; va1[i] = va1[sb]; va1[sb] = t; }
                sc += kb + va2[i]; { u8_t t = va2[i]; va2[i] = va2[sc]; va2[sc] = t; }
                sd += kb + va3[i]; { u8_t t = va3[i]; va3[i] = va3[sd]; va3[sd] = t; }
                se += kb + va4[i]; { u8_t t = va4[i]; va4[i] = va4[se]; va4[se] = t; }
            }
            if (j == nlen) j = -1;
            j++;
            // 迭代 i+1 (j)
            if (j >= vary_start) {
                sa += nm_a[j] + va0[i+1]; { u8_t t = va0[i+1]; va0[i+1] = va0[sa]; va0[sa] = t; }
                sb += nm_b[j] + va1[i+1]; { u8_t t = va1[i+1]; va1[i+1] = va1[sb]; va1[sb] = t; }
                sc += nm_c[j] + va2[i+1]; { u8_t t = va2[i+1]; va2[i+1] = va2[sc]; va2[sc] = t; }
                sd += nm_d[j] + va3[i+1]; { u8_t t = va3[i+1]; va3[i+1] = va3[sd]; va3[sd] = t; }
                se += nm_e[j] + va4[i+1]; { u8_t t = va4[i+1]; va4[i+1] = va4[se]; va4[se] = t; }
            } else {
                u8_t kb = nm_a[j];
                sa += kb + va0[i+1]; { u8_t t = va0[i+1]; va0[i+1] = va0[sa]; va0[sa] = t; }
                sb += kb + va1[i+1]; { u8_t t = va1[i+1]; va1[i+1] = va1[sb]; va1[sb] = t; }
                sc += kb + va2[i+1]; { u8_t t = va2[i+1]; va2[i+1] = va2[sc]; va2[sc] = t; }
                sd += kb + va3[i+1]; { u8_t t = va3[i+1]; va3[i+1] = va3[sd]; va3[sd] = t; }
                se += kb + va4[i+1]; { u8_t t = va4[i+1]; va4[i+1] = va4[se]; va4[se] = t; }
            }
            if (j == nlen) j = -1;
            j++;
        }
        for (; i < kN; i++) {
            if (j >= vary_start) {
                sa += nm_a[j] + va0[i]; { u8_t t = va0[i]; va0[i] = va0[sa]; va0[sa] = t; }
                sb += nm_b[j] + va1[i]; { u8_t t = va1[i]; va1[i] = va1[sb]; va1[sb] = t; }
                sc += nm_c[j] + va2[i]; { u8_t t = va2[i]; va2[i] = va2[sc]; va2[sc] = t; }
                sd += nm_d[j] + va3[i]; { u8_t t = va3[i]; va3[i] = va3[sd]; va3[sd] = t; }
                se += nm_e[j] + va4[i]; { u8_t t = va4[i]; va4[i] = va4[se]; va4[se] = t; }
            } else {
                u8_t kb = nm_a[j];
                sa += kb + va0[i]; { u8_t t = va0[i]; va0[i] = va0[sa]; va0[sa] = t; }
                sb += kb + va1[i]; { u8_t t = va1[i]; va1[i] = va1[sb]; va1[sb] = t; }
                sc += kb + va2[i]; { u8_t t = va2[i]; va2[i] = va2[sc]; va2[sc] = t; }
                sd += kb + va3[i]; { u8_t t = va3[i]; va3[i] = va3[sd]; va3[sd] = t; }
                se += kb + va4[i]; { u8_t t = va4[i]; va4[i] = va4[se]; va4[se] = t; }
            }
            if (j == nlen) j = -1;
            j++;
        }
    }
    // ---- 第二遍 (2× unroll) ----
    {
        int i = 0, j = nlen;
        u8_t sa = 0, sb = 0, sc = 0, sd = 0, se = 0;
        for (; i + 1 < kN; i += 2) {
            if (j >= vary_start) {
                sa += nm_a[j] + va0[i]; { u8_t t = va0[i]; va0[i] = va0[sa]; va0[sa] = t; }
                sb += nm_b[j] + va1[i]; { u8_t t = va1[i]; va1[i] = va1[sb]; va1[sb] = t; }
                sc += nm_c[j] + va2[i]; { u8_t t = va2[i]; va2[i] = va2[sc]; va2[sc] = t; }
                sd += nm_d[j] + va3[i]; { u8_t t = va3[i]; va3[i] = va3[sd]; va3[sd] = t; }
                se += nm_e[j] + va4[i]; { u8_t t = va4[i]; va4[i] = va4[se]; va4[se] = t; }
            } else {
                u8_t kb = nm_a[j];
                sa += kb + va0[i]; { u8_t t = va0[i]; va0[i] = va0[sa]; va0[sa] = t; }
                sb += kb + va1[i]; { u8_t t = va1[i]; va1[i] = va1[sb]; va1[sb] = t; }
                sc += kb + va2[i]; { u8_t t = va2[i]; va2[i] = va2[sc]; va2[sc] = t; }
                sd += kb + va3[i]; { u8_t t = va3[i]; va3[i] = va3[sd]; va3[sd] = t; }
                se += kb + va4[i]; { u8_t t = va4[i]; va4[i] = va4[se]; va4[se] = t; }
            }
            if (j == nlen) j = -1;
            j++;
            if (j >= vary_start) {
                sa += nm_a[j] + va0[i+1]; { u8_t t = va0[i+1]; va0[i+1] = va0[sa]; va0[sa] = t; }
                sb += nm_b[j] + va1[i+1]; { u8_t t = va1[i+1]; va1[i+1] = va1[sb]; va1[sb] = t; }
                sc += nm_c[j] + va2[i+1]; { u8_t t = va2[i+1]; va2[i+1] = va2[sc]; va2[sc] = t; }
                sd += nm_d[j] + va3[i+1]; { u8_t t = va3[i+1]; va3[i+1] = va3[sd]; va3[sd] = t; }
                se += nm_e[j] + va4[i+1]; { u8_t t = va4[i+1]; va4[i+1] = va4[se]; va4[se] = t; }
            } else {
                u8_t kb = nm_a[j];
                sa += kb + va0[i+1]; { u8_t t = va0[i+1]; va0[i+1] = va0[sa]; va0[sa] = t; }
                sb += kb + va1[i+1]; { u8_t t = va1[i+1]; va1[i+1] = va1[sb]; va1[sb] = t; }
                sc += kb + va2[i+1]; { u8_t t = va2[i+1]; va2[i+1] = va2[sc]; va2[sc] = t; }
                sd += kb + va3[i+1]; { u8_t t = va3[i+1]; va3[i+1] = va3[sd]; va3[sd] = t; }
                se += kb + va4[i+1]; { u8_t t = va4[i+1]; va4[i+1] = va4[se]; va4[se] = t; }
            }
            if (j == nlen) j = -1;
            j++;
        }
        for (; i < kN; i++) {
            if (j >= vary_start) {
                sa += nm_a[j] + va0[i]; { u8_t t = va0[i]; va0[i] = va0[sa]; va0[sa] = t; }
                sb += nm_b[j] + va1[i]; { u8_t t = va1[i]; va1[i] = va1[sb]; va1[sb] = t; }
                sc += nm_c[j] + va2[i]; { u8_t t = va2[i]; va2[i] = va2[sc]; va2[sc] = t; }
                sd += nm_d[j] + va3[i]; { u8_t t = va3[i]; va3[i] = va3[sd]; va3[sd] = t; }
                se += nm_e[j] + va4[i]; { u8_t t = va4[i]; va4[i] = va4[se]; va4[se] = t; }
            } else {
                u8_t kb = nm_a[j];
                sa += kb + va0[i]; { u8_t t = va0[i]; va0[i] = va0[sa]; va0[sa] = t; }
                sb += kb + va1[i]; { u8_t t = va1[i]; va1[i] = va1[sb]; va1[sb] = t; }
                sc += kb + va2[i]; { u8_t t = va2[i]; va2[i] = va2[sc]; va2[sc] = t; }
                sd += kb + va3[i]; { u8_t t = va3[i]; va3[i] = va3[sd]; va3[sd] = t; }
                se += kb + va4[i]; { u8_t t = va4[i]; va4[i] = va4[se]; va4[se] = t; }
            }
            if (j == nlen) j = -1;
            j++;
        }
    }
    na._ksa_done = true; nb._ksa_done = true; nc._ksa_done = true;
    nd._ksa_done = true; ne._ksa_done = true;
}
#endif

#if defined(EXP15)
// ===== 诊断: 无数据依赖索引的 5 链 KSA (测依赖延迟上限) =====
static void ksa_quint_nodep(char* a, char* b, char* c, char* d, char* e, int nlen, int vary_start,
                     Name& na, Name& nb, Name& nc, Name& nd, Name& ne) {
    na.q_len=-1; nb.q_len=-1; nc.q_len=-1; nd.q_len=-1; ne.q_len=-1;
    memcpy(na.val, na.prefix_loaded ? na.saved_val : na.val_base2, sizeof na.val);
    memcpy(nb.val, nb.prefix_loaded ? nb.saved_val : nb.val_base2, sizeof nb.val);
    memcpy(nc.val, nc.prefix_loaded ? nc.saved_val : nc.val_base2, sizeof nc.val);
    memcpy(nd.val, nd.prefix_loaded ? nd.saved_val : nd.val_base2, sizeof nd.val);
    memcpy(ne.val, ne.prefix_loaded ? ne.saved_val : ne.val_base2, sizeof ne.val);
    u8_t sa=na.s_pre, sb=nb.s_pre, sc=nc.s_pre, sd=nd.s_pre, se=ne.s_pre;
    const char* __restrict nm_a=a;
    u8_t* __restrict va=na.val; u8_t* __restrict vb=nb.val; u8_t* __restrict vc=nc.val;
    u8_t* __restrict vd=nd.val; u8_t* __restrict ve=ne.val;
    const int kN=N;
    for (int i = na.i_pre, j = na.j_pre; i < kN; i++, j++) {
      u8_t kb = nm_a[j];
      int k0 = (i*7+1)&255;
      sa += kb + va[i]; { u8_t t = va[i]; va[i] = va[k0]; va[k0] = t; }
      sb += kb + vb[i]; { u8_t t = vb[i]; vb[i] = vb[k0]; vb[k0] = t; }
      sc += kb + vc[i]; { u8_t t = vc[i]; vc[i] = vc[k0]; vc[k0] = t; }
      sd += kb + vd[i]; { u8_t t = vd[i]; vd[i] = vd[k0]; vd[k0] = t; }
      se += kb + ve[i]; { u8_t t = ve[i]; ve[i] = ve[k0]; ve[k0] = t; }
      if (j == nlen) j = -1;
    }
    sa=0; sb=0; sc=0; sd=0; se=0;
    for (int i = 0, j = nlen; i < kN; i++, j++) {
      u8_t kb = nm_a[j];
      int k0 = (i*7+1)&255;
      sa += kb + va[i]; { u8_t t = va[i]; va[i] = va[k0]; va[k0] = t; }
      sb += kb + vb[i]; { u8_t t = vb[i]; vb[i] = vb[k0]; vb[k0] = t; }
      sc += kb + vc[i]; { u8_t t = vc[i]; vc[i] = vc[k0]; vc[k0] = t; }
      sd += kb + vd[i]; { u8_t t = vd[i]; vd[i] = vd[k0]; vd[k0] = t; }
      se += kb + ve[i]; { u8_t t = ve[i]; ve[i] = ve[k0]; ve[k0] = t; }
      if (j == nlen) j = -1;
    }
    na._ksa_done=nb._ksa_done=nc._ksa_done=nd._ksa_done=ne._ksa_done=true;
}
#endif

#if defined(EXP16)
// ===== 共享前缀 KSA: pass1 的前 sp_len 次迭代对 5 链相同, 只算一次再广播 =====
static void ksa_quint_sp(char* a, char* b, char* c, char* d, char* e, int nlen, int vary_start,
                         int sp_len, Name& na, Name& nb, Name& nc, Name& nd, Name& ne) {
    na.q_len=-1; nb.q_len=-1; nc.q_len=-1; nd.q_len=-1; ne.q_len=-1;
    u8_t S1[256];
    memcpy(S1, na.prefix_loaded ? na.saved_val : na.val_base2, sizeof S1);
    u8_t s1 = na.s_pre;
    int i_cont = na.i_pre, j_cont = na.j_pre;
    // 1. 共享前缀 (单链, sp_len 次迭代)
    for (int k = 0; k < sp_len; k++, i_cont++, j_cont++) {
        s1 += a[j_cont] + S1[i_cont];
        { u8_t t = S1[i_cont]; S1[i_cont] = S1[s1]; S1[s1] = t; }
        if (j_cont == nlen) j_cont = -1;
    }
    // 2. 广播到 5 链
    memcpy(na.val, S1, sizeof S1);
    memcpy(nb.val, S1, sizeof S1);
    memcpy(nc.val, S1, sizeof S1);
    memcpy(nd.val, S1, sizeof S1);
    memcpy(ne.val, S1, sizeof S1);
    const char* __restrict nm_a=a; const char* __restrict nm_b=b; const char* __restrict nm_c=c;
    const char* __restrict nm_d=d; const char* __restrict nm_e=e;
    u8_t* __restrict va=na.val; u8_t* __restrict vb=nb.val; u8_t* __restrict vc=nc.val;
    u8_t* __restrict vd=nd.val; u8_t* __restrict ve=ne.val;
    const int kN=N;
    // 3. pass1 续跑 (5 链, 从 i_cont 开始)
    {
        u8_t sa=s1, sb=s1, sc=s1, sd=s1, se=s1;
        for (int i = i_cont, j = j_cont; i < kN; i++, j++) {
          if (j >= vary_start) {
            sa += nm_a[j] + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
            sb += nm_b[j] + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
            sc += nm_c[j] + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
            sd += nm_d[j] + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
            se += nm_e[j] + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
          } else {
            u8_t kb = nm_a[j];
            sa += kb + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
            sb += kb + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
            sc += kb + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
            sd += kb + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
            se += kb + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
          }
          if (j == nlen) j = -1;
        }
    }
    // 4. pass2 (5 链, 无共享)
    {
        u8_t sa=0, sb=0, sc=0, sd=0, se=0;
        for (int i = 0, j = nlen; i < kN; i++, j++) {
          if (j >= vary_start) {
            sa += nm_a[j] + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
            sb += nm_b[j] + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
            sc += nm_c[j] + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
            sd += nm_d[j] + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
            se += nm_e[j] + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
          } else {
            u8_t kb = nm_a[j];
            sa += kb + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
            sb += kb + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
            sc += kb + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
            sd += kb + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
            se += kb + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
          }
          if (j == nlen) j = -1;
        }
    }
    na._ksa_done=nb._ksa_done=nc._ksa_done=nd._ksa_done=ne._ksa_done=true;
}
#endif

#if defined(EXP17)
// ===== 共享前缀 + 2× 展开续跑 =====
static void ksa_quint_sp_u2(char* a, char* b, char* c, char* d, char* e, int nlen, int vary_start,
                            int sp_len, Name& na, Name& nb, Name& nc, Name& nd, Name& ne) {
    na.q_len=-1; nb.q_len=-1; nc.q_len=-1; nd.q_len=-1; ne.q_len=-1;
    u8_t S1[256];
    memcpy(S1, na.prefix_loaded ? na.saved_val : na.val_base2, sizeof S1);
    u8_t s1 = na.s_pre;
    int i_cont = na.i_pre, j_cont = na.j_pre;
    for (int k = 0; k < sp_len; k++, i_cont++, j_cont++) {
        s1 += a[j_cont] + S1[i_cont];
        { u8_t t = S1[i_cont]; S1[i_cont] = S1[s1]; S1[s1] = t; }
        if (j_cont == nlen) j_cont = -1;
    }
    memcpy(na.val, S1, sizeof S1);
    memcpy(nb.val, S1, sizeof S1);
    memcpy(nc.val, S1, sizeof S1);
    memcpy(nd.val, S1, sizeof S1);
    memcpy(ne.val, S1, sizeof S1);
    const char* __restrict nm_a=a; const char* __restrict nm_b=b; const char* __restrict nm_c=c;
    const char* __restrict nm_d=d; const char* __restrict nm_e=e;
    u8_t* __restrict va=na.val; u8_t* __restrict vb=nb.val; u8_t* __restrict vc=nc.val;
    u8_t* __restrict vd=nd.val; u8_t* __restrict ve=ne.val;
    const int kN=N;
    // pass1 续跑 (2× unroll)
    {
        int i = i_cont, j = j_cont;
        u8_t sa=s1, sb=s1, sc=s1, sd=s1, se=s1;
        for (; i + 1 < kN; i += 2) {
            if (j >= vary_start) {
                sa += nm_a[j] + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
                sb += nm_b[j] + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
                sc += nm_c[j] + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
                sd += nm_d[j] + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
                se += nm_e[j] + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
            } else {
                u8_t kb = nm_a[j];
                sa += kb + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
                sb += kb + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
                sc += kb + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
                sd += kb + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
                se += kb + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
            }
            if (j == nlen) j = -1;
            j++;
            if (j >= vary_start) {
                sa += nm_a[j] + va[i+1]; { u8_t t = va[i+1]; va[i+1] = va[sa]; va[sa] = t; }
                sb += nm_b[j] + vb[i+1]; { u8_t t = vb[i+1]; vb[i+1] = vb[sb]; vb[sb] = t; }
                sc += nm_c[j] + vc[i+1]; { u8_t t = vc[i+1]; vc[i+1] = vc[sc]; vc[sc] = t; }
                sd += nm_d[j] + vd[i+1]; { u8_t t = vd[i+1]; vd[i+1] = vd[sd]; vd[sd] = t; }
                se += nm_e[j] + ve[i+1]; { u8_t t = ve[i+1]; ve[i+1] = ve[se]; ve[se] = t; }
            } else {
                u8_t kb = nm_a[j];
                sa += kb + va[i+1]; { u8_t t = va[i+1]; va[i+1] = va[sa]; va[sa] = t; }
                sb += kb + vb[i+1]; { u8_t t = vb[i+1]; vb[i+1] = vb[sb]; vb[sb] = t; }
                sc += kb + vc[i+1]; { u8_t t = vc[i+1]; vc[i+1] = vc[sc]; vc[sc] = t; }
                sd += kb + vd[i+1]; { u8_t t = vd[i+1]; vd[i+1] = vd[sd]; vd[sd] = t; }
                se += kb + ve[i+1]; { u8_t t = ve[i+1]; ve[i+1] = ve[se]; ve[se] = t; }
            }
            if (j == nlen) j = -1;
            j++;
        }
        for (; i < kN; i++) {
            if (j >= vary_start) {
                sa += nm_a[j] + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
                sb += nm_b[j] + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
                sc += nm_c[j] + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
                sd += nm_d[j] + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
                se += nm_e[j] + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
            } else {
                u8_t kb = nm_a[j];
                sa += kb + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
                sb += kb + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
                sc += kb + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
                sd += kb + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
                se += kb + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
            }
            if (j == nlen) j = -1;
            j++;
        }
    }
    // pass2 (2× unroll)
    {
        int i = 0, j = nlen;
        u8_t sa=0, sb=0, sc=0, sd=0, se=0;
        for (; i + 1 < kN; i += 2) {
            if (j >= vary_start) {
                sa += nm_a[j] + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
                sb += nm_b[j] + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
                sc += nm_c[j] + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
                sd += nm_d[j] + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
                se += nm_e[j] + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
            } else {
                u8_t kb = nm_a[j];
                sa += kb + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
                sb += kb + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
                sc += kb + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
                sd += kb + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
                se += kb + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
            }
            if (j == nlen) j = -1;
            j++;
            if (j >= vary_start) {
                sa += nm_a[j] + va[i+1]; { u8_t t = va[i+1]; va[i+1] = va[sa]; va[sa] = t; }
                sb += nm_b[j] + vb[i+1]; { u8_t t = vb[i+1]; vb[i+1] = vb[sb]; vb[sb] = t; }
                sc += nm_c[j] + vc[i+1]; { u8_t t = vc[i+1]; vc[i+1] = vc[sc]; vc[sc] = t; }
                sd += nm_d[j] + vd[i+1]; { u8_t t = vd[i+1]; vd[i+1] = vd[sd]; vd[sd] = t; }
                se += nm_e[j] + ve[i+1]; { u8_t t = ve[i+1]; ve[i+1] = ve[se]; ve[se] = t; }
            } else {
                u8_t kb = nm_a[j];
                sa += kb + va[i+1]; { u8_t t = va[i+1]; va[i+1] = va[sa]; va[sa] = t; }
                sb += kb + vb[i+1]; { u8_t t = vb[i+1]; vb[i+1] = vb[sb]; vb[sb] = t; }
                sc += kb + vc[i+1]; { u8_t t = vc[i+1]; vc[i+1] = vc[sc]; vc[sc] = t; }
                sd += kb + vd[i+1]; { u8_t t = vd[i+1]; vd[i+1] = vd[sd]; vd[sd] = t; }
                se += kb + ve[i+1]; { u8_t t = ve[i+1]; ve[i+1] = ve[se]; ve[se] = t; }
            }
            if (j == nlen) j = -1;
            j++;
        }
        for (; i < kN; i++) {
            if (j >= vary_start) {
                sa += nm_a[j] + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
                sb += nm_b[j] + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
                sc += nm_c[j] + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
                sd += nm_d[j] + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
                se += nm_e[j] + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
            } else {
                u8_t kb = nm_a[j];
                sa += kb + va[i]; { u8_t t = va[i]; va[i] = va[sa]; va[sa] = t; }
                sb += kb + vb[i]; { u8_t t = vb[i]; vb[i] = vb[sb]; vb[sb] = t; }
                sc += kb + vc[i]; { u8_t t = vc[i]; vc[i] = vc[sc]; vc[sc] = t; }
                sd += kb + vd[i]; { u8_t t = vd[i]; vd[i] = vd[sd]; vd[sd] = t; }
                se += kb + ve[i]; { u8_t t = ve[i]; ve[i] = ve[se]; ve[se] = t; }
            }
            if (j == nlen) j = -1;
            j++;
        }
    }
    na._ksa_done=nb._ksa_done=nc._ksa_done=nd._ksa_done=ne._ksa_done=true;
}
#endif

// ---- 本地 finish_load_name 变体 (实验) ----
static void finish_variant(Name& na) {
#if defined(EXP2)
    (void)0;   // 跳过过滤
#else
    na.q_len = -1;
    simd_mul_add_filter(na.val, na.name_base, na.q_len, 30);
    na.V = 0;
    na._p[6] = median(na.name_base[28], na.name_base[29], na.name_base[30]); na.V += na._p[6];
    if (na.V < 24) return;
    na._p[1] = median(na.name_base[13], na.name_base[14], na.name_base[15]); na.V += na._p[1];
    na._p[2] = median(na.name_base[16], na.name_base[17], na.name_base[18]); na.V += na._p[2];
    na._p[5] = median(na.name_base[25], na.name_base[26], na.name_base[27]); na.V += na._p[5];
    if (na.V < 165) return;
    na._p[0] = median(na.name_base[10], na.name_base[11], na.name_base[12]); na.V += na._p[0];
    na._p[3] = median(na.name_base[19], na.name_base[20], na.name_base[21]); na.V += na._p[3];
    na._p[4] = median(na.name_base[22], na.name_base[23], na.name_base[24]); na.V += na._p[4];
    if (na.V < 250) return;
    sort10(na.name_base);
    na._p[7] = (154 + na.name_base[3] + na.name_base[4] + na.name_base[5] + na.name_base[6]);
    na.V += na._p[7] / 3;
#endif
}

// ---- 复刻 consume_seq (PAIR_WIDTH=5) ----
static void consume_seq_bench(char* a,int nlen,Name& na,char* b,Name& nb,char* c,Name& nc,char* d,Name& nd,char* e,Name& ne,
                              int epre,int evar,uint64_t L,uint64_t R,uint64_t& names_done){
    int vary_start = nlen - SCL;
    uint8_t dig[16];
    {uint64_t now=L;for(int p=evar-1;p>=0;p--){dig[p]=now%CLEN;ENC(a+epre+p*SCL,dig[p]);now/=CLEN;}}
    for(uint64_t i=L;i+4<R;i+=5){
        bool can_shared = dig[evar-1]+4 < (unsigned)CLEN;
#if !defined(EXP9)
        memcpy(b+epre,a+epre,evar*SCL);
        for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(b+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(b+epre+p*SCL,0);}
        memcpy(c+epre,b+epre,evar*SCL);
        for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(c+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(c+epre+p*SCL,0);}
        memcpy(d+epre,c+epre,evar*SCL);
        for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(d+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(d+epre+p*SCL,0);}
        memcpy(e+epre,d+epre,evar*SCL);
        for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(e+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(e+epre+p*SCL,0);}
#endif
        if(can_shared)
#if defined(EXP18)
            na.load_name_quint_shared_key(a,b,c,d,e,nlen,vary_start,nb,nc,nd,ne);
#elif defined(EXP17)
            ksa_quint_sp_u2(a,b,c,d,e,nlen,vary_start,29,na,nb,nc,nd,ne);
#elif defined(EXP15)
            ksa_quint_nodep(a,b,c,d,e,nlen,vary_start,na,nb,nc,nd,ne);
#elif defined(EXP16)
            ksa_quint_sp(a,b,c,d,e,nlen,vary_start,29,na,nb,nc,nd,ne);
#elif defined(EXP14)
            ksa_quint_unroll2(a,b,c,d,e,nlen,vary_start,na,nb,nc,nd,ne);
#else
            ksa_quint(a,b,c,d,e,nlen,vary_start,na,nb,nc,nd,ne);
#endif
        else
#if defined(EXP10)
            ksa_quint(a,b,c,d,e,nlen,0,na,nb,nc,nd,ne);
#elif defined(EXP18)
            {
                int cc = 1;
                for (int p = evar-2; p >= 0 && dig[p] == CLEN-1; p--) cc++;
                int vlen2 = (epre - PLEN)/SCL + evar;
                int sp_len = 1 + (vlen2 - 1 - cc) * SCL;
                if (sp_len < 1) sp_len = 1;
                int first_diff = 5 + (vlen2 - 1 - cc) * SCL;
                na.load_name_quint_sp(a,b,c,d,e,nlen,first_diff,sp_len,nb,nc,nd,ne);
            }
#elif defined(EXP17)
            {
                int cc = 1;
                for (int p = evar-2; p >= 0 && dig[p] == CLEN-1; p--) cc++;
                int vlen2 = (epre - PLEN)/SCL + evar;
                int sp_len = 1 + (vlen2 - 1 - cc) * SCL;
                if (sp_len < 1) sp_len = 1;
                int first_diff = 5 + (vlen2 - 1 - cc) * SCL;
                ksa_quint_sp_u2(a,b,c,d,e,nlen,first_diff,sp_len,na,nb,nc,nd,ne);
            }
#elif defined(EXP16)
            {
                // 进位: 计算 5 个连续名字的公共前缀长度
                int cc = 1;
                for (int p = evar-2; p >= 0 && dig[p] == CLEN-1; p--) cc++;
                int vlen2 = (epre - PLEN)/SCL + evar;
                int sp_len = 1 + (vlen2 - 1 - cc) * SCL;
                if (sp_len < 1) sp_len = 1;
                int first_diff = 5 + (vlen2 - 1 - cc) * SCL;
                ksa_quint_sp(a,b,c,d,e,nlen,first_diff,sp_len,na,nb,nc,nd,ne);
            }
#elif defined(EXP14)
            ksa_quint_unroll2(a,b,c,d,e,nlen,0,na,nb,nc,nd,ne);
#else
            na.load_name_quint(a,b,c,d,e,nlen,nb,nc,nd,ne);
#endif
        finish_variant(na); finish_variant(nb); finish_variant(nc); finish_variant(nd); finish_variant(ne);
        names_done += 5;
#if !defined(EXP9)
        memcpy(a+epre,e+epre,evar*SCL);
        for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(a+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(a+epre+p*SCL,0);}
#endif
    }
    for(uint64_t i=L+((R-L)/5)*5;i<R;i++){
        na._ksa_done=false;
        na.load_name(a,nlen);
        finish_variant(na);
        names_done += 1;
        if(i+1<R){for(int p=evar-1;p>=0;p--){if(++dig[p]<(unsigned)CLEN){ENC(a+epre+p*SCL,dig[p]);break;}dig[p]=0;ENC(a+epre+p*SCL,0);}}
    }
}

// ---- 复刻 consume_mode1 (PAIR_WIDTH=5) ----
static void consume_mode1_bench(char* a,int nlen,Name& na,char* b,Name& nb,char* c,Name& nc,char* buf_d,Name& nd,char* e,Name& ne,int plen,int vlen,uint64_t L,uint64_t R,uint64_t& names_done){
    na.PRELEN=plen;na.load_prefix(a,nlen);
    nb.PRELEN=plen;nb.load_prefix(a,nlen);
    nc.PRELEN=plen;nc.load_prefix(a,nlen);
    nd.PRELEN=plen;nd.load_prefix(a,nlen);
    ne.PRELEN=plen;ne.load_prefix(a,nlen);
    uint8_t dl[16],dr[16];uint64_t now;
    now=L;for(int d=vlen-1;d>=0;d--){dl[d]=now%CLEN;now/=CLEN;}
    now=R-1;for(int d=vlen-1;d>=0;d--){dr[d]=now%CLEN;now/=CLEN;}
    int up=0;while(up<vlen&&dl[up]==dr[up])up++;
    now=L;for(int d=vlen-1;d>=0;d--){int ci=now%CLEN;ENC(a+plen+d*SCL,ci);now/=CLEN;}
    now=L+1;for(int d=vlen-1;d>=0;d--){int ci=now%CLEN;ENC(b+plen+d*SCL,ci);now/=CLEN;}
    now=L+2;for(int d=vlen-1;d>=0;d--){int ci=now%CLEN;ENC(c+plen+d*SCL,ci);now/=CLEN;}
    now=L+3;for(int d=vlen-1;d>=0;d--){int ci=now%CLEN;ENC(buf_d+plen+d*SCL,ci);now/=CLEN;}
    now=L+4;for(int d=vlen-1;d>=0;d--){int ci=now%CLEN;ENC(e+plen+d*SCL,ci);now/=CLEN;}
    int epre=plen+up*SCL,evar=vlen-up;
    consume_seq_bench(a,nlen,na,b,nb,c,nc,buf_d,nd,e,ne,epre,evar,L,R,names_done);
}

int main(){
#if defined(VERIFY)
    // ===== 过滤器正确性验证: 参考标量 vs 新 SIMD 压缩 =====
    {
        std::mt19937_64 rng(12345);
        int ferr = 0;
        for (int trial = 0; trial < 20000; trial++) {
            u8_t val[256];
            for (int i = 0; i < 256; i++) val[i] = (u8_t)(rng() & 0xFF);
            u8_t nb1[128], nb2[128];
            int q1 = -1, q2 = -1;
            for (int i = 0; i < 256 && q1 < 30; i++) {
                u8_t u = (u8_t)(val[i] * 181 + 160);
                if (u >= 89 && u < 217) nb1[++q1] = u & 63;
            }
            simd_mul_add_filter(val, nb2, q2, 30);
            if (q1 != q2) { ferr++; if (ferr<5) printf("FILTER qlen mismatch: %d vs %d\n", q1, q2); continue; }
            for (int i = 0; i <= q1; i++) {
                if (nb1[i] != nb2[i]) { ferr++; if (ferr<5) printf("FILTER val mismatch at %d: %d vs %d\n", i, nb1[i], nb2[i]); break; }
            }
        }
        if (ferr == 0) printf("FILTER VERIFY: ALL MATCH\n"); else printf("FILTER VERIFY: %d ERRORS\n", ferr);
    }
    // ===== 正确性验证: ksa_quint (原始) vs ksa_quint_sp (共享前缀) =====
    {
    Name name_a,name_b,name_c,name_d,name_e;
    name_a.load_team(TEAM);
    memcpy(name_b.val_base,name_a.val_base,sizeof(name_a.val_base));
    memcpy(name_c.val_base,name_a.val_base,sizeof(name_a.val_base));
    memcpy(name_d.val_base,name_a.val_base,sizeof(name_a.val_base));
    memcpy(name_e.val_base,name_a.val_base,sizeof(name_a.val_base));
    char a[512],b[512],c[512],d[512],e[512];
    memcpy(a,PREFIX,PLEN); memset(a+PLEN,0,VLEN*SCL); a[NLEN]=0;
    memcpy(b,a,NLEN+1); memcpy(c,a,NLEN+1); memcpy(d,a,NLEN+1); memcpy(e,a,NLEN+1);
    int vary_start = NLEN - SCL;
    int errors = 0;
    for (uint64_t L = 0; L < 2000000; L += 1000) {
        // 设置 5 个连续名字
        uint64_t now = L;
        for (int dd = VLEN-1; dd >= 0; dd--) { int ci = now % CLEN; ENC(a+PLEN+dd*SCL,ci); now /= CLEN; }
        uint64_t now2 = L+1;
        for (int dd = VLEN-1; dd >= 0; dd--) { int ci = now2 % CLEN; ENC(b+PLEN+dd*SCL,ci); now2 /= CLEN; }
        now2 = L+2; for (int dd = VLEN-1; dd >= 0; dd--) { int ci = now2 % CLEN; ENC(c+PLEN+dd*SCL,ci); now2 /= CLEN; }
        now2 = L+3; for (int dd = VLEN-1; dd >= 0; dd--) { int ci = now2 % CLEN; ENC(d+PLEN+dd*SCL,ci); now2 /= CLEN; }
        now2 = L+4; for (int dd = VLEN-1; dd >= 0; dd--) { int ci = now2 % CLEN; ENC(e+PLEN+dd*SCL,ci); now2 /= CLEN; }
        int sp_len = 29; int first_diff = vary_start;
        int low = (int)(L % CLEN);
        if (low + 4 >= CLEN) {
            int cc = 1; sp_len = 1 + (VLEN-1-cc)*SCL; first_diff = 5 + (VLEN-1-cc)*SCL;
        }
        // 验证 no-carry (can_shared) 情况: 引擎调用 load_name_quint_shared_key
        if (low + 4 < CLEN) {
            Name t1[5]; for (int k=0;k<5;k++){ t1[k].load_team(TEAM); t1[k].PRELEN=PLEN; t1[k].load_prefix(a,NLEN);}
            ksa_quint(a,b,c,d,e,NLEN,vary_start,t1[0],t1[1],t1[2],t1[3],t1[4]);
            Name t2[5]; for (int k=0;k<5;k++){ t2[k].load_team(TEAM); t2[k].PRELEN=PLEN; t2[k].load_prefix(a,NLEN);}
            t2[0].load_name_quint_shared_key(a,b,c,d,e,NLEN,vary_start,t2[1],t2[2],t2[3],t2[4]);
            for (int k=0;k<5;k++) for (int i=0;i<N;i++){
                if (t1[k].val[i] != t2[k].val[i]) { errors++; if (errors<5) printf("NC MISMATCH L=%llu k=%d i=%d: %d vs %d\n",(unsigned long long)L,k,i,t1[k].val[i],t2[k].val[i]); break; }
            }
        } else {
            // 验证进位情况: 新 load_name_quint (内部自检测 SP) vs 原始全变路径
            Name t1[5]; for (int k=0;k<5;k++){ t1[k].load_team(TEAM); t1[k].PRELEN=PLEN; t1[k].load_prefix(a,NLEN);}
            ksa_quint(a,b,c,d,e,NLEN,0,t1[0],t1[1],t1[2],t1[3],t1[4]);   // 原始全变
            Name t2[5]; for (int k=0;k<5;k++){ t2[k].load_team(TEAM); t2[k].PRELEN=PLEN; t2[k].load_prefix(a,NLEN);}
            t2[0].load_name_quint(a,b,c,d,e,NLEN,t2[1],t2[2],t2[3],t2[4]); // 新自检测 SP
            for (int k=0;k<5;k++) for (int i=0;i<N;i++){
                if (t1[k].val[i] != t2[k].val[i]) { errors++; if (errors<5) printf("C MISMATCH L=%llu k=%d i=%d: %d vs %d\n",(unsigned long long)L,k,i,t1[k].val[i],t2[k].val[i]); break; }
            }
        }
        if (errors > 1000) { printf("TOO MANY ERRORS, aborting\n"); break; }
    }
    if (errors == 0) printf("VERIFY: ALL MATCH\n"); else printf("VERIFY: %d ERRORS\n", errors);
    return 0;
    }
#endif

    const uint64_t CHUNK = 1000000ULL;
    const int NCHUNKS = 20;   // 2000 万名字

    Name name_a,name_b,name_c,name_d,name_e;
    name_a.load_team(TEAM);
    memcpy(name_b.val_base,name_a.val_base,sizeof(name_a.val_base));
    memcpy(name_c.val_base,name_a.val_base,sizeof(name_a.val_base));
    memcpy(name_d.val_base,name_a.val_base,sizeof(name_a.val_base));
    memcpy(name_e.val_base,name_a.val_base,sizeof(name_a.val_base));
#if defined(EXP13)
    Name name_f;
    memcpy(name_f.val_base,name_a.val_base,sizeof(name_a.val_base));
    char f[512];
    memcpy(f,PREFIX,PLEN); memset(f+PLEN,0,VLEN*SCL); f[NLEN]=0;
#endif

    char a[512],b[512],c[512],d[512],e[512];
    memcpy(a,PREFIX,PLEN); memset(a+PLEN,0,VLEN*SCL); a[NLEN]=0;
    memcpy(b,a,NLEN+1); memcpy(c,a,NLEN+1); memcpy(d,a,NLEN+1); memcpy(e,a,NLEN+1);

    // 预热
    uint64_t names_done=0;
    for(int ch=0;ch<2;ch++){
        uint64_t L=(uint64_t)ch*CHUNK, R=L+CHUNK;
#if defined(EXP13)
        consume_mode1_6(a,NLEN,name_a,b,name_b,c,name_c,d,name_d,e,name_e,f,name_f,PLEN,VLEN,L,R,names_done);
#else
        consume_mode1_bench(a,NLEN,name_a,b,name_b,c,name_c,d,name_d,e,name_e,PLEN,VLEN,L,R,names_done);
#endif
    }

    auto t0=std::chrono::steady_clock::now();
    for(int ch=0;ch<NCHUNKS;ch++){
        uint64_t L=(uint64_t)ch*CHUNK, R=L+CHUNK;
#if defined(EXP13)
        consume_mode1_6(a,NLEN,name_a,b,name_b,c,name_c,d,name_d,e,name_e,f,name_f,PLEN,VLEN,L,R,names_done);
#else
        consume_mode1_bench(a,NLEN,name_a,b,name_b,c,name_c,d,name_d,e,name_e,PLEN,VLEN,L,R,names_done);
#endif
    }
    auto t1=std::chrono::steady_clock::now();
    double sec=std::chrono::duration<double>(t1-t0).count();
    uint64_t total=(uint64_t)NCHUNKS*CHUNK;
    printf("processed=%llu  sec=%.3f  speed=%.2fM/s\n",
           (unsigned long long)total, sec, total/sec/1e6);
    return 0;
}
