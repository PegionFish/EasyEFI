# EasyEFI 可用方案设计（v2 定稿候选）

- 日期：2026-11-22
- 状态：**已定稿 rev3**（用户已确认 Q1–Q4，见 §12；rev2 = FM/编辑器完全自研；rev3 = 面向大量 subagent 并行开发的架构细化，D17/§11）。本文档是设计规格，不含实现代码；执行待用户指令启动。
- 前序：`docs/design-review.md`（对 WorkBuddy v1 的批判性评审，问题编号 F01–F25 在本文中以 `[Fxx]` 引用）
- 项目：EasyEFI —— Easy-to-Use UEFI Shell Application & Image for system maintenance（本仓库，GPLv3）
- 所有关键机制均已对照 EDK2 master 源码与上游仓库一手资料核实（核实日志见评审文档 §3）；未核实项显式标注 `[V]` 并列入 §9 验证计划。

---

## 1. 目标与非目标

### 目标（原始需求 + 会话演进）

1. 基于**原版 UEFI Shell**（不魔改），Shell 全部原生能力零回归：`map`/`bcfg`/`.nsh`/第三方 EFI 工具（fpt、afu 等）。
2. **单一 GPT raw 镜像**同时支持 x86_64 与 ARM64；允许要求用户在固件启动菜单选择分区 1/2。
3. **TUI Launcher**：JSON 定义启动项，三分区（x86 ESP / ARM ESP / 数据区），工作启动项文件放数据区。
4. 能链式启动外部介质（Windows 安装盘 / Linux LiveCD）。
5. 数据区承载 **>4 GiB 单文件**载荷，且**可在 UEFI 环境现场编辑脚本/配置**（不重启进 GUI OS）。
6. 面向不熟悉 Shell 的用户：**自研**双架构文件管理器 + UTF-8 安全文本编辑器（基础功能，MVP 范围冻结，§7.1–7.3）。
7. Secure Boot：**默认关闭**（用户已拍板，接受"使用前关 SB"）；保留未来自签 SB 扩展路径。

### 非目标（首版不做）

- 不签名任何二进制、不生成/导入 PK/KEK/db（SB 关闭决策的直接推论）。
- 不做 Ventoy 式内存虚拟磁盘 / 自定义 bootloader。
- 不支持纯 Legacy BIOS 机器（无 UEFI 的机器不在范围）。
- 不集成任何第三方文件管理器/编辑器（UefiCommander/ufm/guedit 全部退役，决策 Q4）；不做网络功能。
- 首版 Launcher 不做中文图形界面（决策 D7，用户已确认 ASCII 方案，见 §12）。

---

## 2. 决策日志

| # | 决策 | 理由 / 依据 |
|---|---|---|
| D1 | 三分区 GPT raw `.img`，尺寸参数化（默认 8 GiB，约束 ≥ 载荷总和 + 1.5 GiB） | 需求 R2/R3；4G 镜像装不下 >4GiB 载荷 [F06] |
| D2 | P1/P2 = FAT32 ESP；P3 = NTFS（Microsoft Basic Data GUID） | ESP 规范 + 固件 fallback 只保证 FAT；>4GiB + 现场写入 ⇒ 唯一现成读写驱动是 pbatard/ntfs-3g [F07][F12] |
| D3 | **默认严格双 ESP 布局**（P1 只放 x64 fallback，P2 只放 AA64）；"双 fallback 同放 P1"兼容布局作为构建变体，实机验证后再考虑切换默认 | EDK2 枚举已核实支持逐 ESP 尝试 [F24]；兼容布局脚本依赖未实测语义，不做未验证的默认 |
| D4 | ntfs-3g 驱动放 P1/P2（每架构一份）；**apps（文件管理器/编辑器）也放 P1/P2** | 驱动消除鸡生蛋；apps 放 FAT 保证降级时仍可用 [F09] |
| D5 | Launcher **无条件拉起**；P3 不可达时进入 DEGRADED 模式（内置默认菜单 + 横幅），而不是停在裸 Shell | 驱动失败时最需要的恰是菜单和文件管理器 [F09]；`load` 命令吞错误状态，脚本侧检测不可行 [F25] |
| D6 | SB 默认关闭；威胁模型表述修正；文档写明 SB 锁定机器不可用 + BitLocker 恢复风险 | 用户决策保留；v1 论证过度 [F13] |
| D7 | **三个界面（Launcher/FileManager/Editor）全部 ConOut 文本 TUI，菜单文本 ASCII**（用户已确认）；无 GOP 依赖。中文界面列为可选扩展（GOP+内嵌 CJK 点阵字体） | 固件文本控制台无 CJK 字形 [F04]；自研后无 guedit 的 GOP 依赖 [F17]，三界面固件要求与 Launcher 完全一致 |
| D8 | 核心栈：原版 Shell + ntfs-3g（GPLv2）+ **自研 FileManager/Editor**（本仓库 GPLv3，单一源码 X64/AA64）。**UefiCommander/ufm/guedit 全部退役**（用户决策 Q4，推翻 Q2） | 第三方双架构能力不统一（UC 仅 X64、guedit AA64 未验证、ufm 停更无 UTF-8）[F05][F17]；自研消除许可门槛与未验证移植风险，场景窄（找文件执行/简单改脚本）成本可控 |
| D15 | ~~镜像分 internal/public 两种构建变体~~ **已废止**（Q4 后第三方全退，两种变体的差异消失；厂商工具始终为用户 drop-in，不随任何镜像分发） | 决策 Q4 |
| D16 | 自研基础版 FM/编辑器：**MVP 范围冻结**（§7.2/7.3），共享 TUI/Exec 契约库（细分模块见 §11），两个独立 UEFI_APPLICATION；编辑器 ASCII 显示 + **UTF-8 字节往返保留**（非 ASCII 占位显示、保存不损坏）；无 GOP/鼠标依赖 | 用户场景 = 分区中找文件执行 + 简单编辑脚本；统一双架构能力优先于第三方完整功能 |
| D17 | **并行优先架构**：契约先行（Wave 0 冻结头文件+stub）；应用拆 core（纯状态机，宿主可测）+ glue（UEFI 薄胶水）；Ops 表双后端（UEFI 真实 / 宿主 mock+POSIX）；17 个独占模块流，leader 独占聚合文件；每流独立 worktree/Conf/Build + 最小测试镜像 | 用户要求方案支持大量 subagent 并行开发（2026-11-22）；并行度由架构缝保证而非运气：无共享文件竞争、无串行等待链、验证不抢资源 |
| D9 | `.nsh` 菜单项 = 嵌套 Shell 执行：`LoadImage(Shell.efi)` + `LoadOptions = L"shell.efi <脚本全限定路径> [参数]"`（**argv[0] 镜像名令牌必需**：Shell 跳过 Argv[0]，缺它则 file-name 不解析→startup.nsh 递归→栈耗尽崩溃，S10 QEMU 实证；含空格路径加双引号；file-name 隐式 `-nostartup`）；脚本结束后停在嵌套 Shell 提示符 | `-s` 选项不存在 [F01]；隐式 -nostartup 防递归；停留便于查看刷机输出 [F20] |
| D10 | `auto:` 候选排除启动盘全部分区；`boot:` 默认走动态 NVRAM 子菜单 | 防自链式循环 [F03]；Boot#### 编号逐机不同 [F10] |
| D11 | 工作启动项（entries/payloads/scripts/launcher.json）在 P3；基础设施（Shell/驱动/Launcher/apps/厂商工具）在 P1/P2 | 对 R3"启动项文件在数据区"的显式偏离：基础设施放 FAT 是降级可用性与启动时序的要求 [F09] |
| D12 | 厂商刷机工具（fpt/afu）**用户自备 drop-in，不入库、不随镜像对外分发** | 专有许可 [F21]；仓库 GPLv3 |
| D13 | 第三方组件一律锁定 commit/构建时拉取，仓库不入库二进制；镜像分发附源码获取指引 | GPLv2/GPLv3 合规 [F12]；本项目自身 GPLv3 |
| D14 | 写盘后修复 GPT 备份头（`sgdisk -e`）+ 可选扩容 P3；Windows 写盘用 Rufus DD / balenaEtcher | [F08][F18] |

