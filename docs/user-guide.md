# EasyEFI 用户指南（草稿 v0.1，S18 集成后定稿）

EasyEFI 是一个多架构（x86_64 / ARM64）UEFI Shell 工作镜像，用于系统维护与刷机。
单一 U 盘镜像，内置启动菜单、文件管理器、文本编辑器与 NTFS 数据区。

## 1. 制作启动盘

### 写入镜像
- **Linux/macOS**：`sudo dd if=easyefi.img of=/dev/sdX bs=1M status=progress`（整盘写入，**会清空 U 盘**；确认设备路径）。
- **Windows**：Rufus（选 DD 模式）或 balenaEtcher 写入 .img。
- U 盘容量 ≥ 镜像尺寸（默认 8 GiB）。

### 写盘后修复（U 盘大于镜像时必需）
镜像的 GPT 备份头在镜像末尾；dd 到更大 U 盘后位置错位，固件/系统可能报 GPT 异常：
```bash
sudo bash scripts/fix-gpt.sh /dev/sdX          # 迁移备份头到磁盘末尾
sudo bash scripts/fix-gpt.sh /dev/sdX --grow   # 另：扩容数据区 P3 占满剩余空间
```
Windows 下可用 DiskGenius 修复 GPT 备份头并扩容 NTFS 分区；不修复通常仍可引导，但建议修复。

## 2. 目标机准备（重要）

### 关闭 Secure Boot
本镜像**未签名**，需在目标机固件设置中关闭 Secure Boot（SB）才能引导。
- 常见入口：开机按 Del/F2/F10/F12 → Security/Boot 选项卡 → Secure Boot → Disabled。
- **边界**：SB 被 BIOS 密码/企业策略锁定且无法进入设置的机器，本镜像不可用。
- **BitLocker 警告**：关闭 SB 会改变 TPM PCR7。目标机 Windows 若启用 BitLocker，
  下次启动可能要求恢复密钥——**操作前确认恢复密钥在手或已暂停 BitLocker 保护**。

### 引导
- 插入 U 盘，开机进固件启动菜单（F8/F11/F12/Esc 因厂商而异），选 U 盘的 UEFI 项。
- 镜像自动按架构分流：x86_64 机器从分区 1 引导，ARM64 从分区 2（个别固件只扫第一个 ESP 时，
  手动选对应分区；兼容布局镜像两个架构都在分区 1）。
- 纯 Legacy BIOS（无 UEFI）机器不支持。

## 3. 使用

### 启动菜单（Launcher）
- 开机自动进入菜单；倒计时内按 **ESC** 直接进 UEFI Shell。
- ↑↓/数字选择，**Enter** 执行；危险项（红色 !）需 Y/N 二次确认。
- **S/ESC** 返回 Shell；**R** 重扫介质+重载配置；**?** 帮助。
- 数据区不可用时显示 DEGRADED 横幅：文件管理器/编辑器/Shell/重启仍可用。

### 文件管理器 / 编辑器
- 菜单内置入口；全键盘操作（无鼠标）。
- 文件管理器：卷列表→目录导航→键入过滤→S 递归搜索→预览（文本头/二进制 hex 头）→
  Enter 执行（确认框显示全路径+大小）→F4 编辑→F5 复制/F6 改名/F7 新建/F8 删除。
- 编辑器：≤256 KiB 脚本/配置；UTF-8 安全（中文注释显示为占位符但**保存不损坏**）；
  F2 保存/F3 查找/F5 另存为/F6 跳行/F7 撤销/F10 退出（修改时 S 保存退出/D 放弃/ESC 取消）。

### 数据区（P3，NTFS）
- Windows 下自动挂载盘符，可直接拷入固件/ISO/脚本（支持 >4 GiB 单文件）。
- `launcher.json` 定义菜单工作项（格式见 config/launcher.example.json）。
- `scripts/*.nsh` 可现场编辑适配刷机需求。
- 放一个空文件 `noautorun` 到 P3 根 → 开机不进菜单直接留 Shell。
- 启动分区根放 `noautorun` 同效（专家用法，Windows 下 ESP 无盘符需 diskpart assign）。

### 刷机工具（fpt/afu 等，用户自备）
- 厂商工具为专有软件，**不随镜像分发**。自行获取后放入启动分区 `\EFI\Tools\`：
  - Intel FPT：CSME System Tools 包内 FPT.efi（x64）。
  - AMI AFU：AfuEfi64.efi（x64）。
- 在 P3 `scripts/` 写 .nsh 调用（菜单项 path 指向脚本），例：
  ```nsh
  echo -off
  fs1:\EFI\Tools\fpt.efi -f fs2:\payloads\bios.bin
  ```
  （卷号以运行时 map 为准；脚本内可用 %homefilesystem% 等环境变量。）
- **注意**：Boot Guard / ME Flash 描述符写保护与 SB 无关——fpt 报写保护时检查闪存描述符，
  不是签名问题。刷写 SPI 闪存有变砖风险，确认固件型号匹配。

## 4. 故障排查
| 现象 | 处置 |
|---|---|
| 固件不认 U 盘 | 确认 UEFI 模式（关 CSM 或纯 UEFI）；换 USB 口；重做写盘+fix-gpt |
| 进菜单但 DEGRADED | NTFS 驱动加载失败或 P3 损坏；Shell 下 `map -r` 看卷；Windows chkdsk P3 |
| 菜单项灰显 | 架构不匹配（如 x64 工具在 ARM64 机器） |
| 执行 .efi 报错 | 确认文件架构与目标机一致；损坏文件重新拷入 |
| Windows 弹 BitLocker 恢复 | 输入恢复密钥；事后恢复 SB 设置并重新绑定 BitLocker |

## 5. 安全说明
- SB 关闭后镜像无启动链完整性校验：**U 盘物理保管是第一控制**（跨机器流动的工作盘
  被掉包/植入意味着每台目标机执行攻击者代码）。
- 镜像附 SHA256SUMS 供完整性核对（运维手段，非安全边界）。
- 源码与构建：本仓库 GPLv3；ntfs-3g 驱动 GPLv2（源码获取见 third_party/ntfs-3g/）。
