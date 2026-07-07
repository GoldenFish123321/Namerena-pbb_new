# Windows 预编译发布指南

本文档用于制作“不要求用户安装 Python / g++ / MSYS2 / oneAPI”的 Windows 发布包。

## 适用场景

普通开发运行仍推荐使用仓库根目录下的 `run.bat`，它会自动准备 `.venv`、安装 Python 依赖并编译本机引擎。

预编译发布包面向只想直接运行的用户：

- 用户机器不需要 Python。
- 用户机器不需要 C++ 编译器。
- 发布包内包含 Python runtime、`pbb_core*.pyd` 和 `pbb_engine.exe`。
- `release_entry.py` 会跳过 `build.py` 的编译器探测，直接使用包内 `build/pbb_engine.exe`。

## 发布物不要进仓库

以下目录/文件属于生成物，已由 `.gitignore` 忽略：

- `release/`
- `release_build/`
- `*.spec`
- `*.zip`

仓库只保留发布入口、文档和源码改动。真正发布时上传 `release/*.zip` 到 GitHub Releases 或其他分发渠道。

## 目标机器信息

发布前尽量确认目标用户机器：

| 信息 | 作用 |
|------|------|
| Windows 版本 | 确认是否需要兼容 Windows 10/11 |
| CPU 型号 | 决定 `-march` 和可用 SIMD |
| 指令集 | AVX2 / AVX-512 / FMA 是否可用 |
| 是否必须单文件 | 决定 PyInstaller `--onedir` 或 `--onefile` |

例如 `Windows 10 + Intel i7-8700` 属于 Coffee Lake，适合使用 AVX2/FMA，不应使用 AMD Zen5 的 `-march=znver5`。

## Windows 10 + i7-8700 构建示例

以下命令在发布机上执行。发布机需要有 Python、项目依赖、PyInstaller 和 MSYS2 UCRT64 GCC；用户机器不需要。

创建构建目录：

```powershell
New-Item -ItemType Directory -Force -Path release_build\win10_i7_8700_avx2\build | Out-Null
```

编译 `pbb_core`：

```powershell
C:\msys64\ucrt64\bin\g++.exe `
  -std=c++17 -O3 -funroll-loops -ffast-math `
  -march=skylake -mavx2 -mfma `
  -static -static-libgcc -static-libstdc++ `
  -shared `
  -I src `
  -I .venv\Lib\site-packages\pybind11\include `
  -I C:\Python314\Include `
  -o release_build\win10_i7_8700_avx2\build\pbb_core.cp314-win_amd64.pyd `
  src\bridge.cpp `
  C:\Python314\libs\python314.lib
```

编译 `pbb_engine.exe`：

```powershell
C:\msys64\ucrt64\bin\g++.exe `
  -std=c++17 -O3 -funroll-loops -ffast-math `
  -march=skylake -mavx2 -mfma `
  -static -static-libgcc -static-libstdc++ `
  -I src `
  -o release_build\win10_i7_8700_avx2\build\pbb_engine.exe `
  engine_main.cpp
```

安装 PyInstaller：

```powershell
uv pip install --python .venv\Scripts\python.exe pyinstaller
```

打包：

```powershell
$buildAbs = (Resolve-Path release_build\win10_i7_8700_avx2\build).Path

.venv\Scripts\pyinstaller.exe `
  --noconfirm --clean --onedir `
  --name PBB-win10-i7-8700-avx2 `
  --distpath release `
  --workpath release_build\pyinstaller-work `
  --specpath release_build `
  --paths "$buildAbs" `
  --add-data "$buildAbs;build" `
  release_entry.py
```

复制配置和可选启动脚本：

```powershell
Copy-Item config.toml release\PBB-win10-i7-8700-avx2\config.toml -Force
Copy-Item config.example.toml release\PBB-win10-i7-8700-avx2\config.example.toml -Force
```

压缩发布目录：

```powershell
Compress-Archive `
  -Path release\PBB-win10-i7-8700-avx2 `
  -DestinationPath release\PBB-win10-i7-8700-avx2.zip `
  -Force
```

## 验证

建议至少做两步验证：

```powershell
cd release\PBB-win10-i7-8700-avx2
.\PBB-win10-i7-8700-avx2.exe --help
.\PBB-win10-i7-8700-avx2.exe -c smoke.config.toml
```

运行输出中应能看到类似：

```text
[engine] SIMD: AVX2
```

正式配置通常 range 较大，不建议作为发布前 smoke test。单独准备一个很小的 `smoke.config.toml` 即可验证打包链路、`pbb_core` 加载、引擎启动和输出目录写入。

## 注意事项

- `pbb_core*.pyd` 与 Python ABI 绑定，例如 `cp314-win_amd64` 只能配套 Python 3.14 x64 runtime。
- 目标 CPU 不支持的 `-march` 会导致用户机器无法运行或非法指令崩溃。
- i7-8700 支持 AVX2/FMA，但不支持 AVX-512。
- `--rebuild` 在预编译发布包里没有意义；release 入口会提示不可用并继续使用包内引擎。
