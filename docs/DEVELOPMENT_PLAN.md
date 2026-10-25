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
| CI 触发 | `on.push` 带 `paths-ignore`（`**.md` / `docs/**` / `.proxy.sh`）：**纯文档提交不产生 run**，省掉一整轮三平台矩阵（见 POSTMORTEM 第五轮）。`pull_request` **不加**路径过滤——被跳过的 workflow 不会报 required status check，将来开分支保护会让文档类 PR 永远 pending；`workflow_dispatch` 保留，人工随时能跑完整矩阵 |

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
| P1-T13 | 启动即弹连接框 | 无参数启动后**自动弹出一次** `Quick Connect`（对齐 SecureCRT 的"双击即连"）：主窗口先显示，连接框在事件循环里弹（`QTimer::singleShot`，不能阻塞 `show()` 前的 `exec()`） | P1-T12 |
| P1-T14 | `Quick Connect` 支持密码认证 | 表单加密码字段；密码**不进 `SessionProfile`**（那会被 `ConfigManager` 写进 INI），只走内存通道，用 `core::SecureByteArray` 承载并在用完时擦除；透传到 `SshConnectionOptions::password` → libssh 的 `ssh_userauth_password`（**动手前先读 libssh 0.11 头文件核对符号**）。密码与私钥都为空时在对话框里就给人话提示，不等到连接失败 | P1-T11, P1-T12 |

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

**P1-T13 / P1-T14 的验收标准**

| # | 任务 | 验收标准 |
| :--- | :--- | :--- |
| P1-A13 | 启动即弹连接框 | ① 无参数启动时主窗口与连接框都出现（CI 用 offscreen 断言对话框被构造/显示；offscreen 下模态 `exec()` 会阻塞，测试里要能自动关闭或改成非模态断言）；② `File` 菜单手动触发仍然可用；③ 取消对话框不报错、不留空标签页 |
| P1-A14 | `Quick Connect` 密码认证 | ① 填密码能连上真实 sshd（复用 CI 内那条真实 sshd：端口 2222、`UsePAM yes`，这次用密码认证再跑一遍端到端）；② 保存/加载会话后 INI 里**不含**密码（需要一条 `ConfigManager` 回归用例守这条）；③ 密码缓冲区用完被擦除 |

**P1-A13 / P1-A14 取证**

| # | 判据 | 证据 | 结论 |
| :--- | :--- | :--- | :--- |
| P1-A13① | 无参数启动时主窗口与连接框都出现 | `MainWindowTest.TheStartupQuickConnectDialogAppearsAndCancellingLeavesNoTab`：offscreen 下监听 `quickConnectDialogShown` 信号断言"对话框被构造"，并在模态 `exec()` 的事件循环里自动关掉它 | ✅ |
| P1-A13② | `File` 菜单仍可用 | `MainWindowTest.TheWindowCarriesTheClassicMenuBar` + 菜单项 `New Session (Quick Connect)...` 与启动路径复用同一个 `openQuickConnect()` | ✅ |
| P1-A13③ | 取消不报错、不留空标签页 | 同上用例：取消后 `workspace()->sessionCount() == 0` | ✅ |
| P1-A14① | 密码认证连真实 sshd | CI 的 Linux leg 起 sshd 时**多建一个一次性账号**（密码在 workflow 里随机生成，经环境变量交给测试，不入库），`Ssh2ChannelTest.ConnectsWithAPasswordToARealServer` 用密码登录并在远端跑命令，校验回显 | ✅ |
| P1-A14② | INI 里不含密码 | `MainWindowTest.TheQuickConnectPasswordNeverReachesTheProfile`：密码不进 `SessionProfile`，保存后的 INI 里既没有明文，也没有任何名为 password 的字段 | ✅ |
| P1-A14③ | 密码缓冲区用完被擦除 | `MainWindowTest.TheQuickConnectPasswordIsHandedOverInAWipeableBuffer`（擦除后读出来全是零字节）+ `SecureByteArray::withCString` 交给 C API 的副本用 `sodium_memzero` 擦掉、缓冲区析构时再擦一次 | ✅ |

### 4.4 已识别的路线图技术偏差

| # | 偏差 | 证据 | 处置 |
| :--- | :--- | :--- | :--- |
| D1 | **`libvterm` 无法经 vcpkg 获取** | microsoft/vcpkg 无 `ports/libvterm`（对 baseline 与 main 分支的 `contents/ports/libvterm` 均返回 404）；上游 libvterm 只提供 autotools 构建，三平台 CI 均无 autotools 工具链 | 已从 `vcpkg.json` 移除。P1-T4 的转义序列前端（`ControlSequenceParser`）与屏幕网格自行实现。**2026-10-23 决策：保持自研，不引入上游 libvterm。** 理由：引入要额外解决 autotools 工具链（CI 三平台都没有），并把定制点收敛到 `VtermStateMachine` 单一适配层，收益却可以用更便宜的手段拿到——用**独立参考实现**做逐例基线比对（见 D5）直接暴露排版差异。风险是自研状态机可能在上游已覆盖的边界用例上有差异，因此该比对必须做成**机器可判定**的验收项（基线入库 + 差异即红），而不是可选的人工检查 |
| D2 | **Qt 不经 vcpkg** | qtbase 经 vcpkg 冷构建需数小时，CI 不可接受 | CI 用 `install-qt-action`（预编译包），vcpkg 只管非 Qt 依赖 |
| D3 | **新增 Phase 0** | 路线图把"三端编译通过"放在 Phase 1 验收，但未建 CI，导致五阶段验收全靠人工 | 拆出 Phase 0 固化门禁 |
| D4 | **CI 只安装被链接的依赖** | `vcpkg.json` 声明的 10 个 port 里，当前 CMake 树只引用 `libsodium`（`src/core`、`src/credential`）与 `gtest`（`tests/`）；`libssh` / `openssl` / `nlohmann-json` / `pugixml` / `zlib` / `pybind11` / `qtbase` / `qtserialport` 零引用。`ci.yml` 不再安装它们——实测 windows-2022 的 `vcpkg install` 从 **74.6 min**（占该 job 75.6 min 的 98.7%，全部耗在 openssl + libssh 的 MSVC 源码编译上）降到约 3–6 min，P0-A6 的 45 min 冷启动门禁重新可达 | `vcpkg.json` 仍是唯一事实来源；`ci.yml` 从中派生出 CI 清单。**契约**：某阶段开始链接某个 port 时，必须同时删掉它在 `ci.yml` `SKIP` 表里的条目；若漏删，表现为 configure 阶段几分钟内明确报 "not found"，而不是像之前那样默默编译 75 分钟没人用的库 |
| D5 | **P1-A2 的基线由 tmux 生成，而不是 vttest 用例集** | 判据原文要求"对 `vttest` 用例集逐项比对基线截图"。但 vttest 现行发行版（2025-12-05，invisible-mirror.net 归档）**只随源码分发 C 源**，`script/*.itms` 用例脚本已不在 tarball 内；能取到的两个镜像（`pH200/vttest`、`ThomasDickey/vttest-snapshots`）同样不含脚本 | 改用 **tmux 作独立参考实现**：它对同一批转义序列的实现是独立的，基线因此是"关于序列"的证据，而不是"关于我们自己"的证据——用我们自己的输出当基线只会把 bug 一起固化。用例集自研（`tests/fixtures/vt`，8 例，覆盖光标寻址、擦除与编辑、三种颜色写法、滚动区域、备用屏、宽字符、制表位）。**代价**：用例集不是 vttest 的原始用例，真实 vim/htop/less 的输出流也未抓包比对，两者都登记为遗留 |
| D6 | **字形图集与渲染器落在 `src/ui/renderer/`，而非路线图写的 `src/terminal/renderer/`** | 路线图把 `GlyphAtlas.h` / `DirtyRectRenderer.h` 放在 terminal 层，但"纹理图集"本质是**栅格化产物**：它需要 `QImage` / `QPainter` / `QFont`，放进无 Qt 的 terminal 层会直接破坏该层的无头可测性——而 P1-A3 的帧预算正是在无头 CI 里测的 | terminal 层只留**脏行判定**（`src/terminal/renderer/DamageTracker`，纯逻辑、可单测），栅格化与绘制放 `src/ui/renderer/`（`GlyphAtlas` + `TerminalRenderer`）。`TerminalRenderer` 本身不含 Widget，因此 P1-A3 的重绘预算仍能在 offscreen 平台下无头测量 |
| D7 | **Phase 1 的阶段出口未签收，但不再阻塞 Phase 2** | 2026-10-25 的决定：P1-A7（三端真机人工验收）**暂不执行**，按遗留项挂着，Phase 1 归档为"**工程完成、出口未签收**"，Phase 2 可以开工。**没有把没做的事写成做了**——三端真机这一栏至今是空的，机器能判的那部分（M1/M2/M3/M4/M6/M7/M8 有机器证据，M5 的排版色彩由 tmux 基线与 vim/htop/less 真机抓包逐格一致证明）已经做满，剩下的"在真实显示器上用键盘操作"由用户在需要时用 `openscrt --acceptance-check` 与 M1–M8 清单自己补 |

