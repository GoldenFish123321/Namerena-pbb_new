# PBB 名字评分测号器

基于 RC4 状态机 + 多项式模型的名字评分枚举工具。通过 XP（虚评）和 XD（虚单）筛选高分名字。

## 快速开始

```bash
# 复制示例配置, 修改参数
cp config.example.yaml config.yaml

# 一键启动 (自动检测环境、安装依赖、编译引擎)
./run.sh -c config.yaml          # Linux / Termux / Docker
./run.sh -y -c config.yaml       # 跳过确认 (CI/Docker)
run.bat -c config.yaml           # Windows
run.bat -y -c config.yaml        # Windows 跳过确认
```

首次运行自动：检测系统包管理器 → 编译 C++ 引擎 → 启动。所有系统级安装前会确认（`-y` 跳过）。

## 配置

支持 **JSON / YAML / TOML** 三种格式，扩展名自动识别。不指定 `-c` 时依次尝试 `config.json → config.yaml → config.toml`。

```yaml
debug_mode: 0                   # 0=关闭 1=开启 (--debug 启用)
team_name: "test"
prefixes:
  - name: "test-"               # '+' = 无前缀
suffixes:
  - name: "+"

character_set:
  single_char_length: 4         # 1/2/3/4 字节
  types: [2]                    # 字符集类型 (见下方对照表)
  custom_values: "🐒🐵🐊🦎🐍🐛🙈🙉🙊🐉"  # 字符串 (也兼容旧格式数字列表)

enumeration:
  mode: 1                       # 1=顺序 2=随机(区间) 3=随机(逐位) 4=随机(配对)
  variable_length: 8             # 可变字符数
  ranges:
    - start: 0                  # 起始编号
      end: 100000000            # 结束编号 (不含)

collection:
  xp_min: 4900                  # 虚评最低阈值
  xd_min: 5600                  # 虚单最低阈值
  collect_mode: 1               # 0=不收集 1=硬编码阈值 2=自定义阈值
  special_thresholds:           # collect_mode=2 时生效
    eight_v_min: 777
    seven_v_min: 700
    hl_min: 93
    hp398_eight_v_min: 741

output:
  output_xp: 1                  # 输出文件是否包含分数
  log_output: 1                 # 任务日志: 0=文件 1=屏幕
  speed_output: 1               # 速度信息: 0=文件 1=屏幕
  result_file: "-"              # - =时间戳 + =随机名 其他=自定义

threads:
  worker_threads: -1            # -1 =自动检测CPU核心数
```

示例配置文件见 `config.example.json` / `config.example.yaml` / `config.example.toml`。

## 枚举模式

| mode | 行为 | 受控参数 |
|------|------|----------|
| **1** 顺序 | `[start, end)` 区间内按编号逐个枚举 | `ranges[0].start` / `end` |
| **2** 随机(区间) | 每个 chunk 随机起点 + 剩余位顺序展开 | `ranges` 控 chunk 数，范围 = `charset_len^vlen` |
| **3** 随机(逐位) | 每位独立随机选择 | 同上 |
| **4** 随机(区间+配对) | 类似 mode 2，prefix/suffix 配对 | 同上 |

**随机模式关键概念**：
- **次数** = `(end - start) / 1M` 个 chunk，每个 chunk 1M 个名字
- **随机范围** = `charset 字符数 ^ variable_length`（自动计算）
- 例：10 个 emoji + `variable_length: 4` → 范围 = 10⁴ = 10000
- `ranges[0].end` 设大一些获得更多采样（如 100M = 100 chunks = 1 亿次随机）

> mode 2/3/4 配合 `seed` 可实现**确定性随机**——同 seed + 同 range → 任意线程数下结果完全一致。

## 字符集 type 对照

| scl | type | 内容 |
|-----|------|------|
| 1   | 1-5  | 数字 / 小写字母 / 大写字母 / 自定义 / ASCII |
| 2   | 1-8  | 希腊 / 俄文 / 拉丁 / 自定义 / Unicode |
| 3   | 1-8  | 全部汉字 / 常用汉字 / 平假名 / 片假名 / 盲文 / 扩展汉字 / 自定义 / Unicode |
| 4   | 1-3  | 扩展汉字 / 自定义 / Unicode |

**自定义字符集** 直接写原始字符，自动 UTF-8 编码：

```yaml
# scl=3 用中文
custom_values: "你好世界测试名字"

# scl=4 用 emoji
custom_values: "🐒🐵🐊🦎🐍🐛🙈🙉🙊🐉"
```

## 命令行参数

