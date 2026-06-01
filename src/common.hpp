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

// Windows CRT 安全警告
#if defined(_WIN32) || defined(_WIN64)
  #ifndef _CRT_SECURE_NO_WARNINGS
    #define _CRT_SECURE_NO_WARNINGS
  #endif
#endif

// 编译器优化指令 (GCC/Clang)
#if defined(__GNUC__) || defined(__clang__)
  #pragma GCC optimize("Ofast")
  #if PBB_HAS_AVX2
    #pragma GCC target("avx2")
  #endif
  #pragma GCC optimize("unroll-loops")
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
