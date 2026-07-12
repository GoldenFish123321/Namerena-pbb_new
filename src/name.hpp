#pragma once
// ============================================================================
// name.hpp — RC4-like 名字状态机 (PBB 核心评分引擎)
//
// Name 结构体实现了基于 RC4 流密码变体的名字评分算法:
//
//   评分流程:
//     1. load_team()     → RC4 KSA 用队伍名初始化 val_base
//     2. load_prefix()   → 部分 KSA 预处理共享前缀 (性能优化)
//     3. load_name()     → 完整 KSA + SIMD 属性评分 → 计算 V 值
//     4. calc_skills()   → RC4 PRGA 生成技能分布
//     5. score_full()    → (在 scoring_1035.hpp) 封装 3+4 + 模型评分
//
//   V 值 (八围评分) 计算:
//     V = Σ 7 个属性的中位数 + HP/3 加成
//     属性从 ual 数组过滤后 (89 <= ual < 217) 的 name_base 中取中位数
//     包含三级渐进式早退: V < 24, V < 165, V < 250
//
//   对齐: alignas(64) 确保 AVX2 加载对齐到 cache line 边界
// ============================================================================
#include "common.hpp"
#include "utils.hpp"

struct alignas(64) Name {
  // ---- RC4 状态数组 ----
  alignas(64) u8_t val[N];          // RC4 S-box (256 字节, 对齐到 64 字节)
  alignas(64) u8_t ual[N];          // val 的线性变换结果 (val*181+160) & 0xFF
#if PBB_HAS_SIMD
  alignas(64) u8_t ual_skills[N];   // 技能评分用变换 (val*181+71) & 0xFF
  u8_t saved_val[256];              // AVX2 优化: 保存 load_prefix 后的 val 状态
  bool prefix_loaded;               // 是否已执行 load_prefix (用于 AVX2 快速恢复)
#endif

  // ---- KSA 基础状态 ----
  u8_t val_base[N];                 // 队伍名 KSA 后的初始 S-box (所有名字共享)
  u8_t val_base2[N];                // load_prefix 修改后的 S-box 副本

  // ---- 属性/技能缓冲区 ----
  u8_t name_base[M];                // 过滤后的 ual 值 (低 6 位, 最多 30 个有效值)
  u8_t freq[16];                    // 16 组技能的频次分布
  u8_t skill[skill_cnt];            // 技能排列 (40 个技能 ID 的随机排列)

  // ---- RC4 状态变量 ----
  u8_t p, q;                        // RC4 PRGA 的 i, j 指针
  u8_t i_pre, j_pre;                // load_prefix 保存的中间状态
  u8_t s_pre;                       // load_prefix 保存的累加和

  // ---- 派生属性 ----
  int q_len;                        // name_base 中有效值的数量 (0-30)
  int V;                            // 八围评分 (load_name 计算)
  int seed;                         // 种子 (未使用, 保留)
  int PRELEN;                       // load_prefix 预处理的名字字节数
  int NAMELEN;                      // 名字总长度
  bool _ksa_done = false;           // 内部: load_name_pair 已做完 KSA 时跳过重做
  bool _finish_done = false;        // 内部: finish_load_quad 已做完 SIMD 后处理时跳过重做

  // ===== m(): RC4 PRGA 单步 =====
  // 标准 RC4: i++, j = (j + S[i]), swap(S[i], S[j]), 输出 S[(S[i]+S[j]) & 255]
  inline u8_t m() {
    q += val[++p];
    std::swap(val[p], val[q]);
    return val[(val[p] + val[q]) & 255];
  }

  // ===== gen(): 生成技能索引 =====
  // 两次 PRGA 输出拼接为 16 位，对 skill_cnt (40) 取模得到 0-39 的技能 ID
  inline int gen() {
    int u = m();
    return (u << 8 | m()) % skill_cnt;
  }

  // ===== load_team(): 队伍名 KSA (密钥调度) =====
  // 用队伍名字节初始化 val_base[0..255] 为 0..255 的随机排列。
  // 使用标准 RC4 KSA: 对每个 i, s += key[i % keylen] + S[i], swap(S[i], S[s])
  void load_team(const char *_team) {
    int t_len = strlen(_team) + 1;
    u8_t s;
    for (int i = 0; i < N; i++)
      val_base[i] = i;
    for (int i = s = 0; i < N; ++i) {
      if (i % t_len)
        s += _team[i % t_len - 1];
      s += val_base[i];
      std::swap(val_base[i], val_base[s]);
    }
  }

  // ===== load_prefix(): 部分 KSA (共享前缀预处理) =====
  // 对名字的前 PRELEN 字节执行 KSA，避免为每个名字重复处理固定前缀。
  // 从 NAMELEN 位置开始循环读取名字字节 (模拟 RC4 密钥循环)。
  // 保存 i_pre/j_pre/s_pre 供 load_name 接续。
  // AVX2 优化: 额外保存 saved_val 副本以加速 load_name 恢复。
  void load_prefix(const char *name, int name_len) {
    memcpy(val_base2, val_base, sizeof val_base2);
    NAMELEN = name_len;
    int i, j;
    u8_t s;
    for (i = s = 0, j = NAMELEN; i < PRELEN; i++, j++) {
      s += name[j] + val_base2[i];
      std::swap(val_base2[i], val_base2[s]);
      if (j == NAMELEN)
        j = -1;
    }
    i_pre = i;
    j_pre = j;
    s_pre = s;
    if (i_pre == 0)
      j_pre = name_len, s_pre = 0;
#if PBB_HAS_SIMD
    memcpy(saved_val, val_base2, sizeof saved_val);
    prefix_loaded = true;
#endif
  }

