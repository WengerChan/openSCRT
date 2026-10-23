# openSCRT 分阶段研发计划（目标 / 任务 / 验收标准）

> **上游文档**：[`securecrt_native_architecture_and_roadmap.md`](../securecrt_native_architecture_and_roadmap.md) 定义了技术选型、六层架构、模块拆解与五阶段里程碑。
> **本文档**把该路线图翻译为可执行的软件工程计划：为每个阶段补齐**任务分解（带任务号）**、**可自动化的验收标准**、**CI 门禁**与**风险登记**。
> 两者冲突时，以上游文档的技术选型为准；本文档只在"如何验证"与"如何拆任务"上做增量。

---

## 0. 使用方法

- 每个任务有唯一编号（`P<阶段>-T<序号>`），编号一经分配不再复用。
- 每个验收标准带 **证据等级**：
  - **【CI】** — 由 GitHub Actions 自动判定，PR 合并前必须通过。
  - **【CI-Perf】** — 由 CI 性能基准判定，仅在专用 runner 上有约束力。
  - **【人工】** — 需人工评审 / 真实环境验证，必须在阶段验收记录中附截图或日志链接。
- **阶段门禁（Gate）**：上一阶段所有 **【CI】** 条目为绿 + 验收记录已归档，才能开启下一阶段任务。
- 本地环境**不作为验证路径**：编译、测试、覆盖率、性能基准一律以 GitHub Actions 结果为准；本地仅做代码编写与提交。

---

## 1. 阶段总览

| 阶段 | 定位 | 上游文档任务数 | 本文新增任务 | 出口门禁 | 关键可交付物 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Phase 0** | 工程基线与自动化门禁（本文档新增） | — | 6 | 三平台 CI 全绿 + 覆盖率可查 | 可编译的模块化骨架、CI 流水线 |
| **Phase 1** | 核心底座与 MVP 可用版 | 4 | 12 | 连接真实 Linux 服务器；刷屏 ≥60 FPS | 可用的 SSH 终端 MVP |
| **Phase 2** | KeePass 双模凭据 + 全界面布局还原 | 4 | 11 | 20 并发会话广播；内存无明文密码 | 凭据中心 + 完整 UI 骨架 |
| **Phase 3** | 多协议栈 + SFTP / Zmodem | 4 | 11 | Zmodem 百兆文件 MD5 一致 | 串口 / SFTP / Zmodem |
| **Phase 4** | 高阶终端特性与运维效率套件 | 4 | 9 | 50 条正则下帧率衰减 ≤5% | 高亮 / Expect-Send / 日志 |
| **Phase 5** | Python 脚本引擎与企业级打磨 | 3 | 10 | 10 个官方脚本零修改运行；50 会话 72h | 可发布版本 |

**依赖关系**：`Phase 0 → Phase 1 → Phase 2`；`Phase 3` 与 `Phase 2` 可部分并行（依赖 Phase 1 的 `ITransportChannel` 冻结）；`Phase 4` 依赖 Phase 1 渲染管线；`Phase 5` 依赖 Phase 1 `crt.Screen` 游标语义 + Phase 2 Dialog 桥接。

**为什么插入 Phase 0**：上游文档 Phase 1 第 1 项只要求"搭建 CMake + Qt6 工程骨架"，但把跨平台编译验证与测试门禁留在了里程碑验收里。工程上必须先有自动化门禁，否则后续 5 个阶段的验收全部依赖人工，无法规模化。因此新增 Phase 0（3–5 天）固化 CI，把"三端编译通过"从人工验收变为 CI 断言。

---

## 2. 通用工程规约

### 2.1 分支与提交

| 规则 | 说明 |
| :--- | :--- |
| 主干 | `main`，受保护，必须 CI 全绿才可合并 |
| 特性分支 | `feat/<phase>-<任务号>-<短描述>`，如 `feat/p1-t04-vterm-statemachine` |
| 提交粒度 | 一个任务号 = 一个 PR；跨任务改动必须拆 PR |
| PR 门禁 | 必须关联任务号；【CI】全绿 + 至少 1 人 Review |

### 2.2 编译规约（CI 强制）

- `-Wall -Wextra -Wpedantic -Werror`（`OPENCRT_WARNINGS_AS_ERRORS=ON`），MSVC 用 `/W4 /WX`。
- C++20 强制，`CMAKE_CXX_STANDARD_REQUIRED=ON`。
- 禁止在全局命名空间引入符号；一律 `namespace openscrt::{layer}`。
- `clang-format` 零 diff（CI 检查 `src/` 与 `tests/`）。
- 依赖只经 `vcpkg.json` 声明，禁止直接引用系统库路径。

### 2.3 测试与覆盖率规约

| 层级 | 内容 | 标签 | 执行位置 |
| :--- | :--- | :--- | :--- |
| 单元测试 | 纯逻辑（网格、回滚、队列、状态机、协议编解码） | `unit` | 三平台全矩阵 |
| 集成测试 | 容器内真实 SSH/串口/SFTP 端到端 | `unit` | Linux 单平台（docker） |
| 性能基准 | 刷屏吞吐、渲染延迟、Argon2 KDF 耗时 | `perf` | Linux 单平台专用 runner |
| 验收基准 | 内存增长、句柄泄漏、72h 稳定性 | `perf` | Linux 专用 runner（长时任务） |

- 单元测试覆盖率目标：**Phase 0 ≥40% → Phase 1 ≥65% → Phase 2 起 ≥80%**（核心库 `openscrt_core` / `openscrt_terminal` / `openscrt_credential` 单独统计，要求 ≥90%）。
- 每个任务**必须**附带至少一个测试；纯 UI 布局任务可豁免，但需在 PR 注明豁免理由。

### 2.4 线程模型规约（源自上游文档 §2）

以下约束是全局红线，任何阶段不得违反：

