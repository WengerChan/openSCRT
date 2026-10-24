# SecureCRT 的会话文件格式（P2-T11 的依据）

这里记的每一条都是**从来源核对过**的事实，不是记忆。三方对齐：一个 SecureCRT 自己产出的真实
文件、两个互相独立的实现（一个导出器、一个导入器）。三者一致才算数。

## 出处

| 代号 | 是什么 | 地址 |
| --- | --- | --- |
| **[真实]** | SecureCRT 自己写出的会话文件（`CMCSSW1.ini`，10882 字节，SHA-256 `02317a02308d0b6438f3ed189f17dbc3ad75827c3fa2eb3d5f63a5045ff5cac5`） | <https://github.com/TrungKhanhNguyen/HighLevelExport/blob/master/Config/Sessions/CP_CDR_MC_Site/CMCSSW1.ini> |
| **[真实夹]** | 同一个目录里的文件夹清单（`__FolderData__.ini`，SHA-256 `787c4d077ca10b9fcc5bf9683f083fca231853df8ed2ccd81b75af0bd5219b5a`） | <https://github.com/TrungKhanhNguyen/HighLevelExport/blob/master/Config/Sessions/CP_CDR_MC_Site/__FolderData__.ini> |
| **[RustConn]** | Rust 写的 SecureCRT **导出**器 | <https://github.com/totoshko88/RustConn/blob/master/rustconn-core/src/export/securecrt.rs> |
| **[oryxis]** | Rust 写的 SecureCRT **导入**器 | <https://github.com/wilsonglasser/oryxis/blob/master/crates/oryxis-app/src/importers/securecrt.rs> |

**为什么本仓库里没有放这两个真实文件**：那个仓库没有许可证（GitHub API 返回
`license = none`），不能把别人的文件抄进来。本地 `.cache/` 里留了副本用来核对，
测试用的夹具是按核对出来的格式自己写的（见 `tests/unit/SecureCrtConfigImporterTest.cpp`
里的 `sessionFile()`）。

## 事实

1. **会话文件不是 `[Connection]` 那种 INI。** 我一开始记成那种，核对下来是错的。
   一个会话文件是**扁平的、带类型前缀的键值列表**：

   ```
   S:"Username"=admin
   D:"[SSH2] Port"=00000016
   B:"Normal Font v2"=00000060
    f3 ff ff ff 00 00 ...
   ```

   前缀是类型：`S:` 字符串、`D:` 数字、`B:` 字节（可以跨多行）、`Z:` 另一种二进制。
   键名带引号，后面是 `=` 和值。**[真实] [RustConn] [oryxis]**

2. **键名**（拼写以 `[真实]` 为准，`[RustConn]` 与 `[oryxis]` 同）：
   `Hostname`（不是 `HostName`）、`Protocol Name`、`Username`、`[SSH2] Port`、
   `[SSH1] Port`、`Port`（Telnet/RAW 用这个）、`Identity Filename V2`、
   `Description`、`Password V2`、`Is Session`、`Session Password Saved`。**[真实] [RustConn] [oryxis]**

3. **端口是十六进制的 DWORD，固定 8 位十六进制数字**，且键名里带协议名：
   `D:"[SSH2] Port"=00000016` → 22。**[真实] [RustConn]**（导出器也写 8 位）
   `[oryxis]` 用 `u32::from_str_radix(v, 16)` 读，与之一致。
   **推论（本仓库自己加的）**：只有 8 位十六进制才读。`"22"` 按十六进制是 34，
   比读不出来更糟，所以位数不对就退回协议默认端口。

4. **协议名**在 `S:"Protocol Name"` 里：`SSH2`、`SSH1`、`TELNET`、`RAW`。**[真实]（SSH2） [RustConn] [oryxis]**
   `[oryxis]` 的映射：SSH2/SSH1 → SSH，TELNET → Telnet，RAW → 原生 TCP。

5. **口令是 `S:"Password V2"=02:<hex>`，用安装实例的密钥加密**，没有可导入的东西。
   `[oryxis]` 明确不导入它，只在备注里说一句。本仓库照做：会话照常导入，口令不导入，
   并在结果里留一条说明。**[真实] [oryxis]**

6. **会话树就是目录树**：`Sessions/` 下面是文件夹，文件夹里是会话文件。
   每个文件夹有一个 `__FolderData__.ini`，里面写它的内容和子文件夹：

   ```
   S:"Folder List"=
   S:"Session List"=CMCSFW1:CMCSSW1:CMCSSW2:
   D:"Is Expanded"=00000001
   ```

   列表用冒号分隔，**末尾也有一个冒号**。列表里的名字是会话文件名去掉 `.ini`。
   **[真实夹]**
   **推论（本仓库自己加的）**：文件夹里没有 `__FolderData__.ini` 时，改为扫目录，
   免得漏掉会话；列表里有、目录里没有的文件按"文件不在"记一笔跳过。

7. **文件开头是 UTF-8 BOM**（`EF BB BF`），行尾是 CRLF；`__FolderData__.ini` 却是 LF。
   **[真实] [真实夹]**

8. **会话 id 用 `.` 连接路径**，不用 `/`：id 会变成会话文件里的组名，而 `/` 在那里
   表示"组里的组"，`loadTree()` 的 `childGroups()` 就只能看见第一层。
   **推论（本仓库自己加的）**。

## 我记错的

- 「SecureCRT 的会话是 `[Connection]` 段的 INI」——**错**，是 `S:`/`D:` 扁平键值。
- 「主机名键叫 `HostName`」——**错**，是 `Hostname`。
- 「端口是十进制」——**错**，是十六进制 DWORD。