---

## 3. 镜像布局与启动链

### 3.1 分区表（默认严格布局）

| 分区 | GPT 类型 | 文件系统 | 大小 | 内容 |
|---|---|---|---|---|
| P1 `BOOTX64` | ESP `C12A7328-F81F-11D2-BA4B-00A0C93EC93B`（sgdisk `EF00`） | FAT32 | 512 MiB | 见 3.2 |
| P2 `BOOTAA64` | ESP 同上 | FAT32 | 512 MiB | P1 的 AA64 镜像（同构路径，AA64 二进制） |
| P3 `TOOLKIT` | **Microsoft Basic Data `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7`（sgdisk `0700`）** [F07] | NTFS | 剩余空间 | 见 3.2 |

镜像 1 MiB 对齐；P3 用 `mkfs.ntfs -f`（快速格式化）。

### 3.2 文件树

```
P1 (FAT32, x64)                          P2 (FAT32, AA64)
├── EFI/BOOT/BOOTX64.EFI   ← 原版 Shell  ├── EFI/BOOT/BOOTAA64.EFI
├── EFI/BOOT/startup.nsh   ← 规范位置    ├── EFI/BOOT/startup.nsh   （内容与 P1 完全相同）
├── startup.nsh            ← 根目录兜底副本├── startup.nsh
├── EFI/Drivers/ntfs.efi   ← ntfs-3g x64 ├── EFI/Drivers/ntfs.efi   ← ntfs-3g AA64
├── EFI/Launcher/Launcher.efi ← x64      ├── EFI/Launcher/Launcher.efi ← AA64
├── EFI/Apps/FileManager.efi ← 自研 x64  ├── EFI/Apps/FileManager.efi ← 自研 AA64
├── EFI/Apps/Editor.efi    ← 自研 x64    ├── EFI/Apps/Editor.efi    ← 自研 AA64
└── EFI/Tools/*.efi        ← 厂商工具自备└── EFI/Tools/*.efi        ← 仅收得到 AA64 版的工具

P3 (NTFS)
├── .easyefi               ← 卷标记（内容：镜像版本/构建 UUID，P3 发现用）
├── launcher.json          ← 工作菜单配置（UTF-8 无 BOM，内容 ASCII，见 §6）
├── noautorun              ← 可选空文件：存在则 Launcher 直接退回 Shell [F19]
├── entries/*.efi          ← 工作启动项（WinPE 等）
├── payloads/*             ← >4 GiB ISO/WIM/固件镜像/诊断包
├── scripts/*.nsh          ← 可现场编辑的脚本（fpt.nsh、afu.nsh…）
└── docs/*                 ← 离线文档
```

说明：
- 严格布局下每个 ESP 只含本架构二进制，**文件名架构中性**（`ntfs.efi`/`Launcher.efi`/`FileManager.efi`/`Editor.efi`），两 ESP 的 startup.nsh 逐字节相同。
- `startup.nsh` 放 `\EFI\BOOT\`（Shell 优先查自身所在目录）+ 根目录兜底副本（PATH 查找）——查找顺序已核实 [F14]。
- apps 放 ESP 而非 P3：NTFS 驱动失败时文件管理器/编辑器仍可用 [F09]。P3 只放"工作内容"。
- 厂商工具目录为空出厂；用户按 docs 指引自行放入（不入库不分发 [F21]）。

### 3.3 startup.nsh（默认严格布局，两 ESP 相同）

```nsh
echo -off
load %homefilesystem%\EFI\Drivers\ntfs.efi
map -r
if exist %homefilesystem%\noautorun then
  echo [EasyEFI] noautorun flag on boot partition - staying in UEFI Shell.
else
  %homefilesystem%\EFI\Launcher\Launcher.efi
endif
```

已核实的语义依据：
- `%homefilesystem%` 由 Shell 在定位 startup.nsh 时设置为启动分区映射名（如 `fs0:`），不依赖 fsN 猜测 [F14]。
- `load` 不带 `-nc` 时自动 `ConnectController(..., TRUE)`，驱动即刻绑定 NTFS 分区；`map -r` 刷新映射（Load.c L47 + EfiFs/ntfs-3g 官方用法）。
- **脚本不做驱动加载状态检测**：`load` 吞失败状态 [F25]；降级判定由 Launcher 运行时完成（D5）。普通命令失败不中止脚本（Shell.c RunCommand），脚本必然走到 Launcher。
- Shell 启动自带倒计时（`PcdShellDefaultDelay`，ESC 跳过 startup.nsh）——这是第一层防误触；Launcher 内还有第二层（§5.5）。
- Launcher 返回（用户选"返回 Shell"或 P3 noautorun）→ 脚本结束 → 原版 Shell 提示符，原生功能零回归。

### 3.4 兼容布局变体（构建选项 `LAYOUT=compat`）[F24] —— ShellRunner trampoline（QEMU 双架构实证通过）

适用：实机发现"只扫第一个 ESP"的固件。P1 同时放两个架构的全套二进制，P2 保持 AA64 镜像供手动选择：

```
P1: EFI/BOOT/BOOTX64.EFI = ShellRunnerX64.efi（trampoline）
    EFI/BOOT/BOOTAA64.EFI = ShellRunnerAA64.efi（trampoline）
    EFI/BOOT/ShellX64.efi + ShellAA64.efi（原版 Shell）
    EFI/BOOT/startup-x64.nsh + startup-aa64.nsh（架构专属脚本）
    EFI/Drivers/ntfs_x64.efi + ntfs_aa64.efi
    EFI/Launcher/LauncherX64.efi + LauncherAA64.efi
    EFI/Apps/FileManagerX64.efi + FileManagerAA64.efi + EditorX64.efi + EditorAA64.efi