  // ===== load_name(): 完整 KSA + 属性评分 (AVX2 路径) =====
  // 从 load_prefix 保存的状态接续执行 KSA，两遍循环 (标准 RC4 双 KSA)。
  // 然后用 SIMD 计算 ual 数组，过滤出 name_base 并累加 V 值。
  //
  // V 值计算分级 (渐进式早退):
  //   第 1 级 (V<24 直接返回): 28/29/30 位中位数 (对应高位属性)
  //   第 2 级 (V<165 直接返回): +13/14/15 + 16/17/18 + 25/26/27 中位数
  //   第 3 级 (V<250 直接返回): +10/11/12 + 19/20/21 + 22/23/24 中位数
  //   第 4 级: 排序后 + HP 加成 = (154 + sorted[3..6]) / 3
  //
  // AVX2 优化: 使用 prefix_loaded 标志选择从 saved_val 快速恢复
#if PBB_HAS_SIMD
  // ===== finish_V(): 从 name_base 计算 V 值 (无 SIMD, 供 finish_load/finish_load_quad 共用) =====
  void finish_V() {
    V = 0;
    V += median(name_base[28], name_base[29], name_base[30]);
    if (V < 24) return;
    V += median(name_base[13], name_base[14], name_base[15]);
    V += median(name_base[16], name_base[17], name_base[18]);
    V += median(name_base[25], name_base[26], name_base[27]);
    if (V < 165) return;
    V += median(name_base[10], name_base[11], name_base[12]);
    V += median(name_base[19], name_base[20], name_base[21]);
    V += median(name_base[22], name_base[23], name_base[24]);
    if (V < 250) return;
    sort10(name_base);
    V += (154 + name_base[3] + name_base[4] + name_base[5] + name_base[6]) / 3;
  }

  // ===== finish_load(): KSA 后的公共收尾 (ual/V 计算) =====
  void finish_load() {
    simd_mul_add_dual(val, ual, ual_skills);
    simd_filter_range_attr(ual, name_base, q_len, 30);  // SIMD: 到位掩码迭代, 消除逐字节分支
    finish_V();
  }

  void load_name(const char *name, int name_len_hint = 0) {
    q_len = -1;
    if (!_ksa_done) {
      u8_t s = s_pre;
      int _NAMELEN = name_len_hint ? name_len_hint : NAMELEN;
      memcpy(val, prefix_loaded ? saved_val : val_base2, sizeof val);
      for (int i = i_pre, j = j_pre; i < N; i++, j++) {
        s += name[j] + val[i];
        std::swap(val[i], val[s]);
        if (j == _NAMELEN) j = -1;
      }
      for (int i = s = 0, j = _NAMELEN; i < N; i++, j++) {
        s += name[j] + val[i];
        std::swap(val[i], val[s]);
        if (j == _NAMELEN) j = -1;
      }
    }
    _ksa_done = false;
    if (!_finish_done)
      finish_load();
    _finish_done = false;
  }

  // ===== load_name_pair(): 双候选交错 RC4 KSA (Issue #17 方向一) =====
  // 同时推进两条独立 RC4 置换链, 利用乱序 CPU 的 ILP 隐藏 load-use 延迟。
  // 在 swap 粒度交错——而非 ENC 编码级——直接消除 load_name 内部的瓶颈。
  // 前置: this 和 other 已通过 load_prefix 设置相同的前缀状态。
  // 完成后 _ksa_done 置为 true, 后续 load_name() 跳过 KSA 直接 finish_load()。
  void load_name_pair(const char *name_a, const char *name_b, int name_len, Name& other) {
    q_len = -1;
    other.q_len = -1;
    memcpy(val, prefix_loaded ? saved_val : val_base2, sizeof val);
    memcpy(other.val, other.prefix_loaded ? other.saved_val : other.val_base2, sizeof other.val);
    u8_t s_a = s_pre, s_b = s_pre;

    // 第一遍 KSA — 交错推进两条独立置换链 (共享 j, 等长名字)
    for (int i = i_pre, j = j_pre; i < N; i++, j++) {
      s_a += name_a[j] + val[i];
      std::swap(val[i], val[s_a]);
      s_b += name_b[j] + other.val[i];
      std::swap(other.val[i], other.val[s_b]);
      if (j == name_len) j = -1;
    }

    // 第二遍 KSA — 交错推进, s 各自从 0 开始
    s_a = 0; s_b = 0;
    for (int i = 0, j = name_len; i < N; i++, j++) {
      s_a += name_a[j] + val[i];
      std::swap(val[i], val[s_a]);
      s_b += name_b[j] + other.val[i];
      std::swap(other.val[i], other.val[s_b]);
      if (j == name_len) j = -1;
    }

    _ksa_done = true;
    other._ksa_done = true;
  }

  // ===== load_name_triple(): 三候选交错 RC4 KSA =====
  void load_name_triple(const char *a, const char *b, const char *c, int nlen,
                         Name& ob, Name& oc) {
    q_len = -1; ob.q_len = -1; oc.q_len = -1;
    memcpy(val, prefix_loaded ? saved_val : val_base2, sizeof val);
    memcpy(ob.val, ob.prefix_loaded ? ob.saved_val : ob.val_base2, sizeof ob.val);
    memcpy(oc.val, oc.prefix_loaded ? oc.saved_val : oc.val_base2, sizeof oc.val);
    u8_t sa = s_pre, sb = s_pre, sc = s_pre;
    for (int i = i_pre, j = j_pre; i < N; i++, j++) {
      sa += a[j] + val[i]; std::swap(val[i], val[sa]);
      sb += b[j] + ob.val[i]; std::swap(ob.val[i], ob.val[sb]);
      sc += c[j] + oc.val[i]; std::swap(oc.val[i], oc.val[sc]);
      if (j == nlen) j = -1;
    }
    sa = 0; sb = 0; sc = 0;
    for (int i = 0, j = nlen; i < N; i++, j++) {
      sa += a[j] + val[i]; std::swap(val[i], val[sa]);
      sb += b[j] + ob.val[i]; std::swap(ob.val[i], ob.val[sb]);
      sc += c[j] + oc.val[i]; std::swap(oc.val[i], oc.val[sc]);
      if (j == nlen) j = -1;
    }
    _ksa_done = true; ob._ksa_done = true; oc._ksa_done = true;
  }

