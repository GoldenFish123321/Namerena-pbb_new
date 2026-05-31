# PBB 名字评分测号器

基于 RC4 状态机 + 多项式模型的名字评分枚举工具。通过 XP（虚评）和 XD（虚单）筛选高分名字。

## 快速开始

```bash
./run.sh -c config.yaml --threads -1    # Linux / Termux
run.bat -c config.yaml                  # Windows
```

首次运行自动检测环境、安装依赖、编译引擎。无需手动操作。

## 架构

```
main.py (Python)              pbb_engine (C++ 子进程)
─────────────────            ───────────────────────────
环境检测 + 自动编译           producer-consumer 引擎
YAML/JSON 配置解析            编码循环
字符集构建 (pbb_core)         RC4 状态机 + 评分
stdin 管道传参                fprintf 直接写文件
结果统计
```

Python 负责配置和编排，C++ 负责全部算法。子进程隔离消除 Python 运行时干扰。

## 文件结构

```
├── run.sh / run.bat      # 入口脚本 (自动装依赖)
├── main.py               # 主程序 (环境检测 + 编译 + 运行)
├── engine_main.cpp       # C++ 引擎入口 (2行)
├── setup.py              # pybind11 模块构建
├── config.yaml           # 主配置 (也支持 JSON)
├── .gitignore
└── src/
    ├── common.hpp        # 类型、常量
    ├── charset_data.hpp  # 字符集原始数据
    ├── model_data.hpp    # 评分模型权重
    ├── utils.hpp         # median/sort10/SIMD
    ├── charset.hpp       # Unicode 编码加载
    ├── name.hpp          # RC4 状态机
    ├── scoring.hpp       # hanxu_Poly + 评分
    ├── batch.hpp         # process_batch API
    ├── engine.hpp        # 完整引擎 (stdin 传参)
    └── bridge.cpp        # pybind11 绑定
```

## 配置

支持 YAML 和 JSON，扩展名自动检测。

```yaml
team_name: "队伍名"
prefixes:
  - name: "前缀"          # '+' = 无前缀
suffixes:
  - name: "+"
character_set:
  single_char_length: 4   # 1/2/3/4
  types: [2]              # 见下方 type 对照表
  custom_values: [240,159] # type=自定义时使用
enumeration:
  mode: 1                 # 1=顺序 2=随机区间 3=逐位随机 4=配对随机
  variable_length: 8
  ranges:
    - start: 0
      end: 100000000
collection:
  xp_min: 4900
  xd_min: 5600
  collect_mode: 0         # 0=否 1=写到 out/blue.txt
output:
  output_xp: 1
  result_file: "-"        # - =时间戳  + =随机名  其他=自定义
threads:
  worker_threads: -1      # -1 =自动检测 CPU 线程数
```

## 字符集 type 对照

| scl | type | 内容 |
|-----|------|------|
| 1   | 1-5  | 数字/小写/大写/自定义/ASCII |
| 2   | 1-8  | 希腊/俄文/拉丁/自定义/Unicode |
| 3   | 1-8  | 全部汉字/常用汉字/假名/盲文/扩展汉字/自定义/Unicode |
| 4   | 1-3  | 扩展汉字/自定义/Unicode |

## 平台

| 平台 | 入口 | 说明 |
|------|------|------|
| Linux x86-64 | `./run.sh` | AVX2 自动检测 |
| Linux ARM | `./run.sh` | 标量回退 |
| Termux (Android) | `./run.sh` | 自动 pkg install |
| Windows | `run.bat` | 标量回退 |

## 命令行

```bash
./run.sh -c config.yaml --threads 3    # 覆盖线程数
./run.sh -c config.json                # JSON 配置
python3 main.py -c my.yaml --threads -1 # -1 = 自动
```

## API (pybind11)

```python
import pbb_core
pbb_core.init_exhanzi()
name = pbb_core.Name()
name.load_team(b"Team")
name.load_prefix(b"SomeName", 8)
result = pbb_core.score_full(b"SomeName", name)
```

## 输出格式

```
名字@队伍名 XP XD
test-🙈🐛🐊🐍@test 5021 5003
```

`output_xp=0` 时不显示分数。
