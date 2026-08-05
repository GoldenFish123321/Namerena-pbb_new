# Changelog

## 0.2.1

### 性能

> **本机基准说明**（Intel Core Ultra 7 255H，Arrow Lake，AVX2+VNNI，icpx `-xCORE-AVX2`）：
> 100M 名字 / 2 线程 / 固定 P 核 8,9 / 交替 A/B 配对统计（取中位数）。
> 基线 **5.150 M/s** → 优化后 **5.751 M/s**，本轮两项算法级优化累计 **+11.7%**（配对均值 +11.8%）。
> 下列条目按提交时间**从新到旧**排列。

- **共享前缀 KSA** (`15c35c6`)：连续 5 个候选名字共享 key 前缀区间，pass1 前 `sp_len` 次迭代对 5 条 RC4 链状态完全相同 → 只计算一次后广播到其余 4 链，省 `4×sp_len` 次交换；`load_name_quint` 内置自检测共享前缀、`sp_len=0` 时自动退化为原始行为（对随机枚举同样正确）。**U7 255H 单独 +5.5%**（5.43 vs 5.15 M/s，100M/2 线程）
- **属性过滤器 SIMD 稳定压缩** (`4abf93c`)：AVX2 属性提取循环改用 `vpshufb` + `Compress8Table` 每 8 字节组一次性稳定压缩（先 `&63` 掩码再 shuffle），替代逐字节分支提取。**U7 255H 在共享前缀 KSA 基础上叠加 +3.6%**，两项合计 **+11.7%**（100M 交替 A/B，2 线程）
- SIMD 过滤提前终止 (`2232e81`)：simd_mul_add_filter 收集到 max_len+1 个有效 name_base 值后立即 break，约在第 2~3 次 AVX2 迭代退出 (原始终 8 次)
- 技能频次映射优化 (`0f900fc`)：score_full 中 35×16=560 次嵌套循环改为 16 次直接 skill[k]→freq 查表，消除分支和内层初始化
- 惰性 ual_skills 计算 (`b10f5e6`)：finish_load_name 改用 simd_mul_add_filter (仅属性过滤)，ual_skills 延迟到 calc_skills 中按需计算。99.96% 名字 V<24 早退时跳过整个技能变换 SIMD 通路
- 缓存中位数属性 `_p[8]` (`689b606`)：finish_load_name/loading_name 计算 V 值时保存 7 个 median + HP 到 Name::_p[8]，score_full 直接读取避免 7 次重复 median() + HP 求和
- NEON branchless filter + KSA __restrict 优化 (`a9c0a03`)：finish_load_name 标量 for→simd_filter_range_attr (vtbl1_u8)，load_name_pair/load_prefix __restrict+const locals，ARM Cortex-A55 **+26.4%**
- V 值快检提前 (`26cc8c5`)：score_full 中 V*3<1140 提前返回跳过 8 属性提取，g++ 构建 +11.9%（icpx 编译器已自动优化）
- PAIR_WIDTH=5 五路交错 KSA (`a6ee0ed`)：Intel 12-14代 / Core Ultra / AMD Zen4+ 自动五路，Golden Cove +13.4%
- SIMD ual 计算与 name_base 过滤融合 (`08607eb`)：消除 256B 中间数组 store/reload，Intel U7 255H +2.2%
- 进位增量编码替代除法 (`4414e1e`)：consume_seq 中仅首候选做除法，后续候选 memcpy + 进位增量，x86 +6.5%
- float 特征数组 + SIMD 点积 (`4df77c1`)：score_full 中 xp_x/xp_array/hanxu_Poly 改 float，点积换 `simd_dot_f32`（AVX2 2×8 FMA / NEON 2×4 FMA / 标量回退），icpx -xCORE-AVX2 **+12%**（g++ 自动向量化已覆盖，0% 差异）
- KSA SoA 交错存储 + 32 位合并内存操作 (`a95a510`)：四候选 val 从 AoS（4 个独立数组）改为行交错 `val4[i][0..3]`，val[i] 的 load/store 由 4 条 1 字节合并为 1 条 32 位，KSA 每步 L1 内存操作 16→10.5，x86（3 线程, 1e8 区间, 严格交替 ×2）**+13.8%**，输出 17 条逐行一致
- 融合多项式展开+模型点积 (`48a7e6b`)：hanxu_Poly（1034 次标量查表 + 写中间数组）与 simd_dot 融合为按行 SIMD（利用 POLY_TABLE 行分组），消除 xp_array 中间数组 store/reload + 稀疏 0 特征行跳过，x86（3 线程, 1e8 区间）**+3.1%**，与 SoA KSA 组合 **+17.9%**