  // ===== load_name_quad(): 四候选交错 RC4 KSA =====
  void load_name_quad(const char *a, const char *b, const char *c, const char *d,
                       int nlen, Name& ob, Name& oc, Name& od) {
    q_len = -1; ob.q_len = -1; oc.q_len = -1; od.q_len = -1;
    memcpy(val, prefix_loaded ? saved_val : val_base2, sizeof val);
    memcpy(ob.val, ob.prefix_loaded ? ob.saved_val : ob.val_base2, sizeof ob.val);
    memcpy(oc.val, oc.prefix_loaded ? oc.saved_val : oc.val_base2, sizeof oc.val);
    memcpy(od.val, od.prefix_loaded ? od.saved_val : od.val_base2, sizeof od.val);
    u8_t sa = s_pre, sb = s_pre, sc = s_pre, sd = s_pre;
    for (int i = i_pre, j = j_pre; i < N; i++, j++) {
      sa += a[j] + val[i]; std::swap(val[i], val[sa]);
      sb += b[j] + ob.val[i]; std::swap(ob.val[i], ob.val[sb]);
      sc += c[j] + oc.val[i]; std::swap(oc.val[i], oc.val[sc]);
      sd += d[j] + od.val[i]; std::swap(od.val[i], od.val[sd]);
      if (j == nlen) j = -1;
    }
    sa = 0; sb = 0; sc = 0; sd = 0;
    for (int i = 0, j = nlen; i < N; i++, j++) {
      sa += a[j] + val[i]; std::swap(val[i], val[sa]);
      sb += b[j] + ob.val[i]; std::swap(ob.val[i], ob.val[sb]);
      sc += c[j] + oc.val[i]; std::swap(oc.val[i], oc.val[sc]);
      sd += d[j] + od.val[i]; std::swap(od.val[i], od.val[sd]);
      if (j == nlen) j = -1;
    }
    _ksa_done = true; ob._ksa_done = true; oc._ksa_done = true; od._ksa_done = true;
  }

