# EasyEFI 最终测试评估报告（S18）

日期：2026-11-22 ｜ 平台：macOS (Apple Silicon) + Docker 构建基座 + 原生 QEMU
方法：19 任务包 subagent 并行开发 → leader 集成 → QEMU 双架构（x64 TCG / AA64 HVF）测试评估

## 1. 结论摘要

**EasyEFI 多架构 UEFI Shell 工作镜像已完整实现并通过 QEMU 双架构集成验证。** 单一 GPT 镜像
同时支持 x86_64 与 ARM64，自动按架构分流引导；内置 TUI 启动菜单（JSON 配置）、自研文件管理器、
UTF-8 文本编辑器、NTFS 读写数据区（>4GiB 单文件）；原版 UEFI Shell 零回归。刷机工作流锚定用例
（菜单→执行脚本→嵌套 Shell）实证通过。Secure Boot 默认关闭（用户决策）。

- **19/19 任务包交付合并**，全部双架构构建 + 宿主测试（ASan/UBSan）+ QEMU 冒烟验证。
- **完整集成镜像双架构引导 PASS**：真实 Launcher 菜单渲染、P3 检出、架构过滤、危险标记全部工作。
- **刷机执行链路 PASS**：Launcher 选项→危险确认→ExecNsh→嵌套 Shell→脚本执行。
- 并行集成测试发现并修复 **9 个平台/契约 bug + 8 个 Shell 机制实证 + 8 个固件 quirks**（含 4 个崩溃级）。

## 2. 交付架构

```
GPT 镜像（默认 8GiB，可配）
├─ P1 ESP FAT32 512M  x64：BOOTX64.EFI(原版Shell) + startup.nsh + ntfs.efi + Launcher/FM/Editor + Tools
├─ P2 ESP FAT32 512M  AA64：BOOTAA64.EFI + 同构 AA64 二进制
└─ P3 NTFS 剩余       数据区：launcher.json + scripts/ + entries/ + payloads/（>4GiB）+ .easyefi 标记
```

- **引导**：固件 fallback 逐 ESP 尝试 `\EFI\BOOT\BOOT{ARCH}.EFI`（EDK2 BmBoot 实证）→ x64 命中 P1、AA64 命中 P2。
  兼容布局变体用 ShellRunner trampoline（卷枚举+驱动预加载+架构脚本）应对"只扫第一 ESP"固件。
- **启动链**：Shell → startup.nsh（%homefilesystem% 绝对路径 load ntfs.efi → map -r）→ Launcher.efi。
- **Launcher**：core 纯状态机（宿主可测）+ UEFI glue；JSON 工作项 + 内置系统项；P3 不可达→DEGRADED 内置菜单。
- **FM/Editor**：core/glue 分离，FsOps 双后端（UEFI SimpleFileSystem + 宿主 POSIX）；Editor UTF-8 字节往返保留。
- **库层**：Utf8/EditorModel/ConfigCore/ExecPath/VolumeModel/TuiScreen/TuiInput/TuiWidget/ExecVolume/ExecImage。

## 3. 验证矩阵（V-01–V-12）

| # | 用例 | 状态 | 证据 |
|---|---|---|---|
| V-01 | 双架构引导路由 | ✅ PASS | 集成镜像 x64→P1 / AA64→P2 引导 PASS；S15 严格+兼容布局双架构 |
| V-02 | AA64 构建运行 | ✅ PASS | 全部库+三应用 AARCH64 构建 Done，QEMU virt 运行 |
| V-03 | UTF-8 字节往返 | ✅ PASS | S13 中文脚本编辑→保存→重开读回逐字节一致（双架构）|
| V-04 | 驱动链路 + >4GiB 读 | ✅ PASS（驱动）/ ⚠️ 大文件未专测 | S14 ntfs 双架构 load+map+读写；>4GiB 单文件读取未单独构造测试 |
| V-05 | 写入持久化 | ✅ PASS | S09 ntfs 写→重开读回；S13 保存→重启读回 |
| V-06 | 降级注入 | ✅ PASS | S11 degraded 场景（假驱动→P3:DEGRADED+横幅+系统项可用）双架构 |
| V-07 | noautorun / ESC | ✅ PASS | S15 noautorun（启动分区）；S11 ESC→Shell；P3 noautorun 由 Launcher 处理 |
| V-08 | 刷机闭环 | ✅ PASS | V-08b Launcher→选项→危险确认→ExecNsh→嵌套 Shell→fpt.nsh 执行；S12 FM 全链路（P3进入→列目录→预览→跨卷复制→执行→嵌套Shell→退出）；S13 编辑保存 |
| V-09 | auto: 排除启动盘 | ✅ PASS | S10 auto 候选 n=1 仅外部盘（启动盘含同文件被 VolSameDisk 排除）|
| V-10 | BootNext | ✅ PASS（宿主）/ ⚠️ QEMU 消费未专测 | S10 T10-2 变量编码黄金；QEMU varstore BootNext 实际消费未单独验证 |
| V-11 | JSON 容错 | ✅ PASS | S03 坏 JSON 逐条错误码；S11 坏配置→内置菜单+横幅 |
| V-12 | 文本模式 | ✅ PASS | S06 80x25(x64)/80x50(aa64)；S11 小屏 10x40 全状态不越界 |