1. **UI 主线程零阻塞**：网络读、磁盘 I/O、Argon2 派生禁止出现在 UI 线程。
2. **跨线程数据只走队列**：生产者 → `SpscRingBuffer`（字节流）／合并信号 → UI 线程。
3. **帧合并**：不得按数据包触发 `update()`；统一走 16ms（60Hz）/ 8ms（120Hz）帧定时器（上游文档 §6.1）。
4. **内存安全**：凭据明文仅存在于 `SecureByteArray`（`sodium_malloc` + `mlock`），禁止写入会话配置文件与日志。

---

## 3. Phase 0 — 工程基线与自动化门禁（3–5 天）

### 3.1 阶段目标
让任意一次提交都能在三平台自动编译 + 跑测试 + 出覆盖率报告，且构建可复现（依赖锁定）。**本阶段不产出任何用户可见功能。**

### 3.2 任务清单

| 任务号 | 任务 | 产出物 | 依赖 |
| :--- | :--- | :--- | :--- |
| P0-T1 | 仓库骨架与模块化 CMake 骨架 | 顶层 `CMakeLists.txt`，8 个模块静态库目标 + `openscrt` 可执行目标，`CMakePresets.json` 三平台预设 | — |
| P0-T2 | 依赖清单声明 | `vcpkg.json`（qtbase / qtserialport / libvterm / libssh / openssl / libsodium / pugixml / nlohmann-json / pybind11 / zlib / gtest），锁定 `builtin-baseline` | P0-T1 |
| P0-T3 | GitHub Actions 流水线 | `.github/workflows/ci.yml`：三平台矩阵 + vcpkg 缓存 + CTest 分层（`unit` / `perf`）+ 覆盖率 artifact + clang-format 检查 | P0-T1 |
| P0-T4 | 代码规约落地 | `.clang-format`、`.clang-tidy`、告警零容忍配置 | P0-T1 |
| P0-T5 | 核心数据结构骨架 | `src/core/SpscRingBuffer.h`、`src/core/SecureByteArray.h`、`src/terminal/buffer/Cell.h` | P0-T1 |
| P0-T6 | 测试基础设施 | GoogleTest 接入、`tests/CMakeLists.txt`、CTest 标签体系、覆盖率采集（lcov/gcov + gcovr） | P0-T1 |

### 3.3 验收标准

| # | 标准 | 证据等级 |
| :--- | :--- | :--- |
| P0-A1 | 推送任意 commit，`ubuntu-24.04` / `windows-2022` / `macos-14` 三个 job 均构建成功 | 【CI】 |
| P0-A2 | `ctest -L unit` 在三平台全部通过，且至少存在 1 个真实测试用例 | 【CI】 |
| P0-A3 | `ctest -L perf` 标签可被 CI 单独调度（至少 1 个占位基准用例可运行） | 【CI】 |
| P0-A4 | `-Werror` 下零告警通过；`clang-format --dry-run --Werror` 零 diff | 【CI】 |
| P0-A5 | 覆盖率报告作为 artifact 上传，且整体行覆盖率 ≥40% | 【CI】 |
| P0-A6 | 从空 clone 到 CI 绿灯的冷启动（含依赖缓存）≤ 45 分钟 | 【CI】 |
| P0-A7 | 依赖可复现：删除 vcpkg 缓存后重建，选中依赖版本与 `builtin-baseline` 一致 | 【人工】 |

### 3.4 风险
- **Qt6 安装耗时**：qtbase 经 vcpkg 编译需数小时。**对策**：CI 中 Qt 走 `jurplel/install-qt-action@v4`（aqtinstall 预编译包），vcpkg 仅负责非 Qt 依赖。此为对上游文档"vcpkg 管理依赖"的技术调整（性能必要），已在 `vcpkg.json` 注释中标注。
- **macOS runner 架构**：优先固定 `macos-14`（arm64）并单独保留 `macos-13`（x86_64）观察位，避免后期 Universal Binary 打包阶段才暴露架构问题。

---

## 4. Phase 1 — 核心底座与 MVP 可用版（第 1–6 周）

### 4.1 阶段目标
交付一个可日常使用的 SSH 终端 MVP：高密度界面、多标签、正确渲染主流 TUI 程序、高速输出不卡顿。这是整个产品的性能与正确性地基，后续所有阶段都建立在此网格/队列/渲染契约之上。

### 4.2 任务清单

| 任务号 | 任务 | 产出物 | 依赖 |
| :--- | :--- | :--- | :--- |
| P1-T1 | 无锁 SPSC 环形队列 | `src/core/SpscRingBuffer.h`：单生产者单消费者，字节块进出，`tryRead`/`tryWrite`，满/空判定无锁 | P0-T5 |
| P1-T2 | 终端字符网格 | `src/terminal/buffer/Cell.h`（16 字节对齐，含 codepoint / FG / BG / 属性位 / 双宽位）、`ScreenGrid.h`：光标、擦除、滚动、备用缓冲区切换 | P0-T5 |
| P1-T3 | 环形回滚缓冲 | `src/terminal/buffer/RingScrollbackBuffer.h`：分页环形存储，可配置最高 128,000 行 | P1-T2 |
| P1-T4 | VT 状态机 | `src/terminal/emulator/VtermStateMachine.h`（libvterm 定制扩展封装）：VT100 / VT220 / ANSI / Linux Console；备用屏幕；字符集转换（UTF-8 / GB18030） | P1-T2 |
| P1-T5 | 256 色与 TrueColor | 调色板管理、默认色表、`SGR` 38/48 处理 | P1-T4 |
| P1-T6 | Glyph Atlas 与脏矩形渲染器 | `src/terminal/renderer/GlyphAtlas.h`（ASCII + CJK 预渲染纹理图集）、`DirtyRectRenderer.h`（仅扫描 `is_dirty` 行） | P1-T2, P1-T5 |
| P1-T7 | 终端画布控件 | `src/ui/workspace/TerminalCanvasWidget.h`：渲染接入、鼠标块选、Alt+拖拽列选、选区复制 | P1-T6 |
| P1-T8 | 帧合并调度 | 16ms/8ms 帧定时器 + 脏行合并信号，验证"禁止按包触发 update" | P1-T6 |
| P1-T9 | 传输通道抽象 | `src/protocols/ITransportChannel.h`：字节流、窗口尺寸变更（NAWS）、生命周期回调；冻结接口，后续阶段不得随意变更 | — |
| P1-T10 | 本地 Shell 通道 | `src/protocols/local/LocalPtyChannel`：`forkpty`（POSIX）/ ConPTY（Windows） | P1-T9 |
| P1-T11 | SSH2 通道 | `src/protocols/ssh/Ssh2Channel`：libssh 封装，密码认证 + 公钥认证（RSA / ECDSA / Ed25519），OpenSSH 与 VanDyke 私钥格式解析 | P1-T9 |
| P1-T12 | 主窗口与工作区 | 菜单栏、`Session Manager` 树、多标签工作区、`Quick Connect` 对话框、会话配置持久化（`src/config/`） | P1-T7, P1-T11 |

