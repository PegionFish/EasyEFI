## S10 冒烟独立平台（TEST-ONLY，我目录内；不触碰 leader 独占的 EasyEfiPkg.dsc）。
## VolumeModelLib/ExecVolumeLib 解析到契约替身 fixture（worktree 内 S05/S09 是 stub）；
## 集成后若要用真实 S05/S09 跑冒烟，把两行 fixture 解析换成真实 .inf 即可（fixture 仅定义
## S10 引用的符号，真实库是其超集，换解析后无符号冲突）。
## EfiCompatLib 用真实库（merge 已含 main 84c6f4c 的 EfiMem 修复）。
[Defines]
  PLATFORM_NAME           = S10Smoke
  PLATFORM_GUID           = 644B78E9-D4E7-463C-8DB2-8CD04F8B0C9C
  PLATFORM_VERSION        = 0.1
  DSC_SPECIFICATION       = 0x0001001b
  OUTPUT_DIRECTORY        = Build/S10Smoke
  SUPPORTED_ARCHITECTURES = X64|AARCH64
  BUILD_TARGETS           = RELEASE|DEBUG
  SKUID_IDENTIFIER        = DEFAULT

!include MdePkg/MdeLibs.dsc.inc

[LibraryClasses]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  DebugLib|MdePkg/Library/UefiDebugLibConOut/UefiDebugLibConOut.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  EfiCompatLib|EasyEfiPkg/Library/EfiCompatLib/EfiCompatLib.inf
  ExecImageLib|EasyEfiPkg/Library/ExecImageLib/ExecImageLib.inf
  # TEST-ONLY 契约替身（S05/S09 worktree stub 未定义契约符号；集成后换真实 .inf）
  VolumeModelLib|EasyEfiPkg/Library/ExecImageLib/fixture/SmokeVolumeModelLib.inf
  ExecVolumeLib|EasyEfiPkg/Library/ExecImageLib/fixture/SmokeExecVolumeLib.inf

[BuildOptions]
  GCC:*_*_*_CC_FLAGS = -DUEFI_BUILD

[Components]
  EasyEfiPkg/Library/ExecImageLib/ExecImageLib.inf
  EasyEfiPkg/Library/ExecImageLib/smoke/SmokeApp.inf
  EasyEfiPkg/Library/ExecImageLib/smoke/SmokeTarget.inf