```

**架构分流在 UEFI 层完成，不在 .nsh 层**（实证教训：脚本内启动异架构应用失败会**中止脚本**，
try-both 脚本方案不可行）。ShellRunner（boot/ShellRunner/，~110 行，双架构同源码）作为 fallback：
1. 枚举 SimpleFileSystem 卷，`FileDevicePath` 逐卷尝试 LoadImage `\EFI\BOOT\Shell{Arch}.efi`
   （**不能依赖自身 LoadedImage->FilePath 做前缀手术**——短格式 USB 引导展开时它可能不含 FILEPATH 节点，实证）；
2. 同卷预加载 `\EFI\Drivers\ntfs_{arch}.efi`（LoadImage+StartImage+ConnectController；失败不致命，降级归 Launcher）；
3. 设 Shell 镜像 `LoadOptions = L"shell.efi \EFI\BOOT\startup-{arch}.nsh"`
   （**首 token 被 Shell 当镜像名跳过，必须带 Argv0 前缀**，实证；LoadOptions 经 LoadedImage 协议设置，gBS->LoadImage 无此参数）；
4. StartImage → 嵌套 Shell 以 file-name 模式运行架构脚本（隐式 -nostartup，无递归）。

架构脚本（嵌套 Shell **不设置 %homefilesystem%**，用相对路径；驱动已由 trampoline 预加载）：

```nsh
echo -off
map -r
if exist noautorun then
  echo [EasyEFI] noautorun flag on boot partition; staying in UEFI Shell.
else
  EFI\Launcher\LauncherX64.efi
endif
```

（AA64 镜像同理；echo 文案不得含独立 `-` token——Shell echo 会当 flag 解析，实证。）
固件先试 BOOTX64 再 BOOTAA64（异架构 trampoline 被 LoadImage 拒绝，一行预期噪音）；
trampoline 失败时 ArmVirtQemu 类固件回退内置 Shell（Boot0002 "EFI Internal Shell"）——失败模式可接受但非设计路径。
默认布局是否切换由实机矩阵结果决定（D3）。

### 3.5 固件引导路径（已核实）

EDK2 `BmBoot.c`：`BmEnumerateBootOptions()` 按整盘生成启动项（先可移动后固定）；`EfiBootManagerBoot()` 展开时对该盘每个 SimpleFileSystem 分区尝试 `\EFI\BOOT\BOOT{机器类型}.EFI`，执行第一个可加载者。x64 命中 P1；ARM64 在 P1 落空后命中 P2。真实固件存在差异 → 兼容布局兜底 + 固件启动菜单手动选分区（需求 R2 明确允许）。

---

## 4. Secure Boot  posture（D6）

- 镜像按"目标机已关闭 SB"设计；全部 `.efi` 不签名。Launcher 状态栏读 `SecureBoot`/`SetupMode` 变量（`EFI_GLOBAL_VARIABLE_GUID`）实时显示，不硬编码。
- **诚实的威胁模型**（修正 v1 [F13]）：SB 关闭后镜像无启动链完整性校验。本镜像是跨机器流动的工作盘，U 盘被掉包/植入意味着每台目标机在固件上下文执行攻击者代码——SB+自签 db 本可防御此类攻击。接受该风险的前提是**物理保管**；SHA256 清单（§10）只能防意外损坏/低水平篡改，不是安全边界。
- **部署边界**：SB 锁定（BIOS 密码/企业策略）且无法进入固件设置的机器，本镜像不可用。写入用户文档首页。
- **BitLocker 警告**：关闭 SB 改变 PCR7，目标机 Windows 可能要求恢复密钥。操作前确认密钥在手或已暂停保护。写入文档 + Launcher `boot:` 子菜单帮助文本。
- **Boot Guard / ME Flash 描述符写保护 ≠ SB**：fpt/afu 被拦可能是闪存写保护，与签名无关（恢复 v1 会话早期丢失的提示）。
- 可选扩展（不在首版）：自签 PK/KEK/db + sbsign 全部第一方与第三方二进制（ntfs-3g GPLv2 自签分发需履行源码义务；厂商工具可技术签名但许可仍禁止分发）。届时 Windows/Linux 介质需 db 含微软 CA 或另行处理。

---

## 5. Launcher 规格（EasyEfiPkg）

### 5.1 工程与依赖

```
EasyEfiPkg/
├── EasyEfiPkg.dsc / .dec        ← leader 独占聚合文件（!include 各模块碎片）
├── Include/Contract/            ← leader 独占：冻结契约头 + Ops 表（§11.3）
├── Library/
│   ├── Utf8Lib/                 ← 纯逻辑：UTF-8 编解码（宿主可测）
│   ├── EditorModelLib/          ← 纯逻辑：行数组/撤销/查找（宿主可测）
│   ├── ConfigCoreLib/           ← 纯逻辑：schema 校验 + 解析适配器（双后端）
│   ├── ExecPathLib/             ← 纯逻辑：路径文法/规范化（宿主可测）
│   ├── VolumeModelLib/          ← 纯逻辑：GPT 设备路径匹配/P3 发现算法（宿主可测）
│   ├── TuiScreenLib/            ← UEFI 胶水：ConOut 模式/缓冲/脏区重绘
│   ├── TuiInputLib/             ← UEFI 胶水：ConIn 按键解码
│   ├── TuiWidgetLib/            ← 控件：列表/对话框/输入行/横幅（mock 屏幕可宿主测）
│   ├── ExecVolumeLib/           ← UEFI 胶水：卷枚举/P3 发现（FsOps 双后端）
│   └── ExecImageLib/            ← UEFI 胶水：LoadImage/StartImage/嵌套 Shell
├── Application/
│   ├── Launcher/                ← core（状态机，宿主测）+ glue
│   ├── FileManager/             ← core + glue（§7.2）
│   └── Editor/                  ← core + glue（§7.3）
└── （每模块自带 .inf + module.dsc.inc 碎片 + 宿主测试 + 冒烟断言脚本）
```

- 模块类型 `UEFI_APPLICATION`，编译 X64 + AARCH64 两份；架构由 `MDE_CPU_X64`/`MDE_CPU_AARCH64` 编译期决定。
- JSON：**RedfishPkg JsonLib**（Jansson 封装）。已核实 `LIBRARY_CLASS = JsonLib|DXE_DRIVER UEFI_APPLICATION UEFI_DRIVER`，应用可用；`JsonLoadBuffer(Buffer, Len, Flags, &Error)` 存在。**JsonLib 不支持浮点/科学计数**（JsonLib.h 原文）→ schema 只允许整数 [F15][F22]。
- `.dsc` 库解析（v1 片段不完整 [F22]，以锁定 edk2 树实际路径为准）：

```
[LibraryClasses]
  JsonLib|RedfishPkg/Library/JsonLib/JsonLib.inf
  Ucs2Utf8Lib|RedfishPkg/Library/Ucs2Utf8Lib/Ucs2Utf8Lib.inf
  RedfishCrtLib|RedfishPkg/Library/RedfishCrtLib/RedfishCrtLib.inf
  # 其余 BaseLib/PrintLib/MemoryAllocationLib/DevicePathLib/UefiLib/
  # UefiBootServicesTableLib/UefiRuntimeServicesTableLib 等按 ShellPkg.dsc 模板