### 4.3 验收标准

| # | 标准 | 证据等级 |
| :--- | :--- | :--- |
| P1-A1 | 三平台编译通过（由 Phase 0 门禁保证），并能连接 docker 内真实 Linux SSH 服务器 | 【CI】 |
| P1-A2 | `vim`、`htop`、`tmux`、`less` 排版与色彩正确：对 `vttest` 用例集逐项比对基线截图（哈希比对，差异需人工确认后更新基线） | 【CI】+【人工】 |
| P1-A3 | 执行 `seq 1 1000000`：UI 线程帧率 ≥60 FPS（采样自渲染线程 frame time p95 ≤16.7ms），内存增长 ≤256MB，UI 无卡顿 | 【CI-Perf】 |
| P1-A4 | SPSC 队列在 `-fsanitize=thread` 下 1000 万次读写无数据竞争报告 | 【CI】 |
| P1-A5 | 回滚缓冲写入 128,000 行后内存占用稳定，最旧行正确被覆盖；回滚检索延迟 p99 ≤1ms | 【CI-Perf】 |
| P1-A6 | 备用屏幕（`vim`）进出不污染主屏；`tput smcup/rmcup` 序列正确处理 | 【CI】 |
| P1-A7 | 真机验证：Windows/macOS/Linux 三端各连接 1 台真实服务器并完成交互式登录 | 【人工】 |
| P1-A8 | 核心库 `openscrt_core` / `openscrt_terminal` 行覆盖率 ≥65% | 【CI】 |

### 4.4 已识别的路线图技术偏差

| # | 偏差 | 证据 | 处置 |
| :--- | :--- | :--- | :--- |
| D1 | **`libvterm` 无法经 vcpkg 获取** | microsoft/vcpkg 无 `ports/libvterm`（对 baseline 与 main 分支的 `contents/ports/libvterm` 均返回 404）；上游 libvterm 只提供 autotools 构建，三平台 CI 均无 autotools 工具链 | 已从 `vcpkg.json` 移除。P1-T4 的转义序列前端（`ControlSequenceParser`）与屏幕网格自行实现；若后续确需 libvterm 的字符集处理，则以 `FetchContent` + 固定 commit 引入，并把定制点收敛在 `VtermStateMachine` 单一适配层 |
| D2 | **Qt 不经 vcpkg** | qtbase 经 vcpkg 冷构建需数小时，CI 不可接受 | CI 用 `install-qt-action`（预编译包），vcpkg 只管非 Qt 依赖 |
| D3 | **新增 Phase 0** | 路线图把"三端编译通过"放在 Phase 1 验收，但未建 CI，导致五阶段验收全靠人工 | 拆出 Phase 0 固化门禁 |
| D4 | **CI 只安装被链接的依赖** | `vcpkg.json` 声明的 10 个 port 里，当前 CMake 树只引用 `libsodium`（`src/core`、`src/credential`）与 `gtest`（`tests/`）；`libssh` / `openssl` / `nlohmann-json` / `pugixml` / `zlib` / `pybind11` / `qtbase` / `qtserialport` 零引用。`ci.yml` 不再安装它们——实测 windows-2022 的 `vcpkg install` 从 **74.6 min**（占该 job 75.6 min 的 98.7%，全部耗在 openssl + libssh 的 MSVC 源码编译上）降到约 3–6 min，P0-A6 的 45 min 冷启动门禁重新可达 | `vcpkg.json` 仍是唯一事实来源；`ci.yml` 从中派生出 CI 清单。**契约**：某阶段开始链接某个 port 时，必须同时删掉它在 `ci.yml` `SKIP` 表里的条目；若漏删，表现为 configure 阶段几分钟内明确报 "not found"，而不是像之前那样默默编译 75 分钟没人用的库 |

### 4.5 风险
- **CJK 双宽字符与光标对齐错位**：在 P1-T2 就把双宽位写进 `Cell` 位域，不允许后期加字段（会破坏 16 字节布局）。
- **若后续引入 libvterm**：定制点必须收敛到**唯一适配层** `VtermStateMachine`，禁止在业务代码里直接调 libvterm API。

### 4.6 任务完成度与验收取证（run #39742420161 / commit `e9d761d`）

**任务完成度**