### 4.5 风险
- **CJK 双宽字符与光标对齐错位**：在 P1-T2 就把双宽位写进 `Cell` 位域，不允许后期加字段（会破坏 16 字节布局）。
- **若后续引入 libvterm**：定制点必须收敛到**唯一适配层** `VtermStateMachine`，禁止在业务代码里直接调 libvterm API。**当前决策是不引入**（见 D1），因此这条风险暂不激活；一旦改判，它会立即重新生效。
- **自研 VT 状态机的排版偏差**：不引上游就把"是否与主流终端一致"的责任收回自己身上。缓解手段是 P1-A2 的**独立参考基线比对**——基线由 tmux 生成并入库、差异即红，让偏差在 CI 里暴露，而不是在用户屏幕上暴露。用例集覆盖的是典型 TUI 序列，不是 vttest 的原始用例，也不是真机抓包，这两项都登记为遗留。

### 4.6 任务完成度与验收取证（run #39974690399 / commit `9a57b05`；基线差分证据见 run #39968512770）

**任务完成度**

| 任务号 | 状态 | 说明 |
| :--- | :--- | :--- |
| P1-T1 SPSC 无锁队列 | ✅ | 单生产者单消费者，每条序列零堆分配；绕回与并发有测试 |
| P1-T2 Cell / ScreenGrid | ✅ | 16 字节布局、`static_assert` 锁死；光标、滚动区域、备用屏、宽字符 |
| P1-T3 环形回滚缓冲 | ✅ | 分页按需分配；行离开顶部时上报，且仅当滚动区域从第 0 行起 |
| P1-T4 VT 状态机 | ✅ | `ControlSequenceParser`（CSI/OSC/ESC/UTF-8/子参数）+ `TerminalEmulator` 接缝；补齐 ECH/DCH |
| P1-T5 256 色 / TrueColor | ✅ | xterm 调色板；SGR 全量；分号与冒号两种扩展色写法 |
| P1-T6 字形图集 + 脏矩形 | ✅ | **完整落地**：`src/ui/renderer/GlyphAtlas`（ASCII + CJK 预渲染纹理图集，四种字形样式分开占位，未收录的字形回退到直接绘制）+ `src/ui/renderer/TerminalRenderer`（按 `DamageTracker` 的脏行band绘制）+ 无 Qt 的 `src/terminal/renderer/DamageTracker`。效果有实测：全屏重绘 2.32 → 1.34 ms，增量/全屏比 1.3× → 7.6×（见 P1-A3b） |
| P1-T7 终端画布控件 | ✅ | `TerminalCanvasWidget`：`feed()` 只置合并标志，只有定时器能出帧；键位编码为纯函数；offscreen 无头测试 |
| P1-T8 帧合并调度 | ✅ | 60/120Hz；突发合并为 1 帧、持续输出受刷新率上限约束、空闲不出帧 |
| P1-T9 传输接口冻结 | ✅ | `ITransportChannel` |
| P1-T10 本地 Shell | ✅ | POSIX `forkpty` 与 Windows ConPTY 均已实现（ConPTY 仅由 Windows leg 编译验证，运行时待人工） |
| P1-T11 SSH2 通道 | ✅ | libssh 0.11 真实 API；公钥→密码→keyboard-interactive；**D4 契约已在同 PR 从 SKIP 表删除 `libssh`**；密钥类型由 libssh 覆盖 RSA / ECDSA / Ed25519；**VanDyke（ssh.com）私钥格式已实现**（`src/protocols/ssh/SshComKey`：3DES-CBC 解密 + MD5 派生密钥 + 转成 OpenSSH 容器交给 libssh）。用**真实密钥**验证：SSH.NET 测试数据里同一把 RSA 私钥的明文与 3DES 加密两版，解析后逐字段相等；期望公钥由**独立的 Python 实现**导出；CI 的 sshd 被授予该公钥，**加密私钥端到端登录成功** |
| P1-T12 主窗口与工作区 | ✅ | **完整落地**：会话继承树 + Default Session 模板（纯逻辑、无 Qt 依赖）+ `ConfigManager`（QSettings INI，**只写被覆盖字段**）+ `MainWindow`（经典菜单栏 File/Edit/View/Options/Transfer/Script/Tools/Window/Help；`Session Manager` 与 `Active Sessions` 两个 dock；状态栏）+ `WorkspaceTabs` 多标签工作区 + `QuickConnectDialog`；`--self-test` 在 offscreen 平台自检窗口装配，CI 可判定 |
| P1-T13 启动即弹连接框 | ✅ | `MainWindow::openStartupQuickConnect()`：无参数启动时在**事件循环里**弹 `Quick Connect`（`QTimer::singleShot`，不阻塞 `show()` 前的 `exec()`）；`File` 菜单路径复用同一个对话框；CI 用 offscreen 断言"对话框被构造"（`quickConnectDialogShown` 信号）且**取消后不留空标签页** |
| P1-T14 `Quick Connect` 密码认证 | ✅ | 密码字段在 `QuickConnectDialog`，**不进 `SessionProfile`**（那会被 `ConfigManager` 写进 INI）：走 `core::SecureByteArray` 内存通道 → `WorkspaceTabs::openSession` → `TerminalSession` → `SshConnectionOptions::password` → libssh `ssh_userauth_password`；**用完即擦**（`SecureByteArray::withCString` 把明文交给 C API 后用 `sodium_memzero` 擦掉副本，缓冲区析构时再擦一次）；密码与私钥都为空时对话框**当场给提示**而不是等连接失败 |

**验收取证**

