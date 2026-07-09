# Windows 预编译发布指南

本文档用于制作“不要求用户安装 Python / g++ / MSYS2 / oneAPI”的 Windows 10/11 预编译发布包。

## 发布目标

预编译发布包面向只想直接运行的用户：

- 用户机器不需要 Python。
- 用户机器不需要 C++ 编译器。
- 发布包内包含 Python runtime、`pbb_core*.pyd` 和 `pbb_engine.exe`。
- `release_entry.py` 会跳过 `build.py` 的编译器探测，直接使用包内 `build/pbb_engine.exe`。

普通开发运行仍推荐使用仓库根目录下的 `run.bat`，它会自动准备 `.venv`、安装 Python 依赖并编译本机引擎。

## 发布物不要进仓库

以下目录/文件属于生成物，已由 `.gitignore` 忽略：

- `release/`
- `release_build/`
- `*.spec`
- `*.zip`

仓库只保留发布入口、文档和源码改动。真正发布时上传 `release/*.zip` 到 GitHub Releases 或其他分发渠道。

## 硬性发布要求

每个发布包必须满足：

- 包名格式：`pbb-最大型号范围-版本号.zip`。
- `最大型号范围` 要尽可能大，但范围内所有 CPU 的最佳编译参数必须相同。
- 包名使用小写 ASCII、连字符分隔，例如 `pbb-windows-x64-intel-raptorlake-avx2-0.1.2.zip`。
- 包内 README 必须使用中文。
- 包内 README 必须写明版本号、最佳适用范围、编译参数和详细用法。
- 包内必须包含且只包含以下配置示例文件：
  - `config.example.json`
  - `config.example.toml`
  - `config.example.yaml`
- 包内不得包含 `config.json`、`config.toml`、`config.yaml`、`smoke.config.*`、个人配置或运行输出。
- 验证用 smoke 配置必须放在发布目录外，例如 `release_build/smoke.config.toml`。
- 验证时使用 `--output-dest stdout`，避免在发布目录里生成 `out/`。

## CPU 分组原则

发布前根据 CPU 型号确认微架构和指令集，然后按“最佳编译参数相同”的最大范围合并。

| CPU 范围 | 推荐编译目标 | SIMD |
|----------|--------------|------|
| Intel Alder Lake | `-march=alderlake` | AVX2/FMA |
| Intel Raptor Lake / Raptor Lake Refresh | `-march=raptorlake` | AVX2/FMA |
| AMD Zen4 / Hawk Point | `-march=znver4` | AVX-512/FMA |
| AMD Zen5 / Zen5c / Fire Range | `-march=znver5` | AVX-512/FMA |

不要为了减少包数量而把不同最佳 `-march` 的 CPU 合并成一个包。

## 包内 README 模板

README 必须是中文，并至少包含以下内容：

```text
PBB Windows 预编译包

版本号：0.1.2

最佳适用范围：
  Windows 10/11 x64
  <CPU 范围说明>

编译参数：
  <完整 -march / SIMD 参数>

包含文件：
  PBB 主程序
  Python runtime
  pbb_core*.pyd
  pbb_engine.exe
  config.example.json / config.example.toml / config.example.yaml

使用方法：
  1. 从三个 config.example.* 中任选一种复制为 config.json / config.toml / config.yaml。
  2. 修改配置里的 team_name、prefixes、suffixes、character_set、enumeration、collection、threads 等字段。
  3. 运行：
       pbb-xxx.exe -c config.toml
     或：
       run.bat -c config.toml
  4. 不指定 -c 时，程序会依次尝试 config.json、config.yaml、config.toml。
  5. 结果默认写入 out/。

常用命令：
  pbb-xxx.exe -c config.toml
  pbb-xxx.exe -c config.toml --threads 8
  pbb-xxx.exe -c config.toml --mode 1 --vlen 4 --range-start 0 --range-end 50000
  pbb-xxx.exe -c config.toml --xp-min 5000 --xd-min 6000
  pbb-xxx.exe -c config.toml --output-dest both -o result.txt
  pbb-xxx.exe --help

注意：
  本包是预编译包，--rebuild 不用于用户机器。
```

## 构建流程

发布机需要：

- Windows x64
- Python 与项目 `.venv`
- `pybind11`、`PyYAML`
- PyInstaller
- MSYS2 UCRT64 GCC，示例路径：`C:\msys64\ucrt64\bin\g++.exe`

编译 `pbb_core`：

```powershell
C:\msys64\ucrt64\bin\g++.exe `
  -std=c++17 -O3 -funroll-loops -ffast-math `
  <CPU_FLAGS> `
  -static -static-libgcc -static-libstdc++ `
  -shared `
  -I src `
  -I .venv\Lib\site-packages\pybind11\include `
  -I C:\Python314\Include `
  -o release_build\<target>\build\pbb_core.cp314-win_amd64.pyd `
  src\bridge.cpp `
  C:\Python314\libs\python314.lib
```

编译 `pbb_engine.exe`：

```powershell
C:\msys64\ucrt64\bin\g++.exe `
  -std=c++17 -O3 -funroll-loops -ffast-math `
  <CPU_FLAGS> `
  -static -static-libgcc -static-libstdc++ `
  -I src `
  -o release_build\<target>\build\pbb_engine.exe `
  engine_main.cpp
```

打包：

```powershell
$buildAbs = (Resolve-Path release_build\<target>\build).Path

.venv\Scripts\pyinstaller.exe `
  --noconfirm --clean --onedir `
  --name pbb-<最大型号范围> `
  --distpath release `
  --workpath release_build\pyinstaller-work `
  --specpath release_build `
  --paths "$buildAbs" `
  --add-data "$buildAbs;build" `
  release_entry.py
```

发布目录准备：

```powershell
Copy-Item config.example.json release\<package>\config.example.json -Force
Copy-Item config.example.toml release\<package>\config.example.toml -Force
Copy-Item config.example.yaml release\<package>\config.example.yaml -Force
```

压缩：

```powershell
Compress-Archive `
  -Path release\<package> `
  -DestinationPath release\<package>-<version>.zip `
  -Force
```

## 验证

建议至少做三步验证：

```powershell
release\<package>\<exe>.exe --help
release\<package>\<exe>.exe -c release_build\smoke.config.toml --output-dest stdout
Get-ChildItem release\<package> -Filter "config*.json","config*.toml","config*.yaml"
```

验证输出中必须看到版本号和 SIMD：

```text
[main] Version: 0.1.2
[engine] SIMD: AVX2
```

或 AMD Zen4/Zen5 包：

```text
[main] Version: 0.1.2
[engine] SIMD: AVX-512
```

最后确认发布目录内没有个人配置、smoke 配置和 `out/`。

## 注意事项

- `pbb_core*.pyd` 与 Python ABI 绑定，例如 `cp314-win_amd64` 只能配套 Python 3.14 x64 runtime。
- 目标 CPU 不支持的 `-march` 会导致用户机器无法运行或非法指令崩溃。
- Intel 12/13/14 代消费级混合架构通常使用 AVX2/FMA，不使用 AVX-512 包。
- AMD Zen4/Zen5 包会启用 AVX-512 路径，不应发给不支持 AVX-512 的 AMD 老平台。
- `--rebuild` 在预编译发布包里没有意义；release 入口会提示不可用并继续使用包内引擎。