```

- 入口防御性动作：`gBS->SetWatchdogTimer(0, 0, 0, NULL)`（防个别固件看门狗未关导致菜单停留时复位）。

### 5.2 P3 发现（不写死 fsN）

1. 由自身 `LoadedImage.DevicePath` 提取启动分区的 HD(GPT) 节点 → 得到**启动盘身份**（父设备路径 + GPT Signature）与分区号。
2. 枚举全部 `SimpleFileSystem` 句柄，匹配"同盘 + PartitionNumber == 3"的卷。
3. 验证候选卷根目录存在 `\.easyefi` 标记（或 `launcher.json`）→ 确认为 P3。
4. 兜底：全盘扫描 `\.easyefi` 标记（多卷命中时优先启动盘；再多个则列候选让用户选）。
5. 全部失败 → **DEGRADED 模式**（D5）：红色横幅 `DATA VOLUME UNAVAILABLE - NTFS driver failed or P3 missing`，菜单 = 内置系统条目（§5.4）+ `auto:` 外部介质条目；`data:` 条目灰显并标注原因。

### 5.3 配置加载与容错

- 读 `P3:\launcher.json`（UTF-8 无 BOM；`JsonLoadBuffer` + `EDKII_JSON_REJECT_DUPLICATES`）。
- 解析失败/文件缺失 → 内置默认菜单 + 错误横幅（显示 JsonLib 错误行号），**不挂死**。
- 逐项校验（§6 规则）：非法项跳过并在帮助页列出原因；未知字段告警忽略（前向兼容）。
- `R` 键 = 重扫外部介质 + **重新加载 launcher.json**（现场改完配置立即生效）。

### 5.4 菜单构成

- **系统条目（内置，不在 JSON 里）**：文件管理器（自研 FileManager）、文本编辑器（自研 Editor）、NVRAM 启动项子菜单（`boot:menu`）、返回 Shell、重启。apps 路径按当前架构解析到启动 ESP 的 `\EFI\Apps\`（兼容布局用架构后缀名）。自研应用与 Launcher 同固件要求（ConOut + SimpleFileSystem），无 GOP/鼠标依赖，**双架构能力完全一致**（D16）。
- **工作条目（来自 JSON）**：按 `arch` 过滤——不匹配当前架构的灰显保留展示；`danger` 红色 `!` 标记。
- 内置默认菜单（DEGRADED/JSON 缺失时）：上述系统条目 + `auto:/EFI/BOOT/BOOT{当前架构}.EFI` 外部介质发现项。

### 5.5 交互与 UI（D7）

- 启动 `skip_timeout` 秒倒计时（JSON 顶层字段，默认 2，0=禁用），ESC → 直接回 Shell。
- `QueryMode` 枚举 ConOut 文本模式，取最大可用（保底 80×25）；**1280×720 是 GOP 像素分辨率，不是文本模式** [F16]。
- 布局：顶部状态栏 `EasyEFI vX.Y | x64/AA64 | SB:OFF | P3:OK/DEGRADED | 100x31`；中部编号菜单（选中行高亮 + 展开 desc）；底部键位条。纯 ASCII 字符画框（固件字体不保证 CP437 制表符）。
- 颜色语义（三界面统一，由共享 TUI 契约库强制一致）：高亮=选中，红=危险/错误，灰=不可用。
- 键位：↑↓/数字选择，Enter 执行，`S`/ESC 返回 Shell，`R` 重扫+重载配置，`Q` 同 S，`?` 帮助。
- `danger=true`（隐含 confirm）→ 全屏 Y/N 二次确认，默认 N。
- **中文显示限制**：固件 ConOut 无 CJK 字形，name/desc 强制 ASCII [F04]（用户已确认）。中文菜单 = 未来 GOP+内嵌点阵字体扩展。

### 5.6 动作语义

| action | 行为 |
|---|---|
| `exec` | 解析 path → `gBS->LoadImage`（`options` UTF-8→UCS-2 作为 LoadOptions）→ 清理自身事件/恢复控制台 → `StartImage`。目标调 ExitBootServices 则一去不返（预期）；返回则显示 EFI_STATUS 错误页后回菜单。**path 以 `.nsh` 结尾** → 嵌套 Shell：LoadImage(本 ESP 的 Shell.efi) + LoadOptions=`L"<运行时全限定脚本路径> [args]"`（隐式 -nostartup [F01]）；脚本结束停在嵌套 Shell 提示符，`exit` 回 Launcher [F20]。脚本内部不得硬编码 fsN（嵌套 Shell 重新 map）。 |
| `boot` | `boot:XXXX`（十六进制 Boot####）或 `boot:menu`（动态读 BootOrder/Boot#### 描述枚举，过滤 ACTIVE）。选中 → 写 `BootNext`（属性按 UEFI 规范 §3.3，BS\|RT）→ `gRT->ResetSystem(EfiResetCold, ...)`。帮助文本含 BitLocker 警告。 |
| `shell` | 置 `EASYEFI_RAN=yes`（若 Shell 协议可用）→ `return EFI_SUCCESS` 回父 Shell。 |
| `reset` | 确认 → `ResetSystem(EfiResetCold)`。 |

- Launcher 任何正常退出路径都先置 `EASYEFI_RAN=yes`（兼容布局架构探测依赖，§3.4）。
- `auto:` 解析：枚举 SimpleFileSystem，**排除启动盘（同 GPT Signature）的所有分区** [F03]，按相对路径找文件；0 命中 → 提示；多命中 → 候选列表（显示卷描述/设备路径摘要）让用户选。

---

## 6. launcher.json schema v1

顶层：

| 字段 | 类型 | 约束 |
|---|---|---|
| `title` | string | ASCII，≤40 字符 |
| `version` | string | 语义化版本字符串（仅展示/日志） |
| `skip_timeout` | int | 0–30 秒，默认 2（取代 v1 语义不明的 autostart/skip_key [F15]） |
| `items` | array | 工作条目 |

item：

| 字段 | 类型 | 约束 |
|---|---|---|
| `id` | string | 必填，ASCII，唯一 |
| `name` | string | 必填，ASCII ≤40（ConOut 无 CJK [F04]） |
| `desc` | string | 可选，ASCII ≤76 |
| `arch` | array | `["x64","aarch64"]` 子集；**规范枚举，废弃 v1 的 `arm64` 写法** [F15] |
| `action` | enum | `exec` / `shell` / `boot` / `reset` |
| `path` | string | 见下方路径文法；`shell`/`reset` 不需要 |
| `options` | string | 可选，仅 exec；UTF-8→UCS-2 后作 LoadOptions |
| `danger` | bool | 默认 false；true 隐含强制确认 |
| `confirm` | bool | 默认 false；普通确认（danger 之外的一般二次确认） |

路径文法（统一正斜杠；反斜杠输入规范化接受 [F15]）：

| 前缀 | 语义 |
|---|---|
| `data:/<rel>` | P3 根相对路径（运行时解析为实际 fsN，不写死） |
| `auto:/<rel>` | 枚举外部介质（排除启动盘）找该相对路径 |
| `fs<N>:/<rel>` | 显式卷（不推荐，卷号漂移；帮助页警告） |
| `boot:XXXX` / `boot:menu` | NVRAM 启动项 |

数值一律整数（JsonLib 无浮点 [F22]）。未知字段告警忽略；非法项跳过可追溯。

示例（替换 v1 样例；修正中文 name、`data:` 斜杠混乱、linux-live 架构/路径错配、boot:0001 硬编码 [F04][F10][F15]）：

```json
{
  "title": "EasyEFI Work Toolkit",
  "version": "1.0.0",
  "skip_timeout": 2,
  "items": [
    { "id": "win-usb", "name": "Windows installer (external)",
      "desc": "Find BOOTX64.EFI on other media",
      "arch": ["x64"], "action": "exec",
      "path": "auto:/EFI/BOOT/BOOTX64.EFI" },

    { "id": "linux-x64", "name": "Linux LiveCD x64 (external)",
      "arch": ["x64"], "action": "exec",
      "path": "auto:/EFI/BOOT/BOOTX64.EFI" },

    { "id": "linux-arm64", "name": "Linux LiveCD ARM64 (external)",
      "arch": ["aarch64"], "action": "exec",
      "path": "auto:/EFI/BOOT/BOOTAA64.EFI" },

    { "id": "winpe-local", "name": "Local WinPE (data volume)",
      "arch": ["x64"], "action": "exec",
      "path": "data:/entries/winpe.efi" },

    { "id": "fpt", "name": "Intel FPT flash tool",
      "desc": "x64 only. HIGH RISK: writes SPI flash",
      "arch": ["x64"], "action": "exec",
      "path": "data:/scripts/fpt.nsh", "danger": true },

    { "id": "afu", "name": "AMI AFU flash tool",
      "desc": "x64 only. HIGH RISK: writes SPI flash",
      "arch": ["x64"], "action": "exec",
      "path": "data:/scripts/afu.nsh", "danger": true },

    { "id": "nvram-boot", "name": "Boot installed OS (NVRAM menu)",
      "desc": "Pick firmware boot entry. BitLocker: have recovery key ready",
      "arch": ["x64", "aarch64"], "action": "boot",
      "path": "boot:menu" },

    { "id": "shell", "name": "Return to UEFI Shell",
      "arch": ["x64", "aarch64"], "action": "shell" },

    { "id": "reboot", "name": "Reboot",
      "arch": ["x64", "aarch64"], "action": "reset", "confirm": true }
  ]
}
```

（文件管理器/编辑器是内置系统条目，不再占 JSON 位——基础设施与工作内容分离，D11。）

---

## 7. 工具栈与许可（D8/D12/D13/D16）

| 组件 | 来源（锁定 commit/tag） | 许可 | 架构 | 角色 |
|---|---|---|---|---|
| UEFI Shell | edk2 ShellPkg（锁定 edk2-stable 标签） | BSD-2+Patent | X64/AA64 源码构建 | 原版引导 + 原生能力 |
| NTFS 读写驱动 | github.com/pbatard/ntfs-3g（默认分支 rebase-on-2026.9.28，锁 commit；`uefi-driver.dsc`） | **GPLv2** [F12] | X64/AA64 源码构建（README 确认全架构） | P3 可见可写；最后句柄关闭即完整卸载（防拔盘损坏，操作中途除外） |
| 文件管理器 FileManager | **自研**（EasyEfiPkg/Application/FileManager） | GPLv3（本仓库） | 单一源码 X64/AA64 | 刷机导向 MVP（§7.2）：找固件/EFI → 核对 → 执行；ConOut 纯键盘 |
| 文本编辑器 Editor | **自研**（EasyEfiPkg/Application/Editor） | GPLv3（本仓库） | 单一源码 X64/AA64 | 脚本导向 MVP（§7.3）：≤256 KiB，UTF-8 字节往返保留；ConOut 纯键盘 |
| fpt.efi / AfuEfi64.efi 等 | 用户自备（Intel CSME System Tools / AMI AFU 包） | 专有 | 多为 x64 | drop-in 到 `\EFI\Tools\`；**不入库、不随镜像对外分发** [F21] |
| Shell 内置 edit | 随 Shell | BSD | 双架构 | 最后兜底（ASCII/UCS-2，无 UTF-8） |

合规要点：
- 本仓库 GPLv3；镜像内仅聚合 ntfs-3g（GPLv2）与原版 Shell（BSD-2+Patent），自研应用全部 GPLv3，**无许可门槛**。对外分发镜像须附 ntfs-3g 对应源码获取指引（构建脚本记录锁定 commit，`third_party/` 拉取脚本即满足书面要约形式）。
- 厂商刷机工具为用户 drop-in，不入库、不随镜像分发（内部使用与对外分发的边界在 README 声明）。
- 退役记录（决策 Q4）：UefiCommander（自身许可未声明/仅 X64 验证/俄英 UI）、guedit（GOP+LVGL，AA64 未验证）、ufm（2023 后停更/内置编辑无 UTF-8）——评审 F05/F17 的最终解是自研。

### 7.1 共享基础设施：契约库 + Ops 表（并行开发缝，D17）

三应用不直接依赖彼此实现，全部经由冻结契约（`Include/Contract/`，leader 独占，§11.3）：

| Ops 表 / 契约 | 职责 | 真实后端（UEFI） | 宿主后端（core 单测用） |
|---|---|---|---|
| TuiOps（Screen/Input/Widget/Theme） | 模式/缓冲/按键/控件/配色键位常量 | TuiScreenLib/TuiInputLib/TuiWidgetLib（ConOut/ConIn） | mock 屏幕（字符矩阵断言）+ 脚本化按键序列 |
| FsOps | open/read/write/close/flush/delete/rename/mkdir/enum/info | EFI_SIMPLE_FILE_SYSTEM_PROTOCOL（ExecVolumeLib） | **POSIX**（临时目录真实读写） |
| ExecOps | resolve/exec_efi/exec_nsh/boot_next/reset | ExecImageLib + ExecPathLib | mock 记录器（断言调用路径/参数） |
| 纯逻辑库 | Utf8Lib / EditorModelLib / ConfigCoreLib / ExecPathLib / VolumeModelLib | —（纯 C，零 UEFI 依赖） | 直接编译直接测 |

- **应用 = core（纯状态机，只依赖 Ops 表，宿主可测）+ glue（UEFI 后端装配与入口，QEMU 冒烟）**。Launcher/FM/Editor 三者同构。
- 单一源码、X64/AA64 双编译；配色/键位/确认语义由 TUI 契约强制三界面一致。
- 阈值参数走 PCD（可配置不硬编码）：`PcdEditorMaxFileSize`（默认 256 KiB）、`PcdPreviewMaxBytes`（默认 16 KiB）、`PcdUndoDepth`（默认 256）、Shell 倒计时 PCD 等。
- 模块分解、流所有权、波次依赖与隔离配方：见 §11。

### 7.2 FileManager MVP（刷机导向，范围冻结）

锚定场景：**找到要刷的固件 / 要执行的 EFI 的位置 → 核对 → 执行；必要时改脚本**。

| 能力 | 说明 |
|---|---|
| 卷列表 | 全部 SimpleFileSystem（标签/FS 类型/容量/P3 标记），键盘切换 |
| 目录导航 | 进入/上级/返回；目录优先按名排序；列：名称/大小/修改时间 |
| 查找 | 当前目录键入即过滤；递归文件名子串搜索（结果列表跳转）——服务"找固件位置" |
| 预览栏 | 右侧 bounded 读 ≤16 KiB：文本显示头部；**二进制显示大小 + hex 头部**（固件识别）；不整文件加载、不做流式翻页 |
| 执行 | .efi → LoadImage+StartImage；.nsh → 嵌套 Shell（D9）；**确认框显示全路径 + 文件大小**（刷错文件是最高风险） |
| 编辑 | F4 → Editor（文本文件）；二进制拒绝并提示看 hex 预览 |
| 文件操作 | 单文件级：复制（含跨卷，覆盖确认）/改名/删除（文件或空目录，确认）/mkdir/新建空文件 |
| 其它 | R 刷新重扫；状态栏常驻当前全路径（便于抄进脚本）；ESC 逐级返回 |

键位：↑↓/PgUp/PgDn 导航，Enter 进入目录/执行文件，Backspace/ESC 上级，F3 预览切换文本/hex，F4 编辑，F5 复制，F6 改名，F7 新建目录/文件，F8 删除，V 卷列表，`/` 过滤，S 递归搜索，R 刷新，F10 退出。底部键位条常驻。

**不做**：移动（复制+删除达成）、递归目录复制/删除、双栏标记批量操作、按内容搜索、压缩包/网络/虚拟磁盘、任何磁盘/分区级操作、鼠标。

### 7.3 Editor MVP（脚本导向，范围冻结）

- 对象：`.nsh` / `launcher.json` / 小型配置；**硬上限 256 KiB（PCD）**，超限拒绝打开并提示用 FM 预览。场景明确不处理大文本。
- 模型：行数组（每行保留原始终止符）；打开/保存/另存为；插入/删除/回车/方向键/PgUp/PgDn/Home/End；行号；状态栏 = 文件名 + 修改标记 `*` + 行:列 + 编码。
- **UTF-8 安全策略**：解码为码点序列编辑；非 ASCII 显示为单格黄色占位符，光标处状态栏显示 U+XXXX；保存按原 token 重编码 → **非 ASCII 字节往返保留**（中文注释不损坏，只是无字形显示）；非法 UTF-8 字节按不透明 token 原样保留；BOM 有则保留、不添加；行终止符逐行保留，新行随文件主约定。
- 查找：子串、大小写开关、上一个/下一个；跳行；有界撤销栈（256 步，连续插入合并为一步）。
- 退出：有修改时三键确认（保存/放弃/取消）。
- **不做**：替换、多文件/标签页、鼠标、hex 编辑、语法高亮、GOP、>256 KiB 文件。

### 7.4 刷机工作流闭环（锚定用例，进验证矩阵 V-08）

```
Windows 把固件/脚本放入 P3
  → 启动进 Launcher → 文件管理器
  → 卷列表选 P3 → 导航/搜索找到固件（预览栏核对大小 + hex 头部）
  → 找到 fpt.nsh → F4 编辑（改固件路径/参数）→ 保存
  → 执行 fpt.nsh（确认框：全路径 + 大小）→ 嵌套 Shell 运行 fpt
  → 查看输出 → exit 回 Launcher
