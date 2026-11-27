# EasyEFI 设计批判性评审（WorkBuddy v1 → v2）

- 日期：2026-11-22
- 评审对象：WorkBuddy 会话产物 `uefi-toolkit-design.md`（22,809 字节终版）+ `launcher.example.json`
  - 来源：https://workbuddy.link/p/mzvLLNTFkuGKonF4om5W5E （2026-11-20 会话，含 4 轮需求演进）
  - 终版由会话内 Write/Edit 记录按时间序重放重建，字节数与 artifactMap 记录完全一致
- 评审方法：关键断言逐条对照一手来源核实（EDK2 master 源码、上游仓库 README/BUILD/NOTICES、GitHub API），不采信二手博客
- 结论：**方向成立，按 v1 直接开工必然返工**。6 个 HIGH 问题（其中 2 个是工具选型/法律层面），全部已在 v2 方案（`docs/design.md`）中给出修正。

## 0. v1 做对了什么（保留项）

| 决策 | 评审核实结果 |
|---|---|
| 不魔改 Shell，原版 Shell.efi 改名做 fallback | ✅ 成立，满足"Shell 功能零回归" |
| TUI 用独立 EDK2 应用而非 .nsh 菜单 | ✅ 成立，.nsh 无可靠按键/JSON 能力 |
| 双 ESP 按架构分流 | ✅ 在 EDK2 系固件上成立（见 F24 源码证据）；真实固件有差异，需兼容布局兜底 |
| 驱动放 P1/P2 而非 P3 | ✅ 成立，消除先有鸡先有蛋 |
| P3 = NTFS + ntfs-3g 读写 | ✅ 成立：>4GiB 载荷 + 现场写入两个约束下，这是唯一现成读写驱动（pbatard/ntfs-3g README 明确 read/write、全架构可编译） |
| SB 默认关闭（用户拍板） | ✅ 决策保留；但论证有漏洞，见 F13 |
| 降级/卷号漂移/固件差异的风险意识 | ✅ 方向对，但具体机制写错或没落地，见 F02/F09/F14 |

## 1. 需求追溯

| 原始需求 | v1 状态 | v2 状态 |
|---|---|---|
| R1 标准 Shell + 自带签名文件支持 SB 启动 | 会话中用户改为"SB 不强制、接受关闭 SB"，签名链整体删除 | 保留用户决策；§9 可选自签 SB 层级具体化；修正"SB 无用论"论证（F13） |
| R2 单一镜像支持 x86_64 + ARM64（可要求用户选分区 1/2） | 双 ESP 自动分流 | 保留，另提供"双 fallback 同放 P1"兼容布局为默认（F24） |
| R3 TUI + JSON 定义启动项 + 三分区 + 启动项文件在数据区 | 基本满足；apps/驱动/工具放 P1/P2 属未声明的偏离 | 满足；偏离显式声明并给理由（降级可用性，F09）；JSON schema 规范化（F15） |
| R4 Shell 原生功能不受影响（fpt/afu、链式启动其它镜像） | 架构满足，执行机制写错 | 满足；.nsh 执行机制修正（F01/F02），auto: 排除自盘（F03） |

## 2. 问题清单（按严重度）

### HIGH — 不修则方案不可用

**F01 `.nsh` 执行机制不存在：`Shell.efi -s <script>`**
- v1 §5："菜单里通过 action=exec + path=data:scripts\xxx.nsh 调用时，实际执行 `Shell.efi -s <script>`"。
- 核实：EDK2 `ShellPkg/Application/Shell/Shell.c` `ProcessCommandLine()` 的选项表为 `-startup/-nostartup/-noconsoleout/-noconsolein/-nointerrupt/-nomap/-noversion/-nonest/-delay/-exit`，**没有 `-s`**；未知选项返回 `EFI_INVALID_PARAMETER`。正确语法：`shell.efi [options] <file-name> [file-name-options]`，且**指定 file-name 时隐式 `-nostartup`**（源码注释原文："By default, if file-name is specified, then -nostartup is implied"）。
- 影响：fpt/afu 两个核心菜单项按 v1 写法直接失败。
- v2：`LoadImage(Shell.efi) + LoadOptions = L"<fsN:\scripts\xxx.nsh> [args]"`；隐式 -nostartup 恰好避免嵌套 Shell 重跑 startup.nsh 造成递归。