| 任务号 | 状态 | 说明 |
| :--- | :--- | :--- |
| P1-T1 SPSC 无锁队列 | ✅ | 单生产者单消费者，每条序列零堆分配；绕回与并发有测试 |
| P1-T2 Cell / ScreenGrid | ✅ | 16 字节布局、`static_assert` 锁死；光标、滚动区域、备用屏、宽字符 |
| P1-T3 环形回滚缓冲 | ✅ | 分页按需分配；行离开顶部时上报，且仅当滚动区域从第 0 行起 |
| P1-T4 VT 状态机 | ✅ | `ControlSequenceParser`（CSI/OSC/ESC/UTF-8/子参数）+ `TerminalEmulator` 接缝；补齐 ECH/DCH |
| P1-T5 256 色 / TrueColor | ✅ | xterm 调色板；SGR 全量；分号与冒号两种扩展色写法 |
| P1-T6 字形图集 + 脏矩形 | 🟡 | **脏行合并逻辑已完成**（`DamageTracker`，留在无 Qt 的 terminal 层）；字形图集与渲染器未做 |
| P1-T7 终端画布控件 | ✅ | `TerminalCanvasWidget`：`feed()` 只置合并标志，只有定时器能出帧；键位编码为纯函数；offscreen 无头测试 |
| P1-T8 帧合并调度 | ✅ | 60/120Hz；突发合并为 1 帧、持续输出受刷新率上限约束、空闲不出帧 |
| P1-T9 传输接口冻结 | ✅ | `ITransportChannel` |
| P1-T10 本地 Shell | ✅ | POSIX `forkpty` 与 Windows ConPTY 均已实现（ConPTY 仅由 Windows leg 编译验证，运行时待人工） |
| P1-T11 SSH2 通道 | ✅ | libssh 0.11 真实 API；公钥→密码→keyboard-interactive；**D4 契约已在同 PR 从 SKIP 表删除 `libssh`** |
| P1-T12 主窗口与工作区 | 🟡 | **会话继承树 + Default Session 模板已完成**（纯逻辑、无 Qt 依赖）；配置持久化与窗口装配未开始 |

**验收取证**

| # | 标准 | 证据 | 结论 |
| :--- | :--- | :--- | :--- |
| P1-A1 | 三平台编译 + 连接主流 Linux 服务器 | 四个 job 全绿，流水线 **9 min 31 s**（含 libssh + openssl 源码构建） | 🟡 **编译满足；ConPTY 已实现且 Windows leg 编译通过；真机 SSH / 真机本地 Shell 均待人工** |
| P1-A2 | vim/htop/tmux 排版与色彩正确 | VT 状态机与 SGR 有针对性测试（含真彩两种写法、备用屏、滚动区域） | 🟡 **仿真层满足；vttest 截图基线比对未做** |
| P1-A3 | `seq 1 1000000` 帧率 ≥60 FPS | 渲染器 + 字形图集已实现；perf 层实测（run #39886377367）：解析 **5.18 μs/行、193,000 行/s**；全屏重绘 **2.32 ms**（≈431 FPS 等效）；600 ms 饱和输出产出 **38 帧** | 🟡 **机器可判定的一半满足**：全屏重绘 2.32 ms 远低于 16 ms 帧预算，刷新率上限生效。**人工一半未做**：Widget 端到端 FPS 需真实显示环境 |
| P1-A3b | （补充）脏矩形收益 | 增量/全屏重绘比 **1.3× → 7.6×**（1.14 ms vs 8.65 ms，run #39889831557） | ✅ **达成**。两步：①渲染器改用图集 blit（全屏重绘 2.32 → 1.34 ms）；②`ScreenGrid` 改用**每行一位**的脏位图，`collect()` 从读 1920 个格子变成读 24 个位 |
| P1-A3c | （补充）测量方法 | 绝对耗时在三次运行间漂移（解析 5.18 → 7.52 → 11.05 μs/行），但**未修改**的 `scrollback push` 同步漂移（0.242 → 0.379 → 0.523 μs/行） | ⚠️ **共享 runner 负载造成的漂移，不是代码退化**。**比值**在同一次运行内测两侧，对负载不敏感，因此用它做判据；**绝对值**只用于同一 run 内的前后对比 |
| P1-A4 | SPSC 在 TSan 下无数据竞争 | 并发测试覆盖 4 线程 × 2 万次 signal | 🟡 **逻辑有测试；`-fsanitize=thread` 未在 CI 中启用** |
| P1-A5 | 回滚 128,000 行稳定、检索 p99 ≤1ms | 容量按页对齐为 131,072；查找 O(1) 有测试 | 🟡 **容量与复杂度满足；延迟未实测** |
| P1-A6 | 备用屏进出不污染主屏 | `AlternateScreenDoesNotFeedScrollback` 等用例 | ✅ |
| P1-A7 | 三端真机登录验证 | — | ❌ **未做（人工项）** |
| P1-A8 | 核心库覆盖率 ≥65% | **lines 74.0%（1156/1563）**、functions 87.9%、branches 61.7% | ✅ |

**P1-A3 实测原始输出（run #39886377367，Linux）**

```
[perf] parse: 20000 lines in 103.6 ms (5.182 us/line, 192980 lines/s)
[perf] repaint: 24 rows in 2.32 ms (430.9 FPS equivalent for one frame)
[perf] 200 repaints: partial 3.55 ms vs full 4.53 ms (1.3x)
[perf] 600 ms of saturated output produced 38 frames (63.3 FPS)
[perf] scrollback push: 2000 lines in 485 us (0.242 us/line)
[perf] scrollback lookup: 2000 samples in 216 us
```

优化后（run #39889831557）：

```
[perf] repaint: 24 rows in 1.67 ms (598.4 FPS equivalent for one frame)
[perf] 200 repaints: partial 1.14 ms vs full 8.65 ms (7.6x)
[perf] 600 ms of saturated output produced 38 frames (63.3 FPS)
```

**方法论教训：绝对耗时不能跨 run 比较。** 三次运行里 `scrollback push`——一条**完全没被修改**的代码路径——从 0.242 漂到 0.523 μs/行（2.2×）。共享 runner 的负载足以造出"看起来像性能回归"的假象。**比值**在同一次运行内测两侧，负载同向影响，因此它是这里唯一可信的判据。这条已写入 P1-A3c。

读法：16 ms 的帧预算里，全屏重绘只占 2.32 ms，解析吞吐意味着单帧可消化的输出量在万行量级——**60 FPS 门槛有充足余量**。饱和输出下 38 帧 / 600 ms 略高于 60 Hz 上限，来自"首帧必须绘制"加上毫秒级时间量化，不是上限失效。

