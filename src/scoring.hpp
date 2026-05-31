#pragma once
// ============================================================================
// scoring.hpp — 多项式特征扩展与完整评分流水线
//
// 核心函数:
//   hanxu_Poly()  — 44 → 1034 维多项式特征扩展 (hanxu 算法)
//   score_full()  — 封装 load_name → V check → calc_skills → model scoring
//
// 评分模型:
//   MODEL[1035]   — XP (虚评) 线性模型: bias + 1034 个权重
//   MODELQD[1035] — XD (虚单/duel) 线性模型 (仅当 XP >= 4300 时计算)
//
//   hanxu_Poly 生成二阶交互特征 (44 维 → 1034 维):
//     一阶: x[i]                    (i=0..43, 共 44 个)
//     二阶: x[p] * x[p+q]           (p=0..6, q=1..44-p, 共 990 个)
//     (具体索引计算见函数内注释)
// ============================================================================
#include "common.hpp"
#include "model_data.hpp"
#include "name.hpp"

// ===== ScoreResult: 评分结果结构体 =====
struct ScoreResult {
  int xp;           // XP (虚评) 分数
  int xd;           // XD (虚单/duel) 分数
  int sum;          // V + 252 = 八围总分 (V + 7*36)
  int props[8];     // 8 个属性值 (加 36 偏移后的值, prop[7] 为 HP)
  int skills[16];   // 16 个槽的技能 ID
  int freqs[16];    // 16 个槽的技能频次
  int flag3;        // 特定技能频次和 (用于动态阈值调整: flag3>=50 → XPmin-=300)
  int shadow;       // 暗影技能值 (xp_x[42], 即技能 34 "治愈魔法" 的频次)
  bool flag;        // 是否通过技能标志检查
};

// ===== hanxu_Poly: 多项式特征扩展 =====
// 将 44 维特征向量 x 扩展为 1034 维向量 xp。
//
// 算法 (hanxu 格式):
//   对每个输出维度 y (0..1033):
//     从 y 开始沿三角矩阵反向追踪:
//       i 递增 (1,2,3,...)
//       p = 前 i 个三角行号 (i>2 时递增)
//       q = 当前列号
//       j = j - l + p (l 初始为 44, 逐步递减)
//     最终:
//       i=1 → xp[y] = x[q]         (一阶: 单变量)
//       i>1 → xp[y] = x[p] * x[p+q] (二阶: 交互项)
//
// 生成的 1034 个特征:
//   一阶: x[0..43] (44 个)
//   二阶: x[p]*x[p+q] 其中 p=0..6, q=1..min(?, 44-p) (990 个)
inline void hanxu_Poly(double *xp, double *x) {
  int l, i, p, q, j;
  double r;
  // y 循环: 生成 1034 个特征
  for (int y = 0; y < 1034; y++) {
    l = 44;                        // 初始搜索范围 = 特征维度
    i = 0, p = 0, q = 0;
    r = 0;
    j = y;
    // 反向追踪: 最多 45 次迭代
    for (int k = 0; k < 45; k++) {
      i++;                         // 维度计数 (1-based)
      p += (i > 2);                // 仅 i>2 时递增行号 p
      q = j;                       // 列号
      j = j - l + p;               // 更新 j: 跳到上一行对应位置
      if (j < 0) break;            // 已到第一行, 结束追踪
    }
    // 根据追踪结果填充特征
    if (i == 1)
      r = x[q];                    // 一阶: 直接取单变量
    if (i > 1)
      r = x[p] * x[p + q];         // 二阶: 交互项
    xp[y] = r;
  }
}

