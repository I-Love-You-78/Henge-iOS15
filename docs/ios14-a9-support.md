# iOS 14 与 A9（iPhone 6s / SE）支持

这份分支在 [Henge 0.2](../README.md) 的基础上，把应用和引擎改到能在
**iOS 14 及以上**、**A9 及以上的 arm64 设备**（iPhone 6s、iPhone SE 第一代、
iPhone 7/8、iPad 5/6 等）上安装和运行，并且用 GitHub Actions 直接产出 `.ipa`
（不需要 Mac、不需要 Husk 检出）。

先说结论，再说为什么。

## 结论

| 项 | 状态 |
| --- | --- |
| 代码能在 iOS 14 SDK 下编译、产出 ipa | 已实现，CI 每次构建都验证 |
| A9 上不会再因为 Armv8.1 原子指令直接 SIGILL | 已实现（原子指令改走 C 模拟） |
| A9 上能否真正进到 macOS 桌面 | **未验证，预期很可能失败**，见“还差什么” |
| 2 GB 内存机型（6s/SE/7/8）跑 macOS | 只能给 1.5 GB，远低于 macOS 需要的 4 GB |
| 免越狱/免 TrollStore 的 JIT | 做不到：iOS 14–16 只能靠签名给 JIT |

也就是说：**这一版把“装不上、跑不了第一个块”的问题解决了，把“必然 SIGILL”
的问题解决了，但 A9 + 2 GB 仍然是踩在 macOS 的最低需求之下的**。
请把它当作实验构建。

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
  装 SIGILL 处理器 + `sigsetjmp`），以此给出确定答案；缓存结果。
- `core/translate.c`：宿主没有 `FEAT_LSE` 时，`HV_MEM_ATOMIC` / `HV_MEM_CAS`
  改走 `HV_OP_MEM` helper；没有 `FEAT_LRCPC2` 时，`LDAPUR/STLUR`（imm9
  ordered）同样改走 helper。
- 这条 helper 路径本来就有完整实现：`core/memop.c` 的 `do_atomic()` /
  `do_casp()` 用 C11 `__atomic_compare_exchange_n` 做 RMW，编译到 A9 上就是
  `ldaxr/stlxr` 循环，不需要 LSE。CASP 的 16 字节比较交换用 `__uint128_t`
  原子操作，A9 也支持（LL/SC 对）。
- 启动时日志会打一行：`henge: host features: LSE=no LRCPC2=no DotProd=… FP16=… (probed)`，
  在 `henge.log` 里能看到判定结果和是否用了探测。

代价：A14 以前的机器上所有原子操作从“一条指令”变成“一次 helper 调用 + C 模拟”，
`os_unfair_lock`、libdispatch 这类热点会明显变慢。A14+ 上逻辑不变（仍然内联）。

### 2. iOS 14 部署目标与 UI 兼容层

`ios/project-legacy.yml` 是第二份 xcodegen spec：`deploymentTarget: 14.0`、
编译条件 `HENGE_LEGACY HENGE_NO_SCENE_ACCESSORY`、不链接 StikJIT/rppairing、
不带 JIT helper appex、不带 `AppIcon.icon`（Icon Composer 需要新 Xcode）。

`ios/Henge/Legacy/Compat.swift` 把“iOS 14 之后才有”的 API 全部收口成带
`#available` 的薄封装，两份构建共用同一套源码：

| 用了什么（版本） | 兼容层 |
| --- | --- |
| `NavigationStack`（16） | `CompatNavigationStack`（14 走 `NavigationView`） |
| `navigationDestination(for:)`（16） | `compatDestination` + `CompatLink`（14 把目的地塞进 `NavigationLink`） |
| `LabeledContent`（16） | `LCRow` |
| `ShareLink`（16） | `ShareButton`（`UIActivityViewController`） |
| `confirmationDialog`（15） | `compatConfirm`（14 走 `Alert`） |
| `TimelineView`（15） | `SecondTicker`（`Timer.publish`） |
| `.task`（15） | `compatTask`（`onAppear` + `Task`） |
| `background(_:in:)`（15） | `shape.fill(color)` |
| `.foregroundStyle`（15） | `.foregroundColor` |
| `.tint`（15） | `.accentColor` |
| `Button(_:role:)`（15） | `DestructiveButton` |
| `.animation(_:value:)`（15） | `.animation(_:)` |
| `FormatStyle`（15） | `DateText`（`DateFormatter`） |
| `.scrollContentBackground` / `.persistentSystemOverlays`（16） | `compatHide…()` |
| iOS 27 `sceneAccessory` | `#if !HENGE_NO_SCENE_ACCESSORY` 关掉 |

