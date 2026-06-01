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

// 技能名称映射 (0-34 有效, 35-39 占位)
// 索引: 11=魅惑 16=蓄力 21=护盾 23=分身 24=隐匿 28=护身符 32=潜行 34=治愈
const char skillNameMap[][13] = {
    "火球术", "冰冻术", "雷击术", "地裂术", "吸血攻击", "投毒", "连击",
    "会心一击", "瘟疫", "生命之轮", "狂暴术", "魅惑", "加速术", "减速术",
    "诅咒", "治愈魔法", "苏生术", "净化", "铁壁", "蓄力", "聚气",
    "潜行", "血祭", "分身", "幻术", "防御", "守护", "伤害反弹",
    "护身符", "护盾", "反击", "吞噬", "召唤亡灵", "垂死抗争", "隐匿",
    "啧", "啧", "啧", "啧", "啧"};
