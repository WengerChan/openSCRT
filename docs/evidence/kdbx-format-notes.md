# KDBX 字节级事实（Phase 2 的 KDBX 读取器照着这个写）

这份文件记的是 **KDBX 文件格式的字节级事实**，每条都标了出处。写它的原因很直接：
实现 KDBX 读取器的过程中，我**凭记忆写错过两次**，两次都是会让人静默读错数据的错：

| 记错的 | 实际 | 怎么发现的 |
| :--- | :--- | :--- |
| 版本号是 `0x03040000` / `0x04000000` | `0x00030001` / `0x00040000` / `0x00040001` | 真实 `.kdbx` 样本的头 12 字节 |
| KDBX 3.1 没有第二个签名 | 3.1 的第二个签名是 `0xB54BFB66`，4.x 才是 `0xB54BFB67` | 参考实现的常量表 |

**所以：格式跟 Win32 默认行为一样，凭记忆推断必错。下面每条都能指到出处。**

## 出处

1. **官方规范（4.1）**：<https://keepass.info/help/kb/kdbx.html> —— 字节级规范，字段表、VariantDictionary、KDF 参数都在这里。
2. **官方规范（4.0 的变化说明）**：<https://keepass.info/help/kb/kdbx_4.html>
3. **参考实现**：<https://github.com/dlech/KeePass2.x> 的 `KeePassLib/Serialization/KdbxFile.cs`、`KdbxFile.Read.cs`、`KeePassLib/Cryptography/CryptoRandomStream.cs`、`KeePassLib/Cryptography/KeyDerivation/{AesKdf,Argon2Kdf}.cs`。
4. **独立实现（交叉校验用）**：`pykeepass` 4.2.0（Python），`pykeepass/kdbx_parsing/{kdbx,kdbx3,kdbx4,common}.py`。
5. **真实样本**：`pykeepass` 随包发布的 `blank_database.kdbx`（KDBX 4.0），下面的十六进制来自它。

## 签名与版本

```
Signature 1 (UInt32) = 0x9AA2D903     字节序 D9 A2 9A 03（样本实测）
Signature 2 (UInt32) = 0xB54BFB67     KDBX 4.x
                     = 0xB54BFB66     KDBX 3.1（KeePass 2.x 预发布签名）
                     = 0xB54BFB65     KeePass 1.x（老格式，直接拒绝）
版本号     (UInt32)   高 16 位 = major，低 16 位 = minor
```

版本常量（`KdbxFile.cs:88-94`）：

| 名称 | 值 | 含义 |
| :--- | :--- | :--- |
| `FileVersion32_3_1` | `0x00030001` | 3.1 |
| `FileVersion32_4` | `0x00040000` | 4.0 |
| `FileVersion32_4_1` | `0x00040001` | 4.1 |
| `FileVersionCriticalMask` | `0xFFFF0000` | major 在高 16 位；major 更大就必须拒绝 |

**样本头 12 字节（实测）**：

```
03 d9 a2 9a | 67 fb 4b b5 | 00 00 04 00
sig1 0x9AA2D903 | sig2 0xB54BFB67 | 版本 0x00040000（4.0）
```

## 头部布局

| 版本 | 布局 |
| :--- | :--- |
| 3.1 | sig1(4) ‖ sig2(4) ‖ 版本(4) ‖ 字段序列 ‖ 载荷 |
| 4.x | sig1(4) ‖ sig2(4) ‖ 版本(4) ‖ 字段序列 ‖ 头部 SHA-256(32) ‖ 头部 HMAC-SHA256(32) ‖ 载荷 |

- 3.1 的版本字段在 sig2 之后，**sig2 = 0xB54BFB66**。
- 4.x 的头部 SHA-256 与 HMAC 都覆盖"从 sig1 到字段序列结束"的全部字节。
- 4.1 的头部哈希与 4.0 不同（KeePass 2.47 引入 PBKDF2 版），**本项目第一版只保证 4.0 与 3.1**，
  4.1 遇到时按"需要额外一轮派生"处理，不猜。

## 头部字段

字段 = `t(1 字节) ‖ s(长度) ‖ V`。

**长度 `s` 的宽度随版本变**（`KdbxFile.Read.cs` 的 `ReadHeaderField`）：

| 版本 | 长度字段 |
| :--- | :--- |
| 3.1 | **UInt16**（2 字节） |
| 4.x | **Int32**（4 字节） |

> 规范页只写了 4.1 的 Int32；3.1 的 UInt16 只有参考实现里有。**这是最容易写错的一处**：
> 按 4 字节去读 3.1 的文件，会把长度的高位当成字段 ID。

字段 ID（`KdbxFile.cs:241-256`）：

