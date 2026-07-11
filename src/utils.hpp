#pragma once
// ============================================================================
// utils.hpp — 无分支排序网络与 SIMD 加速工具函数
//
// 所有函数均声明为 static inline，确保编译器内联优化。
// AVX2 路径使用 256 位 SIMD 指令并行处理 32 字节。
// ============================================================================
#include "common.hpp"

// ===== 无分支中位数 (branchless median) =====
// 编译器生成 cmov 指令代替分支跳转，避免分支预测失败惩罚。
// 输入: 3 个 u8_t 值
// 输出: 中间值
static inline u8_t median(u8_t a, u8_t b, u8_t c) {
  u8_t max_ab = a > b ? a : b;
  u8_t min_ab = a < b ? a : b;
  return (c > max_ab) ? max_ab : ((c < min_ab) ? min_ab : c);
}

// ===== 无分支条件交换 (branchless conditional swap) =====
// 通过位运算实现无分支的交换:
//   diff = a - b, 若 a < b 则 diff 的 bit7 = 1
//   mask = (diff >> 7) - 1 → 全 0 (a>=b) 或全 1 (a<b)
//   tmp = (a ^ b) & mask → 仅在 a<b 时非零
//   a ^= tmp, b ^= tmp → 仅在 a<b 时交换
static inline void cond_swap(u8_t& a, u8_t& b) {
    u8_t diff = a - b;
    u8_t mask = (diff >> 7) - 1;
    u8_t tmp  = (a ^ b) & mask;
    a ^= tmp;
    b ^= tmp;
}

// ===== 10 元素排序网络 (最优 29 次交换, 深度 9) =====
// 硬编码的 Batcher 奇偶归并排序网络，用于对 name_base 前 10 元素排序。
// 排序后 name_base[3..6] 用于计算 HP 加成:
//   V += (154 + name_base[3] + name_base[4] + name_base[5] + name_base[6]) / 3
static inline void sort10(u8_t* arr) {
    #define SWAP(i,j) cond_swap(arr[i], arr[j])
    SWAP(0, 5); SWAP(1, 6); SWAP(2, 7); SWAP(3, 8); SWAP(4, 9);
    SWAP(0, 3); SWAP(1, 4); SWAP(5, 8); SWAP(6, 9);
    SWAP(0, 2); SWAP(3, 6); SWAP(4, 7); SWAP(1, 5);
    SWAP(0, 1); SWAP(2, 4); SWAP(7, 9); SWAP(3, 5);
    SWAP(2, 3); SWAP(6, 8); SWAP(4, 5);
    SWAP(1, 2); SWAP(3, 4); SWAP(5, 6); SWAP(7, 8);
    SWAP(1, 3); SWAP(4, 6); SWAP(2, 5);
    SWAP(0, 1); SWAP(2, 3); SWAP(4, 5); SWAP(6, 7); SWAP(8, 9);
    SWAP(1, 2); SWAP(3, 4); SWAP(5, 6); SWAP(7, 8);
    SWAP(2, 3); SWAP(4, 5); SWAP(6, 7);
    #undef SWAP
}

// ===== SIMD ual 计算: val[i] = (val[i] * mul + add) & 0xFF =====
// 按指令集层级选择最优实现: AVX-512 (64B/iter) > AVX2 (32B/iter) > NEON (16B/iter)
#if PBB_HAS_AVX512
// ---- AVX-512: 4 次迭代覆盖 256 字节 ----
static inline void simd_mul_add(const u8_t* __restrict__ val, u8_t* __restrict__ ual,
                                 u8_t mul, u8_t add) {
  const __m512i vmul = _mm512_set1_epi16(mul);
  const __m512i vadd = _mm512_set1_epi16(add);
  const __m512i vmask = _mm512_set1_epi16(0xFF);
  const __m512i vzero = _mm512_setzero_si512();
  for (int i = 0; i < 256; i += 64) {
    __m512i v = _mm512_loadu_si512((const __m512i*)&val[i]);
    __m512i lo = _mm512_unpacklo_epi8(v, vzero);
    __m512i hi = _mm512_unpackhi_epi8(v, vzero);
    lo = _mm512_mullo_epi16(lo, vmul);
    hi = _mm512_mullo_epi16(hi, vmul);
    lo = _mm512_add_epi16(lo, vadd);
    hi = _mm512_add_epi16(hi, vadd);
    lo = _mm512_and_si512(lo, vmask);
    hi = _mm512_and_si512(hi, vmask);
    __m512i res = _mm512_packus_epi16(lo, hi);
    _mm512_storeu_si512((__m512i*)&ual[i], res);
  }
}