```

FM/Editor 的存在意义就是这条链；不服务这条链的功能请求默认拒绝（范围冻结，D16）。

---

## 8. 构建与打包流水线

环境：WSL2 Ubuntu 22.04+ / Linux；`build-essential gcc-aarch64-linux-gnu uuid-runtime mtools sgdisk dosfstools ntfs-3g`；edk2 以 submodule 锁定 tag，`git submodule update --init --recursive`。

```bash
# 1) Shell（两架构）
build -p ShellPkg/ShellPkg.dsc -a X64 -t GCC5 -b RELEASE
export GCC5_AARCH64_PREFIX=aarch64-linux-gnu-
build -p ShellPkg/ShellPkg.dsc -a AARCH64 -t GCC5 -b RELEASE

# 2) EasyEfiPkg 全部应用（Launcher/FileManager/Editor，两架构）
build -p EasyEfiPkg/EasyEfiPkg.dsc -a X64 -t GCC5 -b RELEASE
build -p EasyEfiPkg/EasyEfiPkg.dsc -a AARCH64 -t GCC5 -b RELEASE

# 3) ntfs-3g（在其仓库内，PACKAGES_PATH 含 edk2；两架构）
build -a X64 -b RELEASE -t GCC5 -p uefi-driver.dsc
build -a AARCH64 -b RELEASE -t GCC5 -p uefi-driver.dsc
```

镜像（尺寸参数化 [F06]；GUID/losetup 修正 [F07][F18]；单一构建变体——第三方全退后 internal/public 差异消失，厂商工具始终不随镜像，D15 已废止）：

```bash
IMG_SIZE_GB=${IMG_SIZE_GB:-8}        # 校验：>= 载荷总和 + 1.5
truncate -s "${IMG_SIZE_GB}G" easyefi.img
sgdisk -Z easyefi.img
sgdisk -n 1:1M:+512M -t 1:EF00 -c 1:BOOTX64  easyefi.img
sgdisk -n 2:0:+512M  -t 2:EF00 -c 2:BOOTAA64 easyefi.img
sgdisk -n 3:0:0      -t 3:0700 -c 3:TOOLKIT  easyefi.img
LOOP=$(sudo losetup -fP --show easyefi.img)
sudo mkfs.vfat -F32 -n BOOTX64  "${LOOP}p1"
sudo mkfs.vfat -F32 -n BOOTAA64 "${LOOP}p2"
sudo mkfs.ntfs -f -L TOOLKIT    "${LOOP}p3"
# 挂载三分区，按 §3.2 文件树拷贝；P3 写入 .easyefi 标记（版本+构建UUID）
# 无签名环节（SB 关闭）
sudo losetup -d "$LOOP"
```

写盘与写盘后修复 [F08][F14]：

```bash
# Linux: dd 到整盘（U 盘容量 >= 镜像）
sudo dd if=easyefi.img of=/dev/sdX bs=1M status=progress
# 修复备份 GPT 到磁盘末尾（U 盘大于镜像时必需）
sudo sgdisk -e /dev/sdX && sudo partprobe /dev/sdX
# 可选：扩容 P3 占满剩余空间（sgdisk 重建分区3 + ntfsresize），由 fix-gpt 脚本提供
```

Windows 写盘：Rufus（DD 模式）或 balenaEtcher。Windows 日常访问：P3 自动挂载盘符（0700+NTFS）；P1/P2 为 ESP 不自动分配盘符（专家可用 diskpart assign，文档说明）→ **更新启动区走 Linux/WSL 或 Shell 内操作，更新工作内容直接在 Windows 资源管理器写 P3**。

---

## 9. 验证计划

**三层验证体系（并行开发适配，D17）**：
1. **宿主单测（各流内部，秒级）**：纯逻辑库与应用 core 在宿主（Linux/macOS gcc + `tests/host` 框架）直接编译测试；FsOps 走 POSIX 后端在临时目录真实读写。不依赖 QEMU/edk2 构建，并行流互不抢资源。
2. **每流 QEMU 冒烟（各流内部，分钟级）**：胶水/应用/引导/驱动流用 `scripts/mktestimage.sh` 生成**最小测试镜像**（FAT 变体：Shell+被测应用；NTFS 变体：+ntfs-3g+小型 P3），`scripts/qemu-run.sh` 启动并做 serial 日志断言。各流用自己的镜像，不等整镜像。并发 QEMU 预算 ≤4（各 2 GB），编排器信号量控制。
3. **集成矩阵（leader，本节 V-01–V-12）**：完整三分区镜像跑全矩阵，全绿才上实机（§9.2）。

### 9.1 集成矩阵：QEMU（全绿才上实机）[F11]

x64（OVMF 分体 pflash + **USB 挂盘**，测真实可移动介质 fallback 路径与 NVRAM）：

```bash
qemu-system-x86_64 -machine q35 -m 2G \
  -drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -drive id=stick,file=easyefi.img,format=raw,if=none \
  -device qemu-xhci -device usb-storage,drive=stick \
  -serial file:serial-x64.log
