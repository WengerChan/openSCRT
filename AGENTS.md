# AGENTS.md

openSCRT — C++20 + Qt 6 的 SecureCRT 对标终端。读代码前先读
[`securecrt_native_architecture_and_roadmap.md`](securecrt_native_architecture_and_roadmap.md)（技术选型的唯一来源）、
[`docs/DEVELOPMENT_PLAN.md`](docs/DEVELOPMENT_PLAN.md)（任务与验收标准）、
[`docs/POSTMORTEM.md`](docs/POSTMORTEM.md)（已犯过的错，见下）。

## 硬约束

1. **验证只走 CI。** 本地不编译、不跑测试、不装构建工具。编译/测试/覆盖率/性能基准
   一律以 GitHub Actions 的结果为准。本地只写代码、格式化、提交。
2. **需要 Python 时用 uv**，cache 目录写项目内可写路径（`UV_CACHE_DIR=$PWD/.cache/uv`）。
   不要用系统 `python3`/`pip3` 装包。
3. **访问外网走 `./.proxy.sh`**（`github.com:443` 直连常超时）。CI 里所有网络调用同样走代理环境。
4. **依赖只经 `vcpkg.json` 声明。** CI 从中派生一份更小的清单（`.github/scripts/ci-vcpkg-manifest.py`
   的 SKIP 表）。**开始 link 某个 port 时，必须在同一个 PR 里删掉它的 SKIP 条目**，
   否则 configure 会报 not found。
5. **一个任务号 = 一个 PR。** 分支名 `feat/<phase>-<任务号>-<短描述>`。`main` 受保护，
   必须 CI 全绿才能合并。
6. **告警零容忍**：`-Wall -Wextra -Wpedantic -Werror`（MSVC `/W4 /WX`）。
   不要为了消告警放弃有意的设计（例如 `SpscRingBuffer` 的 `alignas(64)`），
   正确做法是局部 `#pragma warning(disable)` 并写清理由。
7. **平台无关代码**：CI 脚本禁用 `mapfile`/`readarray`/数组展开，用 `while IFS= read -r`
   并剥 `\r`；写给 CI 读的文件一律 `newline="\n"`。

## 排障纪律（来自 21 轮 CI 排障的复盘）

- **先分类**：环境 / 编译期契约 / 运行时逻辑。分错类，后面全是浪费。
- **"工具报告成功"不等于"产物在预期位置"。** 下游找不到时，第一反应是 `ls -R <路径>`，
  顺序永远是：产物存在吗 → 路径对吗 → 工具怎么找。**跳过第一步是最大的浪费来源。**
- **同一处失败连修两轮而症状不变 → 停止推理，进观测模式。** 加一行 `ls` / `printenv`
  比再猜一轮便宜得多。临时探针用完要删。
- **每个假设先写成"如果为真，日志里应该看到 X"**，再看日志。不能用一条同时兼容
  "假设成立"和"假设无关"的日志当依据。
- **测试红了先分诊**：断言错还是实现错，逐条判断并写明理由。**一次改动里改了 3 个以上
  测试期望值，就是在用"改期望"代替"改实现"。**
- **平台专属的路径由写入方显式指定**（例如 vcpkg 的 `--x-install-root`），绝不依赖默认值。
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