| # | 标准 | 证据 | 结论 |
| :--- | :--- | :--- | :--- |
| P1-A1 | 三平台编译 + 连接主流 Linux 服务器 | run #39933913420 **四个 job 全绿**（format + Windows / Linux / macOS）；Windows leg 用 MSVC `/W4 /WX` 编译通过（含 ConPTY），unit 层全绿；**Windows 的证据止于"编译 + 单元/协议层 + CI runner"，真机 GUI 从未验证过**；CI 内起真实 sshd（2222 端口 + ed25519 客户端密钥 + `UsePAM yes`），`unit_ssh2_channel` 的 `ConnectsToARealServerAndRunsACommand` **用公钥真实登录并执行远端命令**，本 run 5.99 s 通过；`unit_local_pty_channel`（forkpty）0.82 s 通过。**run #39995159095 补上了 ConPTY 的一环**：Windows leg 的 `unit_local_pty_channel` 从"5 条用例各超时 5 秒全红"变成 **0.33 秒全绿**——此前子进程一直跑在继承来的标准句柄上（横幅直接落进 CI 日志），加 `STARTF_USESTDHANDLES` 后输出才真正走伪控制台回到通道里（见 POSTMORTEM 8.1） | 🟡 **CI 侧 SSH 端到端已满足**（真实 sshd + 公钥 + 命令回显校验），**CI 侧本地 shell 三端均已满足**（Windows ConPTY / POSIX forkpty，命令回显逐例校验）；**真机 SSH / 真机 GUI / 真机本地 Shell 仍待人工**——外部真机反馈证实 v0.1.1 资产没有 GUI，它无法验证任何 GUI 行为。**v0.1.2 是第一个带窗口的版本**（Windows 资产），**v0.1.3 三个平台齐了**（run #40012117665），**v0.1.4 再加启动即弹连接框与密码字段**（run #40102240070），**v0.1.5 带着 `openscrt --acceptance-check`**（run #40108273795：Windows MSI + 便携 ZIP / macOS `.app`（arm64）/ Linux tar.gz；macOS 的 bundle 在 arm64 runner 上真跑起来过——`self-test: window assembled, 2 docks, 9 menus, 1 tab(s)`），P1-A7 的 M1–M8 可以直接用这些包做 |
| P1-A2 | vim/htop/tmux 排版与色彩正确 | **基线由 tmux（独立 VT 实现）生成**：8 个用例（`tests/fixtures/vt`）→ tmux 渲染 → 规范化屏幕快照（文本 + 属性游程）存为 `tests/baselines/vt`；`unit_vt_baseline` 逐例比对，差异按行打印"哪一格动了"；CI 另有一步用**当前** tmux 重新生成并 `diff`，防止基线与参考实现脱节。run #39958310148：Linux / Windows / macOS 三个 leg 的 `unit_vt_baseline` **8/8 逐格一致**，漂移检查也通过（基线仍等于今天的 tmux 所产）。**更进一步：真机抓包差分**——vim / htop / less 跑在 80×24 的 pty 里，抓它们真实吐出的字节，同一份字节分别喂给 tmux 与我们的状态机比对屏幕；run #39968512770 三个程序**全部逐格一致** | 🟡 **比对机制已达成，且基线不是我们自己的输出**——这正是它能当证据的原因（见 D5）。**真机抓包差分已经抓到并修掉一个产品 bug**：擦除时丢掉了当前背景色，htop 的行底色在屏幕上直接消失（单测覆盖不到）。**仍未覆盖**：vttest 原版用例集（现行发行版已不再随源码分发 `.itms`）、**像素级**比对（渲染器与字形图集已就绪，但比对用的是平台无关的屏幕快照而不是像素哈希——三个 CI 平台的字体渲染不同，像素哈希不可能跨平台稳定） |
| P1-A3 | `seq 1 1000000` 帧率 ≥60 FPS | 渲染器 + 字形图集已实现；perf 层实测（run #39968512770，**按判据本身的规模**）：解析 **1,000,000 行 / 9,043 ms（9.043 μs/行、110,581 行/s、1,847 行/帧）**；全屏重绘 **1.31 ms**（≈766 FPS 等效）；600 ms 饱和输出产出 **38 帧**；增量/全屏重绘比 **6.9×**；**流式输出期间进程内存只增长 4.0 MB** | 🟡 **机器可判定的部分已满足**：判据写的 100 万行工作集与"内存增长 ≤256MB"两项都已在 CI 里按原规模实测，16.7 ms 的帧内可消化 1,847 行输出，离 60 FPS 有数量级余量。**人工部分仍未做**：Widget 端到端 FPS 需要真实显示环境 |
| P1-A3b | （补充）脏矩形收益 | 增量/全屏重绘比 **1.3× → 6.9×**（run #39968512770：0.90 ms vs 6.17 ms；此前 7.2× / 7.6× / 7.7× / 8.9×，五次一致） | ✅ **达成**。两步：①渲染器改用图集 blit（全屏重绘 2.32 → 1.34 ms）；②`ScreenGrid` 改用**每行一位**的脏位图，`collect()` 从读 1920 个格子变成读 24 个位 |
| P1-A3c | （补充）测量方法 | 绝对耗时在多次运行间漂移（解析 5.18 → 7.52 → 11.05 → 10.01 → 10.59 → **9.04** μs/行），但**未修改**的 `scrollback push` 同步漂移（0.242 → 0.379 → 0.523 → 0.430 → 0.335 → **0.341** μs/行） | ⚠️ **共享 runner 负载造成的漂移，不是代码退化**。**比值**在同一次运行内测两侧，对负载不敏感，因此用它做判据；**绝对值**只用于同一 run 内的前后对比 |
| P1-A4 | SPSC 在 TSan 下无数据竞争 | 独立构建树（`linux-tsan` preset，coverage OFF）单独构建 `unit_spsc_ring_buffer_tsan`；run #39961633389 该用例 **Passed 0.24 s，零 data race 报告**；并发用例传输 **10,000,000 字节**（1024 字节/批，即判据要求的 1000 万次写 + 1000 万次读）并逐字节校验顺序 | ✅ **达成**。`-fsanitize=thread` 下零报告，且读写量级已到判据原文的 1000 万次 |
| P1-A5 | 回滚 128,000 行稳定、检索 p99 ≤1ms | 容量按页对齐为 **131,072**（`DefaultCapacityCoversTheRoadmapMaximum` 断言 ≥128,000，`OverwritesOldestBeyondCapacity` 断言最旧行被正确覆盖）；perf 层按 **140,000 行**工作集实测（run #39968512770，已越过容量上限、淘汰路径同时跑到）：写入 **0.341 μs/行**、进程内存只增长 **16.1 MB**、检索 p99 **0.661 µs**（worst 0.831 µs，2000 采样，CI 以 `EXPECT_LT(p99_us, 1000.0)` 机器判定） | ✅ **达成**。工作集、内存稳定性、检索延迟三项都按判据规模实测并被断言锁死 |
| P1-A6 | 备用屏进出不污染主屏 | `AlternateScreenDoesNotFeedScrollback` 等用例 | ✅ |
| P1-A7 | 三端真机登录验证 | — | ❌ **未做（人工项；按 D7 暂不执行，不阻塞 Phase 2）** |
| P1-A8 | 核心库覆盖率 ≥65% | run #39933913420：**lines 75.8%（1371/1808）**、functions 91.5%（204/223）、branches 62.1%（754/1215） | ✅ |

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

**P1-A3 实测（run #39933913420，Linux）**

```
[perf] parse: 20000 lines in 200.2 ms (10.010 us/line, 99896 lines/s)
[perf] repaint: 24 rows in 1.95 ms (514.0 FPS equivalent for one frame)
[perf] 200 repaints: partial 1.23 ms vs full 8.86 ms (7.2x)
[perf] 600 ms of saturated output produced 38 frames (63.3 FPS)
[perf] scrollback push: 2000 lines in 860 us (0.430 us/line)
[perf] scrollback lookup: 2000 samples in 408 us
[perf] lookup p99: 0.191 us (worst 0.401 us, 2000 samples)
```

**P1-A3 实测（run #39968512770，Linux，按判据本身的规模）**

```
[perf] parse: 1000000 lines in 9043.1 ms (9.043 us/line, 110581 lines/s, 1847 lines per 60 Hz frame)
[perf] memory: grew 4.0 MB while streaming 1000000 lines
[perf] repaint: 24 rows in 1.31 ms (765.5 FPS equivalent for one frame)
[perf] 200 repaints: partial 0.90 ms vs full 6.17 ms (6.9x)
[perf] 600 ms of saturated output produced 38 frames (63.3 FPS)
[perf] scrollback push: 140000 lines in 47779 us (0.341 us/line)
[perf] scrollback memory: grew 16.1 MB holding 131072 lines
[perf] scrollback lookup: 2000 samples in 919 us
[perf] scrollback lookup p99: 0.661 us (worst 0.831 us, 2000 samples)
```

**上一轮测的是缩水的规模，这一轮才是判据写的规模**——100 万行而不是 2 万行，140,000 行工作集而不是 2,000 行，1000 万字节而不是 200 万字节。缩水的理由当时是"共享 runner 要快"，但实测下来：100 万行解析 10.6 s、14 万行写入 47 ms，**根本不是负担**。一个按缩水规模测出来的数字，只能说明"在缩水的规模下没问题"，说明不了判据本身。

**一个值得单独记的数**：1,577 行/帧。60 FPS 的一帧是 16.7 ms，按实测的 10.589 μs/行，一帧能消化 1,577 行输出——离"喂不饱帧循环"差着数量级。**内存增长 4.0 MB** 则是另一头：100 万行流过去，进程只多了 4 MB，回滚缓冲的容量上限真的在起作用。