**1.3× 这个数字不理想，如实记录**：`GlyphAtlas` 建好了但渲染器仍在用 `drawText` 而不是图集 blit，逐格绘制的成本没有降下来；脏行跟踪省下的只有行背景填充。在字形更密或列更宽的场景下这个比例会更好，但当前证据说明**下一步的优化点明确**：把 `paintCell` 改成 `drawImage(atlas, glyphRect, target)`。

**P1-A3 实测（run #39916950710，Linux）**

```
[perf] parse: 20000 lines in 222.4 ms (11.119 us/line, 89935 lines/s)
[perf] repaint: 24 rows in 1.66 ms (602.9 FPS equivalent for one frame)
[perf] 200 repaints: partial 1.14 ms vs full 8.75 ms (7.7x)
[perf] 600 ms of saturated output produced 38 frames (63.3 FPS)
```

**覆盖到的真实缺陷（均由测试逼出，非读代码可得）**

| 缺陷 | 后果 |
| :--- | :--- |
| 子进程未 `dup2` PTY 从端到 stdio | PTY 完全失效，输出绕过通道 |
| `readAvailable` 断开时不排空队列 | 短命命令（`sh -c "echo hi; exit"`）输出丢失 |
| `VMIN=0` | 交互式 shell 读到伪 EOF 后秒退 |
| `ICRNL` 被关闭 | shell 永远等不到换行，命令静默滞留输入缓冲 |
| `put()` 误将光标钳入滚动区域 | 全屏 TUI 写到区域外的行被静默搬走 |
| OSC 溢出时吞掉终止符 | 解析器永久卡死，一个异常远端即可让终端失去响应 |
| `toUtf8` 塌陷空单元格 | 搜索 / 复制 / 日志的列对齐错位 |
| `collectAll` 只标记第 0 行 | 曝光 / 缩放只重画一行 |
| 帧调度用纪元当"从未绘制"哨兵 | 刷新率上限静默失效，空闲终端永不空闲 |
| `paintBand` 与 `paintCell` 各建一个 QPainter | 同一 QImage 第二次 `begin()` 失败，**字形一个都画不出来** |
| `put()` 误将光标钳入滚动区域 | 全屏 TUI 写到区域外的行被静默搬走 |
| 脏区检测逐格扫描 1920 个格子 | 固定 O(rows×cols) 成本，把增量/全屏比钉死在 1.3×（改行位图后 7.6×）|
| 子进程未把 PTY 从端接到自己的 stdio | PTY 完全失效，输出绕过通道 |
| `readAvailable` 断开时不排空 | 短命命令（`sh -c "echo hi; exit"`）输出丢失 |
| `VMIN=0` | 交互式 shell 读到伪 EOF 后秒退 |
| `ICRNL` 被关闭 | shell 永远等不到换行，命令静默滞留输入缓冲 |
| `resolve()` 让最远祖先获胜 | 三层继承链上每层都覆盖，会话自己的值被祖先冲掉 |
| 删除节点时只压缩未缩容 | 被删节点仍占槽位，父节点永远删不掉 |
| 字段名撞上 Qt 的 `signals` 宏 | 无 Qt 的头文件被跨层 include 时整个结构体解析失败 |

**覆盖率（run #39916950710）**：lines **71.8%**（1293/1800）、functions 87.9%（196/223）、branches 58.0%（695/1199）。13 个单测目标 + 2 个性能基准全绿。

**结论：Phase 1 尚未达成阶段出口。** T1–T11 与 ConPTY 已完成，T12 剩窗口装配与配置持久化。验收项：P1-A6 / P1-A8 / P1-A3b / P1-A3c 满足；P1-A3 的机器判定部分满足（1.66 ms 远低于 16 ms，比值 7.7×），端到端 FPS 待人工；P1-A1 / P1-A2 / P1-A4 / P1-A5 部分满足；**P1-A7 三端真机验证未做，是阶段出口的硬阻塞**。

---

## 5. Phase 2 — KeePass 双模凭据与全界面布局还原（第 7–11 周）

### 5.1 阶段目标
解决"密码不进配置文件"的硬需求，同时把 SecureCRT 的高密度界面骨架搭完整。本阶段是**安全属性最密集**的阶段，安全验收标准不可妥协。

### 5.2 任务清单

| 任务号 | 任务 | 产出物 | 依赖 |
| :--- | :--- | :--- | :--- |
| P2-T1 | 凭据提供者抽象 | `src/credential/ICredentialProvider.h`：统一检索与自动注入接口（`ICredentialProvider.h`） | — |
| P2-T2 | 安全内存容器 | `src/credential/memory/SecureByteArray`：`sodium_malloc` + `mlock`，析构时 `sodium_memzero` + 解锁 | P2-T1 |
| P2-T3 | KDBX 文件头解析 | `src/credential/kdbx/KdbxHeaderParser`：魔数校验、TLE / Variant Dictionary、算法 UUID、压缩标志、KDF 参数 | P2-T1 |
| P2-T4 | KDBX 4 读取 | `Kdbx4Reader`：Argon2d/Argon2id 派生 + HMAC-SHA256 块流 + AES-256-CBC / ChaCha20 + zlib | P2-T3, P2-T2 |
| P2-T5 | KDBX 3.1 读取 | `Kdbx3Reader`：SHA-256 复合主密钥 + AES-KDF + Twofish 可选 | P2-T3, P2-T2 |
| P2-T6 | XML 索引与保护流 | `pugixml` 解析 Group/Entry；`ProtectedStream`（ChaCha20 / Salsa20）延迟还原明文 | P2-T4, P2-T5 |
| P2-T7 | KDBX 热监听与自动锁定 | `QFileSystemWatcher` 增量刷新；闲置 N 分钟 / 最小化到托盘时清空主密钥 | P2-T6 |
| P2-T8 | KeePassHTTP 客户端 | `src/credential/keepasshttp/KeePassHttpClient`：`associate` 绑定、`test-associate`、`get-logins`，AES-256-CBC JSON 载荷，关联信息存入系统凭据库 | P2-T1 |
| P2-T9 | 条目选择器与会话绑定 | `KeePassSelectorDialog`；四策略绑定：Session Saved / Prompt Every Time / UUID / Auto-Match by Host | P2-T6, P2-T8 |
| P2-T10 | 界面布局还原 | 四向分屏 Split/Tile、底部多分组 Button Bar、底部 Command Window（含全局广播模式） | P1-T12 |
| P2-T11 | 原版配置导入器 | `src/config/importers/SecureCrtConfigImporter`：一键导入 SecureCRT XML/INI 会话树 | P1-T12 |