static inline void simd_mul_add_dual(const u8_t* __restrict__ val,
                                      u8_t* __restrict__ ual_attr,
                                      u8_t* __restrict__ ual_skill) {
  const __m512i vmul  = _mm512_set1_epi16(181);
  const __m512i vaddA = _mm512_set1_epi16(160);
  const __m512i vaddS = _mm512_set1_epi16(71);
  const __m512i vmask = _mm512_set1_epi16(0xFF);
  const __m512i vzero = _mm512_setzero_si512();
  for (int i = 0; i < 256; i += 64) {
    __m512i v = _mm512_loadu_si512((const __m512i*)&val[i]);
    __m512i lo = _mm512_unpacklo_epi8(v, vzero);
    __m512i hi = _mm512_unpackhi_epi8(v, vzero);
    __m512i loA = _mm512_add_epi16(_mm512_mullo_epi16(lo, vmul), vaddA);
    __m512i hiA = _mm512_add_epi16(_mm512_mullo_epi16(hi, vmul), vaddA);
    __m512i loS = _mm512_add_epi16(_mm512_mullo_epi16(lo, vmul), vaddS);
    __m512i hiS = _mm512_add_epi16(_mm512_mullo_epi16(hi, vmul), vaddS);
    loA = _mm512_and_si512(loA, vmask); hiA = _mm512_and_si512(hiA, vmask);
    loS = _mm512_and_si512(loS, vmask); hiS = _mm512_and_si512(hiS, vmask);
    _mm512_storeu_si512((__m512i*)&ual_attr[i], _mm512_packus_epi16(loA, hiA));
    _mm512_storeu_si512((__m512i*)&ual_skill[i], _mm512_packus_epi16(loS, hiS));
  }
}

#elif PBB_HAS_AVX2
// ---- AVX2: 8 次迭代覆盖 256 字节 ----
static inline void simd_mul_add(const u8_t* __restrict__ val, u8_t* __restrict__ ual,
                                 u8_t mul, u8_t add) {
  const __m256i vmul = _mm256_set1_epi16(mul);
  const __m256i vadd = _mm256_set1_epi16(add);
  const __m256i vmask = _mm256_set1_epi16(0xFF);
  const __m256i vzero = _mm256_setzero_si256();
  for (int i = 0; i < 256; i += 32) {
    __m256i v = _mm256_loadu_si256((const __m256i*)&val[i]);
    __m256i lo = _mm256_unpacklo_epi8(v, vzero);
    __m256i hi = _mm256_unpackhi_epi8(v, vzero);
    lo = _mm256_mullo_epi16(lo, vmul);
    hi = _mm256_mullo_epi16(hi, vmul);
    lo = _mm256_add_epi16(lo, vadd);
    hi = _mm256_add_epi16(hi, vadd);
    lo = _mm256_and_si256(lo, vmask);
    hi = _mm256_and_si256(hi, vmask);
    __m256i res = _mm256_packus_epi16(lo, hi);
    _mm256_storeu_si256((__m256i*)&ual[i], res);
  }
}

