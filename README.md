# PBB 名字评分测号器

基于 RC4 状态机 + 多项式模型的名字评分枚举工具。通过 XP（虚评）和 XD（虚单）筛选高分名字。

## 快速开始

```bash
# 复制示例配置, 修改参数
cp config.example.yaml config.yaml

# 一键启动 (自动创建虚拟环境、安装依赖、编译引擎)
./run.sh -c config.yaml          # Linux / Termux
run.bat -c config.yaml           # Windows
```

首次运行自动检测环境、安装系统包、创建 `.venv`、编译 C++ 引擎。无需手动操作。

## 配置

支持 **JSON / YAML / TOML** 三种格式，扩展名自动识别。不指定 `-c` 时依次尝试 `config.json → config.yaml → config.toml`。

```yaml
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
    - count: 1
      start: 0                  # 起始编号
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
# 14 个参数直接覆盖配置文件
python3 main.py -c config.yaml \
  --team "测试队" \
  --threads 8 \
  --mode 1 --vlen 4 \
  --range-start 0 --range-end 50000 \
  --xp-min 5000 --xd-min 6000 \
  --collect-mode 2 \
  --output-xp 1 \
  --scl 3 --types 7 --custom-values "你好世界"
```

| 参数 | 覆盖字段 | 示例 |
|------|----------|------|
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
| `--scl` | `character_set.single_char_length` | `--scl 3` |
| `--types` | `character_set.types` | `--types 1,7` |
| `--custom-values` | `character_set.custom_values` | `--custom-values "你好"` |

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

结果写入 `out/` 目录：

```
名字@队伍名 XP XD
test-🙈🐛🐊🐍@test 5021 5003
```

`collect_mode=1/2` 时特殊属性号写入 `out/blue.txt`。

`output_xp=0` 时不显示分数。

## 平台

| 平台 | 入口 | 说明 |
|------|------|------|
| Linux x86_64 | `./run.sh` | AVX2 自动检测, g++ -O3 |
| Linux ARM | `./run.sh` | 标量回退 |
| Termux (Android) | `./run.sh` | 自动 pkg install python/clang |
| Windows | `run.bat` | MSVC cl.exe 优先, MinGW g++ 回退 |

## 架构

```
main.py (Python)              pbb_engine (C++ 子进程)
─────────────────            ───────────────────────────
环境检测 + 自动编译           producer-consumer 引擎
JSON/YAML/TOML 配置解析       编码循环
字符集构建 (pbb_core .so)     RC4 状态机 + 评分
stdin 管道传参                fprintf 直接写文件
结果统计                      进度实时输出
```

Python 负责配置和编排，C++ 负责全部算法。子进程隔离消除 Python 运行时干扰。

## 文件结构

```
├── run.sh / run.bat          # 入口 (建 .venv + 装依赖)
├── main.py                   # Python 编排层
├── engine_main.cpp           # C++ 引擎入口
├── setup.py                  # pybind11 模块构建
├── config.example.json       # JSON 配置示例
├── config.example.yaml       # YAML 配置示例
├── config.example.toml       # TOML 配置示例
└── src/
    ├── common.hpp            # 类型、常量
    ├── charset_data.hpp      # 字符集原始数据
    ├── model_data.hpp        # 评分模型权重
    ├── utils.hpp             # median/sort10/SIMD
    ├── charset.hpp           # 字符集 + Unicode 编码
    ├── name.hpp              # RC4 状态机
    ├── scoring.hpp           # hanxu_Poly + 完整评分
    ├── engine.hpp            # 引擎 (stdin传参, 进度显示)
    └── bridge.cpp            # pybind11 绑定
```
