# S14 依赖锁定记录（ntfs-3g UEFI 读写驱动）

## ntfs-3g（pbatard UEFI 分支）
- 仓库：`https://github.com/pbatard/ntfs-3g`
- 分支：`rebase-on-2026.9.28`（GitHub API 实测默认分支）
- commit SHA：`0964af83716d57a15bad94da53e8f38c4dc266c2`
  （2026-11-16T12:02:30Z，"Remove unused variable"；锁定日 2026-11-22 经
  `api.github.com/repos/pbatard/ntfs-3g/commits/HEAD` 获取）
- 来源：codeload tarball `https://codeload.github.com/pbatard/ntfs-3g/tar.gz/<sha>`（本机实测可行）
- tarball SHA256：`4a25b19a13fbb3dc129aa613c380a7c64b339749e4ad2b423e0f9f48111fc366`
  （2026-11-22 实测；commit SHA 为权威锁定，tarball 哈希为完整性校验——若 GitHub 重生成
  tarball 导致哈希漂移，以 commit SHA 为准并更新本记录）
- 拉取脚本：`third_party/ntfs-3g/fetch.sh`（幂等；`src/.pin` 记录锁定；`--force` 重拉）。
  上游源码不入库，按需拉取（本脚本即 GPLv2 源码书面要约的机器可执行形式，design §8）。
- 许可：GPL-2.0-or-later（上游源文件 SPDX 头）；源码提供指引见 `docs/handoffs/S14.md`。
- 零补丁：上游源码未修改（任务书禁止；如需补丁走 leader 评审）。

## 构建
- 脚本：`scripts/build-ntfs3g.sh [X64|AARCH64|all]`（S14 独占）
- 平台 DSC：上游 `uefi-driver.dsc`（已含 `MdePkg/MdeLibs.dsc.inc` 与 AARCH64
  CompilerIntrinsicsLib NULL 链接，与 S00 经验一致，无需补丁）
- 工具链：`-t GCC`（**非上游 README 的 GCC5**，edk2-stable202608 已移除 GCC5）；
  RELEASE；读写版（DSC 默认 `FORCE_READONLY=FALSE`，未定义 FORCE_READONLY）
- edk2 依赖：S00 锁定 `edk2-stable202608`（见 `docs/pins/S00-edk2.md`），
  容器内 `WORKSPACE=third_party/ntfs-3g/src`、`PACKAGES_PATH=/edk2`、
  `CONF_PATH=third_party/ntfs-3g/conf`（显式预创建）
- 版本宏：`-D DRIVER_VERSION=1.3 -D COMMIT_INFO=0964af8`（嵌入驱动名字符串，便于 dh 溯源）
- 环境修正（S14 实证）：ntfs-3g `types.h` 直接 `#include <stdint.h>`，X64 交叉编译需
  目标架构 libc 头；基座镜像 `--no-install-recommends` 未装 `libc6-dev-amd64-cross`，
  `build-ntfs3g.sh` 在**临时容器内**按需补装（不改共享镜像；AA64 在 arm64 容器内即原生目标，无需补）。
  建议 leader 将该包加入 docker/Dockerfile（见交接注记）。

## 产物（third_party/ntfs-3g/out/，SHA256 清单 out/SHA256SUMS）
| 文件 | SHA256 | PE 机器类型 |
|---|---|---|
| ntfs_x64.efi | `dfda7a8617b7a902f85e750cd930aa0098590496f7cc6464c393746887eaafe6` | 0x8664 (AMD64) |
| ntfs_aa64.efi | `bad650f9a0f9b2f70a271bcef5133454d39bbdf94f8e9a3220dd5548ea158592` | 0xAA64 (ARM64) |

## 冒烟（卷可见性；读写持久化归 S09）
- 探针：`third_party/ntfs-3g/testapp/NtfsProbe.{c,inf}` + `testapp.dsc`
  （经 `build-module.sh` DSC 环境变量构建，双架构）
- 运行器：`third_party/ntfs-3g/smoke/run-smoke.sh [x64|aa64|all]`
  （mktestimage ntfs 变体 + qemu-run serial 断言；断言文件 expect-ntfs-{x64,aa64}.txt）
- startup.nsh：`load %homefilesystem%\EFI\Drivers\ntfs.efi`（design §3.3 形式）。
  **实测坑**：mktestimage 默认 startup 的裸相对路径 `load EFI\Drivers\ntfs.efi` 报
  "File not found"（文件确在 ESP 上）。机制：`load`→EfiShellOpenFileList 相对路径要求
  GetCurDir(NULL) 非空，否则 EFI_NOT_FOUND；应用执行走 ShellFindFilePathEx 不受此限。
  主 Shell startup 上下文用 %homefilesystem% 绝对路径；裸相对 load 仅在 CurDir 已设置时可靠。
  S15 compat 嵌套 Shell 脚本（无 homefilesystem）用裸相对 load，需实证 CurDir 状态，
  详见 docs/handoffs/S14.md 集成注意事项 1。
- 结果：见 `docs/handoffs/S14.md` 测试证据节（serial 日志随冒烟目录提交）。