**集成冒烟**：完整镜像（真实三应用）AA64 + x64 引导 PASS，Launcher 菜单完整渲染
（状态栏 P3:OK、JSON 工作项架构过滤 [arch mismatch]、FPT/AFU 危险标记 !、内置 FileManager/Editor/NVRAM/Shell/Reboot、键位条）。

## 4. 并行开发成效

- **19 任务包**，契约先行（Wave 0 冻结 13 契约头 + stub）+ core/glue 分离 + Ops 双后端 + worktree 隔离。
- **契约审批 2 项**（JsonDomArrayCount 改名、ModelTokenAt 新增）+ 所有权裁决（VolFree 不销毁 Fs）。
- **跨流互查价值**：单流宿主测试全绿但集成暴露的 bug——
  - 崩溃级 4：SELECTED 属性 bit7 越掩码（S08）、auto: 候选 Fs 悬空（S11）、嵌套 Shell argv[0] 递归（S10）、GetMapFromDevicePath 延迟释放陷阱（S12）。
  - 平台级 5：EfiMem ReallocatePool 参数序、ASSERT 缺 DebugLib、SCAN_* 宏重定义、SortLib 传递依赖、ExecVolumeLib Guids 漏列。
- **宿主 ASan/UBSan** 抓内存错误（S02 两个 use-after-free）；**QEMU 冒烟**抓固件交互/集成 bug；两者互补。
- **MockTui 属性守卫**（S08 建议）：非法属性宿主测试即拦截，防回归。
- 详见 docs/integration-findings.md（A 平台/契约 9 项 + B Shell 机制 8 项 + C 固件 quirks 8 项 + D 工具链 10 项 + E EfiMem 纪律）。

## 5. 局限与残余风险

1. **实机未验证**：全部验证在 QEMU（OVMF/ArmVirtQemu）。真实固件（AMI/Insyde/Phoenix/Ampere）的多 ESP 枚举、
   ConOut 行为、SB 关闭流程需实机矩阵确认（design §9.2）。兼容布局默认切换决策依赖实机结果。
2. **V-04 >4GiB 大文件、V-10 BootNext QEMU 消费**未单独构造测试（组件级已证，集成级待补）。
3. **Secure Boot 关闭**：镜像未签名，SB 锁定机器不可用；关 SB 触发 BitLocker PCR7 恢复（用户文档已警示）。
   跨机器流动工作盘的完整性风险靠物理保管（design §4 诚实 threat model）。
4. **厂商工具（fpt/afu）专有**：不随镜像分发，用户 drop-in；Boot Guard/Flash 描述符写保护与 SB 无关。
5. **ntfs-3g 驱动 quirks**：GetInfo 非标准尺寸协商、SetInfo 改名只认绝对路径（S09 已健壮处理）；
   驱动 GPLv2，分发须附源码要约（third_party/ntfs-3g/）。
6. **V-08 全链路盲键自动化脆弱**：菜单渲染在 exec 控制台切换后 serial 捕获不全；功能由组件冒烟+执行链路标记共同证明。

## 6. 交付物清单

- 代码：EasyEfiPkg/（10 库 + 3 应用 + 契约）、boot/（startup.nsh + ShellRunner）、third_party/ntfs-3g/、
  scripts/（build-module/build-shell/build-ntfs3g/mkimage/mktestimage/fix-gpt/verify-image/qemu-run/integrate）、
  config/launcher.example.json、tests/host/（框架+mocks）、docker/Dockerfile。
- 文档：docs/design.md（规格 D1–D17）、design-review.md（评审 F01–F25）、parallel-tasks.md（19 任务包）、
  integration-findings.md（集成实证）、user-guide.md（用户指南）、handoffs/S00–S17（各流交接）、pins/（依赖锁定）。
- 构建配方：docs/build-recipe.md（Docker easyefi-build + ~/.cache/easyefi/edk2 + -t GCC + CONF_PATH）。

## 7. 后续建议

1. 实机矩阵（≥2 x64 固件厂 + Ampere ARM64），确认多 ESP 路由与 SB 关闭流程，决定兼容布局默认。
2. 补 V-04 >4GiB 大文件读、V-10 BootNext QEMU 消费集成测试。
3. 可选自签 SB 扩展层（design §4，首版不做）。
4. 中文菜单 GOP+点阵字体扩展（首版 ASCII，design D7）。
5. 发布物附 SHA256SUMS + ntfs-3g 源码要约 + 用户指南。

**总评**：方案经批判性评审（25 项）→ 细化（17 决策）→ 并行实现（19 任务包）→ 集成验证（QEMU 双架构），
核心刷机工作流端到端跑通，架构缝（契约/core-glue/双后端）经受住集成考验。可进入实机验证与发布准备阶段。