| ID | 名称 | 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| 0 | EndOfHeader | Byte[4] | 必须是 `0D 0A 0D 0A`，出现且仅出现一次，字段序列的末尾 |
| 1 | Comment | 字节串 | |
| 2 | CipherID | UUID(16) | 加密算法 |
| 3 | CompressionFlags | UInt32 | 0 = 不压缩，1 = GZip |
| 4 | MasterSeed | Byte[32] | 主盐/种子 |
| 5 | TransformSeed | Byte[32] | **仅 3.1**：AES-KDF 的种子 |
| 6 | TransformRounds | UInt64 | **仅 3.1**：AES-KDF 轮数 |
| 7 | EncryptionIV | 字节串 | AES-256 是 16 字节，ChaCha20 是 12 字节 |
| 8 | InnerRandomStreamKey | Byte[32] | **仅 3.1**：内层保护流密钥 |
| 9 | StreamStartBytes | Byte[32] | **仅 3.1**：密钥正确性校验 |
| 10 | InnerRandomStreamID | UInt32 | **仅 3.1**：0=无 1=ARCFOUR 2=Salsa20 3=ChaCha20 |
| 11 | KdfParameters | VariantDictionary | KDF 参数（4.x 用它取代 5/6） |
| 12 | PublicCustomData | VariantDictionary | 插件的自定义数据 |

**不认识的 ID 必须按长度跳过**——这是能同时吃下 3.1 / 4.0 / 4.1 的唯一办法。

算法 UUID：

| UUID | 算法 |
| :--- | :--- |
| `31C1F2E6-BF71-4350-BE58-05216AFC5AFF` | AES-256-CBC（PKCS#7） |
| `D6038A2B-8B6F-4CB5-A524-339A31DBB59A` | ChaCha20（RFC 8439） |
| `AD68F29F-576F-4BB9-A36A-D47AF965346C` | Twofish-CBC（插件提供） |

KDF UUID：

| UUID | KDF |
| :--- | :--- |
| `C9D9F39A-628A-4460-BF74-0D08C18A4FEA` | AES-KDF |
| `EF636DDF-8C29-444B-91F7-A9A403E30A0C` | Argon2d |
| `9E298B19-56DB-4773-B23D-FC3EC6F0A1E6` | Argon2id |

## VariantDictionary

```
[2 字节] 版本，UInt16 LE，当前 0x0100
[n 项]   每项：
           type(1) ‖ key长度(UInt32 LE) ‖ key(UTF-8) ‖ value长度(UInt32 LE) ‖ value
[1 字节] 0x00 结束
```

值的类型：

| type | 含义 |
| :--- | :--- |
| 0x04 | UInt32 |
| 0x05 | UInt64 |
| 0x08 | Bool（1 字节） |
| 0x0C | Int32 |
| 0x0D | Int64 |
| 0x18 | String（UTF-8） |
| 0x42 | ByteArray |

KDF 参数（规范页的表）：

| 名称 | 类型 | 适用 | 含义 |
| :--- | :--- | :--- | :--- |
| `$UUID` | ByteArray(16) | 全部 | KDF 的 UUID |
| `S` | Byte[32] | AES-KDF | 盐 |
| `R` | UInt64 | AES-KDF | 轮数 |
| `V` | UInt32 | Argon2 | 版本，0x10 或 0x13（推荐 0x13） |
| `S` | Byte[] | Argon2 | 盐，最小 8 字节 |
| `I` | UInt64 | Argon2 | 迭代次数 |
| `M` | UInt64 | Argon2 | **内存，单位是字节**（≥8192） |
| `P` | UInt32 | Argon2 | 并行度 |

> Argon2 的 `M` 是**字节**，而 Argon2 库（libsodium）的 `memlimit` 是 **KiB**——换算要除以 1024。
> 这也是 `pykeepass` 里 `memory_cost=kdf_parameters['M'].value // 1024` 的原因。

## 密钥链

```
复合密钥 composite = SHA-256( SHA-256(密码的 UTF-8) ‖ 密钥文件内容(32 字节) )
派生密钥 transformed = Argon2d/Argon2id(composite) 或 AES-KDF(composite)
主密钥   master     = SHA-256(MasterSeed ‖ transformed)
```

> **这里我第一遍也写错了**：复合密钥是**外面还要再哈希一次**的
> `SHA-256(password_composite ‖ keyfile_composite)`，不是把两段直接拼起来当复合密钥。
> 出处：`pykeepass/kdbx_parsing/common.py` 的 `compute_key_composite` 最后一行的
> `hashlib.sha256(password_composite + keyfile_composite).digest()`。
> 差这一层哈希，派生出的密钥就全错，而症状是"解出来的是乱码"——不报错，只是读不出内容。