```bash
python3 main.py -c config.yaml \
  --team "测试队" \
  --threads 8 \
  --mode 1 --vlen 4 \
  --range-start 0 --range-end 50000 \
  --xp-min 5000 --xd-min 6000 \
  --collect-mode 2 \
  --output-xp 1 \
  -o myrun.txt \                 # 输出文件名 (可选: + 随机, - 时间戳)
  --output-dest both \            # 输出目标 (可选: file/stdout/both)
  --scl 3 --types 7 --custom-values "你好世界"
```

| 参数 | 覆盖字段 | 示例 |
|------|----------|------|
| `--rebuild` | (无) 强制重编 C++ 引擎 | `--rebuild` |
| `--team` | `team_name` | `--team "测试"` |
| `--threads` | `threads.worker_threads` | `--threads 8` |
| `--mode` | `enumeration.mode` | `--mode 1` |
| `--vlen` | `enumeration.variable_length` | `--vlen 4` |
| `--range-start` | `enumeration.ranges[0].start` | `--range-start 0` |
| `--range-end` | `enumeration.ranges[0].end` | `--range-end 50000` |
| `--xp-min` | `collection.xp_min` | `--xp-min 5000` |
| `--xd-min` | `collection.xd_min` | `--xd-min 6000` |
| `--collect-mode` | `collection.collect_mode` | `--collect-mode 2` |
| `--output-xp` | `output.output_xp` | `--output-xp 1` |
| `-o` / `--output-file` | `output.result_file` | `-o myrun.txt` / `-o +` |
| `--output-dest` | (无) 控制输出位置 | `--output-dest stdout` / `--output-dest both` |
| `--debug` | `debug_mode` | 引擎诊断输出 |
| `--scl` | `character_set.single_char_length` | `--scl 3` |
| `--types` | `character_set.types` | `--types 1,7` |
| `--custom-values` | `character_set.custom_values` | `--custom-values "你好"` |

## 环境检测与指令集优化

启动时自动检测环境并输出摘要：

```
[env] OS: linux x86_64
[env] CPU: x86_64 (6 cores)
[env] Python: 3.12.3
[env] compilers: icpx(AVX-512) -> g++(AVX2)
```

编译器按性能优先级自动选择，编译失败自动回退：

| 平台 | engine 编译器优先级 | pbb_core 编译器优先级 | 指令集 |
|------|--------------------|----------------------|--------|
| Linux x86_64 | icpx → g++ | icpx → g++ | AVX-512 > AVX2 |
| Linux ARM / Termux | g++ | g++ | NEON (128bit) |
| Windows x86_64 | icpx → g++ → MSVC | icpx → MSVC | AVX2 (icpx 固定) |

> Windows icpx 固定 `-xCORE-AVX2`。`-xHost` 在 Arrow Lake 上仍导致 LLVM 编译期 crash（2026-06-01 确认）。pbb_core 排除 g++（MinGW ABI 不兼容 Python）。

编译输出（正常模式简洁，`--debug` 显示完整命令）：

```
[build] pbb_core: icpx (AVX2)
[build] engine: icpx (AVX2)
```

运行时会打印 SIMD 级别：

```
[engine] SIMD: AVX2
```

## 进度显示

引擎运行时实时输出进度 (每 100 个 task)：

```
task99 finished,task_mex=100,count:0.000100T
tot=17, (741,5615,5933),time: 28.54s, speed: 0.3027T/d,time left:0h0m0s
```

- `task_mex` — 最小未完成 task_id (全部完成时 = 总 task 数)
- `(八围, XP, XD)` — 当前最大八围总分、虚评、虚单
- `time left` — 预估剩余时间

## 输出

### 输出文件名

配置 `output.result_file` 控制输出文件名：

| 值 | 含义 | 示例 |
|----|------|------|
| `-` | 时间戳 (默认) | `result_20260601_153045.txt` |
| `+` | 随机 hex | `result_a3f2b1c8.txt` |
| 其他 | 自定义名称 | `my_output.txt` |

文件名自动加上 `out/` 前缀，无需手动写。CLI 可覆盖：`--output-file myrun.txt` / `-o +`

### 输出目标

`--output-dest` 控制结果输出位置：

| 值 | 行为 |
|----|------|
| `file` | 仅写入 `out/<文件名>` (默认) |
| `stdout` | 仅打印到终端 (不保留文件) |
| `both` | 写入文件 + 终端打印 |

结果格式：

```
名字@队伍名 XP XD
test-🙈🐛🐊🐍@test 5021 5003
```

`collect_mode=1/2` 时特殊属性号写入 `out/blue.txt`。`output_xp=0` 时不显示分数。