```

AA64：`qemu-system-aarch64 -machine virt -cpu cortex-a57 -m 2G -bios QEMU_EFI.fd`（edk2 ArmVirtPkg 产物），USB 挂盘同上。

| # | 用例 | 通过标准 |
|---|---|---|
| V-01 | 双架构引导路由（严格布局）+ 兼容布局 try-both 脚本 | x64 命中 P1、AA64 命中 P2；compat 下两架构均进 Launcher，`EASYEFI_RAN` 语义正确 |
| V-02 | 自研三应用 + ntfs-3g 的 AARCH64 构建与运行 | Launcher/FileManager/Editor/驱动 AA64 产物在 QEMU virt 全部可用（纯 EDK2 单一源码，无 LVGL 移植风险） |
| V-03 | 编辑器 UTF-8 往返 | 含中文注释的 .nsh：编辑 ASCII 部分→保存→非 ASCII 字节逐字节不变；BOM/CRLF 保留；非法 UTF-8 字节保留；256 KiB 上限拒绝并提示 |
| V-04 | 驱动链路 | load+map -r 后 P3 出现；读 >4GiB 文件；创建/改名/删除成功 |
| V-05 | 写入持久化 + FM 文件操作 | NTFS 上复制/改名/删除/mkdir 成功；写文件 → 重启 → 读回一致（防缓存假写） |
| V-06 | 降级注入 | 改名驱动文件模拟加载失败 → Launcher 仍启动，DEGRADED 横幅，apps 可用，data: 灰显 |
| V-07 | noautorun / ESC | P3 与启动分区两处标志均生效；Shell 倒计时 ESC 与 Launcher skip_timeout 均回 Shell |
| V-08 | 刷机工作流闭环（锚定用例 §7.4） | FM 找到固件（大小/hex 头预览）→ F4 改 fpt.nsh 替身脚本路径/参数 → 保存 → 执行（确认框显示全路径+大小）→ 嵌套 Shell 运行成功、无 startup.nsh 递归、脚本后停提示符、exit 回 Launcher |
| V-09 | auto: 排除自盘 | 候选不含本镜像任何分区；插入第二介质可发现并链式启动 |
| V-10 | boot: BootNext | QEMU varstore 中 BootNext 写入并被下次引导消费 |
| V-11 | JSON 容错 | 坏 JSON/缺文件 → 内置菜单+错误横幅；非法项跳过；arch 灰显；danger 强制确认 |
| V-12 | 文本模式 | 80×25 与最大可用模式均完整可用 |

### 9.2 实机矩阵（QEMU 全绿后）

- x64：至少 2 台不同固件厂（AMI/Insyde/Phoenix 系）桌面/笔记本；记录多 ESP 枚举行为（决定 D3 是否切换默认布局）。
- ARM64：**Ampere 类 ARM 服务器**（用户确认的目标硬件；标准 UEFI + ACPI）。注意：服务器固件常默认开启 SB 且 Setup 有管理员密码——提前确认能进固件关 SB，否则落入 §4 部署边界。QEMU virt 仍为前置验证。Windows on ARM 锁 PK 设备明确排除。
- 每台记录：SB 关闭步骤、引导命中分区、P3 读写、apps 可用性、fpt 写保护边界（Boot Guard/描述符）。
- 链式启动实测：Windows 安装盘 + Ubuntu LiveUSB（shim 在 SB 关闭下直通）。

---

## 10. 安全与运维

- 物理保管为第一控制（SB 关闭的代价，§4）。
- 镜像内附 `SHA256SUMS`（P1/P2 全部二进制）+ Shell 侧校验脚本（`sha256sum` 无原生 Shell 命令时用第三方工具或 Launcher 内置校验页）——**定位为防意外损坏/低水平篡改的运维手段，不是安全边界**（无 SB 时校验器本身可被替换）。
- NTFS 操作指引：ntfs-3g 最后句柄关闭即完整卸载 → 非操作中途拔盘安全；写操作中途勿断电；改脚本前用文件管理器另存副本；P3 脏卷（Windows 快速启动残留）先 chkdsk。
- 危险操作：danger 强制 Y/N；FM 执行任何 .efi/.nsh 前确认框显示**全路径 + 文件大小**；删除/覆盖强制确认；自研应用不提供任何磁盘/分区级编辑功能（范围冻结 D16，从根上消除 UC 式 Recovery/Hex 磁盘工具的风险面）。
- 更新流程：工作内容 Windows 直写 P3；启动区更新走构建流水线重刷或 Shell 内拷贝。

## 11. 并行开发执行模型（大量 subagent 并行，D17）

**任务书：`docs/parallel-tasks.md`** —— 19 个任务包（S00 地基 + S01–S17 并行流 + S18 集成），每包可直接作为 subagent 派单 prompt：独占目录 / 输入契约 / 交付物 / 接口签名 / 编号测试用例 / 冒烟断言 / DoD / 禁止事项。本节只述模型，细节以任务书为准。

### 11.1 三条架构缝（并行度的来源）

1. **契约先行**：Wave 0 冻结全部契约头（`Include/Contract/`）+ 每契约的 stub/mock。流间依赖只指向契约，不指向实现 → 17 流同时开工，无等待链。
2. **core/glue 分离 + Ops 双后端**（§7.1）：纯逻辑库与应用 core 宿主秒级单测（FsOps POSIX 后端真实读写临时目录），不抢 QEMU/edk2 构建；UEFI 胶水薄，每流最小镜像冒烟。
3. **独占所有权**：每流一个目录 + 一个 .inf + 一个 .dsc 碎片 + 一份交接注记；leader 独占聚合文件（§11.7）→ 零共享文件竞争。

### 11.2 流总览（Wave 1，17 流并行）

| 流 | 模块 | 类型 | 规模 | 宿主测试 | QEMU 冒烟 |
|---|---|---|---|---|---|
| S01 | Utf8Lib | 纯逻辑 | S | ✅ 往返恒等 fuzz | — |
| S02 | EditorModelLib | 纯逻辑 | M | ✅ 编辑/撤销/查找 | — |
| S03 | ConfigCoreLib + JSON 双适配器 | 纯逻辑+薄胶水 | M | ✅ schema 校验 | 适配器 |
| S04 | ExecPathLib | 纯逻辑 | S | ✅ 文法 | — |
| S05 | VolumeModelLib | 纯逻辑 | M | ✅ 合成设备路径 | — |
| S06 | TuiScreenLib | UEFI 胶水 | M | ✅ mock ConOut | 模式/渲染 |
| S07 | TuiInputLib | UEFI 胶水 | S | ✅ 映射表 | QMP 按键注入 |
| S08 | TuiWidgetLib | 控件 | M | ✅ mock 屏幕快照 | 控件操作 |
| S09 | ExecVolumeLib + FsOps 后端 | 胶水+POSIX | L | ✅ 真实文件操作 | NTFS 卷发现/读写 |
| S10 | ExecImageLib | UEFI 胶水 | M | ✅ mock 记录器 | exec/嵌套 Shell/auto: |
| S11 | Launcher（core+glue） | 应用 | L | ✅ 状态机 | 菜单/降级 |
| S12 | FileManager（core+glue） | 应用 | L | ✅ POSIX 临时树 | NTFS 操作链 |
| S13 | Editor（core+glue） | 应用 | L | ✅ UTF-8 往返 | 打开/保存/读回 |
| S14 | ntfs-3g 集成 | 第三方构建 | S | — | 双架构驱动+持久化 |
| S15 | startup.nsh 严格+兼容 | 引导脚本 | M | — | 双架构启动链 |
| S16 | mkimage/fix-gpt | 打包 | M | ✅ loop GPT 断言 | 整镜像引导 |
| S17 | Shell 构建固化 | 第三方构建 | S | — | 提示符/倒计时 |

### 11.3 契约冻结与变更控制

- 契约清单与签名规格：任务书 §1 + 各流接口节；数据契约 = launcher.json schema（§6）、.easyefi 标记格式、分区布局（§3.1）、设备路径匹配规则（§5.2）。
- Wave 0 评审后冻结；变更流程 = 提案（交接注记）→ leader 批准 → 版本 bump → 通知全部消费流（任务书 §4）。接口窄而深，模块内能定的不外露。

### 11.4 构建/测试隔离

- 每流一个 git worktree + 分支 `stream/Sxx-<name>`；WORKSPACE=worktree，独立 Conf/Build；edk2 共享只读（PACKAGES_PATH）+ BaseTools 预编译（EDK_TOOLS_PATH）。配方与实证见任务书 S00。
- 三层验证（§9）：宿主单测 → 每流最小镜像冒烟（mktestimage fat/ntfs 变体 + qemu-run serial 断言 + QMP 按键）→ 集成矩阵。
- **Wave 0 硬出口**：两个 dummy 模块双 worktree 并行构建成功 + 宿主框架跑通 + 最小镜像引导到 Shell + 全部 stub 可编译。配方未实证不得批量派发。

### 11.5 波次与关键路径

- Wave 0（串行）：S00 平台与契约。
- Wave 1（并行 ≤17）：S01–S17 同时开工；唯一同步点 = 契约版本与 leader 变更通知。
- Wave 2（leader + 2–3 agent）：S18 按依赖序合并（纯逻辑→胶水→应用→boot/打包）→ V-01–V-12 全矩阵 → V-08 刷机闭环 → 发布候选。
- 关键路径：S00 → S09/S10 → S11/S12/S13 → S18；纯逻辑流（S01–S05）与构建流（S14/S16/S17）不在关键路径，可最先完成反哺 stub 替换。

### 11.6 每流 DoD 与验收

① 模块目录完整（源码/.inf/碎片）② 宿主编号用例全绿 ③ QEMU 冒烟断言全绿（附脚本+日志）④ 契约零变更或已批准 ⑤ 交接注记（固定模板）⑥ 本分支逻辑提交。leader 凭客观证据验收，代码抽查聚焦契约边界与危险操作（文件写/exec）。

### 11.7 leader 独占文件（agent 不得修改）

`EasyEfiPkg.dsc/.dec`（!include 聚合）、`Include/Contract/**`、`scripts/build-module.sh`、`scripts/mktestimage.sh`、`scripts/qemu-run.sh`、`tests/host/` 框架、`docs/design.md`、`docs/parallel-tasks.md`、edk2 submodule 指针、`config/launcher.example.json`。scripts/ 其余文件按流独占（S16：mkimage/fix-gpt/verify-image；S17：build-shell）；版本锁定记录分流独占 `docs/pins/Sxx-*.md`。

### 11.8 并行风险与缓解

| 风险 | 缓解 |
|---|---|
| 契约质量单点故障 | Wave 0 leader 评审；stub 可用性实证（消费流编译跑通）；接口窄而深 |
| EDK2 多工作区坑（Conf/BaseTools/PACKAGES_PATH） | Wave 0 硬出口实证配方；配方文档化 |
| 胶水层固件怪癖集成才暴露 | 每流 QEMU 冒烟是 DoD 硬条件；集成矩阵合并后+发布前各跑一遍 |
| leader 评审带宽 | 客观测试+交接证据验收；抽查契约边界/危险操作 |
| 范围蔓延 | D16 MVP 冻结适用所有流；超范围想法进交接注记由 leader 裁决 |
| 资源竞争 | 并发 QEMU ≤4（2GB/实例，信号量）；宿主测试不限；worktree ~2GB/流 |

### 11.9 与原 P 阶段映射

| 原阶段 | 并行模型 |
|---|---|
| P0 决策 | ✅ 完成（Q1–Q4 + 并行要求） |
| P1 骨架+引导链 | Wave 0 + S06/S07/S10/S11/S15/S17 |
| P2 数据区 | S05/S09/S14（+S11 降级） |
| P3 自研应用 | S01–S04/S08/S12/S13 |
| P4 打包+矩阵 | S16 + S18 |
| P5 实机/发布 | S18 尾段 |

仓库结构：`edk2/`(submodule，leader)、`EasyEfiPkg/`(Contract + 10 库 + 3 应用，按流独占)、`third_party/`(ntfs-3g，S14)、`boot/`(S15)、`scripts/`(文件级所有权)、`config/`、`docs/`(design/parallel-tasks/handoffs//pins/)、`tests/host/`。

## 12. 决策记录（用户已拍板，2026-11-22）

- **Q1 UI 语言 → ASCII 英文菜单**：ConOut 文本 TUI，零字体依赖；中文界面留作后续 GOP+点阵字体扩展（D7）。
- **Q2 UefiCommander → 内部自用先收录**（⚠️ 已被 Q4 推翻，仅作历史记录）。
- **Q3 ARM64 目标硬件 → Ampere 类 ARM 服务器**：实机验证按 §9.2 执行；注意服务器固件 SB 默认开启与 Setup 权限问题。
- **Q4 第三方去留 → 完全自研，第三方全退**（推翻 Q2）：自研 FileManager/Editor 为唯一文件管理/编辑方案，单一源码双架构统一能力；UefiCommander/ufm/guedit 全部退役（D8/D16，规格 §7.1–7.4）。
- **场景约束（用户补充，2026-11-22）**：FM/编辑器服务于刷机工作流——找固件/EFI 位置、必要时改执行脚本；不做复杂应用、不处理大文本（编辑器上限 256 KiB、预览 ≤16 KiB）、全键盘操作。MVP 范围据此冻结，锚定用例 §7.4。
- **并行开发要求（用户补充，2026-11-22）**：方案须支持大量 subagent 并行开发，且每个 subagent 任务尽可能细化 → D17/§11 执行模型 + `docs/parallel-tasks.md` 任务书（S00–S18，含接口签名/编号测试用例/DoD）。
- 备注（非问题）：镜像默认 8GiB 可调；默认严格双 ESP 布局，实机矩阵后再评估是否切换兼容布局为默认（D3）。

**方案到此定稿（rev3）。下一步是 §11 的 Wave 0（S00 平台与契约），验收后批量派发 Wave 1 的 17 个流；等用户指令再动代码。**