  // ===== finish_load_quad(): 四候选交错 finish_load (Issue #17 建议三) =====
  // 在 load_name_quad 交错 KSA 之后，将 finish_load 的 SIMD 后处理也交错执行。
  // 四个候选的 simd_mul_add_dual / simd_filter_range_attr 在指令级交错，
  // 利用乱序核心隐藏 load-use 延迟。
  // 完成后设置 _finish_done=true，后续 load_name() 跳过 finish_load()。
#if PBB_HAS_AVX512
  void finish_load_quad(Name& ob, Name& oc, Name& od) {
    const __m512i vmul  = _mm512_set1_epi16(181);
    const __m512i vaddA = _mm512_set1_epi16(160);
    const __m512i vaddS = _mm512_set1_epi16(71);
    const __m512i vmask = _mm512_set1_epi16(0xFF);
    const __m512i vzero = _mm512_setzero_si512();
    // simd_mul_add_dual: 四候选交错 (64B/chunk)
    for (int i = 0; i < 256; i += 64) {
      __m512i va = _mm512_loadu_si512((const __m512i*)&val[i]);
      __m512i vb = _mm512_loadu_si512((const __m512i*)&ob.val[i]);
      __m512i vc = _mm512_loadu_si512((const __m512i*)&oc.val[i]);
      __m512i vd = _mm512_loadu_si512((const __m512i*)&od.val[i]);
      // a
      __m512i lo = _mm512_unpacklo_epi8(va, vzero);
      __m512i hi = _mm512_unpackhi_epi8(va, vzero);
      __m512i loA = _mm512_add_epi16(_mm512_mullo_epi16(lo, vmul), vaddA);
      __m512i hiA = _mm512_add_epi16(_mm512_mullo_epi16(hi, vmul), vaddA);
      __m512i loS = _mm512_add_epi16(_mm512_mullo_epi16(lo, vmul), vaddS);
      __m512i hiS = _mm512_add_epi16(_mm512_mullo_epi16(hi, vmul), vaddS);
      _mm512_storeu_si512((__m512i*)&ual[i], _mm512_packus_epi16(_mm512_and_si512(loA,vmask), _mm512_and_si512(hiA,vmask)));
      _mm512_storeu_si512((__m512i*)&ual_skills[i], _mm512_packus_epi16(_mm512_and_si512(loS,vmask), _mm512_and_si512(hiS,vmask)));
      // b
      lo = _mm512_unpacklo_epi8(vb, vzero); hi = _mm512_unpackhi_epi8(vb, vzero);
      loA = _mm512_add_epi16(_mm512_mullo_epi16(lo, vmul), vaddA);
      hiA = _mm512_add_epi16(_mm512_mullo_epi16(hi, vmul), vaddA);
      loS = _mm512_add_epi16(_mm512_mullo_epi16(lo, vmul), vaddS);
      hiS = _mm512_add_epi16(_mm512_mullo_epi16(hi, vmul), vaddS);
      _mm512_storeu_si512((__m512i*)&ob.ual[i], _mm512_packus_epi16(_mm512_and_si512(loA,vmask), _mm512_and_si512(hiA,vmask)));
      _mm512_storeu_si512((__m512i*)&ob.ual_skills[i], _mm512_packus_epi16(_mm512_and_si512(loS,vmask), _mm512_and_si512(hiS,vmask)));
      // c
      lo = _mm512_unpacklo_epi8(vc, vzero); hi = _mm512_unpackhi_epi8(vc, vzero);
      loA = _mm512_add_epi16(_mm512_mullo_epi16(lo, vmul), vaddA);
      hiA = _mm512_add_epi16(_mm512_mullo_epi16(hi, vmul), vaddA);
      loS = _mm512_add_epi16(_mm512_mullo_epi16(lo, vmul), vaddS);
      hiS = _mm512_add_epi16(_mm512_mullo_epi16(hi, vmul), vaddS);
      _mm512_storeu_si512((__m512i*)&oc.ual[i], _mm512_packus_epi16(_mm512_and_si512(loA,vmask), _mm512_and_si512(hiA,vmask)));
      _mm512_storeu_si512((__m512i*)&oc.ual_skills[i], _mm512_packus_epi16(_mm512_and_si512(loS,vmask), _mm512_and_si512(hiS,vmask)));
      // d
      lo = _mm512_unpacklo_epi8(vd, vzero); hi = _mm512_unpackhi_epi8(vd, vzero);
      loA = _mm512_add_epi16(_mm512_mullo_epi16(lo, vmul), vaddA);
      hiA = _mm512_add_epi16(_mm512_mullo_epi16(hi, vmul), vaddA);
      loS = _mm512_add_epi16(_mm512_mullo_epi16(lo, vmul), vaddS);
      hiS = _mm512_add_epi16(_mm512_mullo_epi16(hi, vmul), vaddS);
      _mm512_storeu_si512((__m512i*)&od.ual[i], _mm512_packus_epi16(_mm512_and_si512(loA,vmask), _mm512_and_si512(hiA,vmask)));
      _mm512_storeu_si512((__m512i*)&od.ual_skills[i], _mm512_packus_epi16(_mm512_and_si512(loS,vmask), _mm512_and_si512(hiS,vmask)));
    }
    // simd_filter_range_attr: 四候选交错 (64B/chunk)
    for (int i = 0; i < 256; i += 64) {
      __m512i da = _mm512_loadu_si512((const __m512i*)&ual[i]);
      __m512i db = _mm512_loadu_si512((const __m512i*)&ob.ual[i]);
      __m512i dc = _mm512_loadu_si512((const __m512i*)&oc.ual[i]);
      __m512i dd = _mm512_loadu_si512((const __m512i*)&od.ual[i]);
      __mmask64 ge_a = _mm512_cmp_epu8_mask(da, _mm512_set1_epi8(88), _MM_CMPINT_GT);
      __mmask64 lt_a = _mm512_cmp_epu8_mask(_mm512_set1_epi8(217), da, _MM_CMPINT_GT);
      __mmask64 mask_a = ge_a & lt_a;
      __mmask64 ge_b = _mm512_cmp_epu8_mask(db, _mm512_set1_epi8(88), _MM_CMPINT_GT);
      __mmask64 lt_b = _mm512_cmp_epu8_mask(_mm512_set1_epi8(217), db, _MM_CMPINT_GT);
      __mmask64 mask_b = ge_b & lt_b;
      __mmask64 ge_c = _mm512_cmp_epu8_mask(dc, _mm512_set1_epi8(88), _MM_CMPINT_GT);
      __mmask64 lt_c = _mm512_cmp_epu8_mask(_mm512_set1_epi8(217), dc, _MM_CMPINT_GT);
      __mmask64 mask_c = ge_c & lt_c;
      __mmask64 ge_d = _mm512_cmp_epu8_mask(dd, _mm512_set1_epi8(88), _MM_CMPINT_GT);
      __mmask64 lt_d = _mm512_cmp_epu8_mask(_mm512_set1_epi8(217), dd, _MM_CMPINT_GT);
      __mmask64 mask_d = ge_d & lt_d;
      while (mask_a && q_len < 30) { int idx = __builtin_ctzll(mask_a); name_base[++q_len] = ual[i+idx] & 63; mask_a &= mask_a - 1; }
      while (mask_b && ob.q_len < 30) { int idx = __builtin_ctzll(mask_b); ob.name_base[++ob.q_len] = ob.ual[i+idx] & 63; mask_b &= mask_b - 1; }
      while (mask_c && oc.q_len < 30) { int idx = __builtin_ctzll(mask_c); oc.name_base[++oc.q_len] = oc.ual[i+idx] & 63; mask_c &= mask_c - 1; }
      while (mask_d && od.q_len < 30) { int idx = __builtin_ctzll(mask_d); od.name_base[++od.q_len] = od.ual[i+idx] & 63; mask_d &= mask_d - 1; }
    }
    finish_V(); ob.finish_V(); oc.finish_V(); od.finish_V();
    _finish_done = ob._finish_done = oc._finish_done = od._finish_done = true;
  }
#elif PBB_HAS_AVX2
  void finish_load_quad(Name& ob, Name& oc, Name& od) {
    const __m256i vmul  = _mm256_set1_epi16(181);
    const __m256i vaddA = _mm256_set1_epi16(160);
    const __m256i vaddS = _mm256_set1_epi16(71);
    const __m256i vmask = _mm256_set1_epi16(0xFF);
    const __m256i vzero = _mm256_setzero_si256();
    // simd_mul_add_dual: 四候选交错 (32B/chunk, 共 8 次迭代)
    for (int i = 0; i < 256; i += 32) {
      __m256i va = _mm256_loadu_si256((const __m256i*)&val[i]);
      __m256i vb = _mm256_loadu_si256((const __m256i*)&ob.val[i]);
      __m256i vc = _mm256_loadu_si256((const __m256i*)&oc.val[i]);
      __m256i vd = _mm256_loadu_si256((const __m256i*)&od.val[i]);
      // a
      __m256i lo = _mm256_unpacklo_epi8(va, vzero);
      __m256i hi = _mm256_unpackhi_epi8(va, vzero);
      __m256i loA = _mm256_add_epi16(_mm256_mullo_epi16(lo, vmul), vaddA);
      __m256i hiA = _mm256_add_epi16(_mm256_mullo_epi16(hi, vmul), vaddA);
      __m256i loS = _mm256_add_epi16(_mm256_mullo_epi16(lo, vmul), vaddS);
      __m256i hiS = _mm256_add_epi16(_mm256_mullo_epi16(hi, vmul), vaddS);
      _mm256_storeu_si256((__m256i*)&ual[i], _mm256_packus_epi16(_mm256_and_si256(loA,vmask), _mm256_and_si256(hiA,vmask)));
      _mm256_storeu_si256((__m256i*)&ual_skills[i], _mm256_packus_epi16(_mm256_and_si256(loS,vmask), _mm256_and_si256(hiS,vmask)));
      // b
      lo = _mm256_unpacklo_epi8(vb, vzero); hi = _mm256_unpackhi_epi8(vb, vzero);
      loA = _mm256_add_epi16(_mm256_mullo_epi16(lo, vmul), vaddA);
      hiA = _mm256_add_epi16(_mm256_mullo_epi16(hi, vmul), vaddA);
      loS = _mm256_add_epi16(_mm256_mullo_epi16(lo, vmul), vaddS);
      hiS = _mm256_add_epi16(_mm256_mullo_epi16(hi, vmul), vaddS);
      _mm256_storeu_si256((__m256i*)&ob.ual[i], _mm256_packus_epi16(_mm256_and_si256(loA,vmask), _mm256_and_si256(hiA,vmask)));
      _mm256_storeu_si256((__m256i*)&ob.ual_skills[i], _mm256_packus_epi16(_mm256_and_si256(loS,vmask), _mm256_and_si256(hiS,vmask)));
      // c
      lo = _mm256_unpacklo_epi8(vc, vzero); hi = _mm256_unpackhi_epi8(vc, vzero);
      loA = _mm256_add_epi16(_mm256_mullo_epi16(lo, vmul), vaddA);
      hiA = _mm256_add_epi16(_mm256_mullo_epi16(hi, vmul), vaddA);
      loS = _mm256_add_epi16(_mm256_mullo_epi16(lo, vmul), vaddS);
      hiS = _mm256_add_epi16(_mm256_mullo_epi16(hi, vmul), vaddS);
      _mm256_storeu_si256((__m256i*)&oc.ual[i], _mm256_packus_epi16(_mm256_and_si256(loA,vmask), _mm256_and_si256(hiA,vmask)));
      _mm256_storeu_si256((__m256i*)&oc.ual_skills[i], _mm256_packus_epi16(_mm256_and_si256(loS,vmask), _mm256_and_si256(hiS,vmask)));
      // d
      lo = _mm256_unpacklo_epi8(vd, vzero); hi = _mm256_unpackhi_epi8(vd, vzero);
      loA = _mm256_add_epi16(_mm256_mullo_epi16(lo, vmul), vaddA);
      hiA = _mm256_add_epi16(_mm256_mullo_epi16(hi, vmul), vaddA);
      loS = _mm256_add_epi16(_mm256_mullo_epi16(lo, vmul), vaddS);
      hiS = _mm256_add_epi16(_mm256_mullo_epi16(hi, vmul), vaddS);
      _mm256_storeu_si256((__m256i*)&od.ual[i], _mm256_packus_epi16(_mm256_and_si256(loA,vmask), _mm256_and_si256(hiA,vmask)));
      _mm256_storeu_si256((__m256i*)&od.ual_skills[i], _mm256_packus_epi16(_mm256_and_si256(loS,vmask), _mm256_and_si256(hiS,vmask)));
    }
    // simd_filter_range_attr: 四候选交错 (32B/chunk)
    const __m256i v88  = _mm256_set1_epi8(88);
    const __m256i v216 = _mm256_set1_epi8(216);
    for (int i = 0; i < 256; i += 32) {
      __m256i da = _mm256_loadu_si256((const __m256i*)&ual[i]);
      __m256i db = _mm256_loadu_si256((const __m256i*)&ob.ual[i]);
      __m256i dc = _mm256_loadu_si256((const __m256i*)&oc.ual[i]);
      __m256i dd = _mm256_loadu_si256((const __m256i*)&od.ual[i]);
      __m256i ge_a = _mm256_xor_si256(_mm256_cmpeq_epi8(_mm256_subs_epu8(da, v88), vzero), _mm256_set1_epi8(0xFF));
      __m256i lt_a = _mm256_cmpeq_epi8(_mm256_subs_epu8(da, v216), vzero);
      unsigned mask_a = (unsigned)_mm256_movemask_epi8(_mm256_and_si256(ge_a, lt_a));
      __m256i ge_b = _mm256_xor_si256(_mm256_cmpeq_epi8(_mm256_subs_epu8(db, v88), vzero), _mm256_set1_epi8(0xFF));
      __m256i lt_b = _mm256_cmpeq_epi8(_mm256_subs_epu8(db, v216), vzero);
      unsigned mask_b = (unsigned)_mm256_movemask_epi8(_mm256_and_si256(ge_b, lt_b));
      __m256i ge_c = _mm256_xor_si256(_mm256_cmpeq_epi8(_mm256_subs_epu8(dc, v88), vzero), _mm256_set1_epi8(0xFF));
      __m256i lt_c = _mm256_cmpeq_epi8(_mm256_subs_epu8(dc, v216), vzero);
      unsigned mask_c = (unsigned)_mm256_movemask_epi8(_mm256_and_si256(ge_c, lt_c));
      __m256i ge_d = _mm256_xor_si256(_mm256_cmpeq_epi8(_mm256_subs_epu8(dd, v88), vzero), _mm256_set1_epi8(0xFF));
      __m256i lt_d = _mm256_cmpeq_epi8(_mm256_subs_epu8(dd, v216), vzero);
      unsigned mask_d = (unsigned)_mm256_movemask_epi8(_mm256_and_si256(ge_d, lt_d));
      while (mask_a && q_len < 30) { int idx = __builtin_ctz(mask_a); name_base[++q_len] = ual[i+idx] & 63; mask_a &= mask_a - 1; }
      while (mask_b && ob.q_len < 30) { int idx = __builtin_ctz(mask_b); ob.name_base[++ob.q_len] = ob.ual[i+idx] & 63; mask_b &= mask_b - 1; }
      while (mask_c && oc.q_len < 30) { int idx = __builtin_ctz(mask_c); oc.name_base[++oc.q_len] = oc.ual[i+idx] & 63; mask_c &= mask_c - 1; }
      while (mask_d && od.q_len < 30) { int idx = __builtin_ctz(mask_d); od.name_base[++od.q_len] = od.ual[i+idx] & 63; mask_d &= mask_d - 1; }
    }
    finish_V(); ob.finish_V(); oc.finish_V(); od.finish_V();
    _finish_done = ob._finish_done = oc._finish_done = od._finish_done = true;
  }
#elif PBB_HAS_NEON
  void finish_load_quad(Name& ob, Name& oc, Name& od) {
    // NEON: 四候选交错 simd_mul_add_dual (16B/chunk, 共 16 次迭代)
    const uint8x16_t vmul  = vdupq_n_u8(181);
    const uint8x16_t vaddA = vdupq_n_u8(160);
    const uint8x16_t vaddS = vdupq_n_u8(71);
    for (int i = 0; i < 256; i += 16) {
      uint8x16_t va = vld1q_u8(&val[i]);
      uint8x16_t vb = vld1q_u8(&ob.val[i]);
      uint8x16_t vc = vld1q_u8(&oc.val[i]);
      uint8x16_t vd = vld1q_u8(&od.val[i]);
      // a: 两个输出都需要 mul+add, NEON vmlaq_u8 是乘加指令
      uint8x16_t ua_attr  = vaddq_u8(vmulq_u8(va, vmul), vaddA);
      uint8x16_t ua_skill = vaddq_u8(vmulq_u8(va, vmul), vaddS);
      vst1q_u8(&ual[i], ua_attr);
      vst1q_u8(&ual_skills[i], ua_skill);
      uint8x16_t ub_attr  = vaddq_u8(vmulq_u8(vb, vmul), vaddA);
      uint8x16_t ub_skill = vaddq_u8(vmulq_u8(vb, vmul), vaddS);
      vst1q_u8(&ob.ual[i], ub_attr);
      vst1q_u8(&ob.ual_skills[i], ub_skill);
      uint8x16_t uc_attr  = vaddq_u8(vmulq_u8(vc, vmul), vaddA);
      uint8x16_t uc_skill = vaddq_u8(vmulq_u8(vc, vmul), vaddS);
      vst1q_u8(&oc.ual[i], uc_attr);
      vst1q_u8(&oc.ual_skills[i], uc_skill);
      uint8x16_t ud_attr  = vaddq_u8(vmulq_u8(vd, vmul), vaddA);
      uint8x16_t ud_skill = vaddq_u8(vmulq_u8(vd, vmul), vaddS);
      vst1q_u8(&od.ual[i], ud_attr);
      vst1q_u8(&od.ual_skills[i], ud_skill);
    }
    // simd_filter_range_attr: 四候选交错 (NEON 8B/chunk)
    const uint8x8_t lower = vdup_n_u8(89);
    const uint8x8_t upper = vdup_n_u8(217);
    const uint8x8_t low_bits = vdup_n_u8(63);
    const uint8x8_t bit_weights = {1, 2, 4, 8, 16, 32, 64, 128};
    for (int i = 0; i < 256; i += 8) {
      uint8x8_t da = vld1_u8(&ual[i]), db = vld1_u8(&ob.ual[i]);
      uint8x8_t dc = vld1_u8(&oc.ual[i]), dd = vld1_u8(&od.ual[i]);
      int ma = vaddv_u8(vand_u8(vand_u8(vcge_u8(da, lower), vclt_u8(da, upper)), bit_weights));
      int mb = vaddv_u8(vand_u8(vand_u8(vcge_u8(db, lower), vclt_u8(db, upper)), bit_weights));
      int mc = vaddv_u8(vand_u8(vand_u8(vcge_u8(dc, lower), vclt_u8(dc, upper)), bit_weights));
      int md = vaddv_u8(vand_u8(vand_u8(vcge_u8(dd, lower), vclt_u8(dd, upper)), bit_weights));
      if (ma) { uint8x8_t comp = vtbl1_u8(vand_u8(da, low_bits), vld1_u8(compress8_table.indexes[ma])); vst1_u8(name_base+q_len+1, comp); q_len+=compress8_table.counts[ma]; if(q_len>30)q_len=30; }
      if (mb) { uint8x8_t comp = vtbl1_u8(vand_u8(db, low_bits), vld1_u8(compress8_table.indexes[mb])); vst1_u8(ob.name_base+ob.q_len+1, comp); ob.q_len+=compress8_table.counts[mb]; if(ob.q_len>30)ob.q_len=30; }
      if (mc) { uint8x8_t comp = vtbl1_u8(vand_u8(dc, low_bits), vld1_u8(compress8_table.indexes[mc])); vst1_u8(oc.name_base+oc.q_len+1, comp); oc.q_len+=compress8_table.counts[mc]; if(oc.q_len>30)oc.q_len=30; }
      if (md) { uint8x8_t comp = vtbl1_u8(vand_u8(dd, low_bits), vld1_u8(compress8_table.indexes[md])); vst1_u8(od.name_base+od.q_len+1, comp); od.q_len+=compress8_table.counts[md]; if(od.q_len>30)od.q_len=30; }
    }
    finish_V(); ob.finish_V(); oc.finish_V(); od.finish_V();
    _finish_done = ob._finish_done = oc._finish_done = od._finish_done = true;
  }
#else
  // 回退: 无 SIMD 时串行调用 finish_load()
  void finish_load_quad(Name& ob, Name& oc, Name& od) {
    q_len = -1; ob.q_len = -1; oc.q_len = -1; od.q_len = -1;
    finish_load(); ob.finish_load(); oc.finish_load(); od.finish_load();
    _finish_done = ob._finish_done = oc._finish_done = od._finish_done = true;
  }
#endif
#else
  // ===== load_name(): 标量回退路径 =====
  // 无 AVX2 时的纯 C++ 实现。ual 计算通过手动展开循环 (每次 8 个)。
  // 分批处理前 96 字节和后 160 字节以减少不必要的计算。
  void load_name(const char *name, int name_len_hint = 0) {
    q_len = -1;
    (void)name_len_hint;
    if (!_ksa_done) {
      u8_t s = s_pre;
      memcpy(val, val_base2, sizeof val);
      for (int i = i_pre, j = j_pre; i < N; i++, j++) {
        s += name[j] + val[i];
        std::swap(val[i], val[s]);
        if (j == NAMELEN) j = -1;
      }
      for (int i = s = 0, j = NAMELEN; i < N; i++, j++) {
        s += name[j] + val[i];
        std::swap(val[i], val[s]);
        if (j == NAMELEN) j = -1;
      }
    }
    _ksa_done = false;
    // 标量 ual 计算: val * 181 + 160 (前 96 字节)
    for (int i = 0; i < 96; i += 8) {
      ual[i+0] = val[i+0] * 181 + 160; ual[i+1] = val[i+1] * 181 + 160;
      ual[i+2] = val[i+2] * 181 + 160; ual[i+3] = val[i+3] * 181 + 160;
      ual[i+4] = val[i+4] * 181 + 160; ual[i+5] = val[i+5] * 181 + 160;
      ual[i+6] = val[i+6] * 181 + 160; ual[i+7] = val[i+7] * 181 + 160;
    }
    for (int i = 0; i < 96 && q_len < 30; i++)
      if (ual[i] >= 89 && ual[i] < 217)
        name_base[++q_len] = ual[i] & 63;
    // 仅在前 96 字节不足 30 个有效值时处理剩余 160 字节
    if (q_len < 30) {
      for (int i = 96; i < N; i += 8) {
        ual[i+0] = val[i+0] * 181 + 160; ual[i+1] = val[i+1] * 181 + 160;
        ual[i+2] = val[i+2] * 181 + 160; ual[i+3] = val[i+3] * 181 + 160;
        ual[i+4] = val[i+4] * 181 + 160; ual[i+5] = val[i+5] * 181 + 160;
        ual[i+6] = val[i+6] * 181 + 160; ual[i+7] = val[i+7] * 181 + 160;
      }
      for (int i = 96; i < N && q_len < 30; i++)
        if (ual[i] >= 89 && ual[i] < 217)
          name_base[++q_len] = ual[i] & 63;
    }
    // 与 AVX2 路径相同的 V 值计算
    V = 0;
    V += median(name_base[28], name_base[29], name_base[30]);
    if (V < 24) return;
    V += median(name_base[13], name_base[14], name_base[15]);
    V += median(name_base[16], name_base[17], name_base[18]);
    V += median(name_base[25], name_base[26], name_base[27]);
    if (V < 165) return;
    V += median(name_base[10], name_base[11], name_base[12]);
    V += median(name_base[19], name_base[20], name_base[21]);
    V += median(name_base[22], name_base[23], name_base[24]);
    if (V < 250) return;
    sort10(name_base);
    V += (154 + name_base[3] + name_base[4] + name_base[5] + name_base[6]) / 3;
  }

