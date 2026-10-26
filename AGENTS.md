# AGENTS.md

openSCRT — C++20 + Qt 6 的 SecureCRT 对标终端。读代码前先读
[`securecrt_native_architecture_and_roadmap.md`](securecrt_native_architecture_and_roadmap.md)（技术选型的唯一来源）、
[`docs/DEVELOPMENT_PLAN.md`](docs/DEVELOPMENT_PLAN.md)（任务与验收标准）、
[`docs/POSTMORTEM.md`](docs/POSTMORTEM.md)（已犯过的错，见下）。

## 硬约束

0. **CI 红着的时候不要继续推 main。** 要么等这一轮绿了再推下一个提交，要么在提交信息里
   写明"这次推送为什么是安全的"。本仓库没有 PR 保护，`main` 的唯一门禁就是 CI。
   **纯文档提交不产生 run**：CI 触发器带 `paths-ignore`（`**.md`、`docs/**`、`.proxy.sh`），
   推完没有 run 是设计如此，不是 CI 没跑。查某个提交自己的结论用 `?head_sha=<sha>`。
1. **验证只走 CI。** 本地不编译、不跑测试、不装构建工具。编译/测试/覆盖率/性能基准
   一律以 GitHub Actions 的结果为准。本地只写代码、格式化、提交。
2. **需要 Python 时用 uv**，cache 目录写项目内可写路径（`UV_CACHE_DIR=$PWD/.cache/uv`）。
   不要用系统 `python3`/`pip3` 装包。
3. **访问外网走 `./.proxy.sh`**（`github.com:443` 直连常超时）。CI 里所有网络调用同样走代理环境。
   `api.github.com` 直连也能通，但外网访问统一走代理，别给"直连更快"找借口。
4. **依赖只经 `vcpkg.json` 声明。** CI 从中派生一份更小的清单（`.github/scripts/ci-vcpkg-manifest.py`
   的 SKIP 表）。**开始 link 某个 port 时，必须在同一个 PR 里删掉它的 SKIP 条目**，
   否则 configure 会报 not found。
5. **一个任务号 = 一个提交。** 本仓库**直推 `main`**，没有分支也没有 PR（与早期
   "一任务号一 PR"的写法不同，以本文为准）。`main` 受保护，合并前必须 CI 全绿。
6. **告警零容忍**：`-Wall -Wextra -Wpedantic -Werror`（MSVC `/W4 /WX`）。
   不要为了消告警放弃有意的设计（例如 `SpscRingBuffer` 的 `alignas(64)`），
   正确做法是局部 `#pragma warning(disable)` 并写清理由。
7. **平台无关代码**：CI 脚本禁用 `mapfile`/`readarray`/数组展开，用 `while IFS= read -r`
   并剥 `\r`；写给 CI 读的文件一律 `newline="\n"`。
8. **不要在用户的机器上启动应用或改动机器状态。** agent 的 shell 起的进程**杀不掉**
   （`killall`/`pgrep`/`osascript` 都被沙箱拒），只能让用户自己收尾。要验证"程序在这台机器
   上能不能起来"，用 CI 的 runner，或让用户自己跑。

## 纪律的自查（规则 0 / 2 / 3 都被违反过，所以写成检查）

2026-10-24 这一轮，规则 0、2、3 三条**同时被违反**：CI 还红着就推了两次 main、用系统
`python3` 改文档、外网请求（改 release body）没走代理。三条规则都写在上面，违反了也没有
任何东西拦下来——**写下来的纪律如果没有检查点，就只是愿望**。所以每条都配一个能做的动作：

| 规则 | 提交前/发版前实际要做的 |
| --- | --- |
| 0（红着不推） | **推之前跑 `tools/pre_push_check.sh`**（它读最近一次真实 run 的结论：绿就放行；红或还在跑，就检查这次提交的信息里有没有"为什么安全"那句，没有就退出非零）✔。**它的判定会追加到 `docs/push-log.md`** ✔——那一行随下一个提交进仓库，**表里缺哪一次推送，哪一次就是没走检查点的** ✔。手工查法：`curl -s -H "Authorization: Bearer $TOKEN" "https://api.github.com/repos/WengerChan/openSCRT/actions/runs?per_page=1" \| jq -r '.workflow_runs[0] \| "\(.head_sha[0:7]) \(.status) \(.conclusion)"'`。**红或还在跑，就先等**；实在要推，提交信息里写明为什么安全。**注意**：CI 触发器现在有 `paths-ignore`，纯文档提交**不产生 run**——所以这条查的是"最近一次真实 run"，它对应的 SHA 可能不是你要推的那个提交；要看某个提交自己的结论，就用 `?head_sha=<sha>` 查。 |
| 2（Python 走 uv） | 要跑 Python 就 `uv run python ...`（`UV_CACHE_DIR=$PWD/.cache/uv`），不要 `python3`；改文件优先用编辑工具而不是脚本。 |
| 3（外网走代理） | 每一条访问外网的命令都 `source ./.proxy.sh`——**包括 `api.github.com`**，哪怕它直连也能通。 |
| 8（不动用户机器） | 验证一律走 CI runner；不要在用户的桌面启动应用。 |
| 格式门禁（clang-format） | 推之前把新写的 C++ 过一遍**钉住版本**的 clang-format——CI 的 format job 跑的就是 `clang-format==23.1.3` 的 `--dry-run --Werror`，用别的版本格式化等于把代码改成另一个样子，CI 照样红。`uv tool install clang-format==23.1.3` 后 `clang-format -i <files>`；沙箱不让写 `~/.local/share` 时装到项目内：`UV_TOOL_DIR=$PWD/.cache/uv-tools UV_TOOL_BIN_DIR=$PWD/.cache/uv-bin uv tool install clang-format==23.1.3`，然后用 `.cache/uv-bin/clang-format`。**格式化是规则 1 明文允许的本地动作**，不是"本地不能做"。 |