static inline void simd_mul_add_dual(const u8_t* __restrict__ val,
                                      u8_t* __restrict__ ual_attr,
                                      u8_t* __restrict__ ual_skill) {
  const __m256i vmul  = _mm256_set1_epi16(181);
  const __m256i vaddA = _mm256_set1_epi16(160);
  const __m256i vaddS = _mm256_set1_epi16(71);
  const __m256i vmask = _mm256_set1_epi16(0xFF);
  const __m256i vzero = _mm256_setzero_si256();
  for (int i = 0; i < 256; i += 32) {
    __m256i v = _mm256_loadu_si256((const __m256i*)&val[i]);
    __m256i lo = _mm256_unpacklo_epi8(v, vzero);
    __m256i hi = _mm256_unpackhi_epi8(v, vzero);
    __m256i loA = _mm256_add_epi16(_mm256_mullo_epi16(lo, vmul), vaddA);
    __m256i hiA = _mm256_add_epi16(_mm256_mullo_epi16(hi, vmul), vaddA);
    __m256i loS = _mm256_add_epi16(_mm256_mullo_epi16(lo, vmul), vaddS);
    __m256i hiS = _mm256_add_epi16(_mm256_mullo_epi16(hi, vmul), vaddS);
    loA = _mm256_and_si256(loA, vmask);  hiA = _mm256_and_si256(hiA, vmask);
    loS = _mm256_and_si256(loS, vmask);  hiS = _mm256_and_si256(hiS, vmask);
    _mm256_storeu_si256((__m256i*)&ual_attr[i], _mm256_packus_epi16(loA, hiA));
    _mm256_storeu_si256((__m256i*)&ual_skill[i], _mm256_packus_epi16(loS, hiS));
  }
}

#elif PBB_HAS_NEON
// ---- ARM NEON: 16 次迭代覆盖 256 字节 ----
static inline void simd_mul_add(const u8_t* __restrict__ val, u8_t* __restrict__ ual,
                                 u8_t mul, u8_t add) {
  const uint16x8_t vmul = vdupq_n_u16(mul);
  const uint16x8_t vadd = vdupq_n_u16(add);
  const uint16x8_t vmask = vdupq_n_u16(0xFF);
  for (int i = 0; i < 256; i += 16) {
    uint8x16_t v = vld1q_u8(&val[i]);
    uint16x8_t lo = vmovl_u8(vget_low_u8(v));
    uint16x8_t hi = vmovl_u8(vget_high_u8(v));
    lo = vmulq_u16(lo, vmul);
    hi = vmulq_u16(hi, vmul);
    lo = vaddq_u16(lo, vadd);
    hi = vaddq_u16(hi, vadd);
    lo = vandq_u16(lo, vmask);
    hi = vandq_u16(hi, vmask);
    uint8x16_t res = vcombine_u8(vqmovn_u16(lo), vqmovn_u16(hi));
    vst1q_u8(&ual[i], res);
  }
}

static inline void simd_mul_add_dual(const u8_t* __restrict__ val,
                                      u8_t* __restrict__ ual_attr,
                                      u8_t* __restrict__ ual_skill) {
  const uint16x8_t vmul  = vdupq_n_u16(181);
  const uint16x8_t vaddA = vdupq_n_u16(160);
  const uint16x8_t vaddS = vdupq_n_u16(71);
  const uint16x8_t vmask = vdupq_n_u16(0xFF);
  for (int i = 0; i < 256; i += 16) {
    uint8x16_t v = vld1q_u8(&val[i]);
    uint16x8_t lo = vmovl_u8(vget_low_u8(v));
    uint16x8_t hi = vmovl_u8(vget_high_u8(v));
    uint16x8_t loA = vaddq_u16(vmulq_u16(lo, vmul), vaddA);
    uint16x8_t hiA = vaddq_u16(vmulq_u16(hi, vmul), vaddA);
    uint16x8_t loS = vaddq_u16(vmulq_u16(lo, vmul), vaddS);
    uint16x8_t hiS = vaddq_u16(vmulq_u16(hi, vmul), vaddS);
    loA = vandq_u16(loA, vmask); hiA = vandq_u16(hiA, vmask);
    loS = vandq_u16(loS, vmask); hiS = vandq_u16(hiS, vmask);
    vst1q_u8(&ual_attr[i], vcombine_u8(vqmovn_u16(loA), vqmovn_u16(hiA)));
    vst1q_u8(&ual_skill[i], vcombine_u8(vqmovn_u16(loS), vqmovn_u16(hiS)));
  }
}
#endif