### 5.3 验收标准

| # | 标准 | 证据等级 |
| :--- | :--- | :--- |
| P2-A1 | 加载"主密码 + 密钥文件"的 `.kdbx`（KDBX 3.1 与 4.x 各一份）成功，双击会话自动注入密码完成 SSH 登录 | 【CI】（集成测试用容器内 keepass 生成的测试库） |
| P2-A2 | 与开启 KeePassHttp 插件的 KeePass 2 完成 `associate` → `get-logins` 全流程（用 mock 服务端 + 真实服务端各验证一次） | 【CI】+【人工】 |
| P2-A3 | **内存无明文**：进程 core dump / 内存扫描中无常驻明文密码（自动化扫描测试） | 【CI】 |
| P2-A4 | Argon2 派生在 UI 线程零阻塞：解锁过程主线程事件循环最大卡顿 ≤50ms，有非阻塞进度指示 | 【CI-Perf】 |
| P2-A5 | 解锁结果缓存：连续新建 10 个标签页只触发 1 次 Argon2 派生 | 【CI】 |
| P2-A6 | Command Window 开启广播后，向 20 个并发 SSH 会话同步下发命令，全部收到且顺序一致 | 【CI】（集成测试） |
| P2-A7 | 安全回归：`SecureByteArray` 析构后 mlock 页计数归零；`mlock` 失败时有降级路径而非崩溃 | 【CI】 |
| P2-A8 | KDBX 文件被外部修改后自动刷新，且修改前后的条目查询结果分别正确 | 【CI】 |

### 5.4 风险
- **Argon2 在受限容器内超时**：CI 上 Argon2 参数不可用真实 KeePass 默认值（~1s）。**对策**：测试库使用低参数版本，同时单独保留一个"高参数"手工验证用例，明确 CI 测的是正确性不是性能。
- **`mlock` 在容器中受 `RLIMIT_MEMLOCK` 限制**：必须实现降级路径（关闭 mlock 但仍 `sodium_malloc` + 显式清零），并在日志中明确告知用户降级发生。

---

## 6. Phase 3 — 多协议栈扩展与 SFTP / Zmodem 传输（第 12–16 周）

### 6.1 阶段目标
补齐工业设备接入与文件传输两条高价值链路，并在真实大文件场景验证吞吐与正确性。

### 6.2 任务清单

| 任务号 | 任务 | 产出物 | 依赖 |
| :--- | :--- | :--- | :--- |
| P3-T1 | 串口通道 | `src/protocols/serial/SerialChannel`：QSerialPort，非标波特率、5/6/7/8 数据位、五种校验位、1/1.5/2 停止位、RTS/CTS 与 XON/XOFF | P1-T9 |
| P3-T2 | 串口热插拔与 Break / HexView | USB 串口热插拔枚举、Break 信号（Alt+B）、十六进制双栏监控视图 | P3-T1 |
| P3-T3 | Telnet / Rlogin / Raw TCP | `src/protocols/telnet/`：RFC 854 状态机、NAWS 协商、Keep-Alive 空包、Telnet over TLS | P1-T9 |
| P3-T4 | SFTP 子通道 | `src/transfer/sftp/SftpSubsystem`：复用已有 `ssh_session` 开启 `sftp` 子通道，不重复认证 | P1-T11 |
| P3-T5 | SFTP 交互命令与双栏面板 | `lpwd`/`lcd`/`ls`/`cd`/`get`/`put`/`mget`/`mput`；`SftpDualPaneWidget` 拖拽传输 | P3-T4 |
| P3-T6 | Zmodem 嗅探器 | `src/transfer/modem/ZmodemSniffer`：检测 `**\x18B00` / `**\x18B01`，拦截二进制流不污染屏幕 | P1-T4 |
| P3-T7 | X/Y/Zmodem 状态机 | `XyzModemEngine`：lrzsz 兼容，CRC32 校验、流式收发、`OO` 结束后恢复终端仿真 | P3-T6 |
| P3-T8 | SSH 端口转发 | Local / Remote / Dynamic（SOCKS4/5）转发器 | P1-T11 |
| P3-T9 | 多级跳板机 | `SshJumpHostProxy`：级联代理通道 | P1-T11, P3-T8 |

### 6.3 验收标准

| # | 标准 | 证据等级 |
| :--- | :--- | :--- |
| P3-A1 | USB 转串口连接交换机 / 开发板：正常收发指令，Break 信号可被对端捕获 | 【人工】（CI 用虚拟串口对 `socat` 验证协议层） |
| P3-A2 | 串口参数矩阵（波特率 × 数据位 × 校验 × 停止位 × 流控）自动化回归，每组合收发正确 | 【CI】（虚拟串口对，socat） |
| P3-A3 | 终端内 `rz` / `sz` 自动弹窗选择文件并完成 **100MB** 文件传输，**MD5 100% 一致** | 【CI-Perf】 |
| P3-A4 | Zmodem 传输期间屏幕无二进制乱码，`OO` 后终端仿真正确恢复 | 【CI】 |
| P3-A5 | Zmodem 控制字符（`0x11`/`0x13`/`0x18`）正确转义，`can` 取消后通道可用 | 【CI】 |
| P3-A6 | 右键标签页秒开 SFTP 标签，无需重复认证即可上传下载目录 | 【CI】（集成测试） |
| P3-A7 | 端口转发：Local / Remote / Dynamic SOCKS5 三种模式各跑通一次真实转发 | 【CI】+【人工】 |
| P3-A8 | 两级跳板机级联登录成功，端到端延迟增加可测 | 【人工】 |