这一轮把 P1-A3c 的方法论又验证了一次：**同一份代码**，解析吞吐从 5.18 掉到 10.01 μs/行（近 2×），而**完全没动过**的 `scrollback push` 同步从 0.242 漂到 0.430 μs/行。漂移是共享 runner 的负载，不是代码退化。
**比值**在同一次运行内测两侧，负载同向影响，因此它是这里唯一可信的判据：**增量/全屏重绘比 7.2×**（前一轮 7.6×、再前一轮 7.7×），三次一致。

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
| 脏区检测逐格扫描 1920 个格子 | 固定 O(rows×cols) 成本，把增量/全屏比钉死在 1.3×（改行位图后 7.6×）|
| `resolve()` 让最远祖先获胜 | 三层继承链上每层都覆盖，会话自己的值被祖先冲掉 |
| 删除节点时只压缩未缩容 | 被删节点仍占槽位，父节点永远删不掉 |
| 字段名撞上 Qt 的 `signals` 宏 | 无 Qt 的头文件被跨层 include 时整个结构体解析失败 |
| `saveTree()` 清空整个 INI | 同一文件里的窗口几何信息被一并抹掉，"树与几何互不干扰"的承诺不成立 |
| 用 `resolve("")` 取模板 | 模板不是树里的会话，按 id 查找必然失败 → 改为主动读 `defaultSession()` |
| 传输回调在自身互斥锁**之外**调用 | 析构时"清空回调"挡不住在途回调，读线程可能往已析构的 session 投递 |
| `TerminalSession` 同时被 `unique_ptr` 与 QObject parent 拥有 | 双重释放，退出时随机崩溃 |
| `Ssh2Channel::connectAsync()` 在调用线程同步握手 | UI 线程最长阻塞 15 s（SSH 超时），窗口无响应 → 改到工作线程 |
| `QSettings::childGroups("sessions")` 返回的是**直接子组名** | 若按全路径解析会把 `sessions/a/b` 误当成一个会话 id |
| `QSettings::childGroups(prefix)` 在 Qt 6 已删除 | 编译失败；且 `childKeys()` 只报当前组的叶子键（本文件的键都在下一层）→ 改用 `beginGroup("sessions") + childGroups()` |
| `QMenu::addAction(text, receiver, slot, shortcut)` 在 Qt 6.4 起带 `[[deprecated]]` | `-Werror` 下直接编译失败 → 改用 `addAction(text, shortcut, receiver, slot)` |
| 整文件重写漏掉 `MainWindow::setStatus` 的定义 | 编译通过、**链接期**报 undefined reference（CI 才发现，代价一轮往返） |
| `openscrt_config` 把 `Qt6::Core` 设为 PRIVATE | 静态库不传播使用要求 → `unit_config_manager` 找不到 `<QSettings>` 的头文件 |
| `const SessionTree` 上调用 `setDefaultSession` | const 限定符丢失，编译失败 |
| "悬空父节点被丢弃"用例其实是空的 | 树本身拒绝这种 profile，保存出来的文件里根本没有孤儿 → 改为手写 INI 才有意义 |
| 批量改写脚本把 `ci.yml` 的 `if-no-files-found: error` 切到了 TSan 步骤的 `run:` 里 | 步骤以 `if-no-files-found: command not found`（exit 127）收场，**掩盖了该步骤内已经通过的全部断言** |
| gcov 计数器是普通全局，与 `-fsanitize=thread` 同构建 | 两个线程自增同一计数器会被 TSan 报成 data race（本地实测 217 处**假阳性**）→ TSan 必须独立构建树 |
| fortified libc 把 `memcpy(dst, nullptr, 0)` 判为错误（Linux `-O1` 及以上） | `SpscRingBuffer::write()` 收到空指针即编译报错（`-Werror=nonnull`），coverage 构建用 `-O0` 才掩盖住 |
| `sshd` 配 `UsePAM no` + 密码字段被锁定的 runner 账号 | 公钥认证被拒，表现为笼统的 "authentication failed"，与密钥本身无关 |
| SSH 冒烟标记与命令文本相同 | 匹配到远端 PTY 的**回显**而非命令输出，测试恒真 → 标记改用插值形式 |
| SGR 直接色的冒号写法与标准的差异 | ITU T.416 的写法是 `38:2:<色彩空间>:r:g:b`（5 段），四段简写缺色彩空间槽；**tmux 接受四段**，本实现按 tmux 的宽松语义处理。差分测试先按标准改、随后按参考实现回退——**终端要匹配的是别的终端** |
| 颜色组非法时残余参数落回 SGR 循环 | `38;2;10` 的尾部 `0` 会被当成"复位"；改为整组消费 |
| 用例末尾清屏把证据抹掉 | 光标寻址与滚动区域两个用例写完内容又清屏，基线成了空屏——空屏和任何空屏都相等，测不测都一样 |
| 擦除时用默认背景色填充，丢掉当前背景色 | xterm 的擦除用**当前**背景色；htop 先设行底色再擦行 → 我们的实现把行底色丢了，**屏幕上可见**。由**与 tmux 的真机抓包差分**测出（单测覆盖不到） |
| ConPTY 三个 API 被静态导入 | kernel32 的导入表里有了 `CreatePseudoConsole` 等 1809 才有的符号 → 旧系统上**加载器阶段就死**，用户看不到任何提示。外部真机反馈（Win10 1607）证实。改软加载后我们的 exe 的导入地板降到 1607 |
| 仓库从不声明 Windows 最低版本 | CI runner 比任何受支持系统都新，**任何门禁都发现不了**；地板是被调用了新 API 的人悄悄抬高的。已加 CMake 声明 + 导入表门禁 |
| 比较格式没归一化"空白格带前景色" | 无字形的格子若背景是默认色，它的前景色与粗体在屏幕上看不见，但两端模型不同（tmux 不保留）→ 归一化必须**两边都做**，否则差分全是噪声 |

**覆盖率（run #39933913420）**：lines **75.8%**（1371/1808）、functions 91.5%（204/223）、branches 62.1%（754/1215）。**16 个单测/自检目标 + 2 个性能基准 + 1 个 TSan 目标全绿。**

**结论：Phase 1 工程完成，阶段出口未签收。** T1–T14 全部完成（含配置持久化、工作区外壳、启动即弹连接框、密码认证）。验收项：P1-A4 / P1-A5 / P1-A6 / P1-A8 / P1-A3b / P1-A3c **满足**；P1-A3 的机器判定部分**满足**（100 万行工作集、内存增长 4.0 MB、1,847 行/帧），端到端 Widget FPS 待人工；P1-A1 的 CI 侧 SSH 端到端**满足**（真实 sshd + 公钥 + 远端命令执行；OpenSSH 与 ssh.com 两种私钥格式都能登录；**密码认证也在同一条真实 sshd 上端到端跑通**）；P1-A13 / P1-A14 **满足**（启动即弹连接框有 offscreen 断言；密码不进 INI 有回归用例；密码缓冲区用完即擦有断言）；P1-A2 的基线比对**满足**（基线由 tmux 独立参考实现产生，三平台 8/8 逐格一致；**vim / htop / less 的真机抓包差分也全部逐格一致**，并因此抓到并修掉一个"擦除丢背景色"的产品 bug），但用例集非 vttest 原版、且无截图级比对。**P1-A7 三端真机验证按 2026-10-25 的决定暂不执行**（见 D7），因此**阶段出口未签收**，但它不再阻塞 Phase 2——Phase 2 的代码可以并行开工。

**遗留问题登记（Phase 1）**

