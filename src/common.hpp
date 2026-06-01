#pragma once
// ============================================================================
// common.hpp — type aliases, global constants
//
// PBB core infrastructure. AVX2 controlled by compiler -mavx2 flag.
// ============================================================================

// AVX2: compiler passes -mavx2 to define __AVX2__
#if defined(__AVX2__)
  #include <immintrin.h>
  #define PBB_HAS_AVX2 1
#else
  #define PBB_HAS_AVX2 0
#endif

// AVX-512: -mavx512f -mavx512bw
#if defined(__AVX512F__) && defined(__AVX512BW__)
  #define PBB_HAS_AVX512 1
#else
  #define PBB_HAS_AVX512 0
#endif

// ARM NEON: aarch64 强制特性
#if defined(__aarch64__) || defined(__ARM_NEON)
  #include <arm_neon.h>
  #define PBB_HAS_NEON 1
#else
  #define PBB_HAS_NEON 0
#endif

// 是否存在任意 SIMD 后端
#define PBB_HAS_SIMD (PBB_HAS_AVX2 || PBB_HAS_AVX512 || PBB_HAS_NEON)

// SIMD 名称字符串 (编译期确定)
#if PBB_HAS_AVX512
  #define PBB_SIMD_NAME "AVX-512"
#elif PBB_HAS_AVX2
  #define PBB_SIMD_NAME "AVX2"
#elif PBB_HAS_NEON
  #define PBB_SIMD_NAME "NEON"
#else
  #define PBB_SIMD_NAME "none"
#endif

// Windows CRT 安全警告
#if defined(_WIN32) || defined(_WIN64)
  #ifndef _CRT_SECURE_NO_WARNINGS
    #define _CRT_SECURE_NO_WARNINGS
  #endif
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <vector>

// ===== Type aliases =====
typedef unsigned long long u64_t;
typedef unsigned char u8_t;

// ===== Global constants =====
const int N = 256;        // RC4 S-box size
const int M = 128;        // name_base size
const int K = 64;         // skill group offset
const int skill_cnt = 40; // total skills