### 6.4 风险
- **Zmodem 与终端仿真的状态互斥**：这是最容易出"偶发卡死"的模块。**对策**：把"终端仿真 ↔ Zmodem"的切换收敛为单一状态机（`ModemSessionState`），所有切换必须走它，禁止散落的 `if (zmodem_active)` 判定。

---

## 7. Phase 4 — 高阶终端特性与运维效率套件（第 17–21 周）

### 7.1 阶段目标
提供运维日常效率能力（关键字高亮、自动登录序列、日志归档、SSH Agent），且**不牺牲 Phase 1 建立的性能基线**。

### 7.2 任务清单

| 任务号 | 任务 | 产出物 | 依赖 |
| :--- | :--- | :--- | :--- |
| P4-T1 | 正则高亮引擎 | `src/terminal/renderer/KeywordHighlighter`：预编译 DFA 状态机，仅扫描视口，叠加样式不篡改原始缓冲 | P1-T6 |
| P4-T2 | 多规则集管理 | Keyword Set 创建/编辑/启用，IP、`ERROR`/`WARN`、`up`/`down` 内置集 | P4-T1 |
| P4-T3 | Expect / Send 期望序列 | `Logon Actions`：会话级自动登录，支持超时、重试、变量替换 | P1-T12 |
| P4-T4 | `sudo` 自动响应 | 从凭据层取提权密码并应答，密码不落日志 | P4-T3, P2-T9 |
| P4-T5 | 会话日志系统 | `src/terminal/logger/SessionLogger`：`%H_%Y%M%D_%h%m%s.log` 宏、连接自动开启、每行毫秒时间戳前缀、自动轮转 | P1-T12 |
| P4-T6 | 自动重连 | 断线按策略自动重连、退避算法、重连期间会话树状态图标正确 | P1-T12 |
| P4-T7 | SSH Agent 服务 | 托盘常驻 Agent（对标 Activator）+ Agent Forwarding | P1-T11 |
| P4-T8 | 主机密钥管理器 | known_hosts 解析、信任策略（自动接受 / 提示 / 拒绝）、指纹校验 | P1-T11 |
| P4-T9 | 性能回归验证 | 在启用 50 条正则 + 日志开启场景下复测刷屏基线 | P4-T1, P4-T5 |

### 7.3 验收标准

| # | 标准 | 证据等级 |
| :--- | :--- | :--- |
| P4-A1 | 开启 50 条正则的高亮集后，刷屏帧率相对 P1-A3 基线**下降不超过 5%** | 【CI-Perf】 |
| P4-A2 | 日志开启时刷屏帧率下降 ≤3%（相对无日志基线） | 【CI-Perf】 |
| P4-A3 | 正则高亮不篡改底层字符缓冲：关闭高亮后终端内容逐字节一致 | 【CI】 |
| P4-A4 | 日志文件名宏 `%H_%Y%M%D_%h%m%s.log` 展开正确，并按日期与主机名分目录归档 | 【CI】 |
| P4-A5 | 每行日志带毫秒精度时间戳前缀，且可通过选项关闭 | 【CI】 |
| P4-A6 | 会话断开后按配置自动重连成功；重连期间标签图标为"正在重连" | 【CI】+【人工】 |
| P4-A7 | 主机密钥变更时拒绝连接并给出指纹对比提示（防中间人回归测试） | 【CI】 |
| P4-A8 | SSH Agent 转发后在远端 `ssh-add -l` 可见加载的密钥 | 【人工】 |

### 7.4 风险
- **高亮引擎拖慢渲染**：把正则扫描严格限定在**可见视口**且与渲染解耦（渲染读快照，高亮异步算）。P4-T9 的存在就是为了在 CI 里持续监控这条红线。

---

## 8. Phase 5 — Python 脚本引擎与企业级打磨交付（第 22–26 周）

### 8.1 阶段目标
让存量 SecureCRT 脚本零修改运行，并完成可发布的三端打包。**这是兼容性验收最硬的一步**：`crt.Screen.Synchronous` 的游标语义若实现错，脚本会随机死锁。

### 8.2 任务清单

| 任务号 | 任务 | 产出物 | 依赖 |
| :--- | :--- | :--- | :--- |
| P5-T1 | Python 运行时嵌入 | pybind11 + 内嵌 Python 3 宿主 `EmbeddedPythonHost`，模块名 `crt` | P1-T12 |
| P5-T2 | `crt` 顶层应用对象 | `GetScriptTab()` / `GetTab(n)` / `GetTabCount()` / `Sleep(ms)` / `Quit()` / `Version` | P5-T1 |
| P5-T3 | `crt.Screen` 与同步游标 | `Send` / `WaitForString` / `WaitForStrings` / `ReadString` / `Get` / `Clear` / `Synchronous` / `CurrentRow` / `CurrentColumn`；**独立消费游标**，彻底杜绝竞态 | P5-T1, P1-T4 |
| P5-T4 | `crt.Session` | `Connect` / `ConnectInTab` / `Disconnect` / `Connected` / `Log` / `LogFileName` / `RemoteAddress` / `SetStatusText`，线程安全投递至主线程 | P5-T1, P1-T12 |
| P5-T5 | `crt.Dialog` | `MessageBox` / `Prompt` / `FileOpenDialog`，`BlockingQueuedConnection` 主线程模态 | P5-T1 |
| P5-T6 | `crt.Arguments` | `Count` / `GetArg(i)`，接收 `/ARG` 与按钮栏绑定参数 | P5-T1 |
| P5-T7 | 脚本录制器 | `ScriptRecorder`：录制键盘输入与提示符，停止后导出标准 Python 脚本 | P5-T3 |
| P5-T8 | 主题与配色库 | 深/浅色引擎、Solarized / Dracula / Monokai 等配色方案 | P1-T6 |
| P5-T9 | DPI 适配 | 高分屏缩放，字形图集按 DPI 重建 | P5-T8 |
| P5-T10 | 三端打包与签名 | 安装包（MSI / DMG / deb+rpm）、代码签名、macOS Universal Binary | P5-T9 |