**F02 .nsh 错误语义：v1 的"脚本检测加载失败"没有落地，且早期批处理脚本建议是错的**
- v1 §2/§5.1 声称 startup.nsh "检测 %lasterror% 或 map 结果，失败就不拉起 Launcher"，但给出的脚本没有任何检测代码。
- 核实（Shell.c `RunCommand()` + `RunScriptFile()` 循环）：
  - 脚本中**普通命令失败不会中止脚本**——"change return value to success so the script won't halt (unless aborted)"，错误写入 `%lasterror%`（格式 `0x%Lx`，成功为 `0x0`）；
  - **脚本专用命令**（if/for/goto 等）失败才中止（`IsScriptOnlyCommand` → `EFI_ABORTED`）；
  - `If.c` 支持 `if ... then / else / endif` 块、`and/or`、`exist(s)`、`efierror(N)`、字符串/数值比较。
- 影响：v1 会话中期建议的 `for %d in *.efi / load %d` 批量加载在混架构目录下行为不可控；降级逻辑必须用 if 块显式写。
- v2：给出逐行核实过的 startup.nsh（见 design.md §3），用 `%lasterror%` + if/else 实现架构探测与降级。

**F03 `auto:` 会匹配到自己的启动盘 → 自链式循环**
- v1 §4.4：`auto:EFI/BOOT/BOOTX64.EFI` 枚举**全部** SimpleFileSystem。本镜像 P1 的 `\EFI\BOOT\BOOTX64.EFI` 就是 Shell 自己 → 候选列表包含自身 → 选中后 Shell→startup.nsh→Launcher→… 无限循环。
- v2：`auto:` 候选**排除启动盘（同磁盘签名）上的所有分区**；多候选时列出让用户选。

**F04 中文菜单名在固件文本控制台渲染不出来**
- v1 §4.3.1 规定"纯 ConOut 文本 TUI"，但示例 JSON 的 name/desc 全是中文（"Windows 11 安装盘"等），mockup 也按中文画。
- 事实：`EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL` 走固件 HII 字体，PC 固件文本模式字体基本只覆盖 ASCII/Latin-1，**无 CJK 字形**；中文会显示为乱码/方框。v1 的 UI 规范与自己的配置样例互相矛盾。
- v2：schema 强制 `name/desc` 为 ASCII（校验+告警）；中文界面列为可选扩展（GOP + 内嵌 CJK 点阵字体，参考 guedit 的 LVGL 路线），不进首版。**这是需要用户拍板的取舍，见 design.md 决策 D7。**

**F05 UEFI Commander 选型三个硬伤：许可证不明 / 仅 X64 验证 / 俄英双语非中文**
- 核实（GitHub API + 仓库文件，2026-11-22）：
  - `TheBestMKS/UefiCommander` 真实存在、活跃（v1.2.0，2026-09 推送），但**仓库根目录无 LICENSE**，`THIRD_PARTY_NOTICES.md` 只覆盖内嵌组件（ntfs-3g GPL、EfiFs GPLv3、miniz MIT、Clover BSD），**应用自身代码许可未声明** → 默认版权保留，对外分发镜像存在法律风险；
  - `BUILD.md`（俄文）原文："Встроенные драйверы должны иметь ту же архитектуру, что и приложение. Проверенный комплект ... предназначены для X64" —— 内嵌驱动必须与_app_同架构，**验证过的只有 X64**；release 资产仅 `UefiCommander_v.1.2.0.efi`（X64）+ Legacy BIOS 镜像。v1 "两个应用编译为 x64/ARM64" 对 UefiCommander 不成立（AA64 需重新生成驱动 blob，未验证）；
  - README 首句："bilingual **Russian/English**" —— v1 宣称的 CP866/Windows-1251/KOI8-R 是西里尔编码，**不是中文能力**。