## 排障纪律（来自 21 轮 CI 排障的复盘）

- **先分类**：环境 / 编译期契约 / 运行时逻辑。分错类，后面全是浪费。
- **新写的代码提交前，对着头文件核对类型与命名空间。** 本仓库本地不编译（规则 1），
  所以一个 `terminal::` 写成 `openscrt::terminal::`、一个 `[[nodiscard]]` 的返回值没接住，
  都是**一整轮 CI**的价钱——第三、四轮 14 次失败里有 5 次是这个。改动面越大，一轮里能藏的
  笔误越多。
- **Qt 的类里埋着会撞车的成员名。** `QWidget` 有个 protected 成员就叫 `data`
  （`qwidget.h`），所以在 `QWidget` 的子类里写一个叫 `data` 的局部变量或参数，
  `-Wshadow` 会直接拦下（`-Werror` 下就是编译失败）。缓冲区叫 `payload` 或 `bytes`。
- **Qt 的 API 名字要查，不能凭印象写。** `QSplitter` **没有** `removeWidget`（只有
  `replaceWidget` / `insertWidget`）；把窗格从 splitter 里拿掉是把槽位交还成
  `replaceWidget(index, nullptr)`。凭记忆写出来的 Qt 调用几乎必错，而本仓库本地不编译，
  一次写错就是一轮 CI。
- **成员的类型名不进入作用域。** 窗口里有个 `credential::kdbx::KdbxVault` 成员，
  写 `&KdbxVault::opened` 是不行的，要写全 `&credential::kdbx::KdbxVault::opened`。
- **库把 Qt 链成 PRIVATE 时，用到 Qt 的测试要自己点名 Qt。** `openscrt_config` 的
  PRIVATE 依赖不会传给测试，测试不会因此拿到 Qt 的 include 路径——症状是
  `No such file or directory: 'QDir'`。
- **编译错误要读 note 行，不是只读 error 行。** clang 的 `no matching function for call to
  'X'` 下面跟着 `note: candidate function not viable: requires 7 arguments, but 6 were
  provided` / `no known conversion from 'A *' to 'B *'` —— **它把要什么说得很清楚** ✔。
  同一个错误（macOS 的 `mach_vm_region`）连着三轮，每轮的 note 都指向一个具体的差别 ✔：
  参数个数 ✔、联合体是按值还是按指针 ✔、地址类型是 `vm_` 还是 `mach_vm_` ✔。
  **别猜 API 签名**：去取头文件原文 ✔（Windows 的 `wincred.h`、Apple 的 SDK ✔），
  但**镜像里的 SDK 可能是旧的** ✔ —— runner 上装的那份才是裁判 ✔。
- **一轮 CI 只报第一个错误。** ninja 在第一个失败处停下，别的目标根本没编，
  所以**后面的错误藏在后面**。一次失败要把日志里**所有**编译错误修完再推；
  同一批错误在三条腿上表现不同，先怀疑"谁先失败"，不是"平台不一样"。
- **等待子进程必须有期限。** `waitpid(pid, &status, 0)` 这种没有期限的等待，
  碰上不听话的子进程就会把**画窗口的线程**一起挂死（macOS 上真发生过：shell 不因
  SIGHUP 退出，关闭会话就卡了十分钟）。期限到了就杀掉它——还没被 reap 的子进程是僵尸，
  内核不会把僵尸的 pid 发给别人，所以 kill 不会误伤。
- **凡是等待都要有期限，`wait` / `join` / `wait_for` 都一样。** 注释里写"这个等待不会挂"
  是一句**需要证据的话**，不是保证（本仓库真有过：等一个"已经在路上的回调"结束的
  `condition_variable::wait` 没有期限，macOS 上把用例挂满了 600 秒）。