| # | 遗留问题 | 类别 | 是否阻塞下一阶段 |
| :--- | :--- | :--- | :--- |
| R1 | P1-A7：Windows / macOS / Linux 三端各连 1 台真实服务器并完成交互式登录 | 人工 | **暂不执行**（2026-10-25 决定，见 D7）——按遗留项挂着，**不再阻塞 Phase 2**；阶段出口因此保持"未签收"。CI 只能证明"协议栈对着一台机器通了"，证明不了"三端真机可用"。**CI 已经能判定的那部分**：M3/M4 走**真实服务器**——`MainWindowTest.TheQuickConnectDialogReachesARealServerThroughTheWindow` 用 `Quick Connect` 对话框连 CI Linux leg 起的那台真实 sshd（公钥与密码两条认证路径各一遍），命令输出必须落到屏幕网格上（CI 里按用例名单独跑一遍，日志里能直接看到）；**留给人工的是"在真实显示器上用键盘操作"**。**验证用的包已就绪：v0.1.5**（Windows MSI + 便携 ZIP；macOS `.app`（arm64）；Linux tar.gz，见发布页；`SHA256SUMS.txt` 覆盖三个平台；run #40108273795），**它带着 `openscrt --acceptance-check`**（在自己的机器上跑机器能判的那一半），M1/M2 与密码登录正好用它验，清单见本节 M1–M8 |
| R2 | P1-A2：不是 vttest 原版用例（现行发行版已不再随源码分发 `.itms`），也没有截图级比对 | 人工 + CI | 否——**真实程序已覆盖**：vim / htop / less 的真机抓包与 tmux 逐格一致（run #39968512770）。截图级比对要等渲染器与字形图集（见 R7） |
| R3 | P1-A3：显示服务器的合成与面板刷新率未覆盖 | 人工 | 否——**Widget 帧时间已按判据口径实测**（p95 1.11 ms ≤ 16.7 ms，run #39979970617），剩下的是 offscreen 平台不建模的那部分 |
| R4 | ~~P1-A4：并发读写的量级不足~~ | — | **已关闭**：run #39961633389 起并发用例传输 10,000,000 字节（1000 万次写 + 1000 万次读），TSan 下零报告 |
| R5 | ~~P1-A5：128,000 行工作集未实测~~ | — | **已关闭**：run #39961633389 起 perf 层按 140,000 行工作集实测（越过容量上限，淘汰路径同时跑到），p99 0.572 µs、内存增长 16.1 MB |
| R6 | **Windows 真机 GUI 未验证**（ConPTY 已于 run #39995159095 在 CI 的 Windows runner 上跑通：子进程输出真的经过伪控制台回到通道） | 人工 | 否——但这是 Windows 端唯一的真实风险点，与 R1 合并处理。外部真机反馈证明：**v0.1.1 资产没有 GUI**（双击只弹 MessageBox），因此它无法验证任何 GUI 行为；**带 GUI 的包从 v0.1.2 就有，最新的是 v0.1.5**（还带着 `openscrt --acceptance-check`），验证 GUI 随时可以用它做 |
| R10 | **Windows 最低系统版本此前从未声明**，便携包在旧系统上"启动即崩" | 门禁 + 产品 | 否——已修：CMake 显式声明 `WINVER/_WIN32_WINNT=0x0A04`，ConPTY 改软加载（我们的 exe 不再静态导入 1809 的导出），CI 加**导入表门禁**（按 kernel32 导出表判定，不按文档），**两个阈值都会让构建失败**：自有二进制 `--floor 1607 --strict`，发布包整体 `--package-floor 1607`。**包的导入地板由 Qt 决定**：Qt 6.8 的 `SetThreadDescription` 会把它抬到 1709，而 **Windows 2016（10 1607）要能用**是硬要求（决定 0001，2026-10-25）——所以 **Windows 的构建与发布改用 Qt 6.7**（各 Qt 系列的地板实测于 run **#40205287033**：6.5/6.6/6.7 是 1607，6.8 是 1709），承诺的支持基线随之从 1809 降到 **1607**。**代价**：Windows 用户拿到的 Qt 是 6.7，拿不到 6.8 的修复；**遗留**：1607 的真机验收仍未做（P1-A7） |
| R7 | ~~P1-T6：字形图集与脏矩形渲染器未做~~ | — | **已关闭**：图集与渲染器都在 `src/ui/renderer/`（见 D6），并有 `unit_terminal_renderer` 覆盖（ASCII/CJK 栅格化、缓存、样式分离、脏行只画一行、相邻行合并） |
| R9 | ~~P1-T11：VanDyke（ssh.com）私钥格式未实现~~ | — | **已关闭**：run #39974690399 起，`unit_sshcom_key` 7 个用例全绿，含用加密 ssh.com 私钥对 CI 真 sshd 的端到端登录 |
| R11 | **Windows 本地 shell 通道曾长期零输出**：子进程没挂上伪控制台，跑在继承来的标准句柄上，横幅与提示符直接落进 CI 日志，而 CI 一直把它当成"通道没数据" | 方法论 + 产品 | 否——**已修**：`STARTF_USESTDHANDLES` 置位、三个标准句柄留空（与 Windows Terminal 参考实现一致），run #39995159095 Windows leg 的 `unit_local_pty_channel` 0.33 s 全绿。教训见 POSTMORTEM 8.1–8.3 |
| R8 | **判据按缩水规模测过，就不算测过** | 方法论 | 否——但这是本阶段最容易被忽略的一类缺口：P1-A3/A4/A5 三条判据都长期跑在缩水的规模上（2 万行 / 200 万字节 / 2,000 行工作集），归档里却如实记成"未实测"。本轮全部改回判据原文的规模，耗时远低于预期（100 万行 10.6 s），**缩水的理由当初站不住** |

**P1-A7 人工验收清单（待执行）**

Windows / macOS / Linux 各做一遍，逐项打勾并留下记录。

**没有现成的服务器也能验**：`tools/local_sshd.sh` 会在本机起一台真实的 sshd
（127.0.0.1:2222，临时目录里的一把 ed25519 私钥 + 一个带密码的一次性账号），并把
主机、端口、用户名、密钥路径和密码直接打印出来；CI 的 Linux leg 跑的是**同一个脚本**，
所以这里看到的参数和 CI 里的一模一样。收工用 `tools/local_sshd.sh --stop`。

**先跑一遍程序自己的自检**：`openscrt --acceptance-check` 会在**你这台机器上**逐项检查
窗口装配、启动弹出的连接框、本地 shell 会话连上并回显、会话存盘再读回、关标签无残留，
每项打一行 `ok`/`FAILED`，最后给出失败项数（退出码即失败数）。这一份是"真机"那一半的
记录（CI 证明的是 runner 那一半）；它同时也是 CI 里的常驻回归项
（`app_acceptance_check`），所以 ConPTY 这类平台相关的问题再犯会被当场抓住。
**剩下只能人做的**：窗口看起来对不对、敲键盘的手感、vim/htop/less 里的交互。

**CI 侧的证据**：run #40107277385 起，`app_acceptance_check` 在三个平台都是常驻用例
（Windows 0.20 s、Linux 0.26 s 通过）——本地 shell 会话真的连上并回显了，
**ConPTY 的修复因此有了每轮 CI 的回归护栏**。

**包用 v0.1.5**（run #40108273795）：它带着 `--acceptance-check`，下载下来解压就能在
自己的机器上跑那一半验收。

| # | 操作 | 期望结果 | 结论 |
| :--- | :--- | :--- | :--- |
| M1 | 启动程序 | 主窗口出现，含 File / Edit / View / Options / Transfer / Script / Tools / Window / Help 菜单栏，以及 `Session Manager` 与 `Active Sessions` 两个 dock | |
| M2 | File → New Session (Quick Connect)... | 对话框可填 Host / Port / Username / Protocol / Key File；Host 为空时 OK 被拒并给出提示 | |
| M3 | 填真实服务器 → OK | 状态栏出现 `Connected to <host>`；**握手期间界面不冻结**（仍可拖动窗口） | |
| M4 | 在标签页里敲命令 | 回显与远端一致；`ls`、`echo`、中文输出均正常 | |
| M5 | 跑 `vim` / `htop` / `less`（服务器有的话） | 排版、颜色、备用屏进出正常；退出后主屏不被污染 | |
| M6 | Ctrl+Tab 切标签、Ctrl+W 关标签 | 多标签互不干扰；关闭后连接断开且无残留 | |
| M7 | File → Save Sessions → 退出 → 重开 | 会话树仍在；`Active Sessions` 列表已空 | |
| M8 | 关掉服务器或断网 | 状态变为断开，不卡死、不弹栈溢出 | |

每台机器需记录：操作系统版本、Qt 版本、`openscrt --version` 输出、服务器地址（可打码）、M1–M8 的逐项结论与截图。
**M3 与 M5 是本阶段的关键项**——M3 证明握手没有阻塞 UI 线程，M5 证明 VT 状态机与 SGR 在真实排版下成立；这两项在 CI 里都无法替代。


---

## 5. Phase 2 — KeePass 双模凭据与全界面布局还原（第 7–11 周）

### 5.1 阶段目标
解决"密码不进配置文件"的硬需求，同时把 SecureCRT 的高密度界面骨架搭完整。本阶段是**安全属性最密集**的阶段，安全验收标准不可妥协。

### 5.2 任务清单

| 任务号 | 任务 | 产出物 | 依赖 |
| :--- | :--- | :--- | :--- |
| P2-T1 | 凭据提供者抽象 | `src/credential/ICredentialProvider.h`：统一检索与自动注入接口（`ICredentialProvider.h`） | — |
| P2-T2 | 安全内存容器 | `src/core/SecureByteArray`：`sodium_malloc` + `mlock`，析构时 `sodium_memzero` + 解锁（**Phase 1 的 P1-T14 已落地，Phase 2 直接复用；路径取舍见 D8**） | P2-T1 |
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

### 5.5 Phase 2 的偏差