- 密码段：`SHA-256(密码的 UTF-8)`；没有密码时是空。
- 密钥文件段：二进制密钥文件取其 32 字节；64 字节的十六进制文本解成 32 字节；
  XML 密钥文件按 `Meta/Version` 取 `Key/Data`（1.0 是 Base64，2.0 是十六进制，2.0 还要
  对 `Hash` 属性做前 4 字节校验）；其他长度的文件取 `SHA-256(文件内容)`。没有密钥文件时是空。
- **AES-KDF**：`transformed = SHA-256(AES-128-ECB 迭代 R 轮(composite))`，密钥是 `S`（32 字节，
  但 AES-128 只用前 16 字节）。
- **Argon2**：输出 32 字节；`M` 字节要换算成 KiB；版本取 `V`（0x10 或 0x13）。
  **但 libsodium 没有 Argon2d**（只有 `ARGON2I13` / `ARGON2ID13`），而 KeePass 默认写的就是
  Argon2d —— 所以实际派生走 **OpenSSL 3.2+ 的 ARGON2 KDF**（"ARGON2D" / "ARGON2I" /
  "ARGON2ID" 三个名字），参数为 `pass` / `salt` / `iter` / `size` / `memcost` / `lanes` /
  `version`，其中 `memcost` 的单位是 **1 KiB 块**（= 文件里的 `M` ÷ 1024）。
- 载荷解密用 `master` 作 AES-256-CBC 的密钥，用字段 7 的 IV。

## 块流与 HMAC

4.x 的载荷是一串**带 HMAC 的块**：

```
每块：HMAC(32) ‖ 块长度(UInt32 LE) ‖ 块数据
最后一块：块长度为 0（仍带 32 字节 HMAC）
```

块的 HMAC-SHA256：

```
key  = SHA-512( 0xFF×8 ‖ SHA-512(MasterSeed ‖ transformed ‖ 0x01) )
data = 块序号(UInt64 LE) ‖ 块长度(UInt32 LE) ‖ 块数据
```

头部的 HMAC-SHA256 用同一个 key，覆盖头部字节。

3.1 没有块流：整个载荷是**一个**加密流（`HashedBlockStream`），密钥正确性靠解密后前 32 字节
与字段 9 `StreamStartBytes` 比较来判断。

## 载荷加密与压缩

- 加密：AES-256-CBC（PKCS#7 填充）或 ChaCha20。
- 压缩：字段 3 为 1 时是 GZip。

## 内层头部与保护流

解密（并按需解压）之后，先读**内层头部**：

| ID | 名称 |
| :--- | :--- |
| 0 | EndOfHeader |
| 1 | InnerRandomStreamID |
| 2 | InnerRandomStreamKey |
| 3 | Binary |

内层头部之后就是 XML。内层头部长度字段也是 `t(1) ‖ s(UInt32 LE) ‖ V`。

**保护流**（`Protected="True"` 的 `Value` 节点）由 InnerRandomStreamID 决定：

| ID | 算法 | 密钥派生 | nonce |
| :--- | :--- | :--- | :--- |
| 0 | 无 | — | — |
| 1 | ARCFOUR variant | 密钥内容本身 | — |
| 2 | Salsa20 | `SHA-256(密钥)` | 固定 `E8 30 09 4B 97 20 5D 2A` |
| 3 | ChaCha20 | `SHA-512(密钥)` 的前 32 字节 | 同一次 SHA-512 的第 32..43 字节（12 字节） |

出处：`CryptoRandomStream.cs` 的构造函数（`CrsAlgorithm.ChaCha20` / `Salsa20` 分支）与
`pykeepass/kdbx_parsing/common.py` 的 `Salsa20Stream` / `ChaCha20Stream`。

> **Salsa20 不能用 libsodium 的 `crypto_stream_salsa20`**——它的状态布局和 KeePass 的不是同一个 Salsa20。
>
> KeePass（`KeePassLib/Cryptography/Cipher/Salsa20Cipher.cs` 的构造函数）把 16 个字排成：
> `σ0 | key[0..15] | σ1 | nonce(2 字) | counter(2 字) | σ2 | key[16..31] | σ3`。
>
> libsodium（`crypto_stream/salsa20/ref/salsa20_ref.c` + 它的 core）用的是
> `σ0..σ3 | key(8 字) | nonce(2 字) ‖ counter(2 字)`——nonce 和 counter 放在字 12–15。
> **同样的密钥和 nonce，两种布局给出的密钥流不同。**
>
> 所以 P2-T6 自己实现 Salsa20（照 KeePass 的状态布局），不用 libsodium 的。
> ChaCha20 没有这个问题：KDBX 4 用的是 IETF 变体（12 字节 nonce + 32 位计数器），
> 与 libsodium 的 `crypto_stream_chacha20_ietf` 一致。
>
> **教训**：第三方库的"Salsa20"和"ChaCha20"在名字上不保证布局相同。这跟"读它的头文件、
> 别凭记忆"是同一条纪律，只是这次要读的是**状态布局**而不是函数签名。