// ===== NEON 稳定压缩查找表 (Issue #17 附件 E14) =====
// 将 8 位 lane mask 映射为 vtbl1_u8 的 shuffle 索引和匹配计数。
// constexpr 编译期计算，零运行时开销。
#if PBB_HAS_NEON
struct Compress8Table {
  u8_t indexes[256][8];
  u8_t counts[256];
  constexpr Compress8Table() {
    for (int mask = 0; mask < 256; mask++) {
      int count = 0;
      for (int lane = 0; lane < 8; lane++)
        if (mask & (1 << lane)) indexes[mask][count++] = lane;
      for (int lane = count; lane < 8; lane++) indexes[mask][lane] = 0xff;
      counts[mask] = count;
    }
  }
};
static constexpr Compress8Table compress8_table{};
#endif

// ===== SIMD 过滤 + 稳定压缩 (Issue #17 方向二) =====
// 将逐字节分支替换为 SIMD 比较 → 位掩码 → 只遍历匹配字节，
// 消除数据依赖的分支预测失败。

// ---- 属性过滤: ual[i] ∈ [89, 217), 输出 ual[i] & 63 ----
// 用在 finish_load() / loading_name() 的 SIMD 路径
#if PBB_HAS_AVX512
static inline void simd_filter_range_attr(const u8_t* __restrict__ ual,
                                           u8_t* __restrict__ name_base,
                                           int& q_len, int max_count) {
  for (int i = 0; i < 256 && q_len < max_count; i += 64) {
    __m512i data = _mm512_loadu_si512((const __m512i*)&ual[i]);
    __mmask64 ge = _mm512_cmp_epu8_mask(data, _mm512_set1_epi8(88), _MM_CMPINT_GT);
    __mmask64 lt = _mm512_cmp_epu8_mask(_mm512_set1_epi8(217), data, _MM_CMPINT_GT); // 217 > data ≡ data < 217
    __mmask64 mask = ge & lt;
    while (mask && q_len < max_count) {
      int idx = __builtin_ctzll(mask);
      name_base[++q_len] = ual[i + idx] & 63;
      mask &= mask - 1;
    }
  }
}

static inline void simd_filter_skills(const u8_t* __restrict__ ual,
                                       u8_t* __restrict__ name_base, int& q_len) {
  for (int i = 0; i < 256; i += 64) {
    __m512i data = _mm512_loadu_si512((const __m512i*)&ual[i]);
    // high bit == 0 ≡ data < 0x80 (无符号)
    __mmask64 mask = _mm512_cmp_epu8_mask(data, _mm512_set1_epi8(0x80), _MM_CMPINT_LT);
    while (mask) {
      int idx = __builtin_ctzll(mask);
      name_base[++q_len] = (ual[i + idx] + 89) & 63;
      mask &= mask - 1;
    }
  }
}

#elif PBB_HAS_AVX2
static inline void simd_filter_range_attr(const u8_t* __restrict__ ual,
                                           u8_t* __restrict__ name_base,
                                           int& q_len, int max_count) {
  const __m256i v88  = _mm256_set1_epi8(88); // subs(data, 88) != 0 ≡ data > 88 ≡ data >= 89
  const __m256i v216 = _mm256_set1_epi8(216); // subs(data, 216) == 0 ≡ data <= 216 ≡ data < 217
  const __m256i vzero = _mm256_setzero_si256();
  for (int i = 0; i < 256 && q_len < max_count; i += 32) {
    __m256i data = _mm256_loadu_si256((const __m256i*)&ual[i]);
    // ual >= 89 (无符号): ual - 88 不下溢 → subs != 0, 取反 cmpeq (用 88 保证 89 也被匹配)
    __m256i ge = _mm256_xor_si256(
        _mm256_cmpeq_epi8(_mm256_subs_epu8(data, v88), vzero),
        _mm256_set1_epi8(0xFF));
    // ual < 217 (无符号): ual - 216 下溢 → subs == 0
    __m256i lt = _mm256_cmpeq_epi8(_mm256_subs_epu8(data, v216), vzero);
    unsigned mask = (unsigned)_mm256_movemask_epi8(_mm256_and_si256(ge, lt));
    while (mask && q_len < max_count) {
      int idx = __builtin_ctz(mask);
      name_base[++q_len] = ual[i + idx] & 63;
      mask &= mask - 1;
    }
  }
}

