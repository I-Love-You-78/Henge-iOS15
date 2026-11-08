# iOS 15 与 A9（iPhone 6s / SE）支持

这份 fork 在 [Henge 0.2](../README.md) 的基础上，把应用和引擎改到能在
**iOS 15 及以上**、**A9 及以上的 arm64 设备**（iPhone 6s、iPhone SE 第一代、
iPhone 7/8、iPad 5/6 等）上安装和运行，并且用 GitHub Actions 直接产出 `.ipa`
（不需要 Mac、不需要 Husk 检出）。

为什么不是 iOS 14：iPhone 6s/SE 一代最高只能到 iOS 15.8.x，iOS 15 目标已经覆盖
这些机器的全部出厂系统；同时 iOS 15 保留了大量上游用到的 UI API
（`Section(_:)`、`overlay(alignment:)`、`Material`、`foregroundStyle`、
`confirmationDialog`、`Button(role:)`、`submitLabel`），改动面小得多。真正需要
兼容层的只剩 iOS 16 才有的那几个（见下表）。

先说结论，再说为什么。

## 结论

| 项 | 状态 |
| --- | --- |
| 代码能在 iOS 15 SDK / 15.0 部署目标下编译、产出 ipa | 已实现，CI 每次构建都验证 |
| A9 上不会再因为 Armv8.1 原子指令直接 SIGILL | 已实现（原子指令改走 C 模拟） |
| A9 上能否真正进到 macOS 桌面 | **未验证，预期很可能失败**，见“还差什么” |
| 2 GB 内存机型（6s/SE/7/8）跑 macOS | 只能给 1.5 GB，远低于 macOS 需要的 4 GB |
| 免越狱/免 TrollStore 的 JIT | 做不到：iOS 15–16 只能靠签名给 JIT |
| ipa 体积只有几 MB | 正常：里面是模拟器，macOS 是安装时从 Apple 下载的 |

也就是说：**这一版把“装不上、跑不了第一个块”的问题解决了，把“必然 SIGILL”
的问题解决了，但 A9 + 2 GB 仍然踩在 macOS 的最低需求之下**。请把它当作实验构建。

## 上游为什么要求 A14 / iOS 17

- `ios/README.md` 说得很直接：虚拟 CPU 会把 guest 的 Armv8.5 指令**原样复制**
  进 JIT 块执行（guest 和 host 同为 arm64，这是它快的原因），所以宿主芯片必须
  也能执行这些指令 —— A14/M1 起。
- `core/translate.c` 的 `translate_mem()` 里，除了 LDXR/STXR 这类独占访问，
  其余原子指令走 `default:` 分支，注释写的是 *“ORDERED, CAS, ATOMIC: same
  instruction on the host address”*：`LDADD/LDCLR/LDEOR/LDSET/SWP/CAS/CASP/
  LDAPR/LDAPUR` 全部按原编码发出。A9 是 Armv8.0，这些编码未分配，执行即
  `SIGILL`，进程直接死。
- 部署目标 `iOS: 17.0`（`ios/project.yml`），JIT 走的是 iOS 26 的 TXM 之后
  才需要的那套（StikDebug / 内置 StikJIT / 端侧配对）。

## 这一版改了什么

### 1. 引擎：宿主能力探测 + 原子指令 C 模拟

- 新增 `core/hostcaps.c` / `core/hostcaps.h`：查 `hw.optional.arm.FEAT_LSE`、
  `FEAT_LRCPC2` 等 sysctl；sysctl 不认识时，**直接试着执行一条该指令**（临时
  装 SIGILL 处理器 + `sigsetjmp`），以此给出确定答案；结果缓存。
- `core/translate.c`：宿主没有 `FEAT_LSE` 时，`HV_MEM_ATOMIC` / `HV_MEM_CAS`
  改走 `HV_OP_MEM` helper；没有 `FEAT_LRCPC2` 时，`LDAPUR/STLUR`（imm9
  ordered）同样改走 helper。
- 这条 helper 路径本来就有完整实现：`core/memop.c` 的 `do_atomic()` /
  `do_casp()` 用 C11 `__atomic_compare_exchange_n` 做 RMW，编译到 A9 上就是
  `ldaxr/stlxr` 循环，不需要 LSE。CASP 的 16 字节比较交换用 `__uint128_t`
  原子操作，A9 也支持（LL/SC 对）。
- 启动时日志会打一行
  `henge: host features: LSE=no LRCPC2=no DotProd=… FP16=… (probed)`，
  在 `henge.log` 里能看到判定结果以及是否用了探测。