- **关闭资源也有顺序，而且顺序要分平台想。** 读线程还在用着的描述符**不能先关**：在阻塞的
  read 底下关描述符是未定义行为，那个编号还可能已经被别人拿走，读线程于是等在一个跟它
  毫无关系的文件上，`join` 永不返回（POSIX 分支现在先停读线程、再关描述符）。**Windows
  的伪控制台反过来**（先关控制台、再停读线程，见 POSTMORTEM 8.1）——**两边的顺序别抄串**。
- **Qt 的告警不是噪音，是证据。** 日志里一句 `QSplitter::replaceWidget: Widget can't be null`
  直接指出了三处 bug 里的一处：那行 API **根本不管用**（Qt 拒绝了空指针），所以那段代码
  从来没有做过它声称的事（POSTMORTEM 9.9）。
- **格式化器能改坏代码，不只是改风格。** clang-format 会把同一类里的 include 按字母排序，
  而 Windows SDK 的头文件**靠包含顺序**：`wincred.h` 用 `windows.h` 的类型却不自己包含它，
  排错顺序就编不过（症状是"unknown override specifier"，看着像类型写错）。
  **办法**：两个 include 之间放一行注释，clang-format 就不会重排它们（仓库里
  `LocalPtyChannel.cpp` 的 `windows.h` / `consoleapi.h` 就是这么写的）。
- **"工具报告成功"不等于"产物在预期位置"。** 下游找不到时，第一反应是 `ls -R <路径>`，
  顺序永远是：产物存在吗 → 路径对吗 → 工具怎么找。**跳过第一步是最大的浪费来源。**
- **同一处失败连修两轮而症状不变 → 停止推理，进观测模式。** 加一行 `ls` / `printenv`
  比再猜一轮便宜得多。临时探针用完要删。
- **子进程的输出出现在父进程的日志里 = 子进程根本没走通道。** 排查"子进程没有输出"时，
  第一件事是确认它把输出写到了**别处**：横幅/提示符出现在 CI 日志里，说明它用的是继承
  来的标准句柄，通道压根没接上。"没有数据"有无穷多种原因，"数据去了别处"只有一个原因。
- **Win32 的默认行为不要靠记忆推断，去读参考实现。** `CreateProcess` 的七个参数默认值
  与 `STARTUPINFO` 的标志位组合出来的行为，凭记忆必错——本仓库在 ConPTY 上连续猜错
  三轮（管道句柄属性、释放时机、`STARTF_USESTDHANDLES`），最后是照着 Windows Terminal
  的 `ConptyConnection.cpp` 一次读对的。**先找官方 sample / 参考实现，再写代码。**
- **假设要写成可判定的实验，而不是一次一改。** 四个 spawn 形状并排跑一次，得到的结论
  比连续三轮"改一处、跑一轮、猜一轮"便宜得多。临时矩阵测试用完要删。
- **改"看起来更合理"的顺序之前，先问这段代码现在靠什么活着。** 旧代码里的怪癖常常是
  踩坑后留下的支撑点：本仓库的 Windows 关闭路径必须**先关伪控制台、再停读线程**——
  关闭时 console host 会往输出管道写最后一帧并等它被消费，读线程先停就变成死锁
  （run #39997401173 / #40002061349，Windows 用例超时 600 s）。
- **"我刚改的地方"不等于"根因"。** 同一个症状在两轮 CI 里都出现，就说明它不是这一轮
  改出来的。先复现一次旧版本再下结论。
- **挂死类问题，先让程序说出它停在哪一步。** 生命周期探针（进入/退出每个阻塞点各打印
  一行）一行日志就能定位，比推理几轮便宜。临时探针用完要删。
- **同步 I/O 的"等待"在不同实现里语义不同。** `poll()` 有超时所以能醒，
  `PeekNamedPipe` 在同步句柄上会阻塞，而 `ClosePseudoConsole` 会**等管道被消费完**。
  移植或重构等待逻辑时，先确认它到底会不会自己醒、谁负责把它唤醒。
- **每个假设先写成"如果为真，日志里应该看到 X"**，再看日志。不能用一条同时兼容
  "假设成立"和"假设无关"的日志当依据。
- **测试红了先分诊**：断言错还是实现错，逐条判断并写明理由。**一次改动里改了 3 个以上
  测试期望值，就是在用"改期望"代替"改实现"。**
- **平台专属的路径由写入方显式指定**（例如 vcpkg 的 `--x-install-root`），绝不依赖默认值。
- **脚本化改文件后，必须 grep 确认改动真的落盘**。`str.replace` 在锚点文本漂移时
  **静默不替换、不报错**，本仓库已经因此踩坑 3 次（成员变量漏加、函数调用漏改、
  位图初始化漏写），每次都白白多付一轮 CI 往返。改动提交前用 `grep` 验证结果。