### 构建与发布

- Release zip 修复 (`a1d0fe3`)：pbb_core.pyd 重复打包（省 ~2.9MB/包）、README 恢复命令行参数说明、zip 父目录包裹
- Windows PAIR_WIDTH CPUID 检测 (`51eca29`)：Intel/AMD 现代 CPU 自动识别微架构
- PAIR_WIDTH 配置项 (`c96af46`)：环境变量 `PBB_PAIR_WIDTH=2~8` 覆盖 CPU 自动检测（越界值告警并回退），可在特定机器手动调整 KSA 交错宽度

### 修复

- 修复非空后缀时共享 key KSA 错误 (`1adfc58`)：`vary_start` 改为 `epre+(evar-1)*scl`（最低位数字起点），此前 `nlen-scl` 在后缀非空时会越过差异字节，导致 shared_key 把名字 a 的最后一位错误广播给候选 b..e（b..e 评分静默错误）
- 修复长名字 (nlen>256) 时共享前缀单链越界写 val (`1adfc58`)：`sp_len` 钳制到 `N-i_pre`，防止写穿 256 字节 S-box 破坏相邻结构体成员
- 修复 clen>256（大字库/汉字集）时数字截断 (`1adfc58`)：`dig`/`dl`/`dr` 由 uint8_t 改 uint16_t，避免枚举字符错位
- 修复区间跨 `clen^vlen` 整倍+1 时 `evar==0` 导致 `dig[-1]` 越界读 (`1adfc58`)：`can_shared` 加 `evar>0` 守卫
- 防御性初始化 Name 成员 + mode 2/4 空/单字符集死循环修复 (`1adfc58`)：`prefix_loaded`/`i_pre`/`j_pre`/`s_pre`/`q_len`/`V`/`_p`/`PRELEN`/`NAMELEN` 加默认值；`while(x<CHUNK_SIZE)` 加 `varlen_task<vlen` 上限
- CSV 分隔符从逗号改为 SOH (`63d6268`)：前缀内含逗号时不再被误拆分
- 修复 `_p[8]` u8_t 溢出 (`7a4dd3e`)：HP 原始值最大 406 超出 u8_t，改为 int _p[8] 避免截断导致评分错误
- 非 Windows engine 编译补 `-pthread` (`48ae684`)：g++/clang++ 使用 std::thread 链接缺参导致 `undefined reference to pthread_create`，感谢 [@abrucestd](https://github.com/abrucestd) 报告 ([#38](https://github.com/GoldenFish123321/Namerena-pbb_new/issues/38))
- convert_old 转换产物不合法 + 缺字段无提示 (`231ac4b`)：旧版 debug_mode 2/3（不更改线程）映射为 0 并警告；缺字段明确报错（文件截断/类型错误）；worker_threads 未预存默认 -1；空 prefix/suffix 自动补 '+', 感谢 [@abrucestd](https://github.com/abrucestd) 报告 ([#36](https://github.com/GoldenFish123321/Namerena-pbb_new/issues/36))
- Python 最低版本 3.10 → 3.9 (`5f239e9`)：代码实际只需 PEP 585 泛型注解（3.9 引入），3.10 门槛无依据；tomllib 已有 tomli 兜底，感谢 [@abrucestd](https://github.com/abrucestd) 报告 ([#37](https://github.com/GoldenFish123321/Namerena-pbb_new/issues/37))

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