`UISceneSession.Role.windowExternalDisplayNonInteractive`（16）在 iOS 14/15 用
字符串构造角色；`HengeLog` 里读 TXM 的 `#available` 分支保持原样。

### 3. JIT：iOS 14–16 只有一条路

iOS 26 之前的设备没有 TXM，所以**签了 `dynamic-codesigning` 的应用可以自己
拿可执行内存**（`JITBootstrap.prewarm()` 里早就有这条 pre-iOS-26 分支：
`MAP_JIT`，或 `henge_ios_jit_prewarm_self`）。因此这一版：

- 不编译 StikDebug / StikJIT 助手 / 端侧配对（它们要 iOS 17.4 / 26 / 27）；
- `ios/Henge/Legacy/JITLegacy.swift` 提供一套简版 JIT 流程：TrollStore 的
  “Enable JIT”、越狱的 “Allow JIT in Apps”、或带 `dynamic-codesigning` 的
  重签，三者任一即可；界面上只有“Turn On JIT”和当前状态；
- `ios/Henge/Henge.legacy.entitlements` 带 `get-task-allow`、`dynamic-codesigning`、
  `increased-memory-limit`、`extended-virtual-addressing`。**TrollStore 安装时
  会保留这些**（iOS 14–16 没有 PPL/TXM 那一层限制）。

### 4. 内存：2 GB 设备是真的不够

`ios/Henge/Hardware.swift`：

- `Device.lowMemoryDevice`（< 3.5 GB 物理内存）时，Mac 内存按 512 MB 步进，
  选项 `1.0 / 1.5 / 2.0 / 2.5 GB`，默认 **1.5 GB**；界面会明确说 macOS 想要
  4 GB 起。
- `JITBootstrap.jitBytes` 在这类设备上降到 **256 MB**（原来是 1 GB 代码缓存，
  2 GB 机器上等于自杀式分配）；缓存被填满时翻译器会整体清空重来，会更慢。
- `Installer.installRAMFloorMB`：安装（跑 Apple 的 restore 系统）在 2 GB 机器上
  从 2 GB 降到 **1.5 GB**，很可能被 restore 系统拒绝，这属于预期。

### 5. CI：GitHub Actions 直接出 ipa

`.github/workflows/ios14-build.yml`（`macos-15` 镜像，Xcode 16.x）：

```
make ios-lib HENGE_NO_NET=1 IOS_MIN=14.0 build/henge-agent.gz
IOS_MIN=14.0 ./tools/build_ios_restore.sh
HENGE_OUT=$PWD/build/out/Henge-iOS14.ipa ./ios/scripts/package_legacy_ipa.sh
```

关键点：**这一版不链接 libslirp/glib**（`HENGE_NO_NET=1`，用
`host/net_null.c` 顶替），所以不需要 Husk 检出、不需要交叉编译 glib，
构建链只需要 Xcode + xcodegen + cmake。代价是 **guest 没有网络**，共享文件夹
（WebDAV on 10.0.2.4）也不可用；macOS 安装本身不受影响，因为安装走的是宿主侧
libcurl。

`ios/scripts/package_legacy_ipa.sh` 产出 **未用开发者证书签名、但已 ad-hoc 签名并
嵌入 entitlements** 的 ipa，由 TrollStore / SideStore / AltStore / Sideloadly 重签。

## 怎么装