| # | 偏差 | 证据 | 处置 |
| :--- | :--- | :--- | :--- |
| D8 | **P2-T2 的安全内存容器留在 `src/core/SecureByteArray`，没有迁到计划书写的 `src/credential/memory/`** | Phase 1 的 P1-T14（密码认证）已经需要它，于是它落在 `src/core/`，并被 `protocols`（`SshConnectionOptions::password`）、`ui`（`QuickConnectDialog` / `TerminalSession` / `WorkspaceTabs`）共同依赖。而 `credential` 是**依赖** `core` 的一层（`openscrt_credential` 链 `openscrt_core`）：把基元搬到上层，会让 `core` 反过来依赖 `credential`，也逼 `protocols` / `ui` 改依赖方向 | **复用，不复制。** `src/credential/` 只放 KeePass 专有的东西（KDBX 解析、HTTP 客户端、条目选择器）。P2-A7 的两条断言落在 `tests/unit/SecureByteArrayTest.cpp`：① 析构后 `VmLck` 页计数回到基线（**只有 Linux 有这个观测手段**——`/proc/self/status`；macOS / Windows 无对应计数器，测试显式 skip 并说明原因，不假装验过）；② `mlock` 失败时走降级路径而不是崩溃（`locked()` 诚实性） |
| D9 | **Argon2 派生走 OpenSSL 的 ARGON2 KDF，而不是路线图写的 libsodium** | 路线图 §3.5 写的是"libsodium（`crypto_pwhash_argon2id`）"。但 **libsodium 没有 Argon2d**：它的常量只有 `crypto_pwhash_ALG_ARGON2I13` 与 `crypto_pwhash_ALG_ARGON2ID13`，`crypto_pwhash_ALG_ARGON2D13` 不存在。而 **KeePass 默认写的就是 Argon2d**（真实样本的 `$UUID` = `EF636DDF…`），所以"用 libsodium"这条路在 KDBX 4 上根本走不通 | 改用 **OpenSSL 3.2+ 自带的 ARGON2 KDF**（`EVP_KDF_fetch` 的 "ARGON2D" / "ARGON2I" / "ARGON2ID" 三个变体都有）。参数：`pass`（复合密钥）、`salt`、`iter`、`lanes`、`version`，以及 `memcost`——**它的单位是 1 KiB 块**，所以文件里的 `M`（字节）要 ÷ 1024（`67108864` → `65536`）。CI 装的是 **OpenSSL 3.5.2** ✔。libsodium 仍用于 `sodium_memzero` 与 ChaCha20 |

### 5.6 任务完成度与验收取证

每一条都指到一次真实的 CI run 或一次可复现的观测；**没有证据的那一格是空的**。

