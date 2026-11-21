# EasyEFI 构建配方（S00 实证版，所有流适用）

## 环境模型（macOS 适配）

- **构建/打包**：Docker 容器 `easyefi-build`（Ubuntu 22.04 arm64 + 双交叉工具链 + edk2 BaseTools 预编译）。
  设计与任务书中的 WSL2/Linux 命令在容器内原样可用（losetup/sgdisk/mkfs.ntfs 需 `--privileged`）。
- **QEMU 测试**：macOS 原生（aa64 用 HVF 加速；x64 为 TCG 模拟，启动较慢属预期）。
- **宿主单测**：macOS 原生 clang + ASan/UBSan，不依赖容器。
- edk2 共享只读：`<repo>/edk2`（锁定 SHA 见 docs/pins/S00-edk2.md），worktree 经 PACKAGES_PATH 引用，不做 submodule。

## 每流 worktree 配方

```bash
# 主仓库先有 S00 基建提交（worktree 只带已提交内容）
git -C /Users/bob/EasyEFI worktree add /Users/bob/EasyEFI/.worktrees/Sxx -b stream/Sxx-name
cd /Users/bob/EasyEFI/.worktrees/Sxx
# 构建（容器内 WORKSPACE=本 worktree，Conf/Build 独立）：
EDK2_DIR=/Users/bob/EasyEFI/edk2 bash scripts/build-module.sh EasyEfiPkg/Library/XxxLib/XxxLib.inf X64
EDK2_DIR=/Users/bob/EasyEFI/edk2 bash scripts/build-module.sh EasyEfiPkg/Library/XxxLib/XxxLib.inf AARCH64
# 宿主测试（模块目录内）：
make -f host.mk test
# 最小镜像冒烟：
bash scripts/mktestimage.sh --arch X64 --variant fat --out test.img \
  --shell <Shell.efi路径> --app Xxx=<被测.efi路径>
bash scripts/qemu-run.sh --arch x64 --image test.img --expect expect.txt --timeout 180
```

注意：
- worktree 内 `edk2/` 不存在（gitignored）→ 必须传 `EDK2_DIR` 指向主仓库的共享 edk2。
- 容器挂载：worktree → /work，共享 edk2 → /edk2:ro。每流独立 Conf/Build，互不干扰。
- **CONF_PATH 必须显式设为 /work/Conf 且预先 mkdir**（build-module.sh 已内置）：edksetup.sh 默认把
  Conf 放进 edk2 树（$EDK_TOOLS_PATH/../Conf），只读挂载下会静默失败（S00 实证踩坑）。
- **工具链标签 = GCC（不是 GCC5）**：edk2-stable202608 已移除 GCC5（tools_def 3.06）。
  前缀环境变量：X64=`GCC_BIN`，AA64=`GCC_AARCH64_PREFIX`（镜像已设）。上游文档里的
  `-t GCC5`/`GCC5_*_PREFIX` 一律按此替换（含 ntfs-3g 构建）。
- 并发 QEMU ≤4（各 2GB）；宿主测试不限。
- 设计文档/任务书在主仓库 `docs/`（未入库），worktree 内用绝对路径引用。

## 编码纪律（宿主/UEFI 双环境）

1. **CHAR16 字面量一律 `U16("...")`**（EfiCompat.h 宏）：UEFI 展开 `L"..."`，宿主运行时转换。
   裸 `L""` 在宿主是 32 位 wchar_t，必炸（S00 自检实证）。纯 UEFI glue 可用裸 L""。
2. **CHAR16 字符串操作用 EfiStrLen/EfiStrCmp/EfiStrnCmp/EfiStrChr**，禁止 wcs*（宿主 wchar_t 4 字节）。
3. 纯逻辑库只 include Contract/*.h；内存走 MemAlloc/MemRealloc/MemFree；禁止 gBS/gST/MdePkg 协议头。
4. 应用 core 只依赖 Ops 表；glue 装配后端。core 宿主测试用 tests/host/mocks（MockTui/MockExec/MockFs）。
5. 阈值走 PCD（EasyEfiPkg.dec：PcdEditorMaxFileSize/PcdPreviewMaxBytes/PcdUndoDepth/PcdSkipTimeoutDefault）。

## 契约与 stub

- 契约头：`EasyEfiPkg/Include/Contract/`（S00 冻结，leader 独占；变更走任务书 §4 流程）。
- 各模块目录含 S00 stub（`<Name>Stub.c` + .inf + module.dsc.inc）；流负责人以真实实现替换 Sources，
  保持 .inf BASE_NAME/LIBRARY_CLASS 不变。
- 应用 stub 打印 `[STUB] <Name> <arch>` 并置 EASYEFI_RAN=yes —— S15 启动链冒烟直接用 stub Launcher，
  无需单独 TestLauncherStub（任务书 S00 交付物第 2 条的简化，已实证等价）。

## 冒烟断言约定

- 被测应用经 `Print()`/`DEBUG()` 输出标记到 ConOut；OVMF/ArmVirtQemu 的 ConOut 含 serial，
  `qemu-run.sh` 以 `-serial file:` 捕获，`--expect` 文件每行一个 grep -E 模式，全部命中=PASS。
- 按键注入：`--keys` 文件（`<延迟秒> <qcode键名>...`），经 QMP send-key。
- 若个别固件 ConOut 不含 serial（实机阶段），改用 `-debugcon` 或截图人工核验（集成矩阵注明）。

## S00 出口标准（验收记录）

- [x] 宿主框架自检 4/4 通过（ASan/UBSan 干净）
- [ ] easyefi-build 镜像构建成功（BaseTools 含 brotli）
- [ ] Shell X64/AA64 构建成功（ShellPkg，GCC5）
- [ ] stub Launcher X64/AA64 构建成功
- [ ] 最小 fat 镜像 QEMU 双架构引导 → `[STUB] Launcher <arch>` 标记命中
- [ ] 双 worktree 并行构建两个不同模块成功