代价：A14 以前的机器上所有原子操作从“一条指令”变成“一次 helper 调用 + C 模拟”，
`os_unfair_lock`、libdispatch 这类热点会明显变慢。A14+ 上逻辑不变（仍然内联）。

### 2. iOS 15 部署目标与 UI 兼容层

`ios/project-ios15.yml` 是第二份 xcodegen spec：`deploymentTarget: 15.0`、
编译条件 `HENGE_LEGACY HENGE_NO_SCENE_ACCESSORY`、不链接 StikJIT/rppairing、
不带 JIT helper appex、不带 `AppIcon.icon`（Icon Composer 需要更新的 Xcode，
改用经典 `AppIcon.appiconset`）。

`ios/Henge/Legacy/Compat.swift` 把“iOS 15 之后才有”的 API 收口成带 `#available`
的薄封装，两份构建共用同一套源码：

| 用了什么（版本） | 兼容层 |
| --- | --- |
| `NavigationStack`（16） | `CompatNavigationStack`（15 走 `NavigationView`） |
| `navigationDestination(for:)`（16） | `compatDestination` + `CompatLink`（15 把目的地塞进 `NavigationLink`） |
| `LabeledContent`（16） | `LCRow` |
| `ShareLink`（16） | `ShareButton`（`UIActivityViewController`） |
| `PasteButton`（16） | 15 上用普通按钮读 `UIPasteboard` |
| `scrollContentBackground` / `persistentSystemOverlays`（16） | `compatHide…()` |
| EDR headroom（16） | 颜色探针日志里的 `#available` |
| `UISceneSession.Role.windowExternalDisplayNonInteractive`（16） | 15 上用字符串构造角色 |
| iOS 27 `sceneAccessory` | `#if !HENGE_NO_SCENE_ACCESSORY` 关掉 |

其余（`foregroundStyle`、`tint`、`background(_:in:)`、`animation(_:value:)`、
`confirmationDialog`、`Button(role:)`、`monospacedDigit`、`controlSize`、
`FormatStyle`、`TimelineView`…）在 iOS 15 上本来就有，保持上游原样。

### 3. JIT：iOS 15–16 只有一条路

iOS 26 之前没有 TXM，所以**签了 `dynamic-codesigning` 的应用可以自己拿可执行
内存**（`JITBootstrap.prewarm()` 里早就有这条 pre-iOS-26 分支：`MAP_JIT`，或
`henge_ios_jit_prewarm_self`）。因此这一版：

- 不编译 StikDebug / StikJIT 助手 / 端侧配对（它们要 iOS 17.4 / 26 / 27）；
- `ios/Henge/Legacy/JITLegacy.swift` 提供简版 JIT 流程：TrollStore 的
  “Enable JIT”、越狱的 “Allow JIT in Apps”、或带 `dynamic-codesigning` 的
  重签，三者任一即可；界面上只有“Turn On JIT”和当前状态；
- `ios/Henge/Henge.legacy.entitlements` 带 `get-task-allow`、
  `dynamic-codesigning`、`increased-memory-limit`、
  `extended-virtual-addressing`。**TrollStore 安装时会保留这些**
  （iOS 15–16 没有 PPL/TXM 那一层限制）。

### 4. 内存：2 GB 设备是真的不够

`ios/Henge/Hardware.swift`：

- `Device.lowMemoryDevice`（< 3.5 GB 物理内存）时，Mac 内存按 512 MB 步进，
  选项 `1.0 / 1.5 / 2.0 / 2.5 GB`，默认 **1.5 GB**；界面会明确说 macOS 想要
  4 GB 起。
- `JITBootstrap.jitBytes` 在这类设备上降到 **256 MB**（原来 1 GB 代码缓存，
  2 GB 机器上等于自杀式分配）；缓存填满时翻译器整体清空重来，会更慢。
- `Installer.installRAMFloorMB`：安装（跑 Apple 的 restore 系统）在 2 GB 机器
  上从 2 GB 降到 **1.5 GB**，很可能被 restore 系统拒绝，这属于预期。

### 5. CI：GitHub Actions 直接出 ipa

`.github/workflows/ios15-build.yml`（`macos-15` 镜像，Xcode 16.4）：

```
make ios-lib HENGE_NO_NET=1 IOS_MIN=15.0 build/henge-agent.gz
IOS_MIN=15.0 ./tools/build_ios_restore.sh
HENGE_OUT=$PWD/build/out/Henge-iOS15.ipa ./ios/scripts/package_ios15_ipa.sh
```

关键点：**这一版不链接 libslirp/glib**（`HENGE_NO_NET=1`，用
`host/net_null.c` 顶替），所以不需要 Husk 检出、不需要交叉编译 glib，构建链
只需要 Xcode + xcodegen + cmake。代价是 **guest 没有网络**，共享文件夹
（WebDAV on 10.0.2.4）也不可用；macOS 安装本身不受影响，因为安装走宿主侧
libcurl。