| 任务 / 验收 | 状态 | 证据 |
| :--- | :--- | :--- |
| P2-T1 凭据提供者抽象 | ✅ | `src/credential/ICredentialProvider.h`：明文只走 `core::SecureByteArray`；`takeSecret()` 把字节移交给调用方并清空来源；查不到就说查不到（not-found / cancelled / unavailable / error），不返回"空密码冒充成功"；`lock()` 在接口上，自动锁定（P2-T7）才不用向下转型。10 个用例，run **#40122964427** 三平台全绿 |
| P2-T2 安全内存容器 | ✅ | 复用 `src/core/SecureByteArray`（见 D8）：`sodium_malloc` + `sodium_mlock`，析构时 `sodium_memzero` + 解锁。run **#40123563712** |
| P2-A7 安全回归 | ✅ **观测到的** | run **#40123563712** 的 Linux leg 日志（CI 单独跑这一条用例，数字进日志）：`VmLck before=0 kB, during=4 kB, after=0 kB, mlock succeeded=yes` —— 锁的页在缓冲活着时确实被锁上，析构后**回到 0**。macOS / Windows 无此观测手段，那两个平台只验降级路径（`locked()` 诚实性），**不假装验过** |
| P2-T3 KDBX 文件头解析 | ✅ | `src/credential/kdbx/KdbxHeaderParser.cpp`：魔数（3.1 的第二签名是 `0xB54BFB66`、4.x 是 `0xB54BFB67`）、TLV 字段、VariantDictionary、算法 UUID、压缩标志、KDF 参数。**字段长度宽度随版本变**（3.1 是 UInt16，4.x 是 Int32）——这是最容易写错且不报错的一处。夹具是**真实 KeePass 数据库的头 253 字节**（逐字节照抄），不是本项目自己产出的。run **#40127994271** |
| P2-T4 KDBX 4 读取 | ✅ | `src/credential/kdbx/Kdbx4Reader.cpp`：复合密钥 → Argon2d/Argon2id → 主密钥 → 头部 SHA-256 → 头部 HMAC → **每块各自的 HMAC** → AES-256-CBC/ChaCha20 → GZip。**每一步都验签**，所以错密码是报错而不是"解出一堆垃圾"。读的是**真实数据库**（`tests/fixtures/kdbx/blank_database.kdbx`，公开夹具，来源与哈希写在其 README 里），并与参考实现对拍。run **#40127994271**（`unit_kdbx4_reader` 3.71 s 通过——含 64 MiB Argon2d 派生） |
| P2-T5 KDBX 3.1 读取 | ✅ | `src/credential/kdbx/Kdbx3Reader.cpp`：3.1 的三处不同（字段长度 **UInt16**、单一加密流 + **stream start bytes** 校验、块里是普通 **SHA-256** 而非 HMAC、且**没有内层头部**）都写在代码注释里。夹具由 `tools/make_kdbx_fixture.py`（**本项目自己的 Python 写入器**）生成，**主密码 + 密钥文件**一起解锁，条目口令按 KeePass 的做法**受保护**；夹具的合法性由**独立实现**（pykeepass）验证并记录在夹具 README。run **#40130212759**（`unit_kdbx3_reader` 通过） |
| P2-T6 XML 索引 + 保护流 | ✅ | `src/credential/kdbx/KdbxXmlIndex.cpp`（pugixml）+ `KdbxProtectedStream.cpp`（libsodium）。索引**不解密**：口令保持数据库里的密文形态，**只有要用的会话**才在锁定内存里还原（路线图要求的"只在发起连接的瞬间还原"）。保护流的形态是对**真实 KDBX 3.1 文件量出来的**（按 KeePass 的 C# 源码推的那个布局**解不开真实文件**）。run **#40131694513**（`unit_kdbx_xml_index` 通过：索引里看不到明文，按需还原得到 `s3cret`） |
| P2-T7 热监听与自动锁定 | ✅ | `src/credential/kdbx/KdbxVault.h`：持有主密钥与索引、**盯着文件**、**闲置自动上锁**。文件变了就重读（文件被换掉时把 watch 补回去）；闲置超时或窗口主动上锁就丢掉密钥材料，只剩文件名。口令在 vault 打开期间**存于锁定内存**（文件变了要重读，需要它），上锁时与主密钥一起抹掉。run **#40136172244**（`unit_kdbx_vault` 8 个用例通过，含"文件变了会重读"与"闲置会自锁"） |
| P2-T8 KeePassHTTP 客户端 | ✅ | `src/credential/keepasshttp/`：`associate` / `test-associate` / `get-logins` 三个请求，敏感字段 **AES-256-CBC + PKCS#7 + base64**，答案用**响应自己的 nonce** 加密（先验 verifier 再解密条目）。传输层是裸 socket（凭据层是同步的，接口也写明 lookup 要阻塞、跑在工作线程上）。测试是**端到端**的：一个 mock 插件**开自己的线程**、走真实 socket 解密客户端发来的东西、加密它回的东西。run **#40139977099**（`unit_keepasshttp_client` 5 个用例通过，26/26 全绿） |
| 关联信息的系统凭据库 | ✅ **三个平台** | `src/credential/SecretStore.{h,cpp}`：Windows 走 **Credential Manager**（`CredWriteW`/`CredReadW`），macOS 走 **Keychain**（`SecItemAdd`/`SecItemCopyMatching`），Linux 走 **Libsecret**（freedesktop.org 的 Secret Service，GLib 库，用 pkg-config 找）。接口只有"一个名字一个秘密"，不做查询语言：一个能装很多东西的地方，早晚会装进不该装的东西。run **#40196044623** 的日志里能看到 Windows 与 macOS **真的写了又读回来**（`TheSystemStoreKeepsASecret` Windows 16 ms / macOS 15 ms，**没有 skip**——这一步专门用 `ctest -V` 跑，就是为了让"跑过"和"验过"分得开）。**Linux 上 CI 没有 secret service 守护进程**，那里的用例会说明并跳过，**不假装验过** |
| P2-T8 的提供者与界面接线 | ✅ | `src/credential/keepasshttp/KeePassHttpProvider.cpp`：KeePassHTTP 客户端套进凭据接口，**双模这才真的是双模**——本地 .kdbx 与运行中的 KeePass 都从同一个接口取口令，会话层不知道口令从哪来。会话绑定多了一种 `keepass-http`（按主机匹配）；Tools 菜单多了一个 **Pair with KeePass...**（插件自己在 KeePass 里弹确认框），配对信息存进系统凭据库。run **#40196044623**（`unit_keepasshttp_client` 的提供者用例通过：口令从 mock 插件那里解出来，进的是锁定内存） |
| P2-T9 条目选择器与四策略绑定 | ✅ | `src/credential/kdbx/KdbxCredentialResolver.cpp`（**纯逻辑、无 Qt**，可无头测）+ `src/ui/workspace/KeePassSelectorDialog.cpp`（选择器，**只给 uuid，从不碰口令**）。四策略：Session Saved / Prompt Every Time / KeePass by UUID / KeePass by Host（自动匹配会剥掉 scheme 与端口，`.example.local` 这类也算同一台机器）。**绑定本身是配置**，所以 `SessionProfile` 与 INI 往返都保住它（`cred-source` / `cred-uuid` / `cred-host`）；**口令仍然不在配置里**。run **#40141633909**（`unit_kdbx_credential_resolver` 9 个用例 + `unit_config_manager` 往返用例通过） |
| P2-T10 界面布局（第一部分：Command Window） | ✅ | `src/ui/workspace/CommandWindow.cpp`：底部命令条，默认发给当前会话，勾上 **Broadcast** 则发给全部会话。**顺序是这条功能的要点**：会话按打开顺序依次收到，所以"对所有机器执行"在每台机器上是同一个顺序。run **#40141633909**（`unit_command_window`：20 个会话全部收到、字节一致、顺序一致——派发逻辑）＋ run **#40217841310**（`unit_command_window_broadcast_ssh`：**20 个真 SSH 会话**，同一条命令、同一个顺序、**20 个都答了回来**，3.53 s，日志里写着 `[p2-a6] 20 real SSH sessions, 20 of them ran the command`）。macOS / Windows 的 CI 里没有服务器，那里跳过 ✔（R15）✔ |
| P2-T10 界面布局（第二部分：四向分屏 + 多分组 Button Bar） | ✅ | `src/ui/workspace/WorkspacePanes.cpp`：窗格是一棵 splitter 树——向右分屏把新窗格放在正在用的那个旁边，向下分屏放在下面；当上面的 splitter 是另一个方向时，两个窗格进一个嵌套的 splitter，于是左列可以再分而不动右列。**Tile 是四格**，而且是**移动窗格而不是搬会话**。跨窗格的会话保持**一个全局顺序**（广播要靠它才可复现），已关闭会话的指针**从不读**（它的"closed"信号到达时对象已经没了，靠名字找）。`src/ui/workspace/ButtonBar.cpp`：底部的按钮行，**多分组**（右键标签栏建组、右键组内空白处建按钮），每组一个模型、存在会话文件里、随窗口回来。发送串认 SecureCRT 的转义符（`\r` `\n` `\p` `\v`），**`\p` 是一次独立的步骤而不是 sleep**（sleep 会把窗口冻住一秒）。run **#40144230620**（`unit_workspace_panes` + `unit_button_bar` 通过） |
| 凭据接线（绑定 → 会话） | ✅ | 这一段之前是**接不上的**：绑定是配置，但 `effectiveProfile()` 只解析 host/port/user/protocol/key，把三个凭据字段丢了——会话打开的那一刻它看起来像没绑定，注入口根本不会触发。现在绑定随会话的**有效配置**一起走（**不从祖先继承**：子会话多半连的是另一台机器，继承父会话的 KeePass 条目等于把父会话的口令送到那台机器上去）；树要能改写（`update()` 原地替换，不打乱顺序）；选择器接进菜单（Tools > Bind Session to KeePass Entry...）。取密分三种结果：**取消 / 让服务器问 / 有口令**，认不出来的绑定**报错**而不是悄悄连上没有口令。run **#40144230620** |
| P2-T11 SecureCRT 配置导入器 | ✅ | `src/config/importers/SecureCrtConfigImporter.cpp`：读 SecureCRT 的 `Sessions` 目录树（文件夹 = 目录，每个文件夹一个 `__FolderData__.ini` 列出会话与子文件夹）。**格式与我的记忆不符**：SecureCRT 的会话文件不是 `[Connection]` 那种 INI，而是**扁平的带类型前缀的键值**（`S:"Hostname"=...` 字符串、`D:"[SSH2] Port"=00000016` 十六进制数字）。三方对齐后才动手：SecureCRT 自己产出的真实文件 + 两个**互相独立**的实现（RustConn 的导出器、oryxis 的导入器）；事实与出处记在 `docs/evidence/securecrt-config-notes.md`（真实文件因来源仓库无许可证而**不入库**，本地留副本核对）。**口令不导入**（用安装实例的密钥加密，没有可拿的东西），会话照常导入并说明。run **#40145160590** |
| P2-A1 加载 .kdbx + 双击自动注入 | ✅ **机器可判** | KDBX 3.1（主密码 + 密钥文件）与 KDBX 4.x 各一份夹具都能打开（run #40127994271 / #40130212759）；**从 KDBX 里取出口令真登录一台 SSH 服务器**——CI 的 Linux leg 起的是一台真 sshd，测试用**主密码 + 密钥文件**打开夹具、从条目里取出口令、再用它登录，服务器上的账号口令就是条目里的那个（run **#40194953262** 的 `unit_main_window`）。链路：绑定 → 有效配置 → 取密 → 交给传输层（run #40144230620） |
| P2-A2 KeePassHTTP 配对 | 🟡 | **mock 一半有**：associate / test-associate / get-logins 全流程，端到端（mock 插件开自己的线程、走真实 socket）；**提供者与界面接线也有**（Tools > Pair with KeePass、会话绑定 `keepass-http`、配对信息进系统凭据库）。**真服务端一半没做**：需要一台装了 KeePass 2 与 KeePassHttp 插件的机器，属【人工】 |
| P2-A3 内存无明文 | ✅ **实测** | `tests/unit/MemoryScanTest.cpp`：把一个**随机**标记放进 `SecureByteArray`，然后**读进程自己的内存**（Linux `/proc/self/mem` + `pread`、macOS `mach_vm_region` + `mach_vm_read_overwrite`、Windows `VirtualQuery` + `ReadProcessMemory`），只读**可写**的映射——明文只可能写在可写内存里，库文件占大头且读一遍要 600 s。控制断言先证明扫描**确实看得见**那个缓冲区（否则这条用例可以什么都没看就"通过"），缓冲一擦掉，标记必须消失。run **#40211959352**（**三个平台都真跑过**：`unit_memory_scan` macOS 1.54 s / Linux 0.02 s / Windows 0.01 s，**没有 skip**）。macOS 这条腿是补上的，坑记在 `docs/POSTMORTEM.md` 9.8：出参传 `nullptr` 会被 MIG 生成的客户端桩直接往里写 → 段错误，**三轮才定位**（探针一行都没打出来 = 崩在被调用内部） |
| P2-A4 解锁不卡 UI | ✅ **实测** | 派生在**后台线程**上做（`KdbxVault::openAsync`），窗口上是带进度的对话框 + 嵌套事件循环，所以解锁期间窗口还在响应。用例量的是**事件循环最长没回应的时长**（取的是上界：连两轮之间的休眠也算进去）：打开一个 KDBX 4 库（Argon2 要几秒），主线程最长**不超过 50 ms**。run **#40201582144**（`unit_kdbx_vault` 通过） |
| P2-A5 解锁结果缓存 | ✅ | vault 数着派生的次数（`derivationCount()`）：开库派生一次，之后**十个会话**依次取同一条目的口令，计数**仍是 1**。文件变了会再读一次、再派生一次——那种情况下第二次派生是对的。run **#40145160590** |
| P2-A6 广播到 20 个会话 | ✅ **对着真服务器** | 判据原文是"向 **20 个并发 SSH 会话**同步下发命令" ✔。先前只做到"20 个会话替身"（`unit_command_window`：20 个 sink，字节一致、顺序一致）✔——那证明的是命令窗口派发的逻辑，**证明不了二十台机器真的跑了命令** ✔。现在补的是真的：run **#40217841310** 的 Linux 腿开 **20 个真 SSH 会话**（对着 CI 那台 sshd），把同一条命令广播过去，**每一个会话都把结果答了回来** ✔——日志里写着 `[p2-a6] 20 real SSH sessions, 20 of them ran the command` ✔（该用例单独跑一遍，3.53 s）✔。macOS / Windows 的 CI 里没有服务器，用例按同样方式跳过 ✔（R15）✔ |
| P2-A8 文件变了自动刷新 | ✅ | run **#40136172244**（`unit_kdbx_vault`："文件变了会重读"与"修改前后的条目查询结果分别正确"） |
| KDBX 字节级事实 | ✅ | `docs/evidence/kdbx-format-notes.md`：每条事实带出处（官方规范 / KeePass 参考实现 / 独立实现 pykeepass / 真实样本十六进制）。**记错的七次**（版本号 `0x03040000`、3.1 的第二签名、复合密钥少一层哈希、AES-KDF 的形态、块 HMAC 的键、3.1 没有内层头部、保护流要用 `_xor` 变体）都记在那份文件里，作为"格式跟 Win32 默认行为一样，凭记忆必错"的证据。**其中三次是真实文件纠正的**，不是文档纠正的 |
| SecureCRT 会话文件格式 | ✅ | `docs/evidence/securecrt-config-notes.md`：每条事实带出处（**真实文件** / 两个独立实现）。**记错的三次**（`[Connection]` 那种 INI、`HostName` 的拼写、十进制端口）都记在那份文件里，作为同一件事的第二个证据 |

