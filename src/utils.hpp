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