`ios/scripts/package_ios15_ipa.sh` 产出 **未用开发者证书签名、但已 ad-hoc 签名并
嵌入 entitlements** 的 ipa，由 TrollStore / SideStore / AltStore / Sideloadly 重签。

**ipa 只有几 MB 是正常的**：里面是模拟器、安装器（libirecovery/idevicerestore）
和 Metal 渲染器，没有 macOS。上游 Release 的 ipa 也只有这个量级；macOS 的
restore 镜像（约 17 GB）是安装时由应用从 Apple 下载的。

## 怎么装

1. 在仓库 Actions 里下载 artifact `Henge-iOS15-*` 里的 `Henge-iOS15.ipa`。
2. 安装（按你设备能用的）：
   - **TrollStore**（iOS 15.0–15.4.1 用 TrollStore 1.x；15.5–16.6.x 用
     TrollStore 2）：直接装 ipa，然后 TrollStore 里对 Henge 点 **Enable JIT**
     （或在应用内“Turn On JIT”让 TrollStore 接手）。
   - **越狱（Dopamine 等）**：打开 “Allow JIT in Apps”，装 ipa。
   - **SideStore/AltStore/Sideloadly**：这些会用自己的证书重签，能保留
     `get-task-allow`，但 `dynamic-codesigning` 通常保不住；没有它就只能靠
     调试器/`CS_DEBUGGED` 路径，iOS 15–16 上较难成功。
3. 打开 Henge → JIT 页 → **Turn On JIT**，状态变 On 才去装 Mac。
4. 安装 macOS 需要约 43 GB 空闲（17 GB 下载 + 26 GB Mac）；6s 建议先腾空间，
   或者从电脑上用 `tools/export_ios.sh` 导出一台 Mac 再放进 Files。

## Armv8.1+ 指令的模拟（这是这一版的重点）

macOS 的 arm64 代码默认按 `apple-m1` 编译（clang 的 Darwin 驱动对 aarch64 macOS 就返回
`apple-m1`，而 `apple-m1` 在 LLVM 里就是 `apple-a14` 的别名），所以 guest 里出现 v8.1–v8.5
的指令是常态，不是意外。按 LLVM 里 Apple 自己上游的 CPU 表：

| CPU | 基线 | FP16 | FP16FML | DotProd | LSE | PAuth/ComplxNum/JS | SHA3 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| apple-a7（`apple-a9` 是它的别名） | v8.0a | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| apple-a10 | v8.0a + CRC/RDM | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| apple-a11 | v8.2a | ✓ | ✗ | ✗ | ✓ | ✗ | ✗ |
| apple-a12 | v8.3a | ✓ | ✗ | ✗ | ✓ | ✓ | ✗ |
| apple-a13 | v8.4a | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| apple-a14 / `apple-m1` | v8.4a + v8.5 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

特性探针**只在 `hv_vm_create` 里、所有 vCPU 线程启动之前**跑一次：探针会用 `sigaction` +
`siglongjmp` 试着执行一条指令，如果放到翻译路径上惰性执行，几个 CPU 线程并发进入同一个
handler 就会跳进别人的栈——这正是两台机器（6s Plus / 8 Plus）一启动就崩在应用自身代码里的
原因。

`core/simd.c` 现在模拟（编码从 QEMU 的 decodetree 模式用 `tools/decode_table.py` 提取，
不是凭记忆写的）：

| 家族 | 指令 | 怎么算 |
| --- | --- | --- |
| CRC32 / CRC32C | CRC32B/H/W/X、CRC32CB/CH/CW/CX | 逐字节查表，**已对标准校验值验证**（`123456789` → `CBF43926` / `E3069283`） |
| DotProd | SDOT/UDOT（向量形式） | int8 乘积累加进 int32 lane |
| FP16FML | FMLAL/FMLSL/FMLAL2/FMLSL2（向量） | 用 `fmaf`，单次舍入，与硬件的 fused 语义一致 |
| RDM | SQRDMLAH/SQRDMLSH（v/s，16 位与 32 位 lane） | 饱和 + 舍入加倍乘加高半 |
| SHA3 | EOR3 / BCAX / RAX1 / XAR | 纯位运算 |
| JSCVT | FJCVTZS | 向零取整，越界给 INT32_MIN |

**差分测试**：`tests/test_simd.c` 在 Apple Silicon 的 CI runner（`macos-15` 现在就是 arm64）
上把每条指令先用宿主原生执行一遍，再用模拟器执行一遍，四个寄存器 64 字节逐字节比对；
随机输入 64 轮/指令，并检查"普通 FP32/FP64 指令不能被误判成这些家族"。
当前结果：**866 cases ok**。