### 5.7 Phase 2 阶段出口归档（§9.2 要求"缺一不可"的那五样）

**① CI 运行链接**

| 腿 | run | 结论 |
| :--- | :--- | :--- |
| format (ubuntu-24.04) | [#40217841310](https://github.com/WengerChan/openSCRT/actions/runs/40217841310) | ✅ |
| build-test (ubuntu-24.04) | 同上 | ✅ 34 个测试目标全绿 |
| build-test (windows-2022) | 同上 | ✅（Windows 走 Qt 6.7，见决定 0001） |
| build-test (macos-14) | 同上 | ✅（arm64） |

**② 覆盖率报告**（run #40217841310 的 Linux leg，报告以 `coverage-linux` artifact 上传）

| 范围 | lines | functions | branches |
| :--- | :--- | :--- | :--- |
| 合计（core + terminal + protocols + credential） | **78.2%**（3150/4026） | 92.1%（362/393） | 65.7%（2084/3173） |
| `src/core` | 88.7%（126/142） | 90.9%（30/33） | 70.3%（52/74） |
| `src/terminal` | 82.7%（1112/1344） | 93.8%（150/160） | 71.0%（648/913） |
| `src/credential` | 75.5%（1335/1768） | 90.9%（120/132） | 63.9%（964/1508） |

CI 门禁是 lines ≥40% ✔，**通过** ✔。但**计划书写的 Phase 2 目标是 ≥80%，实际 78.2%，没达到** ✔
——原因写在这里，而不是藏起来：**在把 `src/credential` 计入之前，这个数字是 80.8%** ✔
（run #40212467575），而那个 80.8% 量的是 **Phase 1 写的代码** ✔：Phase 2 新增的整整一层
（KDBX 读取器、KeePassHTTP 客户端、系统凭据库）**根本没有进过滤器** ✔。CI 的过滤器已经补上
（run #40217255583），逐库统计也进了流水线 ✔（见 R13）✔。

**③ 性能基线对比表**（Phase 2 vs Phase 1 基线；两个 run 都在 Linux leg 的 perf 层实测）

| 指标 | Phase 1 基线 | Phase 2（run #40217841310） | 差 | 结论 |
| :--- | :--- | :--- | :--- | :--- |
| 解析 100 万行 | 9,043.1 ms（9.043 µs/行、110,581 行/秒、1,847 行/帧） | 9,220.1 ms（9.220 µs/行、108,459 行/秒、1,811 行/帧） | +1.96% | ✅ 无回归 |
| 全屏重绘 | 1.31 ms（765.5 FPS 等效） | 1.33 ms（751.1 FPS 等效） | +1.5% | ✅ |
| 增量 / 全屏重绘比 | 0.90 ms vs 6.17 ms（6.9×） | 0.86 ms vs 6.04 ms（7.0×） | — | ✅ |
| 600 ms 饱和输出 | 38 帧 | 38 帧（63.3 FPS） | 一致 | ✅ |
| 流式输出内存增长 | 4.0 MB | 4.0 MB | 一致 | ✅ |
| scrollback push | 0.341 µs/行 | 0.310 µs/行 | −9.1% | ✅ |
| scrollback 查找 p99 | 0.661 µs | 0.581 µs | −12% | ✅ |
| scrollback 内存 | 16.1 MB / 131,072 行 | 16.1 MB / 131,072 行 | 一致 | ✅ |
| Widget 帧时间 p95 | 1.11 ms（run #39979970617） | 0.70 ms（均值 0.62、最坏 0.74） | −37% | ✅ |

**绝对值只能同 run 内比** ✔：同一个二进制在连续两轮里给出 9.220 与 10.660 µs/行 ✔
（run #40217841310 与 #40217255583）——**15.6% 的差距来自共享 runner 的负载，不是代码** ✔。
被测的两条路径（`src/terminal`、`src/ui/renderer`）**从 Phase 1 到现在一个字节都没改** ✔
（`git diff 9a57b05..HEAD -- src/terminal src/ui/renderer` 为空）✔。这正是 P1-A3c 写下的
判据口径：**比值对负载不敏感，绝对值只用于同一 run 内** ✔。

**④ 人工验收记录**

**空** ✔。本阶段没有一条【人工】判据留下过记录 ✔（P2-A2 的真服务端那一半在 ⑤ 里）✔。
**没有把没做的事写成做了** ✔。

**⑤ 遗留问题登记（Phase 2）**

| # | 遗留问题 | 类别 | 是否阻塞下一阶段 |
| :--- | :--- | :--- | :--- |
| R12 | **P2-A2：真 KeePass 2 + KeePassHttp 插件配对没做**——mock 那一半是端到端的（associate / test-associate / get-logins 全流程），真服务端那一半要一台装了插件的机器 | 人工 | 否——Phase 3 的协议栈不依赖它 |
| R13 | **覆盖率目标没达到**：合计 78.2% 低于计划书写的 Phase 2 ≥80%；三个核心库也都低于计划书写的 ≥90%（core 88.7% / terminal 82.7% / credential 75.5%）。过滤器已补上 `src/credential`（run #40217255583），剩下的差额是**补测试**的事 | CI | 否——但 Phase 3 开工前要补账 |
| R14 | **Linux 上的 Libsecret 运行时没验**：CI 没有 secret service 守护进程，用例在那里跳过并说明 | 人工 | 否 |
| R15 | **P2-A6 的真并发广播只在 Linux 腿上验过**：macOS / Windows 的 CI 里没有 SSH 服务器，用例按同样方式跳过（0.02 s，与其它 SSH 用例一致）；Linux 腿是 20 个真会话、3.53 s、日志里写着 `20 real SSH sessions, 20 of them ran the command` | 人工 + CI | 否——但"三端都验过"这句话不能写 |
| R16 | **性能绝对值跨 run 不可比**（同一份代码两轮相差 15.6%），判据只能用车内比值 | CI-Perf | 否——已写进归档与 P1-A3c |
| R17 | **P1-A7 三端真机人工验收**与 **Windows 1607 真机验收**仍未做（Phase 1 的 R1 / R10 已挂账，1607 是硬要求） | 人工 | 否（按 D7 暂不执行）——但 1607 那笔账**发布前要还** |

**结论：Phase 2 工程完成，阶段出口未签收。** T1–T11 全部落地，P2-A1 / P2-A3 / P2-A4 / P2-A5 /
P2-A6 / P2-A8 满足（其中 P2-A6 现在是**对着 20 个真 SSH 会话**证明的，不再是替身）✔；
P2-A2 只满足 mock 那一半 ✔；P2-A7 的平台相关观测（macOS / Windows 的锁页计数）**没有等价手段**，
如实跳过 ✔；覆盖率**未达计划目标** ✔（R13）✔；人工验收记录**空** ✔。
**因此阶段出口未签收** ✔——与 Phase 1 同样的措辞、同样的理由：**能由机器判的都判完了，
剩下的要人在真机上做** ✔。**放行 Phase 3 的口径待用户决定** ✔：按 AGENTS.md 的门禁纪律
（"门禁的标准不满足时，先问用户怎么收口，不要替他决定"）✔，**D7 的授权只覆盖"P1-A7 暂不执行"
与"Phase 2 照常开工"，不覆盖 Phase 2 自己的出口** ✔ —— 这句话不能由执行者代写 ✔。
而 R12 / R13 / R17 三笔账，无论怎么收口，都得在对应的地方还上 ✔。

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
| Phase 1 | 5 | 2 | 2 | 连接真实服务器 + 刷屏 ≥60 FPS；**出口未签收**——两个【人工】项里，P1-A7 按 D7 暂不执行，Phase 2 照常开工 |
| Phase 2 | 6 | 1 | 1 | 20 会话广播 + 内存无明文；**出口未签收**——P2-A2 的真服务端一半、覆盖率目标与三端真机/1607 真机验收仍未做（见 §5.7 的遗留登记 R12/R13/R17）；**是否放行 Phase 3 待用户决定**（D7 只授权了 P1-A7 的暂缓与 Phase 2 的开工，不覆盖 Phase 2 自己的出口） |
| Phase 3 | 5 | 1 | 3 | Zmodem 100MB MD5 一致 |
| Phase 4 | 5 | 2 | 1 | 50 正则下帧率衰减 ≤5% |
| Phase 5 | 5 | 2 | 3 | 10 官方脚本零修改通过 + 72h 稳定 |