// ===== score_full: 完整评分流水线 =====
// 前置条件: name_obj 必须先调用 load_team() 和 load_prefix()
//
// 流水线步骤:
//   Step 1: load_name()  → 计算 V 值和 8 个属性 prop[0..7]
//   Step 2: V 值检查     → 双路径 (高: V*3>=1200, 低: V*3>=1140+条件)
//   Step 3: calc_skills() → 技能分布
//   Step 4: 技能标志检查 → 护盾(21)/隐匿(24)>70 + flag3>=60 + sum/sum2阈值
//   Step 5: 构建 xp_x[44] → 属性+技能频次特征向量
//   Step 6: 暗影二段评分 → 若有"潜行"技能(32), 追加 "?shadow" 评分
//   Step 7: hanxu_Poly + MODEL/MODELQD 线性评分
//
// 返回值: ScoreResult, 其中 flag=true 表示通过所有检查
//         调用方在 Python 层判断 score >= threshold 决定是否输出
inline ScoreResult score_full(const char* name, int name_len, Name& name_obj) {
  ScoreResult result = {};
  memset(&result, 0, sizeof(result));

  // ---- Step 1: 加载名字, 提取属性 ----
  name_obj.load_name(name, name_len);

  // 8 个属性值 (7 个中位数 + HP)
  // 属性编号: 0=atk, 1=def, 2=spd, 3=mag, 4=res, 5=acc, 6=eva, 7=HP
  int prop[8];
  prop[0] = median(name_obj.name_base[10], name_obj.name_base[11], name_obj.name_base[12]);
  prop[1] = median(name_obj.name_base[13], name_obj.name_base[14], name_obj.name_base[15]);
  prop[2] = median(name_obj.name_base[16], name_obj.name_base[17], name_obj.name_base[18]);
  prop[3] = median(name_obj.name_base[19], name_obj.name_base[20], name_obj.name_base[21]);
  prop[4] = median(name_obj.name_base[22], name_obj.name_base[23], name_obj.name_base[24]);
  prop[5] = median(name_obj.name_base[25], name_obj.name_base[26], name_obj.name_base[27]);
  prop[6] = median(name_obj.name_base[28], name_obj.name_base[29], name_obj.name_base[30]);
  prop[7] = 154 + name_obj.name_base[3] + name_obj.name_base[4]
                + name_obj.name_base[5] + name_obj.name_base[6];

  int V = name_obj.V;

  // ---- Step 2: V 值双路径检查 ----
  // 高路径: V*3 >= 1200 (大部分高分名字走此路径)
  // 低路径: V*3 >= 1140 且满足特定属性条件 (兜底捕获)
  bool check_skills = false;
  bool is_high_path = (V * 3 >= 1200);
  bool is_low_path  = (V * 3 >= 1140 && prop[6] >= 36
                       && prop[1] + prop[2] + prop[5] + prop[6] >= 175);

  if (is_high_path || is_low_path)
    check_skills = true;

  if (!check_skills)
    return result;

  // ---- Step 3: 技能分布 ----
  name_obj.calc_skills(name);

  // ---- Step 4: 技能标志检查 ----
  // flag3: 分身(23)+魅惑(11)+蓄力(16)+护身符(28) 的频次和
  //        flag3 >= 60 → 有特殊技能组合
  // flag:  护盾(21)>70 或 隐匿(24)>70 → 有防御技能
  int flag3 = 0;
  bool has_skill = false;
  for (int _ = 0; _ < 16; _++) {
    if (name_obj.skill[_] == 21 && name_obj.freq[_] > 70) has_skill = true;
    if (name_obj.skill[_] == 24 && name_obj.freq[_] > 70) has_skill = true;
    if (name_obj.skill[_] == 23 || name_obj.skill[_] == 11 ||
        name_obj.skill[_] == 16 || name_obj.skill[_] == 28)
      flag3 += name_obj.freq[_];
  }
  if (flag3 >= 60) has_skill = true;

  int sum = V + 36 * 7, sum2 = 0;
  for (int _ = 0; _ < 16; _++) sum2 += name_obj.freq[_];

  // 根据路径不同的技能频次阈值
  if (is_high_path) {
    if (sum2 >= 155) has_skill = true;
    if (sum >= 697) has_skill = true;
  } else {
    // 低路径 (V*3 >= 1140): 更严格的频次要求
    if (sum2 >= 185) has_skill = true;
  }

  if (!has_skill) {
    result.flag = false;
    return result;
  }

  // ---- Step 5: 构建 44 维特征向量 xp_x ----
  // 结构: xp_x[0] = HP, xp_x[1..7] = 属性0-6, xp_x[8..42] = 技能0-34频次
  //       xp_x[43] = 暗影加成 (Step 6 填充)
  double xp_x[44] = {};
  for (int j = 0; j < 7; j++) {
    prop[j] += 36;                 // 属性标准化 (+36 偏移)
    result.props[j] = prop[j];
    xp_x[j + 1] = prop[j];
  }
  xp_x[0] = prop[7];              // HP 放在位置 0
  result.props[7] = prop[7];

  // 技能频次: 将 freq[k] 分配到对应的技能 ID 槽
  for (int i = 0; i < 35; i++) {
    xp_x[i + 8] = 0;
    for (int k = 0; k < 16; k++) {
      if (name_obj.skill[k] == i) {
        xp_x[i + 8] = name_obj.freq[k];
        result.skills[k] = i;
        result.freqs[k] = name_obj.freq[k];
      }
    }
  }

  // ---- Step 6: 暗影二段评分 (Shadow mechanic) ----
  // 仅当 "潜行" 技能 (skill 32, 即 xp_x[32+8?]) 频次 > 0 时触发。
  // 暗影名字 = 原名 + "?shadow" (8 字节后缀)。
  // 用 loading_name (独立 KSA) 重新评分，计算 shadow_bonus。
  // 公式: shadow_sum = HP/3 + Σ prop[0..6] - prop[6]*3
  //        shadowi = (shadow_sum - 210) * xp_x[32] / 100
  double shadow_bonus = 0;
  if (xp_x[32] > 0) {
    char shadow_name[260];
    memcpy(shadow_name, name, name_len);
    shadow_name[name_len] = '?';
    shadow_name[name_len+1] = 's';
    shadow_name[name_len+2] = 'h';
    shadow_name[name_len+3] = 'a';
    shadow_name[name_len+4] = 'd';
    shadow_name[name_len+5] = 'o';
    shadow_name[name_len+6] = 'w';
    shadow_name[name_len+7] = 0;

    name_obj.loading_name(shadow_name);
    int sprop[8];
    sprop[0] = median(name_obj.name_base[10], name_obj.name_base[11], name_obj.name_base[12]);
    sprop[1] = median(name_obj.name_base[13], name_obj.name_base[14], name_obj.name_base[15]);
    sprop[2] = median(name_obj.name_base[16], name_obj.name_base[17], name_obj.name_base[18]);
    sprop[3] = median(name_obj.name_base[19], name_obj.name_base[20], name_obj.name_base[21]);
    sprop[4] = median(name_obj.name_base[22], name_obj.name_base[23], name_obj.name_base[24]);
    sprop[5] = median(name_obj.name_base[25], name_obj.name_base[26], name_obj.name_base[27]);
    sprop[6] = median(name_obj.name_base[28], name_obj.name_base[29], name_obj.name_base[30]);
    sprop[7] = 154 + name_obj.name_base[3] + name_obj.name_base[4]
                   + name_obj.name_base[5] + name_obj.name_base[6];

    float shadow_sum = sprop[7] / 3.0f;
    for (int j = 0; j < 7; j++) shadow_sum += sprop[j];
    shadow_sum -= sprop[6] * 3;
    float shadowi = shadow_sum - 210;
    shadowi = shadowi * xp_x[32] / 100.0;
    shadow_bonus = shadowi;
  }
  xp_x[43] = shadow_bonus;

  // "治愈魔法" (skill 34, 即 xp_x[42]) 治疗加成: +20
  if (xp_x[42] > 0) xp_x[42] += 20;

  // ---- Step 7: 多项式扩展 + 模型评分 ----
  double xp_array[1034];
  hanxu_Poly(xp_array, xp_x);

  // XP 评分: bias + Σ feature_i * weight_i
  float score = MODEL[0];
  for (int i = 0; i < 1034; i++)
    score += xp_array[i] * MODEL[i + 1];

  // XD 评分: 仅当 XP >= 4300 (基本合格) 时计算
  float scoreQD = 0;
  if (score >= 4300) {
    scoreQD = MODELQD[0];
    for (int i = 0; i < 1034; i++)
      scoreQD += xp_array[i] * MODELQD[i + 1];
  }

  result.xp = (int)score;
  result.xd = (int)scoreQD;
  result.sum = sum;
  result.flag = true;
  result.flag3 = flag3;

  return result;
}