调试过程里被测试抓出来的三个真实错误（也是为什么值得先跑测试再上机）：

1. Q=0 的 64 位形式，硬件会把目的寄存器**高 64 位清零**，原来的实现保留了原值；
2. FMLAL/FMLSL 是 **fused**，用 double 相加会双重舍入，差 1 ulp，改用 `fmaf`；
3. CRC32 的累加值来自 **Wn**、数据来自 **Wm**，最初写反了。

仍未模拟（遇到会写一行日志并给 guest 一条未定义指令，而不是静默算错）：

- 点积/FMLAL/RDM 的 **by-element** 形式；
- **SHA512**（SHA512H/H2/SU0/SU1）——语义没把握，宁可不做；
- **FCADD/FCMLA**（ComplxNum）——差分测试没能把 rotation 对上，降级为记录；
- **FP16 算术与 FCVT/BFCVT**（半精度加减乘除、转换）——最可能缺的一块，下一轮补；
- i8mm（SMMLA 等，A15+/M2+，本就不是 `apple-m1` 基线）。

日志里会看到这样的行，直接告诉我们要补哪条：

```
henge: simd: fp16 is not emulated yet (insn 4e421420 at pc 1f00abcd); the guest sees an undefined instruction
```

## 还差什么（A9 上大概会卡在哪）

按可能性从高到低：

1. **FP16 算术与 FCVT/BFCVT 还没模拟**：见上一节。点积、FMLAL、RDM、SHA3、
   CRC32、JSCVT 已经过差分测试；半精度算术是下一块要补的。
2. **内存**：1.5 GB 给 macOS + 256 MB 代码缓存 + 应用本体，在 2 GB 物理内存上
   基本贴着 jetsam 红线；即使进到桌面，开任何应用都可能被杀。
3. **速度**：A9 单核大约 A14 的三分之一到四分之一，加上原子指令全部走 helper，
   首次启动（Setup Assistant）可能需要几十分钟到几小时。
4. **restore 系统**：安装阶段 macOS 的 restore 环境对内存更敏感，1.5 GB 未验证。

要继续往前推，下一步应该是：给 `translate.c` 里 SIMD 那一类指令补一张
“宿主是否支持”的表（dotprod/fp16/fhm），不支持的新写 helper；以及给
`hv_mem_emulate` 之外再加一个“慢速解释器”兜底，遇到未模拟编码时不直接 SIGILL。

## 排错

- 装好后第一次启动失败，先看 `Documents/henge.log`（应用内 Settings → Share
  henge.log）。开头会有：
  - `host features: LSE=… LRCPC2=… CRC32=… RDM=… FP16=… FHM=… DotProd=… SHA3=… SHA512=… FCMA=… JSCVT=…` —— 进 Mac 之前一次性解析（在任何 vCPU 线程启动前，见下）；
  - `TXM expected: no` —— iOS 15–16 应为 no；
  - `JIT region made by Henge itself (…)` / `MAP_JIT executes here` —— JIT 是否拿到；
  - `henge: booting with N CPUs, M MB RAM, a K MB JIT region`；
  - `henge: built without libslirp: the guest has no network…` —— 预期。
- 崩溃在 `SIGILL`：就是上面第 1 条，说明撞到未模拟的 Armv8.1+ 指令；日志里的
  `stats` 行和 PC 值能定位到是哪个块。
- “Henge has no JIT region”：签名没带上 `dynamic-codesigning`，或 TrollStore
  的 Enable JIT 没点。

## 文件清单

```
core/hostcaps.[ch]                 宿主能力探测（sysctl + 指令探测）
core/translate.c                   原子/LDAPUR 走 C 路径
core/memop.c                       （未改）已经是完整的 C 模拟
Makefile                           CORE 增加 hostcaps.o；IOS_MIN / HENGE_NO_NET
host/net_null.c                    无 libslirp 时的网卡占位
tools/build_ios_restore.sh         IOS_MIN 可配（默认 17.0）
ios/project-ios15.yml              iOS 15 构建 spec
ios/Henge/Henge.legacy.entitlements
ios/Henge/Legacy/Compat.swift      SwiftUI 兼容层
ios/Henge/Legacy/JITLegacy.swift   简版 JIT 流程
ios/Henge/*.swift                  改用兼容层；内存按 MB 选择；A9 放行
ios/scripts/package_ios15_ipa.sh   出 ipa
.github/workflows/ios15-build.yml  CI
```

许可证不变：GPL-2.0-or-later。
