# 对标 SecureCRT 的原生跨平台终端工具（含 KeePass 双模凭据集成）系统架构与研发路线图

## 1. 总体架构采用 C++20 与 Qt 6 原生技术栈以对齐原版性能基准

为实现与 [VanDyke SecureCRT](https://www.google.com/search?q=https%3A%2F%2Fwww.vandyke.com%2Fproducts%2Fsecurecrt%2F) 完全一致的高密度桌面界面布局、毫秒级键盘响应以及百兆级日志流吞吐能力，系统采用 **C++20 + Qt 6** 原生跨平台方案（原版 [SecureCRT 跨平台版本](https://www.google.com/search?q=https%3A%2F%2Fwww.vandyke.com%2Fdownload%2Fsecurecrt%2Fdownload.html) 同样基于 Qt 框架构建 UI 层）。该选型避免了 Web 容器（如 Electron）在数十个并发会话下的高内存开销，同时能够直接调用底层操作系统的网络套接字、串口驱动与内存安全锁定接口。

| 架构层次 | 核心技术选型 | 关键职责与选型依据 |
| :--- | :--- | :--- |
| **构建与工程体系** | [CMake](https://www.google.com/search?q=https%3A%2F%2Fcmake.org%2F) + Ninja + vcpkg | 统一管理 Windows（MSVC）、macOS（Clang / Universal Binary）与 Linux（GCC / Clang）的跨平台编译、依赖注入与安装包打包。 |
| **原生图形界面层** | [Qt 6 Widgets](https://www.google.com/search?q=https%3A%2F%2Fdoc.qt.io%2Fqt-6%2Findex.html) + [QDockWidget](https://www.google.com/search?q=https%3A%2F%2Fdoc.qt.io%2Fqt-6%2Fqdockwidget.html) | 1:1 还原经典菜单栏、可停靠会话树（Session Manager）、多标签/分屏工作区、底部快捷按钮栏（Button Bar）及命令广播窗（Command Window）。 |
| **终端渲染与仿真层** | [libvterm](https://www.google.com/search?q=https%3A%2F%2Fwww.leonerd.org.uk%2Fcode%2Flibvterm%2F)（定制扩展）+ Qt `QRhi` / `QPainter` | 完整解析 VT100/VT220/ANSI/Xterm 控制序列；结合字形纹理图集（Glyph Atlas）与脏矩形（Dirty-Rect）双缓冲机制实现 60/120 FPS 低延迟绘制。 |
| **网络与串口协议层** | [libssh](https://www.google.com/search?q=https%3A%2F%2Flibssh.org%2F)（或 [libssh2](https://www.google.com/search?q=https%3A%2F%2Flibssh2.org%2F)）+ [Qt SerialPort](https://www.google.com/search?q=https%3A%2F%2Fdoc.qt.io%2Fqt-6%2Fqtserialport-index.html) | 提供 SSH2 加密通道、SFTP 子系统、端口转发、X11 转发与跳板机级联；通过 [QSerialPort](https://www.google.com/search?q=https%3A%2F%2Fdoc.qt.io%2Fqt-6%2Fqserialport.html) 原生支持 RS-232/485 串口流控与波特率配置。 |
| **内联文件传输层** | 内置 SFTP 引擎 + [lrzsz](https://www.google.com/search?q=https%3A%2F%2Fwww.ohse.de%2Fuwe%2Fsoftware%2Flrzsz.html) 协议状态机 | 支持独立或共享通道的 SFTP 标签页；在终端字节流中实时检测魔数序列，无缝拉起 Xmodem / Ymodem / Zmodem（`rz`/`sz`）内联传输。 |
| **KeePass 凭据与密码学层** | [libsodium](https://www.google.com/search?q=https%3A%2F%2Flibsodium.gitbook.io%2Fdoc) + [OpenSSL 3](https://www.google.com/search?q=https%3A%2F%2Fopenssl-library.org%2F) + [pugixml](https://www.google.com/search?q=https%3A%2F%2Fpugixml.org%2F) + [nlohmann/json](https://www.google.com/search?q=https%3A%2F%2Fgithub.com%2Fnlohmann%2Fjson) | 原生解析 [KDBX 4](https://www.google.com/search?q=https%3A%2F%2Fkeepass.info%2Fhelp%2Fkb%2Fkdbx_4.html) / [KDBX 3.1](https://www.google.com/search?q=https%3A%2F%2Fkeepass.info%2Fhelp%2Fkb%2Fkdbx.html) 加密数据库（Argon2d/id + AES-256/ChaCha20），并通过 [KeePassHTTP](https://www.google.com/search?q=https%3A%2F%2Fgithub.com%2Fpfn%2Fkeepasshttp%2Fblob%2Fmaster%2FKeePassHttp.plgx) 协议对接运行中的密码库实例。 |
| **脚本自动化引擎层** | [pybind11](https://www.google.com/search?q=https%3A%2F%2Fpybind11.readthedocs.io%2F) + 内嵌 Python 3 运行时 | 1:1 映射 [SecureCRT Python 脚本对象模型](https://www.google.com/search?q=https%3A%2F%2Fwww.vandyke.com%2Fsupport%2Fsecurecrt%2Fpython_examples.html)（`crt.Screen`、`crt.Session`、`crt.Dialog`、`crt.Arguments`），实现存量运维脚本零修改迁移。 |

## 2. 六层解耦架构与多线程异步 I/O 模型保障高频输出下界面零卡顿

终端工具最常见的工程瓶颈在于远端服务器执行 `cat` 大文件或高频滚动日志时，海量字节流直接冲垮 UI 渲染线程导致界面假死。本架构通过严格的**六层模块解耦**与**读写分离并发模型**解决这一问题。

| 线程域 | 归属模块 | 并发模型与跨线程通信机制 |
| :--- | :--- | :--- |
| **UI 主线程（Main GUI Thread）** | 工作区容器、会话树、对话框、终端画布光栅化 | 仅处理用户输入事件与定时帧渲染（每 16ms 触发一次 `VSync` 重绘信号）；绝不执行任何网络阻塞读、磁盘 I/O 或密钥派生计算。 |
| **会话 I/O 工作线程池（Session I/O Workers）** | SSH2 / Telnet / Serial / Local PTY 传输通道 | 采用 `epoll` / `kqueue` / `IOCP` 异步事件循环驱动每个活跃会话的套接字读写，将收到的原始字节块压入无锁环形队列（Lock-Free SPSC Ring Buffer）。 |
| **终端解析与高亮线程（Parser Worker）** | VT 状态机、回滚缓冲区、正则高亮、Zmodem 嗅探器 | 批量消费无锁队列中的字节流，更新屏幕网格（Screen Grid）与环形历史行（Scrollback），触发脏行标记后以合并信号通知 UI 线程绘制。 |
| **文件传输专用线程（Transfer Threads）** | SFTP 并发队列、X/Y/Zmodem 状态机 | 独立管理大文件分块读写、CRC32 校验与断点续传，仅通过节流信号（每 100ms）向主界面上报进度百分比与速率。 |
| **密码学与凭据后台线程（Vault Worker）** | `.kdbx` 解密引擎、`KeePassHTTP` 异步客户端 | 在后台线程池中执行高耗时的 Argon2 密钥派生与本地 HTTP 握手，完成后通过安全内存句柄（`SecureString`）回调通知连接认证状态机。 |
| **脚本沙箱执行线程（Script Runner Thread）** | Python 解释器宿主、`crt.*` API 桥接器 | 每个运行的自动化脚本独占工作线程，通过条件变量（`std::condition_variable`）挂起等待终端解析器的 `WaitForString` 匹配事件。 |

## 3. 核心业务模块深度拆解与实现规范

### 3.1 界面与工作区模块 1:1 复刻 SecureCRT 经典视觉与交互体验

界面层基于 [Qt 6 Widgets](https://www.google.com/search?q=https%3A%2F%2Fdoc.qt.io%2Fqt-6%2Findex.html) 构建，全面对齐 [SecureCRT 核心特性](https://www.google.com/search?q=https%3A%2F%2Fwww.vandyke.com%2Fproducts%2Fsecurecrt%2Ffeatures.html) 的操作习惯与快捷键体系：

* **主窗口停靠体系（MainWindow & Dock System）**：顶部集成经典菜单栏（`File`、`Edit`、`View`、`Options`、`Transfer`、`Script`、`Tools`、`Window`、`Help`）与可自定义图标工具栏；左侧及四周采用 [QDockWidget](https://www.google.com/search?q=https%3A%2F%2Fdoc.qt.io%2Fqt-6%2Fqdockwidget.html) 承载 **Session Manager（会话管理器）**、**Active Sessions（活动会话列表）** 与 **Command Manager（命令管理器）**，支持自动隐藏（Auto-Hide Pin）、浮动与停靠布局持久化。
* **多模式中心终端工作区（Multi-Mode Workspace）**：自定义扩展 `QTabWidget` 与多叉树 `QSplitter` 布局容器，支持**多标签页（Tabbed）**、**标签组并排（Tab Groups）**、**水平/垂直任意拆分（Split Horizontal/Vertical）** 以及 **平铺窗口（Tile Vertically/Horizontally/Cascade）**。标签头内置连接状态指示图标（已连接、正在重连、已断开、未读输出闪烁、已锁定会话）。
* **底部快捷命令按钮栏（Button Bar）**：位于主工作区下方，支持创建多个分组标签（Group Tabs）。每个按钮可绑定：发送字符串（`Send String`，支持 `\r`、`\n`、`\p` 暂停一秒、`\v` 粘贴剪贴板等 SecureCRT 原生转义符）、运行脚本（`Run Script`）、调用菜单功能（`Menu Function`）或打开指定协议会话，并支持配置按钮颜色图标与会话级自动切换。
* **底部命令交互与广播窗（Command Window / Chat Window）**：提供多行命令编辑框与历史命令回溯，支持三种发送模式切换——**仅发送至当前活动会话**、**发送至所有已连接会话（Send Characters Immediately / Broadcast to All Sessions）**、**发送至指定标签组（Send to Tab Group）**，满足批量集群运维需求。
* **三级配置继承树（Options Hierarchy）**：完整还原 `Global Options`（全局选项）与 `Session Options`（会话选项）的经典的左侧树形分类导航（涵盖 `Connection`、`Terminal -> Emulation / Appearance / Advanced`、`SSH2 -> Port Forwarding`、`Log File` 等），并实现 **`Default Session` 模板修改后可选批量应用至所有现有会话** 的核心配置继承机制。

### 3.2 终端仿真与渲染引擎兼顾精准标准兼容与十万行级回滚性能

终端仿真层严格分离“状态机数据模型”与“视图光栅化渲染”，确保在各种复杂 TUI 程序（`vim`、`htop`、`tmux`、网络交换机 CLI）下表现零偏差：

* **多标准终端状态机**：基于定制扩展的 [libvterm](https://www.google.com/search?q=https%3A%2F%2Fwww.leonerd.org.uk%2Fcode%2Flibvterm%2F) 状态机，支持 `Xterm`（含 256 色与 24-bit TrueColor）、`VT100`、`VT102`、`VT220`、`ANSI`、`Linux Console`、`SCO ANSI` 以及政企传统设备常用的 `Wyse 50/60` 与 `Dumb` 模式；支持主屏幕（Primary Screen）与备用屏幕缓冲区（Alternate Screen Buffer）平滑切换。
* **紧凑型内存网格与环形回滚缓冲（Ring Scrollback Buffer）**：每个字符单元格（`Cell`）采用紧凑的 16 字节对齐结构（存储 Unicode Codepoint、前景色/背景色 RGBA 或调色板索引、粗体/斜体/下划线/闪烁/反显位标志及双倍字宽标记）。历史回滚行采用按需分页的环形缓冲区，支持单会话配置最高 **128,000 行回滚记录**，超出行可实时异步刷入本地日志。
* **字形图集与脏矩形加速渲染**：引擎在启动或修改字体时将常用 ASCII 与 CJK 等宽字形预渲染至 GPU/内存纹理图集（Glyph Atlas），重绘时仅扫描带有 `is_dirty` 标记的行，极大降低 CPU 占用；原生支持 **矩形列块选择（Alt + 鼠标拖拽 Column Selection）**、**选中即复制（Copy on Select）**、**右键直接粘贴** 及 **多行粘贴安全确认弹窗**。
* **高性能正则关键字高亮（Keyword Highlighting）**：支持创建多套关键字高亮规则集（Keyword Set），支持单词匹配与正则表达式（基于预编译 DFA 状态机）实时扫描当前可见视口（Viewport），对匹配到的 IP 地址、`ERROR`/`WARN`、端口状态（`up`/`down`）实时叠加前景色、背景色或反转样式，且不篡改底层原始字符缓冲。

### 3.3 多协议通信栈覆盖现代云原生 SSH2 与传统工业硬件接口

所有通信协议统一继承自抽象基类 `ITransportChannel`，向终端仿真层提供标准化的字节流输入输出、窗口大小变更（`SIGWINCH` / `NAWS`）及生命周期回调：

* **全功能 SSH2 协议栈**：基于 [libssh](https://www.google.com/search?q=https%3A%2F%2Flibssh.org%2F)（或 [libssh2](https://www.google.com/search?q=https%3A%2F%2Flibssh2.org%2F) + [OpenSSL 3](https://www.google.com/search?q=https%3A%2F%2Fopenssl-library.org%2F)）构建，支持密码认证、`Keyboard-Interactive`（双因素 OTP/PAM 挑战响应）、公钥认证（RSA、ECDSA、Ed25519，支持 OpenSSH 与 VanDyke 私钥格式）、GSSAPI/Kerberos；内置跨会话共享的 **SSH Agent（对标 SecureCRT Activator）** 与 **Agent Forwarding**；支持 **多级 SSH Jump Host（跳板机代理通道）**、**Local / Remote / Dynamic（SOCKS4/5）端口转发** 以及 **X11 转发**。
* **工业级串口通信（Serial / COM / TTY）**：基于 [Qt SerialPort](https://www.google.com/search?q=https%3A%2F%2Fdoc.qt.io%2Fqt-6%2Fqtserialport-index.html) 的 [QSerialPort](https://www.google.com/search?q=https%3A%2F%2Fdoc.qt.io%2Fqt-6%2Fqserialport.html) 组件，支持自动枚举系统热插拔 USB 转串口设备，支持自定义非标波特率、数据位（5/6/7/8）、校验位（None/Even/Odd/Mark/Space）、停止位（1/1.5/2）及硬件/软件流控（`RTS/CTS`、`DTR/DSR`、`XON/XOFF`），并提供针对数通硬件调试的 **Serial Break 信号发送（`Alt+B`）** 与 **Hex View（十六进制双栏监控视图）**。
* **传统远程协议与本地 Shell**：内置标准状态机实现 `Telnet`（RFC 854，支持 NAWS 窗口协商、Keep-Alive 空包防断连及 Telnet over TLS）、`Rlogin`、`Raw TCP`；本地终端在 Windows 下调用原生 `ConPTY` API（集成 PowerShell、CMD、WSL2），在 macOS 与 Linux 下调用 `openpty` / `forkpty` 启动本地 Shell。

### 3.4 文件传输子系统支持共享连接 SFTP 标签页与内联 Zmodem

* **SFTP 标签页与双栏文件管理器**：用户在任意已连接的 SSH2 标签页右键选择 `Connect SFTP Tab` 时，系统直接复用当前 SSH 会话底层的 `ssh_session` 多路复用开启 `sftp` 子通道（无需二次输入密码或重新握手）。同时支持 SecureCRT 经典的 **SFTP 交互命令行模式**（`lpwd`、`lcd`、`ls`、`cd`、`get`、`put`、`mget`、`mput`）与 **可视化拖拽传输面板**。
* **内联 Xmodem / Ymodem / Zmodem 自动触发**：在终端解析线程前挂载轻量级 **Zmodem 协议嗅探器（Zmodem Sniffer）**。当远端服务器执行 `sz filename` 输出 `**\x18B00`（`ZRQINIT` 帧头）或执行 `rz` 输出 `**\x18B01`（`ZRINIT` 帧头）时，嗅探器立即拦截后续二进制流（停止向屏幕打印乱码），自动弹出原生文件选择框或保存到会话预设的 `Download Directory`，并调用内置的 [lrzsz](https://www.google.com/search?q=https%3A%2F%2Fwww.ohse.de%2Fuwe%2Fsoftware%2Flrzsz.html) 兼容状态机完成高速流式收发与 CRC32 校验。

### 3.5 KeePass 双模凭据引擎实现安全无感密码获取与注入

为满足你提出的“支持读取本地 KeePass 文件与 `KeePassHTTP` 接口获取密码”的核心诉求，系统设计了独立的 **凭据提供者抽象层（`ICredentialProvider`）**，在不将任何主机明文密码写入会话配置文件的前提下，实现毫秒级动态取密。

#### 模式一：本地 `.kdbx` 数据库直读引擎（支持 KDBX 3.1 与 KDBX 4.x）

不依赖外部启动 KeePass 客户端，直接由终端工具内置密码学模块读取并解密 `.kdbx` 文件（遵循官方 [KDBX 4.1 规范](https://www.google.com/search?q=https%3A%2F%2Fkeepass.info%2Fhelp%2Fkb%2Fkdbx_4.1.html) 与 [KDBX 文件格式规范](https://www.google.com/search?q=https%3A%2F%2Fkeepass.info%2Fhelp%2Fkb%2Fkdbx.html)）：

| 解析阶段 | 核心处理逻辑与密码学原语 | 采用技术库 |
| :--- | :--- | :--- |
| **1. 外层文件头解析** | 校验魔数签名 `0x9AA2D903` 与 `0xB54BFB67`；按 TLV（Type-Length-Value）或 Variant Dictionary 格式提取加密算法 UUID（AES-256 / ChaCha20 / Twofish）、压缩标志（GZip）、Master Seed、KDF 参数字典。 | C++20 二进制流解析器 |
| **2. 复合主密钥与 KDF 派生** | 组合用户输入的 Master Password（SHA-256）、可选 Key File（XML 或二进制密钥文件）与 Windows DPAPI（可选）生成复合密钥；调用 Argon2d / Argon2id（[KDBX 4](https://www.google.com/search?q=https%3A%2F%2Fkeepass.info%2Fhelp%2Fkb%2Fkdbx_4.html)）或 AES-KDF（KDBX 3.1）派生出 32 字节主解密密钥与 HMAC 密钥。 | [libsodium](https://www.google.com/search?q=https%3A%2F%2Flibsodium.gitbook.io%2Fdoc) (`crypto_pwhash_argon2id`) + [OpenSSL 3](https://www.google.com/search?q=https%3A%2F%2Fopenssl-library.org%2F) |
| **3. 完整性校验与解密解压** | 验证 Header SHA-256 与 Header HMAC-SHA256；按块读取 HMAC-SHA256 认证分块流（Block Stream）防止篡改；使用 AES-256-CBC 或 ChaCha20 解密载荷并通过 `zlib` 执行 GZip 解压。 | [OpenSSL 3](https://www.google.com/search?q=https%3A%2F%2Fopenssl-library.org%2F) `EVP_Cipher` + `zlib` |
| **4. XML 索引与内存流保护** | 解析解压后的内部 XML 树（提取 `Group` -> `Entry` 的 `UUID`、`Title`、`UserName`、`URL`、自定义属性及 SSH 私钥附件）；对于带有 `Protected="True"` 标记的 `Password` 节点，保存其偏移量与 Inner Random Stream（ChaCha20 / Salsa20）状态，仅在发起 SSH 连接的瞬间在锁定内存中还原明文。 | [pugixml](https://www.google.com/search?q=https%3A%2F%2Fpugixml.org%2F) + [libsodium](https://www.google.com/search?q=https%3A%2F%2Flibsodium.gitbook.io%2Fdoc) |
| **5. 热监听与自动锁定** | 通过 `QFileSystemWatcher` 监听本地 `.kdbx` 文件修改事件，外部保存密码后自动增量刷新索引；支持设置闲置 N 分钟或最小化到托盘时自动清空内存中的主密钥。 | Qt Core `QFileSystemWatcher` |

#### 模式二：`KeePassHTTP` 协议本地加密 RPC 客户端

对接已打开的 KeePass 2（安装 [KeePassHttp 插件](https://www.google.com/search?q=https%3A%2F%2Fgithub.com%2Fpfn%2Fkeepasshttp%2Fblob%2Fmaster%2FKeePassHttp.plgx)）或兼容服务端（默认监听 `http://127.0.0.1:19455`），通信载荷采用 [nlohmann/json](https://www.google.com/search?q=https%3A%2F%2Fgithub.com%2Fnlohmann%2Fjson) 序列化并经由 **256-bit AES-CBC（PKCS#7 填充）** 加密：

* **阶段 1：关联握手与密钥持久化（`test-associate` & `associate`）**：
  * 客户端生成随机 256-bit AES 共享密钥 `Key` 与随机 128-bit 初始向量 `Nonce`（Base64 编码），使用 `Key` 和 `Nonce` 对 `Nonce` 自身执行 AES-256-CBC 加密生成校验值 `Verifier`。
  * 若未绑定，发送 `{"RequestType": "associate", "Key": "<Base64_Key>", "Nonce": "<Base64_Nonce>", "Verifier": "<Base64_Verifier>"}` 至本地 `19455` 端口；用户在 KeePass 弹窗确认并命名连接标识后，响应返回分配的 `Id`（即 `KeyId`）。
  * 客户端将该 `Id` 与 `Key` 加密存入操作系统的原生安全凭据库（Windows Credential Manager / macOS Keychain / Linux Libsecret）；后续每次请求前先发 `test-associate` 验证绑定有效性。
* **阶段 2：按会话特征精准检索凭据（`get-logins`）**：
  * 当用户双击某个终端会话（例如主机 `10.20.30.40`，端口 `22`，或绑定了 KeePass Entry UUID）时，客户端生成新随机 `Nonce` 与 `Verifier`，将目标查询 URL（如 `ssh://10.20.30.40:22` 或主机名/标签名）使用共享 `Key` 和本次 `Nonce` 进行 AES-256-CBC 加密，构造请求：
    `{"RequestType": "get-logins", "Id": "<KeyId>", "Nonce": "<Base64_Nonce>", "Verifier": "<Base64_Verifier>", "Url": "<Encrypted_Url>", "SubmitUrl": "<Encrypted_Host>"}`。
  * 收到响应后，先使用响应中的 `Nonce` 校验服务端 `Verifier` 防止重放与伪造，再解密返回的 `Entries` 数组（包含 `Name`、`Login`、`Password` 及 `StringFields` 自定义字段）。
* **阶段 3：与会话配置的多策略绑定机制**：
  * 在每个会话的 `Session Options -> Connection -> Authentication` 中，密码来源支持选择：`Session Saved Password`（内置加密存储）、`Prompt Every Time`（每次弹窗）、**`KeePass Entry by UUID`（通过树形选择器直接绑定指定条目）**、**`KeePass Auto-Match by Host/Title`（按主机 IP/域名自动匹配）**。
  * 同时支持从 KeePass 条目的自定义字段或附件中提取 **SSH 私钥密码（Passphrase）**、**Jump Host 跳板机密码**、**`sudo` 提权密码** 以及 **动态 TOTP 二次验证码**。

### 3.6 脚本自动化引擎兼容 SecureCRT Python API 对象模型

为保证网络工程师与系统管理员积累的大量 [SecureCRT Python 自动化脚本](https://www.google.com/search?q=https%3A%2F%2Fwww.vandyke.com%2Fsupport%2Fsecurecrt%2Fpython_examples.html) 可以直接运行，系统通过 [pybind11](https://www.google.com/search?q=https%3A%2F%2Fpybind11.readthedocs.io%2F) 将 C++ 核心控制器封装为内置模块 `crt` 注入内嵌 Python 3 解释器：

| 对象名称 | 核心属性与方法 | 底层 C++ 桥接实现机制 |
| :--- | :--- | :--- |
| **`crt` (顶层应用对象)** | `GetScriptTab()`, `GetTab(n)`, `GetTabCount()`, `Sleep(ms)`, `Quit()`, `Version` | 绑定主窗口 `WorkspaceController` 单例，提供跨标签页句柄获取与非阻塞协程休眠。 |
| **`crt.Screen`** | `Send(str)`, `WaitForString(str, timeout)`, `WaitForStrings([...], timeout)`, `ReadString([...], timeout)`, `Get(r1, c1, r2, c2)`, `Clear()`, `Synchronous`, `CurrentRow`, `CurrentColumn` | 当 `Synchronous = True` 时，终端解析器为该脚本维护独立的消费游标（Cursor Offset），确保 `Send("show version\r")` 后调用 `WaitForString("#")` 绝不会因为输出过快而漏检（Race Condition）。 |
| **`crt.Session`** | `Connect(cmd)`, `ConnectInTab(cmd)`, `Disconnect()`, `Connected`, `Log(start)`, `LogFileName`, `RemoteAddress`, `SetStatusText(txt)` | 线程安全地投递事件至主线程会话管理器，控制会话连接启停、动态修改日志文件路径与底部状态栏提示。 |
| **`crt.Dialog`** | `MessageBox(msg, title, flags)`, `Prompt(msg, title, default, isPassword)`, `FileOpenDialog(...)` | 通过 `QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection)` 在 UI 主线程弹出原生模态对话框并将用户输入返回给脚本线程。 |
| **`crt.Arguments`** | `Count`, `GetArg(index)` | 接收命令行 `/ARG` 参数或按钮栏绑定脚本时传入的自定义参数列表。 |

同时提供 **Script Recorder（脚本录制器）**：开启录制后，用户在终端中的每一次键盘输入与远端返回的提示符（Prompt）将被自动捕获，停止录制时自动生成标准的 `crt.Screen.Send(...)` + `crt.Screen.WaitForString(...)` Python 脚本文件。

## 4. 工程目录结构与兼容原版配置的数据存储设计

工程采用模块化静态库 + 主执行程序的 [CMake](https://www.google.com/search?q=https%3A%2F%2Fcmake.org%2F) 组织方式，各核心库均可独立编写单元测试（GoogleTest）：

```text
securecrt-next/
├── CMakeLists.txt
├── vcpkg.json                         # 声明 qtbase, qtserialport, libssh, libsodium, openssl, pugixml, nlohmann-json, pybind11
├── src/
│   ├── app/                           # 程序入口、全局单例、崩溃转储、命令行参数解析（兼容 /S, /SSH2, /SCRIPT）
│   ├── ui/                            # Qt 6 视图层
│   │   ├── mainwindow/                # 主窗口、菜单栏、可定制工具栏、状态栏
│   │   ├── dock/                      # SessionManagerDock, ActiveSessionsDock, CommandManagerDock
│   │   ├── workspace/                 # TabbedWorkspace, SplitContainer, TerminalCanvasWidget
│   │   ├── bars/                      # ButtonBarWidget（底部多组按钮栏）, CommandWindowWidget（底部广播窗）
│   │   └── dialogs/                   # GlobalOptionsDialog, SessionOptionsDialog, QuickConnectDialog, KeePassSelectorDialog
│   ├── terminal/                      # 终端仿真与渲染内核
│   │   ├── emulator/                  # VtermStateMachine（VT100/VT220/Xterm/Wyse）、字符集转换（UTF-8/GB18030）
│   │   ├── buffer/                    # ScreenGrid, RingScrollbackBuffer（十万行级分页环形缓冲）
│   │   ├── renderer/                  # GlyphAtlas, DirtyRectRenderer, KeywordHighlighter（正则高亮引擎）
│   │   └── logger/                    # SessionLogger（支持 %H_%Y%M%D.log 宏替换、每行时间戳前缀、自动轮转）
│   ├── protocols/                     # 多协议传输通道层
│   │   ├── ITransportChannel.h        # 统一异步传输接口
│   │   ├── ssh/                       # Ssh2Channel, SshPortForwarder, SshJumpHostProxy, SshAgentService
│   │   ├── serial/                    # SerialChannel（基于 QSerialPort，支持 RTS/CTS、XON/XOFF、Break、HexView）
│   │   ├── telnet/                    # TelnetChannel, RloginChannel, RawTcpChannel
│   │   └── local/                     # LocalPtyChannel（Windows ConPTY / POSIX openpty）
│   ├── transfer/                      # 文件传输引擎
│   │   ├── sftp/                      # SftpSubsystem, SftpTabShell, SftpDualPaneWidget
│   │   └── modem/                     # ZmodemSniffer, XyzModemEngine（内置 X/Y/Zmodem 状态机）
│   ├── credential/                    # KeePass 双模凭据与密码学安全中心
│   │   ├── ICredentialProvider.h      # 统一凭据检索与自动注入接口
│   │   ├── kdbx/                      # KdbxHeaderParser, Kdbx3Reader, Kdbx4Reader, Argon2Kdf, ProtectedStream
│   │   ├── keepasshttp/               # KeePassHttpClient（127.0.0.1:19455 AES-256-CBC JSON RPC 握手与查询）
│   │   └── memory/                    # SecureByteArray（基于 sodium_malloc / mlock 的防落盘安全内存容器）
│   ├── scripting/                     # 脚本自动化与录制引擎
│   │   ├── python/                    # EmbeddedPythonHost, PyCrtModule, PyScreenWrapper, PySessionWrapper
│   │   └── recorder/                  # ScriptRecorder（交互过程录制转 Python 脚本）
│   └── config/                        # 配置管理与迁移工具
│       ├── ConfigManager.h            # Global.ini / Session Hierarchy 读写与 Default Session 模板继承
│       └── importers/                 # SecureCrtConfigImporter（一键导入原版 SecureCRT XML/INI 会话树）
└── tests/                             # 终端转义序列基准测试、KDBX 4 解密测试、KeePassHTTP 模拟握手测试
```

## 5. 五阶段详细开发目标与里程碑验收标准

为控制研发风险并确保每个阶段都有可独立交付使用的版本，建议按以下五个阶段推进开发：

| 阶段与参考周期 | 阶段定位 | 核心开发模块与任务清单 | 里程碑量化验收标准（DoD） |
| :--- | :--- | :--- | :--- |
| **Phase 1（第 1–6 周）** | **核心底座与 MVP 可用版** | 1. 搭建 [CMake](https://www.google.com/search?q=https%3A%2F%2Fcmake.org%2F) + [Qt 6](https://www.google.com/search?q=https%3A%2F%2Fdoc.qt.io%2Fqt-6%2Findex.html) 跨平台工程骨架与无锁环形队列线程模型。<br>2. 集成 [libvterm](https://www.google.com/search?q=https%3A%2F%2Fwww.leonerd.org.uk%2Fcode%2Flibvterm%2F) 与 Glyph Atlas 双缓冲画布，实现 VT100/Xterm 256色/TrueColor 渲染、10 万行回滚、鼠标块选与搜索。<br>3. 集成 [libssh](https://www.google.com/search?q=https%3A%2F%2Flibssh.org%2F) 实现 SSH2 密码/公钥登录与本地 Shell（ConPTY/openpty）。<br>4. 完成主窗口菜单栏、左侧 `Session Manager` 树、多标签工作区与 `Quick Connect` 对话框。 | 1. 在 Windows/macOS/Linux 三端编译通过并稳定连接主流 Linux 服务器。<br>2. 运行 `vim`、`htop`、`tmux` 界面排版与色彩完全正常。<br>3. 执行 `seq 1 1000000` 高速刷屏时 UI 主线程帧率稳定 ≥ 60 FPS，内存增长可控且界面无卡顿。 |
| **Phase 2（第 7–11 周）** | **KeePass 双模凭据与全界面布局还原** | 1. 基于 [libsodium](https://www.google.com/search?q=https%3A%2F%2Flibsodium.gitbook.io%2Fdoc) + [OpenSSL 3](https://www.google.com/search?q=https%3A%2F%2Fopenssl-library.org%2F) + [pugixml](https://www.google.com/search?q=https%3A%2F%2Fpugixml.org%2F) 实现本地 `.kdbx`（[KDBX 3.1](https://www.google.com/search?q=https%3A%2F%2Fkeepass.info%2Fhelp%2Fkb%2Fkdbx.html) / [KDBX 4](https://www.google.com/search?q=https%3A%2F%2Fkeepass.info%2Fhelp%2Fkb%2Fkdbx_4.html)）直读解密引擎与条目选择器。<br>2. 基于 [nlohmann/json](https://www.google.com/search?q=https%3A%2F%2Fgithub.com%2Fnlohmann%2Fjson) 实现 [KeePassHTTP](https://www.google.com/search?q=https%3A%2F%2Fgithub.com%2Fpfn%2Fkeepasshttp%2Fblob%2Fmaster%2FKeePassHttp.plgx) 客户端（`associate` 绑定、`get-logins` 查询）。<br>3. 实现主窗口四向分屏（Split/Tile）、底部多分组 `Button Bar`、底部 `Command Window`（含全局广播模式）。<br>4. 实现原版 SecureCRT `Sessions` 目录一键导入器。 | 1. 支持直接加载带主密码 + 密钥文件的 `.kdbx` 数据库，双击会话自动注入密码完成 SSH 登录。<br>2. 与开启 KeePassHttp 插件的 KeePass 2 客户端完成配对并自动填充凭据，进程内存 dump 中无常驻明文密码。<br>3. 底部 `Command Window` 开启广播后，可同步向 20 个并发 SSH 会话下发命令。 |
| **Phase 3（第 12–16 周）** | **多协议栈扩展与 SFTP / Zmodem 传输** | 1. 集成 [QSerialPort](https://www.google.com/search?q=https%3A%2F%2Fdoc.qt.io%2Fqt-6%2Fqserialport.html) 实现串口通信（流控、Break 信号、十六进制监控视图）及 Telnet / Raw TCP 协议。<br>2. 实现共享 SSH 连接通道的 `Connect SFTP Tab`（命令行交互 + 双栏可视化文件管理器）。<br>3. 实现终端字节流 Zmodem 嗅探器与内联 [lrzsz](https://www.google.com/search?q=https%3A%2F%2Fwww.ohse.de%2Fuwe%2Fsoftware%2Flrzsz.html)（X/Y/Zmodem）异步文件收发引擎。<br>4. 完成 SSH 端口转发（Local/Remote/Dynamic SOCKS5）与多级跳板机（Jump Host）级联。 | 1. 通过 USB 转串口连接交换机/开发板，正常收发指令并发送 Break 信号。<br>2. 在终端内输入 `rz` 或 `sz` 自动弹窗选择文件并完成百兆文件传输，校验 MD5 100% 一致。<br>3. 右键标签页秒开 SFTP 标签，无需重复认证即可上传下载目录。 |
| **Phase 4（第 17–21 周）** | **高阶终端特性与运维效率套件** | 1. 实现多规则集正则关键字高亮（Keyword Highlighting）与实时词法着色。<br>2. 实现会话级自动登录期望序列（`Logon Actions -> Expect / Send`）及 `sudo` 密码自动响应。<br>3. 实现高级会话日志系统（支持 `%H_%Y%M%D_%h%m%s.log` 宏变量、连接自动开启记录、每行自定义时间戳前缀）。<br>4. 实现独立常驻托盘的 SSH Agent（对标 Activator）与主机密钥管理器。 | 1. 开启包含 50 条正则表达式的高亮集后，滚动刷屏帧率下降不超过 5%。<br>2. 会话断开后按配置自动重连，日志文件按日期与主机名自动分目录归档且每行带精确毫秒时间戳。 |
| **Phase 5（第 22–26 周）** | **Python 脚本引擎与企业级打磨交付** | 1. 基于 [pybind11](https://www.google.com/search?q=https%3A%2F%2Fpybind11.readthedocs.io%2F) 嵌入 Python 3 运行时，完整实现 `crt.Screen`、`crt.Session`、`crt.Dialog`、`crt.Arguments` API。<br>2. 实现脚本录制器（Script Recorder），将交互操作一键导出为 Python 脚本。<br>3. 完成深色/浅色主题引擎、配色方案库（Solarized、Dracula、Monokai 等）、高分屏 DPI 缩放适配及三端安装包签名发布。 | 1. 选取 10 个原版 [SecureCRT 官方 Python 示例脚本](https://www.google.com/search?q=https%3A%2F%2Fwww.vandyke.com%2Fsupport%2Fsecurecrt%2Fpython_examples.html)（含 `WaitForStrings`、`ReadString`、`ConnectInTab`），无需修改代码即可直接运行通过。<br>2. 连续开启 50 个活跃会话运行 72 小时压力测试，零崩溃、零句柄泄漏。 |

## 6. 关键工程技术难点与避坑指南

* **高频输出防假死（Frame Pacing & Batch Coalescing）**：切勿在收到每一个网络数据包时立即调用 `QWidget::update()`。应当在 I/O 线程将字节流持续写入环形缓冲区并推进终端状态机，同时设置一个 16ms（60Hz）或 8ms（120Hz）的 UI 帧合并定时器：只有当存在脏行（Dirty Rows）且到达帧间隔时，才向 UI 主线程触发一次重绘请求。
* **Zmodem 控制字符转义与 SSH 通道隔离**：Zmodem 协议对 `0x11`（XON）、`0x13`（XOFF）、`0x18`（CAN）等控制字符极为敏感。当 Zmodem 嗅探器检测到 `**\x18B` 进入传输状态时，必须临时挂起终端仿真器的本地回显与行缓冲处理，将底层 SSH Channel 的原始二进制字节流直接桥接至 Zmodem 状态机，传输结束收到 `OO`（Over and Out）序列后再恢复终端仿真。
* **KeePass Argon2 密钥派生的主线程隔离**：[KDBX 4](https://www.google.com/search?q=https%3A%2F%2Fkeepass.info%2Fhelp%2Fkb%2Fkdbx_4.html) 默认使用 Argon2d/Argon2id 进行密钥派生，高迭代次数和大内存参数下耗时通常在 300ms～1500ms 之间。解密过程必须封装在 `QThreadPool` 后台任务中执行，期间在界面展示非阻塞的解锁进度指示器，解锁成功后缓存会话级内存句柄，避免每次新建标签页都重新运行 Argon2。
* **`crt.Screen.Synchronous` 的防丢字同步机制**：在实现兼容 [SecureCRT 脚本接口](https://www.google.com/search?q=https%3A%2F%2Fdocumentation.help%2FSecureCRT%2FApplication_Object.htm) 时，如果脚本执行 `crt.Screen.Send("ls -la\r")` 后紧接着调用 `crt.Screen.WaitForString("$")`，在千兆内网环境下服务器可能在 `WaitForString` 被调用前就已经返回了 `$` 提示符。因此，当 `crt.Screen.Synchronous = True` 时，终端解析器必须为脚本线程维护一个**未读输出字节队列游标**：`Send()` 发送指令瞬间锁定当前游标起点，`WaitForString()` 从该起点向后扫描，只有匹配成功后才推进游标，从而彻底杜绝脚本死锁。
