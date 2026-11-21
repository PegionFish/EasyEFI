## @file
#  S14 冒烟探针平台 DSC（独立于 EasyEfiPkg 聚合器，S14 独占）。
#  经 scripts/build-module.sh 的 DSC 环境变量构建：
#    DSC=third_party/ntfs-3g/testapp/testapp.dsc \
#      bash scripts/build-module.sh third_party/ntfs-3g/testapp/NtfsProbe.inf X64
#  WORKSPACE=worktree 根（build-module.sh 设定），组件路径相对其解析。
#  SPDX-License-Identifier: GPL-3.0-or-later
##

[Defines]
  PLATFORM_NAME           = NtfsProbe
  PLATFORM_GUID           = 573C2223-3554-4AC1-B6C4-4C698449B484
  PLATFORM_VERSION        = 1.0
  DSC_SPECIFICATION       = 0x00010005
  OUTPUT_DIRECTORY        = Build/NtfsProbe
  SUPPORTED_ARCHITECTURES = X64|AARCH64
  BUILD_TARGETS           = RELEASE
  SKUID_IDENTIFIER        = DEFAULT

# 标准基础库解析 + AARCH64 CompilerIntrinsicsLib NULL 链接（S00 配方）
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
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf

[Components]
  third_party/ntfs-3g/testapp/NtfsProbe.inf