- v2：核心工具栈换成 **ufm（BSD-2-Clause Plus Patent，纯 EDK2 源码双架构可编，内置 edit/hexedit，走已加载的 ntfs-3g）+ guedit（MIT，UTF-8，LVGL/GOP）**；UefiCommander 降级为 x64 扩展。**最终决策 Q4（2026-11-22）：完全自研基础版 FileManager/Editor（刷机导向 MVP，单一源码双架构，ConOut 纯键盘），UC/ufm/guedit 全部退役**——第三方双架构能力不统一的问题从根上消除（design.md D8/D16、§7.1–7.4）。

**F06 镜像尺寸自相矛盾：4GiB 镜像装不下 >4GiB 载荷**
- v1 §6 `truncate -s 4G work-uefi.img`，而 §1/§5.1 的核心卖点是 P3 承载 >4GiB 的 install.wim/ISO。4G 镜像扣除 2×512MiB ESP 后 P3 仅 ~3GiB。
- v2：镜像尺寸参数化（默认 8GiB，约束：≥ 载荷总和 + 1.5GiB），打包脚本校验。

### MED — 不修则关键场景翻车

**F07 P3 分区类型 GUID 命令与表格矛盾**
- v1 表格写 `EBD0A0A2-…`（Microsoft Basic Data），命令却是 `sgdisk -t 3:8300`（Linux filesystem GUID `0FC63DAF-…`）。后果：Windows 不给该分区自动分配盘符 → "在 Windows 里往数据区拷载荷"的主工作流断掉。
- v2：`-t 3:0700`。

**F08 dd 到更大 U 盘后 GPT 备份头位置错误**
- 4G/8G 镜像的备份 GPT 在镜像末尾；dd 到 32G U 盘后备份头不在磁盘末尾，固件/OS 可能报 GPT 损坏甚至拒绝启动。v1 无修复步骤。
- v2：写盘后 `sgdisk -e /dev/sdX`（迁移备份头到磁盘末尾）+ 可选扩容 P3（`ntfsresize`），提供 `fix-gpt` 脚本；Windows 侧给出 Rufus DD 模式 / balenaEtcher 路径。

**F09 降级设计反了：驱动失败 → 裸 Shell，恰好惩罚了最需要帮助的用户**
- v1：NTFS 驱动加载失败就不拉起 Launcher；且文件管理器/编辑器放 P3。组合后果：驱动一挂，不熟悉 Shell 的用户既没有菜单也没有文件管理器。
- v2：**apps 移到 P1/P2（FAT32，永远可达）**；Launcher 无条件拉起，自带编译期内置默认菜单；P3 不可达时显示 DEGRADED 横幅 + 仍可用的条目（Shell/文件管理器/编辑器/重启/NVRAM 启动/auto: 外部介质）。

**F10 `boot:0001` 硬编码 NVRAM 启动项号**
- Boot#### 编号逐机不同，0001 可能是任何设备甚至不存在。示例把它注释为"启动已安装的 Windows"会误导。
- v2：保留 `boot:XXXX` 显式语法，但默认提供 `boot:menu` 动态子菜单（读 BootOrder/Boot#### 描述实时枚举）；示例不再硬编码语义。

**F11 QEMU 验证方案测不到真实启动路径**
- v1 §7 用 `-drive file=...,format=raw`（固定磁盘）+ `-bios OVMF.fd`（无独立 varstore）。两个问题：
  1. 固件对**可移动介质**才走 `\EFI\BOOT\BOOT{ARCH}.EFI` fallback 语义；固定磁盘路径不代表 U 盘行为；
  2. 无 pflash varstore 时 NVRAM 变量（BootNext/Boot####）测试不可靠。