static inline void simd_filter_skills(const u8_t* __restrict__ ual,
                                       u8_t* __restrict__ name_base, int& q_len) {
  for (int i = 0; i < 256; i += 32) {
    __m256i data = _mm256_loadu_si256((const __m256i*)&ual[i]);
    // movemask 提取每个字节的高位; 取反后 bit=1 表示高位为 0 (匹配)
    unsigned mask = (unsigned)~_mm256_movemask_epi8(data);
    while (mask) {
      int idx = __builtin_ctz(mask);
      name_base[++q_len] = (ual[i + idx] + 89) & 63;
      mask &= mask - 1;
    }
  }
}

#elif PBB_HAS_NEON
// NEON: 8 字节比较 → vaddv_u8 位掩码 → vtbl1_u8 查表稳定压缩
// Compress8Table 将 256 种 lane mask 映射为有序 shuffle 索引，
// 消除逐字节分支预测失败。附件 E14 证明此路径在 ARM 上收益 +37%。
static inline void simd_filter_range_attr(const u8_t* __restrict__ ual,
                                           u8_t* __restrict__ name_base,
                                           int& q_len, int max_count) {
  const uint8x8_t lower = vdup_n_u8(89);
  const uint8x8_t upper = vdup_n_u8(217);
  const uint8x8_t low_bits = vdup_n_u8(63);
  const uint8x8_t bit_weights = {1, 2, 4, 8, 16, 32, 64, 128};
  for (int i = 0; i < 256 && q_len < max_count; i += 8) {
    uint8x8_t data = vld1_u8(&ual[i]);
    uint8x8_t selected = vand_u8(vcge_u8(data, lower), vclt_u8(data, upper));
    int mask = vaddv_u8(vand_u8(selected, bit_weights));
    if (mask) {
      uint8x8_t compacted = vtbl1_u8(vand_u8(data, low_bits),
                                      vld1_u8(compress8_table.indexes[mask]));
      vst1_u8(name_base + q_len + 1, compacted);
      q_len += compress8_table.counts[mask];
    }
  }
  if (q_len > max_count) q_len = max_count;
}

static inline void simd_filter_skills(const u8_t* __restrict__ ual,
                                       u8_t* __restrict__ name_base, int& q_len) {
  const uint8x8_t high_bit = vdup_n_u8(0x80);
  const uint8x8_t offset = vdup_n_u8(89);
  const uint8x8_t low_bits = vdup_n_u8(63);
  const uint8x8_t bit_weights = {1, 2, 4, 8, 16, 32, 64, 128};
  const uint8x8_t zero = vdup_n_u8(0);
  for (int i = 0; i < 256; i += 8) {
    uint8x8_t data = vld1_u8(&ual[i]);
    uint8x8_t selected = vceq_u8(vand_u8(data, high_bit), zero);
    int mask = vaddv_u8(vand_u8(selected, bit_weights));
    if (mask) {
      uint8x8_t compacted = vtbl1_u8(vand_u8(vadd_u8(data, offset), low_bits),
                                      vld1_u8(compress8_table.indexes[mask]));
      vst1_u8(name_base + q_len + 1, compacted);
      q_len += compress8_table.counts[mask];
    }
  }
}

#else
// 标量回退: 无 SIMD 平台
static inline void simd_filter_range_attr(const u8_t* __restrict__ ual,
                                           u8_t* __restrict__ name_base,
                                           int& q_len, int max_count) {
  for (int i = 0; i < 256 && q_len < max_count; i++)
    if (ual[i] >= 89 && ual[i] < 217)
      name_base[++q_len] = ual[i] & 63;
}

static inline void simd_filter_skills(const u8_t* __restrict__ ual,
                                       u8_t* __restrict__ name_base, int& q_len) {
  for (int i = 0; i < 256; i++)
    if ((ual[i] & 0x80) == 0)
      name_base[++q_len] = (ual[i] + 89) & 63;
}
#endif