  // ===== load_name_pair(): 标量回退 — 双候选交错 KSA =====
  void load_name_pair(const char *name_a, const char *name_b, int name_len, Name& other) {
    q_len = -1;
    other.q_len = -1;
    memcpy(val, val_base2, sizeof val);
    memcpy(other.val, other.val_base2, sizeof other.val);
    u8_t s_a = s_pre, s_b = s_pre;

    for (int i = i_pre, j = j_pre; i < N; i++, j++) {
      s_a += name_a[j] + val[i];
      std::swap(val[i], val[s_a]);
      s_b += name_b[j] + other.val[i];
      std::swap(other.val[i], other.val[s_b]);
      if (j == name_len) j = -1;
    }

    s_a = 0; s_b = 0;
    for (int i = 0, j = name_len; i < N; i++, j++) {
      s_a += name_a[j] + val[i];
      std::swap(val[i], val[s_a]);
      s_b += name_b[j] + other.val[i];
      std::swap(other.val[i], other.val[s_b]);
      if (j == name_len) j = -1;
    }

    _ksa_done = true;
    other._ksa_done = true;
  }

  // ===== load_name_triple(): 标量回退 — 三候选交错 KSA =====
  void load_name_triple(const char *a, const char *b, const char *c, int nlen,
                         Name& ob, Name& oc) {
    q_len = -1; ob.q_len = -1; oc.q_len = -1;
    memcpy(val, val_base2, sizeof val);
    memcpy(ob.val, ob.val_base2, sizeof ob.val);
    memcpy(oc.val, oc.val_base2, sizeof oc.val);
    u8_t sa = s_pre, sb = s_pre, sc = s_pre;
    for (int i = i_pre, j = j_pre; i < N; i++, j++) {
      sa += a[j] + val[i]; std::swap(val[i], val[sa]);
      sb += b[j] + ob.val[i]; std::swap(ob.val[i], ob.val[sb]);
      sc += c[j] + oc.val[i]; std::swap(oc.val[i], oc.val[sc]);
      if (j == nlen) j = -1;
    }
    sa = 0; sb = 0; sc = 0;
    for (int i = 0, j = nlen; i < N; i++, j++) {
      sa += a[j] + val[i]; std::swap(val[i], val[sa]);
      sb += b[j] + ob.val[i]; std::swap(ob.val[i], ob.val[sb]);
      sc += c[j] + oc.val[i]; std::swap(oc.val[i], oc.val[sc]);
      if (j == nlen) j = -1;
    }
    _ksa_done = true; ob._ksa_done = true; oc._ksa_done = true;
  }