### 8.3 验收标准

| # | 标准 | 证据等级 |
| :--- | :--- | :--- |
| P5-A1 | 选取 **10 个原版 SecureCRT 官方 Python 示例脚本**（含 `WaitForStrings`、`ReadString`、`ConnectInTab`），**无需修改代码**直接运行通过 | 【CI】（脚本回归套件，逐个断言退出码与输出） |
| P5-A2 | `Synchronous = True` 时，`Send("cmd\r")` 后紧接 `WaitForString("$")` 在 10,000 次循环中零死锁 | 【CI-Perf】 |
| P5-A3 | `Send` 后立即 `WaitForString` 的竞态回归：模拟千兆延迟，零漏检 | 【CI】 |
| P5-A4 | `crt.Dialog.Prompt` 在脚本线程阻塞但不阻塞 UI 主线程（UI 可交互） | 【CI】+【人工】 |
| P5-A5 | 录制器导出的脚本可直接回放成功 | 【CI】 |
| P5-A6 | 高分屏（200% / 300%）下无模糊字形、布局错位 | 【人工】 |
| P5-A7 | **50 个活跃会话连续运行 72 小时，零崩溃、零句柄泄漏**（内存与句柄曲线平稳） | 【CI-Perf】 |
| P5-A8 | 三端安装包可安装、可卸载、可升级，签名校验通过 | 【人工】 |
| P5-A9 | 主题切换后无残留配色（单元测试断言默认色恢复） | 【CI】 |

### 8.4 风险
- **72 小时稳定性测试跑不满 CI**：拆成"短时高频"（CI 必跑，如 2 小时 × 50 会话）+ "长时低频"（专用 runner 每周一次 72 小时），两档都必须有趋势看板。
- **Python GIL 与脚本并发**：每个脚本独占工作线程 + 条件变量等待 `WaitForString`，跨线程唤醒必须走 Qt 队列回到主线程，禁止直接触碰 UI 对象。

---

## 9. 跨阶段验收证据留存规范

### 9.1 Phase 0 出口验收记录（run #39714359097 / commit `a74abf5`）

| 标准 | 证据 | 结果 |
| :--- | :--- | :--- |
| P0-A1 三平台构建 | `build-test (ubuntu-24.04 / windows-2022 / macos-14)` 全绿 | ✅ |
| P0-A2 三平台单测 | 各平台 `ctest -L unit` 100% 通过（5 个测试目标） | ✅ |
| P0-A3 perf 层可独立调度 | Linux 单跑 `-L perf`，1 个基准通过；Windows/macOS 正确跳过 | ✅ |
| P0-A4 零告警 + 格式 | `-Werror`（`/W4 /WX`）下零告警；`format (ubuntu-24.04)` 通过固定版本 clang-format 23.1.3 | ✅ |
| P0-A5 覆盖率 | **lines 80.0%**（695/869）、functions 88.4%（114/129）、branches 66.6%（335/503），门禁 40%，报告以 artifact 上传 | ✅ |
| P0-A6 冷启动 ≤45 min | 依赖安装 4 个包 / 约 65 s（改造前 51 个包 / 74.6 min），整条流水线 **3 min 02 s** | ✅ |
| P0-A7 依赖可复现 | `builtin-baseline` 锁定 `b1b1930…`；CI manifest 由 `vcpkg.json` 派生（见 D4 契约） | ✅ |

**CI 流水线实际耗时对比**：D4 落地前 `build-test (windows-2022)` 单 job 75.6 min（其中 74.6 min 在编译无人引用的 openssl + libssh）；落地后三平台全链路 3 min 02 s。P0-A6 由"不可能达成"变为"宽裕达标"。

**教训记录（供后续阶段参考）**：
1. 不要以"工具报告成功"反推产物位置。vcpkg 报告安装成功十几轮，而实际安装树一直是空的；一次 `ls` 探针比五轮前缀路径调整更快。
2. 环境问题的假设必须能被一条日志证伪。CMake 4 破坏 vcpkg 前缀路径、显式传 port 等于 classic 模式——两个假设都被日志直接推翻。
3. 测试自身也会是错的。首轮测试失败里有 4 处是断言写错（含 `vector(count, value)` 参数用反），它们和真实 bug 混在一起，必须逐条判断而不是批量改期望值。

### 9.2 通用要求

每阶段验收（Gate）时必须归档以下材料，缺一不可：

1. **CI 运行链接**（三平台 + perf runner 的 run URL）。
2. **覆盖率报告**（artifact 或 Codecov 链接）。
3. **性能基线对比表**（本阶段 vs Phase 1 基线，标注回归项与结论）。
4. **人工验收记录**（截图 / 录屏 / 真实设备日志）。
5. **遗留问题登记**（含是否阻塞下一阶段）。

---

## 10. 里程碑与门禁总表

| 阶段 | 【CI】条目 | 【CI-Perf】条目 | 【人工】条目 | 出口判定 |
| :--- | :--- | :--- | :--- | :--- |
| Phase 0 | 6 | 0 | 1 | 三平台绿 + 覆盖率 ≥40% |
| Phase 1 | 5 | 2 | 2 | 连接真实服务器 + 刷屏 ≥60 FPS |
| Phase 2 | 6 | 1 | 1 | 20 会话广播 + 内存无明文 |
| Phase 3 | 5 | 1 | 3 | Zmodem 100MB MD5 一致 |
| Phase 4 | 5 | 2 | 1 | 50 正则下帧率衰减 ≤5% |
| Phase 5 | 5 | 2 | 3 | 10 官方脚本零修改通过 + 72h 稳定 |