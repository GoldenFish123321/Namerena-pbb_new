# Changelog

## 0.2.1

### 性能

- 进位增量编码替代除法 (`4414e1e`)：consume_seq 中仅首候选做除法，后续候选 memcpy + 进位增量，x86 +6.5%
- SIMD ual 计算与 name_base 过滤融合 (`08607eb`)：消除 256B 中间数组 store/reload，Intel U7 255H +2.2%
- PAIR_WIDTH=5 五路交错 KSA (`a6ee0ed`)：Intel 12-14代 / Core Ultra / AMD Zen4+ 自动五路，Golden Cove +13.4%
- V 值快检提前 (`26cc8c5`)：score_full 中 V*3<1140 提前返回跳过 8 属性提取，g++ 构建 +11.9%（icpx 编译器已自动优化）
- NEON branchless filter + KSA __restrict 优化 (`a9c0a03`)：finish_load_name 标量 for→simd_filter_range_attr (vtbl1_u8)，load_name_pair/load_prefix __restrict+const locals，ARM Cortex-A55 **+26.4%**

### 构建与发布

- Release zip 修复 (`a1d0fe3`)：pbb_core.pyd 重复打包（省 ~2.9MB/包）、README 恢复命令行参数说明、zip 父目录包裹
- Windows PAIR_WIDTH CPUID 检测 (`51eca29`)：Intel/AMD 现代 CPU 自动识别微架构

### 修复

- CSV 分隔符从逗号改为 SOH (`63d6268`)：前缀内含逗号时不再被误拆分

## 0.2.0

感谢 [@spdc-elm](https://github.com/spdc-elm) 在 [#17](https://github.com/GoldenFish123321/Namerena-pbb_new/issues/17) 中提出的系统性性能优化建议。

### 性能 ([#17](https://github.com/GoldenFish123321/Namerena-pbb_new/issues/17), [#18](https://github.com/GoldenFish123321/Namerena-pbb_new/pull/18), [#22](https://github.com/GoldenFish123321/Namerena-pbb_new/pull/22))

累计提升：Intel U7 255H 接近翻倍（~+100%），ARM 手机 0.3T/d → 0.5T/d（+67%）。

- 四路 KSA 交错 (PAIR_WIDTH=4, `f117830`)：利用乱序核 ROB 深度隐藏 RC4 延迟，+50% 吞吐
- SIMD 过滤 + 稳定压缩（方向二, `e128401`）：V 值/技能检查全面 SIMD 化，消除逐字节分支预测失败
- 共享 key load（建议四, `f49f511`）：四候选 KSA 公共字节只 load 一次，+6%
- ARM 自适应 (`f902960`)：Cortex-A55 等 in-order 核自动切换双路交错，+12.7%

### 构建与发布

- `version.py`：版本号单一源，发布只需改一行
- 通用 Windows 构建：新增 `universal` 目标（`-march=x86-64`，无 SIMD 特化），所有 x86-64 可用
- Release 工作流 tag 触发改为精确 semver

### 测试

- CI 新增 mode 2（随机种子）回归测试 (`d03b6b9`)

### 修复

- Windows CPUID 64-bit 指针截断 (`d124ae4`)
- VNNI 编译器探测缺宿主机 CPU 验证 (`9d68b0a`)
- `run.sh` 缺 `Python.h` 自动装 `python3-dev` (`09c3071`)

## 0.1.2

- 支持 `enumeration.prefix_ranges`，可按前缀顺序为不同前缀分配独立搜索区间。
- 支持运行时通过 `out/.threads` 动态调整 worker 线程数，并修复降档线程在队列关闭后无法退出导致进程卡住的问题。
- 新增 `convert_old.py`，用于将旧版输入格式转换为新版 YAML 配置。
- `prefix_ranges.end = -1` 统一表示接近 `uint64_t` 上限的无限区间，不再表示跳过此前缀。
- 新增 `PBB_CXXFLAGS` 编译参数覆盖能力，并改进 Windows MinGW Python 链接库生成。
- 未合入旧格式样例输入和未调用的旧版 C++ 参考源码，避免仓库引入无用大文件。
- 版本号更新为 `0.1.2`。

## 0.1.1

- 修复 C++ 引擎在超大搜索区间、多前缀配置下启动崩溃的问题。
- 按枚举模式估算 `mex_vis` 任务数：仅 `mode 1` 按前缀数量放大，`mode 2/3/4` 不再错误乘以 `np`。
- 为 `mex_vis` 预分配增加安全上限，避免 `vector<bool>` 超大分配导致 `std::length_error`。
- 修复 `end = -1` 等接近 `uint64_t` 上限的区间下，总任务量取整溢出导致 `time left` 全部显示为 `0h0m0s` 的问题。
- 修复前后缀配置中合法前导/尾随空格被误删的问题，`+` 仍表示空前缀或空后缀。
- 合并当前 `dev` 分支修复，包括 Ctrl+C 中断时刷新输出，降低结果截断风险。
- 修复 Linux CI 下 C++ 模板类型推导不一致导致的编译失败。
- 在程序启动时输出当前版本号。

## 0.1.0

- `0.1.1` 修改前的基线版本。