  // ===== load_name_quad(): 标量回退 — 四候选交错 KSA =====
  void load_name_quad(const char *a, const char *b, const char *c, const char *d,
                       int nlen, Name& ob, Name& oc, Name& od) {
    q_len = -1; ob.q_len = -1; oc.q_len = -1; od.q_len = -1;
    memcpy(val, val_base2, sizeof val);
    memcpy(ob.val, ob.val_base2, sizeof ob.val);
    memcpy(oc.val, oc.val_base2, sizeof oc.val);
    memcpy(od.val, od.val_base2, sizeof od.val);
    u8_t sa = s_pre, sb = s_pre, sc = s_pre, sd = s_pre;
    for (int i = i_pre, j = j_pre; i < N; i++, j++) {
      sa += a[j] + val[i]; std::swap(val[i], val[sa]);
      sb += b[j] + ob.val[i]; std::swap(ob.val[i], ob.val[sb]);
      sc += c[j] + oc.val[i]; std::swap(oc.val[i], oc.val[sc]);
      sd += d[j] + od.val[i]; std::swap(od.val[i], od.val[sd]);
      if (j == nlen) j = -1;
    }
    sa = 0; sb = 0; sc = 0; sd = 0;
    for (int i = 0, j = nlen; i < N; i++, j++) {
      sa += a[j] + val[i]; std::swap(val[i], val[sa]);
      sb += b[j] + ob.val[i]; std::swap(ob.val[i], ob.val[sb]);
      sc += c[j] + oc.val[i]; std::swap(oc.val[i], oc.val[sc]);
      sd += d[j] + od.val[i]; std::swap(od.val[i], od.val[sd]);
      if (j == nlen) j = -1;
    }
    _ksa_done = true; ob._ksa_done = true; oc._ksa_done = true; od._ksa_done = true;
  }
#endif