1. 在仓库 Actions 里下载 artifact `Henge-iOS14-*` 里的 `Henge-iOS14.ipa`。
2. 安装（按你设备能用的）：
   - **TrollStore**（iOS 14.0–15.x，或 16.x 的 TrollStore Lite）：直接装 ipa，
     然后 TrollStore 里对 Henge 点 **Enable JIT**（或在应用内“Turn On JIT”让
     TrollStore 接手）。
   - **越狱（Dopamine 等）**：打开 “Allow JIT in Apps”，装 ipa。
   - **SideStore/AltStore/Sideloadly**：这些会用自己的证书重签，能保留
     `get-task-allow`，但 `dynamic-codesigning` 通常保不住；没有它就只能靠
     调试器/`CS_DEBUGGED` 路径，iOS 14–16 上较难成功。
3. 打开 Henge → JIT 页 → **Turn On JIT**，状态变 On 才去装 Mac。
4. 安装 macOS 需要约 43 GB 空闲（17 GB 下载 + 26 GB Mac），6s 建议先腾空间，
   或者从电脑上用 `tools/export_ios.sh` 导出一台 Mac 再放进 Files。

## 还差什么（A9 上大概会卡在哪）

按可能性从高到低：

1. **Armv8.2+ 的 SIMD 指令没有模拟**：`SDOT/UDOT`（点积）、`FMLA`/`FCVT` 的
   FP16 形式、`BFCVT`、`FMLAL` 等。macOS 的 vImage/Accelerate/CoreGraphics
   在热点里有这些指令，A9 执行同样 `SIGILL`。这一版**没有**处理它们（翻译器把
   SIMD 指令整类原样复制），要做得给每个编码写 C 模拟，工作量远大于原子指令。
2. **内存**：1.5 GB 给 macOS + 256 MB 代码缓存 + 应用本体，在 2 GB 物理内存上
   基本贴着 jetsam 的红线；即使进到桌面，开任何应用都可能被杀。
3. **速度**：A9 单核大约 A14 的三分之一到四分之一，加上原子指令全部走 helper，
   首次启动（Setup Assistant）可能需要几十分钟到几小时。
4. **restore 系统**：安装阶段 macOS 的 restore 环境对内存更敏感，1.5 GB 未验证。

要继续往前推，下一步应该是：把 `translate.c` 里 SIMD 那一类指令补一张
“宿主是否支持”的表（dotprod/fp16/fhm），不支持的走新写的 helper；以及给
`hv_mem_emulate` 之外再做一个“慢速解释器”兜底，遇到未模拟编码时不直接 SIGILL。

## 排错

- 装好后第一次启动失败，先看 `Documents/henge.log`（应用内 Settings → Share
  henge.log）。开头会有：
  - `host features: LSE=… LRCPC2=…` —— 是否走了模拟路径；
  - `TXM expected: no` —— iOS 14–16 应为 no；
  - `JIT region made by Henge itself (…)` / `MAP_JIT executes here` —— JIT 是否拿到；
  - `henge: booting with N CPUs, M MB RAM, a K MB JIT region`；
  - `henge: built without libslirp: the guest has no network…` —— 预期。
- 崩溃在 `SIGILL`：就是上面第 1 条，说明撞到未模拟的 Armv8.1+ 指令；日志里
  的 `stats` 行和 PC 值能定位到是哪个块。
- “Henge has no JIT region”：签名没带上 `dynamic-codesigning`，或 TrollStore
  的 Enable JIT 没点。

## 工程量与文件清单

```
core/hostcaps.[ch]                 宿主能力探测（sysctl + 指令探测）
core/translate.c                   原子/LDAPUR 走 C 路径
core/memop.c                       （未改）已经是完整的 C 模拟
core/machine.h, Makefile           CORE 增加 hostcaps.o；IOS_MIN / HENGE_NO_NET
host/net_null.c                    无 libslirp 时的网卡占位
tools/build_ios_restore.sh         IOS_MIN 可配（默认 17.0）
ios/project-legacy.yml             iOS 14 构建 spec
ios/Henge/Henge.legacy.entitlements
ios/Henge/Legacy/Compat.swift      SwiftUI 兼容层
ios/Henge/Legacy/JITLegacy.swift   简版 JIT 流程
ios/Henge/*.swift                  改用兼容层；内存按 MB 选择；A9 放行
ios/scripts/package_legacy_ipa.sh  出 ipa
.github/workflows/ios14-build.yml  CI
```

许可证不变：GPL-2.0-or-later。