- **改 YAML / JSON 后，本地先解析一遍再推**。`uv run --with pyyaml python -c
  "import yaml,sys;yaml.safe_load(open(sys.argv[1]))" <file.yml>`（JSON 同理，
  `uv run python -c "import json,sys;json.load(open(sys.argv[1]))" <file.json>`，
  `UV_CACHE_DIR=$PWD/.cache/uv`）。**写下来的命令形状要自己先跑过一遍**——照抄本仓库
  自己的示例去写，示例本身是错的就会把错误复制到下一轮。解析只能发现语法错，
  发现不了"这行跑到了别的 step 的 `run:` 里"——那类错误会让
  一个步骤在**所有断言都通过之后**以 exit 127 收场，job 变红且证据被抹掉。
  改完用 `grep -n` 确认关键行仍属于它该在的块。
- **sanitizer 与覆盖率永不同构建**。gcov 计数器是普通全局，两线程自增同一计数器会被
  TSan 报成 data race（本地实测 217 处假阳性）。要跑 TSan 就换独立构建树
  （`linux-tsan` preset，coverage OFF），并把该步骤放在 job **最后**——它一失败，
  前面的 build / test / coverage 会被一起 skip。
- **整文件重写后，核对"声明 vs 定义"**。整体重写一个 `.cpp` 时最容易漏掉几个成员函数，
  症状是链接期 `undefined reference`，而不是编译错误——CI 才发现，代价一轮往返。
  提交前用一次声明/定义差集自查。
- **用第三方库之前，先读它的头文件**。凭记忆写的 API 调用几乎必错：libssh 0.11 的
  `ssh_pki_import_privkey_file` 是出参形式、`ssh_userauth_keyboard_interactive`
  已删除、`SSH_STATUS_*` 枚举根本不存在。**下载依赖对应的发布包，核对每个用到的符号
  的真实声明**，不要照着旧版本的记忆写。
- **崩溃先分"崩在被调用内部"还是"崩在调用之后"。** 上探针（每一步打印一行），
  **一行都没打出来 = 崩在被调用的内部，函数没返回**；这比推理一轮便宜。
  **系统 API 的出参别传 NULL**：MIG 生成的客户端桩会把 reply 里的结果**直接往用户指针写**，
  不查指针（内核那侧反而查了：`if (object_name) *object_name = IP_NULL;`）→ 传 NULL 就是
  段错误。**同一份协议要读两端**：内核实现 + 客户端桩；桩不开源时，**看真实调用点怎么传**
  （CPython 的 `remote_debug.h`、Rust 的 `mach2`、Zig 的 `darwin.zig`、Chromium 的
  `memory_mac.mm` 都传真实变量，一个传 NULL 的都没有）。
- **绕过包的元数据，就要自己接它的传递依赖**。用 `find_library` 代替
  `find_package` 会丢掉 config 里的 `find_dependency`：libssh 是静态库，OpenSSL
  必须由使用方自己链接。优先用 `find_package`，回退路径要显式补齐依赖，并且**先搜
  vcpkg 安装树**，避免链到 ABI 不一致的系统库。
- **工具链用闭区间或精确版本**（`~3.31.0`、锁 commit、锁 clang-format 版本）。开放式下限
  会让工具链升级变成静默的破坏性变更。
- **preset 字段必须与声明的 `version` 严格对应**，改 schema 就要改 version。

## 提交信息

commit message 要写清**为什么是这个根因**，它就是下次的排查索引。
背景与取舍写在正文，不放标题。

## 阶段门禁

阶段出口必须有证据（CI run 链接、覆盖率、性能基线对比、人工验收记录、遗留问题登记），
归档到 `docs/DEVELOPMENT_PLAN.md` 对应章节后才能开下一阶段。判据见该文档的
P<n>-A<x> 验收标准，证据等级：【CI】机器判定 / 【CI-Perf】性能门禁 / 【人工】真机验证。

**2026-10-25 的例外（D7）**：P1-A7 三端真机验收**暂不执行**，按遗留项挂着，Phase 1
归档为"工程完成、出口未签收"，Phase 2 照常开工。**这条例外的意思只是"先开工"，
不是"出口已签收"**——归档里那一栏至今是空的，谁都不许把它写成做过。
将来要签收时，用 `openscrt --acceptance-check` 在真机上跑一遍机器能判的那一半，
再按 M1–M8 清单补人工部分。

**门禁的标准不满足时，先问用户怎么收口，不要替他决定**——是继续等验收、还是降低口径、
还是记为遗留往下走，这三样的后果完全不同，属于用户的决定。问的时候把已经做完的
（机器能判的那一半）和还没做的（必须人做的那一半）分清楚说，别让人以为只差一点点。
