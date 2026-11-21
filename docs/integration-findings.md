# EasyEFI 集成实证发现汇总（S18 评估报告素材）

并行开发 + QEMU 双架构集成测试暴露的跨流问题与修复。单流宿主测试全绿但集成才暴露——
印证 core/glue 分离 + 契约先行 + 每流 QEMU 冒烟 + leader 集成矩阵的必要性。

## A. 平台/契约层（leader 修复，广播全员）

| # | 问题 | 发现流 | 根因 | 修复 |
|---|---|---|---|---|
| A1 | UEFI MemRealloc 崩溃 | S01/S03/S06 | `ReallocatePool(Ptr,0,NewSize)` 参数序错（签名 OldSize,NewSize,OldBuffer）| 尺寸跟踪头实现（契约签名不变），扩展区零初始化 |
| A2 | EfiMem ASSERT 编译失败 | S08/S09/S13 | ASSERT 缺 `#include <Library/DebugLib.h>`，-Werror implicit-declaration | 补 include |
| A3 | JsonArrayCount 符号冲突 | S03 | JsonDom.h 与 JsonLib.h 同名不同参，UEFI 构建 conflicting types | 改名 JsonDomArrayCount（契约审批）|
| A4 | SCAN_* 宏重定义 | S07 | TuiInput.h SCAN_* 与 MdePkg SimpleTextIn.h 重名，-Werror | #ifndef 守卫（契约审批）|
| A5 | SortLib 未解析 | S03 | JsonLib 传递依赖 SortLib，平台 DSC 缺 | 收编进主 DSC |
| A6 | 选中高亮不渲染 | S08 | TUI_ATTR_SELECTED=0xB0（LIGHTCYAN 背景 bit7）>0x7F 固件掩码，SetAttribute 拒绝 | 改 0x30（CYAN 背景）；约束：背景须非 light ≤0x07 |
| A7 | auto: 候选 Fs 悬空崩溃 | S11 | VolFree Destroy Fs，但 ExecAutoCandidates 借用→0xAF 毒化指针→AA64 对齐异常 | 裁决：Fs 按卷缓存稳定，VolFree 不销毁（契约所有权注释）|
| A8 | GetMapFromDevicePath 崩溃 | S12 | 返回串挂 Shell 延迟释放链表（AddBufferToFreeList），FreePool 提前释放→RestoreBufferList 踩 0xAF | 不 FreePool（Shell 负责）；截断 ';' 首段主映射名 |
| A9 | ExecVolumeLib Guids 缺失 | S13/S12 | VolUefi.c 用 gEfiPartTypeSystemPartGuid 但 .inf [Guids] 未声明→消费方链接失败 | 库 .inf 补 [Guids] |

## B. Shell/引导机制（QEMU 实证，固化 design）

| # | 发现 | 流 | 结论 |
|---|---|---|---|
| B1 | 嵌套 Shell 递归栈耗尽 | S10 | LoadOptions 必须带 argv[0] 镜像名令牌 `L"shell.efi <script> [args]"`；Shell 跳过 Argv[0]，缺令牌→file-name 不解析→startup.nsh 重跑→递归（design D9）|
| B2 | 异架构应用启动中止脚本 | S15 | .nsh 内启动异架构 .efi 失败会中止脚本（与 load 失败不中止不同）→兼容布局架构分流须在 UEFI 层（ShellRunner trampoline）|
| B3 | 短格式 USB 引导无 FILEPATH 节点 | S15/S11 | LoadedImage->FilePath 可能无 HD/FILEPATH 节点→不能做自身 dp 前缀手术；用卷枚举（ShellRunner）/含自身镜像的卷（Launcher 启动 ESP）|
| B4 | load 相对路径失败 | S14 | EfiShellOpenFileList 相对路径要求 GetCurDir 非空；用 %homefilesystem% 绝对路径（严格布局）；嵌套 Shell 不设 %homefilesystem%→trampoline 预加载驱动 |
| B5 | echo 独立 '-' 当 flag | S15 | Shell echo 把独立 `-` token 解析为 flag→文案用分号 |
| B6 | .nsh 注释符 | S15 | 是 # 不是 rem（rem 报 unknown command）|
| B7 | LoadOptions 注入机制 | S10 | gBS->LoadImage 无 LoadOptions 参数；LoadImage 后经 LoadedImage 协议设 LoadOptions/Size 再 StartImage（BmBoot 同款）|
| B8 | LoadOptions 首 token | S13 | Shell 提示符启动整行命令首 token=镜像名→glue 取 ShellParameters.Argv[1]；FM F4 传路径保留回退 |

