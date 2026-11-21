# S00 依赖锁定记录（edk2 / 固件 / 基座镜像）

## edk2
- 标签：`edk2-stable202608`
- commit SHA：`2970e5699ba6267f3384ffab20f96647578aebc8`
- 来源：Gitee 镜像 `https://gitee.com/mirrors/edk2`（浅克隆；GitHub git 协议本机受阻）
- 信任校验：克隆 HEAD == GitHub 官方 tag SHA（api.github.com/repos/tianocore/edk2/git/ref/tags/edk2-stable202608）✅
- 位置：`~/.cache/easyefi/edk2/`（**仓库树外**——在 WORKSPACE 内会触发 edk2 包路径
  case-mismatch 检查，S00 实证；gitignored 约定不再适用，直接外置）
- 子模块（浅克隆不含，按锁定 commit 用 codeload tarball 填充）：
  - BaseTools/BrotliCompress 与 MdeModulePkg/BrotliCustomDecompressLib 所需 brotli：
    `e230f474b87134e8c6c85b630084c612057f253e`（google/brotli）
  - MdePkg/Library/MipiSysTLib/mipisyst（MdePkg.dec [Includes] 无条件引用，缺则 error 000E）：
    `370b5944c046bab043dd8b133727b2135af7747a`（MIPI-Alliance/public-mipi-sys-t）
  其余子模块（openssl 等）本项目构建不需要。已扫描全部相关包 .dec [Includes] 确认无其它缺失。
- 锁定树实测路径修正（相对设计文档假设）：
  - Ucs2Utf8Lib 实例 = `RedfishPkg/Library/BaseUcs2Utf8Lib/BaseUcs2Utf8Lib.inf`
  - RedfishCrtLib 实例 = `RedfishPkg/PrivateLibrary/RedfishCrtLib/RedfishCrtLib.inf`

## 测试固件（QEMU 用，非交付物）
- 来源：Homebrew qemu 包自带 edk2 blobs（/opt/homebrew/share/qemu/）
  - x64：edk2-x86_64-code.fd + edk2-i386-vars.fd（OVMF 分体 pflash）
  - aa64：edk2-aarch64-code.fd（ArmVirtQemu，-bios）
- 版本随 qemu 包（`qemu-system-x86_64 --version` 记录于发布清单）

## 基座镜像 easyefi-build
- 基础：docker.m.daocloud.io/library/ubuntu:22.04（Docker Hub 本机受阻，DaoCloud 镜像）
- apt 源：mirrors.aliyun.com（arm64 = ubuntu-ports）
- 工具链：gcc-x86-64-linux-gnu（X64 交叉）、gcc-aarch64-linux-gnu（AA64，arm64 容器内即原生目标）、nasm、iasl
- 打包工具：gdisk(sgdisk)、dosfstools、ntfs-3g(mkfs.ntfs)、mtools、util-linux(losetup)
- edk2 BaseTools 预编译于镜像内 /edk2/BaseTools
- 重建命令：`docker build -t easyefi-build -f docker/Dockerfile .`

## 宿主测试
- 原生 macOS clang + ASan/UBSan；框架自检 tests/host/selftest（4/4 通过，2026-11-22）