  // ===== loading_name(): 独立 KSA (无前缀) =====
  // 用于暗影二段评分 (shadow mechanic)。
  // 直接用队伍名 KSA 的 val_base，两遍完整 KSA (no prefix)。
  // V 值计算包含全部 7 个属性 (无早退)。
  // 由于名字含 "?shadow" 后缀 (8 字节)，t_len = strlen+1 循环使用这些字节。
#if PBB_HAS_SIMD
  void loading_name(const char *name) {
    memcpy(val, val_base, sizeof val);
    q_len = -1;
    u8_t s;
    int t_len = strlen(name) + 1;
    for (int _ = 0; _ < 2; _++)
      for (int i = s = 0; i < N; i++) {
        if (i % t_len) s += name[i % t_len - 1];
        s += val[i];
        std::swap(val[i], val[s]);
      }
    simd_mul_add(val, ual, 181, 160);
    simd_filter_range_attr(ual, name_base, q_len, 30);  // SIMD 过滤: 替换逐字节分支
    V = 0;
    V += median(name_base[10], name_base[11], name_base[12]);
    V += median(name_base[13], name_base[14], name_base[15]);
    V += median(name_base[16], name_base[17], name_base[18]);
    V += median(name_base[19], name_base[20], name_base[21]);
    V += median(name_base[22], name_base[23], name_base[24]);
    V += median(name_base[25], name_base[26], name_base[27]);
    V += median(name_base[28], name_base[29], name_base[30]);
    sort10(name_base);
    V += (154 + name_base[3] + name_base[4] + name_base[5] + name_base[6]) / 3;
  }
#else
  void loading_name(const char *name) {
    memcpy(val, val_base, sizeof val);
    q_len = -1;
    u8_t s;
    int t_len = strlen(name) + 1;
    for (int _ = 0; _ < 2; _++)
      for (int i = s = 0; i < N; i++) {
        if (i % t_len) s += name[i % t_len - 1];
        s += val[i];
        std::swap(val[i], val[s]);
      }
    for (int i = 0; i < 96; i += 8) {
      ual[i+0] = val[i+0] * 181 + 160; ual[i+1] = val[i+1] * 181 + 160;
      ual[i+2] = val[i+2] * 181 + 160; ual[i+3] = val[i+3] * 181 + 160;
      ual[i+4] = val[i+4] * 181 + 160; ual[i+5] = val[i+5] * 181 + 160;
      ual[i+6] = val[i+6] * 181 + 160; ual[i+7] = val[i+7] * 181 + 160;
    }
    for (int i = 0; i < 96 && q_len < 30; i++)
      if (ual[i] >= 89 && ual[i] < 217)
        name_base[++q_len] = ual[i] & 63;
    if (q_len < 30) {
      for (int i = 96; i < N; i += 8) {
        ual[i+0] = val[i+0] * 181 + 160; ual[i+1] = val[i+1] * 181 + 160;
        ual[i+2] = val[i+2] * 181 + 160; ual[i+3] = val[i+3] * 181 + 160;
        ual[i+4] = val[i+4] * 181 + 160; ual[i+5] = val[i+5] * 181 + 160;
        ual[i+6] = val[i+6] * 181 + 160; ual[i+7] = val[i+7] * 181 + 160;
      }
      for (int i = 96; i < N && q_len < 30; i++)
        if (ual[i] >= 89 && ual[i] < 217)
          name_base[++q_len] = ual[i] & 63;
    }
    V = 0;
    V += median(name_base[10], name_base[11], name_base[12]);
    V += median(name_base[13], name_base[14], name_base[15]);
    V += median(name_base[16], name_base[17], name_base[18]);
    V += median(name_base[19], name_base[20], name_base[21]);
    V += median(name_base[22], name_base[23], name_base[24]);
    V += median(name_base[25], name_base[26], name_base[27]);
    V += median(name_base[28], name_base[29], name_base[30]);
    sort10(name_base);
    V += (154 + name_base[3] + name_base[4] + name_base[5] + name_base[6]) / 3;
  }
#endif