- v2：`-device qemu-xhci -device usb-storage,drive=stick` + OVMF 分体 pflash（CODE 只读 + VARS 可写）；ARM64 用 `-M virt -cpu cortex-a57 -bios QEMU_EFI.fd` 同样挂 USB。

**F12 许可证事实错误：ntfs-3g 是 GPLv2，不是 GPLv3**
- pbatard/ntfs-3g README badge 与仓库许可为 **GPLv2**（EfiFs 才是 GPLv3）。v1 多处写错。GPLv2 与本项目 GPLv3 仓库兼容（聚合分发），但分发镜像须履行对应源码提供义务。
- v2：更正；第三方一律"构建时按锁定 commit 拉取/编译"，仓库不入库二进制；提供源码获取指引。

**F13 "SB 在这里保护不了什么"论证过度 + 两个被忽略的现实约束**
- v1 的论证混淆了**授权**（用户对机器有完全控制权）与**完整性**（启动链未被篡改）。本镜像是**跨机器流动的工作盘**：U 盘被掉包/植入后，每台目标机都会在固件上下文执行攻击者代码（可进一步感染目标机 ESP）。SB + 自签 db 恰恰能防这一类攻击。关闭 SB 是用户已拍板的取舍，可以保留，但论证要诚实。
- 被忽略的约束：
  1. 单位/客户机器可能 **SB 锁定（BIOS 密码/策略）**，"去固件里关 SB"根本做不到 → 镜像在这些机器上不可用，这是部署边界，要写进文档；
  2. **BitLocker**：关闭 SB 会改变 PCR7，目标机 Windows 可能弹恢复界面 → 操作前确认有恢复密钥。
- v2：修正威胁模型表述；保留可选自签 SB 扩展路径并具体化；风险表加入 BitLocker 与 SB 锁定边界；恢复 v1 会话早期提过但终版丢失的 **Boot Guard / ME Flash 描述符写保护 ≠ SB** 提示（fpt 被拦别误判为签名问题）。

### LOW / INFO — 规范与实现细节

**F14 startup.nsh 查找顺序与 `%homefilesystem%`（核实，替代 v1 的猜测）**
- Shell.c `LocateStartupScript()`：先找 **Shell 二进制所在目录**（fallback 启动即 `\EFI\BOOT\startup.nsh`），再按 `PATH` 找（根目录副本是兜底）；同时设置环境变量 **`homefilesystem` = 启动分区映射名（如 `fs0:`）**。v1 "不同固件查找行为有差异"归因错误——Shell 是自己编译的，行为由锁定的 EDK2 版本决定。
- v2：规范脚本放 `\EFI\BOOT\startup.nsh`，路径用 `%homefilesystem%\...`，不再依赖"当前目录通常是启动分区根"的猜测。

**F15 JSON schema 缺口（v1 样例与文档互相矛盾）**
- `autostart`/`skip_key` 语义未定义；arch 枚举混用 `arm64`（文档/UI）与 `aarch64`（JSON）；路径三种写法并存（`data:entries/winpe.efi`、`data:/apps/UefiCommander.efi`、`data:scripts\xxx.nsh`）；`confirm` 与 `danger` 语义重叠；`options` 如何传给 LoadImage（UCS-2 编码）未定义；JsonLib **不支持浮点数**（JsonLib.h 原文："Real number and number with exponent part are not supported"）未约束。
- v2：规范化 schema（design.md §5），含校验规则与错误处理。

**F16 "80×25 到 1280×720 各种文本分辨率"混淆像素与文本模式**
- 1280×720 是 GOP 像素分辨率；文本模式是 80×25、100×31 等。v2：`QueryMode` 枚举文本模式取最大可用，保底 80×25。