## 架构

```
build.py (Python)              pbb_engine (C++ 子进程)
─────────────────            ───────────────────────────
编译器自动检测 (icpx/g++/cl)   producer-consumer 多线程引擎
指令集自动探测 (AVX512/AVX2/   RC4 KSA → 属性评分 → 技能分布
  NEON)                       hanxu_Poly 多项式特征扩展
统一编译 + 回退机制            MODEL 线性模型打分 (XP/XD)
                              stderr 实时进度 → Python 解析

engine.py (Python)             src/ 核心 (header-only)
─────────────────            ───────────────────────────
字符集构建 (pbb_core)          common.hpp  类型 + SIMD 检测
stdin 管道传参                 utils.hpp   AVX512/AVX2/NEON
Popen → stderr 逐行读取        name.hpp    RC4 状态机
结果统计                       scoring.hpp 评分流水线

main.py (Python)               engine.hpp  引擎主循环
─────────────────
CLI + 配置解析 + 调用 engine.run()
```

### 两层并行与分布式预留

项目本质是对整数区间 `[L, R)` 的纯函数遍历（`f(i) → score`，各 `i` 间零依赖），
因此并行切分天然分两层：

| 层 | 单位 | 切分者 | 大小 |
|----|------|--------|------|
| **chunk** | 引擎内部线程级 | `engine.hpp` producer | `CHUNK_SIZE` = 1M 名字 |
| **task** | 分布式级 | 服务端 (规划中) | 服务端按更大粒度切总 range |

`engine.run()` 已收敛为单机 / 服务端 / 客户端三方共用的统一入口：

```python
run(config, engine_bin=None, out_dir=None, result_file="result.txt")
#   config 模式 A: 含 character_set → 内部构建字符集 (单机)
#   config 模式 B: 含 charset_hex   → 直接使用 (服务端下发, 免重复构建)
#   config["seed"]: 随机种子 (mode 2/3/4)
#       None        → 时间熵 (单机默认, 每次结果不同)
#       具体数值     → 确定性 (分布式: 同 seed → 结果可复现/可去重, 与线程数无关)
#   result_file: 多 task 并发同目录时传唯一名, 避免撞文件
# 返回: {results, max_xp, max_xd, max_sum, found, speed}
#   max_*/found/speed 由引擎权威输出 (SUMMARY 行), Python 不重算。
#   speed 为纯算力吞吐 (名字/秒, 不含子进程 fork / 字符集初始化 / 文件读取)。
```

> **随机性与线程数解耦**：每个 chunk 的随机性派生自 `(seed, task_id)`，
> 与"用几个线程、谁来跑、跑的顺序"完全无关。**同 seed + 同 range 在任意线程数下结果完全一致**
> （已用 3/6/12 线程 md5 对拍验证）。这是分布式 task 超时回收重发的前提：
> 服务端为每个 task 存固定 seed，重发时下发同一 seed，无论分给几核机器都产生相同名字集，可去重。


## 文件结构

```
├── run.sh / run.bat          # 入口 (建 .venv + 装依赖, -y 跳过确认)
├── build.py                  # 编译器检测 + SIMD 探测 + 统一编译
├── engine.py                 # 引擎执行: 构建字符集 → Popen → 解析结果
├── main.py                   # 编排层: CLI 解析 + 配置加载
├── engine_main.cpp           # C++ 引擎入口
├── build/                    # 编译产物 (pbb_engine, pbb_core.{so,pyd})
├── out/                      # 运行输出 (result_*.txt, blue.txt)
├── config.example.json       # JSON 配置示例
├── config.example.yaml       # YAML 配置示例
├── config.example.toml       # TOML 配置示例
└── src/
    ├── common.hpp            # 类型 + SIMD 自动检测 (AVX512/AVX2/NEON)
    ├── charset_data.hpp      # 字符集原始数据 (希腊/俄文/拉丁/盲文/汉字)
    ├── model_data.hpp        # 评分模型权重 (MODEL/MODELQD, 1035 floats)
    ├── utils.hpp             # median/sort10 + 三路 SIMD (AVX512/AVX2/NEON)
    ├── charset.hpp           # 字符集加载 + Unicode 编码
    ├── name.hpp              # RC4 状态机 (KSA → PRGA → 技能分布)
    ├── scoring.hpp           # 评分流水线 (V值→技能→hanxu_Poly预计算表→打分)
    ├── engine.hpp            # 引擎主循环 (producer-consumer, stdin 传参)
    └── bridge.cpp            # pybind11 字符集数据绑定
```