## C. 固件/驱动 quirks

| # | 发现 | 流 | 结论 |
|---|---|---|---|
| C1 | OVMF x64 ConSplitter 宽度虚高 | S06 | 报 100x31 但图形台仅可寻址列 0..97→TuiInit 右下角 SetCursorPosition 探测降级 80x25；消费方按 TuiGetSize 布局勿硬编码 |
| C2 | 右下角格画出即滚动 | S06 | GraphicsConsoleDxe 画 (Rows-1,Cols-1) 触发整屏上滚无法稳定恢复→该格永不输出（缓冲留值屏幕空格）|
| C3 | ntfs-3g GetInfo 非标准 | S09/S14 | 不支持 NULL 缓冲问尺寸，缓冲过大也返 TOO_SMALL+精确尺寸→两阶段重试（起始非零缓冲）|
| C4 | FatDelete 警告码 | S09 | 须 READ|WRITE 打开；edk2-stable202608 WARN_DELETE_FAILURE 不带 MAX_BIT（EFI_ERROR=FALSE）→显式转 EFI_DEVICE_ERROR |
| C5 | EfiFs rename 静默 | S09 | SetInfo 改名疑似不生效→绝对路径重试/读源写新删源组合 |
| C6 | ArmVirtQemu 无 SecureBoot 变量 | S11 | SB:? 属预期（该固件无 SecureBoot 变量）|
| C7 | aa64 virt 无默认键盘 | S07 | 须 -device usb-kbd（xhci 之后）；x64 q35 自带 ps2-kbd 勿加（双份事件）|
| C8 | Shell 倒计时消费按键 | S07 | QMP 首键注入须晚于倒计时（aa64 ~15s/x64 ~75s）|

## D. 构建/工具链

| # | 发现 | 结论 |
|---|---|---|
| D1 | edk2-stable202608 移除 GCC5 | 用 -t GCC；前缀 GCC_BIN(X64)/GCC_AARCH64_PREFIX(AA64) |
| D2 | --pcd 语法 | `PcdName=Value`（非 DSC 的 `|`）|
| D3 | CONF_PATH | 须显式 /work/Conf 且预创建（edksetup 默认放 edk2 树，只读挂载失败）|
| D4 | edk2 位置 | 须在仓库树外（~/.cache/easyefi/edk2），否则 WORKSPACE 重复发现包触发 case-mismatch |
| D5 | 子模块填充 | brotli×2 + mipisyst + jansson（浅克隆不含，按锁定 commit codeload 填充）|
| D6 | Docker VM loop | 无 loop 分区节点→偏移 loop（losetup -o）；virtiofs 复用旧 img losetup ENOENT→先 rm |
| D7 | 容器路径 | mktestimage 路径须相对 worktree 根（容器只挂 worktree，宿主绝对路径不可见）|
| D8 | 库构建 | 库须列 [Components] 才能 -m 构建（MdePkg.dsc 模式）；-p <lib.inf> 独立构建 build.py 崩溃 |
| D9 | EFI_STATUS 头 | 在 <Uefi.h> 非 <Base.h>；EfiCompat UEFI 分支 include Uefi.h |
| D10 | 活跃 worktree 并发 | 不可并发跑 smoke/build（共用 Conf/镜像/日志冲突）|

## 评估结论（待 S18 全矩阵补充）
- 并行机制有效：19 任务包，契约审批 2 项，跨流互查发现 9 个平台/契约 bug + 8 个机制实证 + 8 个固件 quirks。
- 单流宿主测试（ASan/UBSan）抓内存错误（S02 两个 use-after-free）；QEMU 冒烟抓固件交互/集成 bug；两者互补缺一不可。
- 完整镜像双架构引导全链路 PASS（双 ESP 路由+Shell+startup.nsh+NTFS 驱动+Launcher）；fix-gpt 备份头迁移+扩容验证。