**F17 guedit 是 GOP 图形应用，v1 "整个 UI 纯 ConOut 不依赖 GOP"以偏概全**
- guedit README：LVGL 9.2.2 + GOP 显示；"自带鼠标"属实——固件无指针设备时启用**内嵌 USB HID Boot Protocol 鼠标驱动**（只认 boot 协议鼠标）。MIT 许可，无预编译 release，需源码构建；AARCH64 构建未验证（列入验证项）。
- v2：UI 栈分层表述——Launcher/ufm 走 ConOut 文本；guedit 走 GOP。GOP 不可用时编辑器降级为 ufm 内置 edit。

**F18 打包脚本小 bug**
- `losetup -fP` 后硬编码 `/dev/loop0`（竞态）→ `--show` 取实际设备；`sgdisk -g` 在 `-Z` 后冗余；缺 Windows 写盘工具指引（Rufus DD / Etcher）。

**F19 `noautorun` 标志位置含糊且 ESP 在 Windows 下不可写**
- Windows 资源管理器不给 ESP 盘符 → 普通用户无法在 P1 创建 `\noautorun`。v2：主标志位 = `P3:\noautorun`（Windows 可写），由 Launcher 检测（读到即退回 Shell）；启动分区根标志保留给专家，startup.nsh 用 `%homefilesystem%` 检测。

**F20 嵌套 Shell 的卷号与退出语义**
- 嵌套 Shell 重新 map，fsN 可能与父 Shell 不同 → .nsh 内不得硬编码 fsN，由 Launcher 把脚本路径解析为运行时全限定路径传入；脚本结束后嵌套 Shell **停在提示符**（不加 `-exit`），便于查看刷机输出，`exit` 返回 Launcher。