  // ===== calc_skills(): 技能分布计算 =====
  // 使用 ual_skills 数组 (val*181+71) 过滤出值的高位为 0 的项。
  // 步骤:
  //   1. 过滤 ual_skills → name_base[64..127] (K=64 偏移)
  //   2. 用 RC4 PRGA 对 skill[0..39] 进行 Fisher-Yates 洗牌
  //   3. 将 name_base[64..] 按 4 个一组取最小值，分配到 16 个频次槽
  //   4. 技巧列表:
  //      - 最后一个 <25 的技能 freq *= 2 (强化)
  //      - 槽 14/15 有特殊加成 (基于 name_base[60..63])
  //
  // AVX2 路径: ual_skills 已在 load_name 中由 simd_mul_add_dual 预计算。
#if PBB_HAS_SIMD
  void calc_skills(const char *name) {
    q_len = -1;
    simd_filter_skills(ual_skills, name_base, q_len);  // SIMD: 替换逐字节高位检查
    u8_t *a = name_base + K;       // 技能值从 name_base[64] 开始
    for (int i = 0; i < skill_cnt; i++) skill[i] = i;
    memset(freq, 0, sizeof freq);
    p = q = 0;
    // Fisher-Yates 洗牌 (两遍)
    for (int s = 0, _ = 0; _ < 2; _++)
      for (int i = 0; i < skill_cnt; i++) {
        s = (s + gen() + skill[i]) % skill_cnt;
        std::swap(skill[i], skill[s]);
      }
    int last = -1;                 // 最后一个 <25 的技能槽
    for (int i = 0, j = 0; i < K; i += 4, j++) {
      u8_t p = std::min({a[i], a[i+1], a[i+2], a[i+3]});
      if (p > 10 && skill[j] < 35) {
        freq[j] = p - 10;
        if (skill[j] < 25) last = j;  // 记录最后一个 "好" 技能
      } else freq[j] = 0;
    }
    if (last != -1) freq[last] <<= 1;                        // 强化最后一个好技能
    if (freq[14] && last != 14) freq[14] += std::min({name_base[60], name_base[61], freq[14]});
    if (freq[15] && last != 15) freq[15] += std::min({name_base[62], name_base[63], freq[15]});
  }
#else
  void calc_skills(const char *name) {
    q_len = -1;
    // 标量 ual 计算: val * 181 + 71 (技能用, add=71 而非 160)
    for (int i = 0; i < N; i += 8) {
      ual[i+0] = val[i+0] * 181 + 71; ual[i+1] = val[i+1] * 181 + 71;
      ual[i+2] = val[i+2] * 181 + 71; ual[i+3] = val[i+3] * 181 + 71;
      ual[i+4] = val[i+4] * 181 + 71; ual[i+5] = val[i+5] * 181 + 71;
      ual[i+6] = val[i+6] * 181 + 71; ual[i+7] = val[i+7] * 181 + 71;
    }
    for (int i = 0; i < N; i++)
      if ((ual[i] & 0x80) == 0)
        name_base[++q_len] = (ual[i] + 89) & 63;
    u8_t *a = name_base + K;
    for (int i = 0; i < skill_cnt; i++) skill[i] = i;
    memset(freq, 0, sizeof freq);
    p = q = 0;
    for (int s = 0, _ = 0; _ < 2; _++)
      for (int i = 0; i < skill_cnt; i++) {
        s = (s + gen() + skill[i]) % skill_cnt;
        std::swap(skill[i], skill[s]);
      }
    int last = -1;
    for (int i = 0, j = 0; i < K; i += 4, j++) {
      u8_t p = std::min({a[i], a[i+1], a[i+2], a[i+3]});
      if (p > 10 && skill[j] < 35) {
        freq[j] = p - 10;
        if (skill[j] < 25) last = j;
      } else freq[j] = 0;
    }
    if (last != -1) freq[last] <<= 1;
    if (freq[14] && last != 14) freq[14] += std::min({name_base[60], name_base[61], freq[14]});
    if (freq[15] && last != 15) freq[15] += std::min({name_base[62], name_base[63], freq[15]});
  }
#endif
};