**F21 厂商刷机工具不可入库分发**
- fpt.efi（Intel CSME System Tools）、AfuEfi64.efi（AMI）均为专有工具，**不得提交进公开 GPLv3 仓库或随镜像对外分发**。v2：用户自备 drop-in（P1/P2 `\EFI\Tools\` + P3 脚本），文档写明获取渠道与 Boot Guard/写保护边界。内部使用与对外分发两种模式分开声明。

**F22 JsonLib 可用性核实（v1 方向对，.dsc 片段不完整）**
- `RedfishPkg/Library/JsonLib/JsonLib.inf`：`LIBRARY_CLASS = JsonLib|DXE_DRIVER UEFI_APPLICATION UEFI_DRIVER` ✅ 应用可用；`JsonLoadBuffer(Buffer, Len, Flags, &Error)` 存在 ✅。但依赖 `Ucs2Utf8Lib`、`RedfishCrtLib` 等，v1 的 .dsc 片段只写了 JsonLib 一行，照抄会构建失败。v2 给出完整库解析清单。

**F23 ntfs-3g 的自动卸载安全特性（v1 未利用）**
- README：最后一个文件句柄关闭即完整卸载卷 → 非操作中途拔盘/断电是安全的。v2 写入操作指引，并新增"写入→重启→读回"持久化验证项（防驱动缓存假写）。

**F25 EDK2 `load` 命令吞掉加载失败状态（上游陷阱，v1 的检测思路因此不可行）**
- 核实（`ShellPkg/Library/UefiShellLevel2CommandsLib/Load.c` `MainCmdLoad()`）：`ShellStatus` 初始化为 `SHELL_SUCCESS`，`LoadDriver()` 的失败状态只写入局部 `Status`，**从不传播到返回值**；仅"文件不存在"分支置 `SHELL_NOT_FOUND`。
- 后果：驱动文件存在但 `LoadImage`/`StartImage` 失败（如架构不符、镜像损坏）时，`%lasterror%` 仍为 `0x0`。v1 "startup.nsh 检测 %lasterror% 判断驱动加载失败"的自然写法**测不到失败**。
- v2：脚本不做驱动状态检测；**降级判定移入 Launcher**（运行时探测 P3 是否可见，本来就必须做）。兼容布局的架构探测不依赖 load 状态，改用 Launcher 设置的环境标记（design.md §3.3）。

**F24 双 ESP 枚举的源码证据与兼容布局**
- EDK2 `MdeModulePkg/Library/UefiBootManagerLib/BmBoot.c`：`BmEnumerateBootOptions()` 按**整盘**（先可移动后固定）生成启动项；`EfiBootManagerBoot()` 展开时对每个 SimpleFileSystem 分区尝试 `EFI_REMOVABLE_MEDIA_FILE_NAME`（`\EFI\BOOT\BOOT{ARCH}.EFI`），命中第一个可加载者。→ 双 ESP 分流在 EDK2/OVMF 成立。
- 真实固件（AMI/Insyde/Phoenix）存在"只扫第一个 ESP"的差异 → v2 默认采用**兼容布局**：P1 同放 BOOTX64 + BOOTAA64 与双架构驱动/Launcher/apps，startup.nsh 用 try-both + `%lasterror%` 探测架构（F02 已核实失败 load 不中止脚本）；P2 保留 AA64 镜像供手动选择。严格双 ESP 布局作为变体保留。

## 3. 核实日志（一手来源）

| 断言 | 来源 | 结果 |
|---|---|---|
| Shell 命令行选项/脚本隐式 -nostartup | edk2 master `ShellPkg/Application/Shell/Shell.c` ProcessCommandLine | ✅ 无 -s；file-name 隐式 -nostartup |
| startup.nsh 查找顺序 / homefilesystem / ESC 倒计时 | 同上 LocateStartupScript / DoStartupScript | ✅ 先 Shell 所在目录再 PATH；ESC 跳过；delay 可配 |
| 脚本普通命令失败不中止 / lasterror 格式 | 同上 RunCommand / RunScriptFile | ✅ 仅脚本专用命令中止 |
| if/else/endif、exist、efierror(N) | `ShellPkg/Library/UefiShellLevel1CommandsLib/If.c` | ✅ 块语法存在 |
| load 自动 connect（无需 -nc 之外操作） | `ShellPkg/Library/UefiShellLevel2CommandsLib/Load.c` L47 | ✅ ConnectController(TRUE) |
| 整盘枚举 + 逐 FAT 分区 fallback | `MdeModulePkg/Library/UefiBootManagerLib/BmBoot.c` | ✅ BmEnumerateBootOptions / EfiBootManagerBoot |
| JsonLib 应用可用 + API + 无浮点 | `RedfishPkg/Library/JsonLib/JsonLib.inf`、`RedfishPkg/Include/Library/JsonLib.h` | ✅ 需补 Ucs2Utf8Lib/RedfishCrtLib 解析 |
| ntfs-3g 读写/全架构/load+map -r/自动卸载/GPLv2 | github.com/pbatard/ntfs-3g README（默认分支 rebase-on-2026.9.28） | ✅ 全部属实；许可为 GPLv2 |
| UefiCommander 存在/仅 X64 验证/俄英双语/自身许可未声明 | GitHub API + README/BUILD.md/THIRD_PARTY_NOTICES.md/releases | ✅ 见 F05 |
| guedit MIT/LVGL-GOP/内嵌 HID 鼠标/无 release | github.com/MikeWuPing/guedit README | ✅ 见 F17 |
| ufm BSD-2+Patent/双栏/内置 edit+hexedit/走 SimpleFileSystem | github.com/Joursoir/ufm README | ✅ 可作核心文件管理器 |
| 四个仓库均真实存在 | GitHub API HTTP 200 ×4 | ✅（排除幻觉引用） |

未核实、留作实现期验证项：guedit/ufm 的 AARCH64 构建、try-both startup.nsh 在 QEMU 双架构的实际行为、真实固件多 ESP 差异、UefiCommander 与外置 ntfs-3g 双驱动共存、guedit 界面 CJK 字体能力。见 design.md §8。
