## EasyEfiPkg 平台聚合器（leader 独占；模块经 !include 碎片接入，避免共享文件竞争）
[Defines]
  PLATFORM_NAME           = EasyEfiPkg
  PLATFORM_GUID           = E88CEE77-842D-4B27-A4BC-2083B97D64D6
  PLATFORM_VERSION        = 0.1
  DSC_SPECIFICATION       = 0x0001001b
  OUTPUT_DIRECTORY        = Build/EasyEfiPkg
  SUPPORTED_ARCHITECTURES = X64|AARCH64
  BUILD_TARGETS           = RELEASE|DEBUG
  SKUID_IDENTIFIER        = DEFAULT

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
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  CompilerIntrinsicsLib|MdePkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  EfiCompatLib|EasyEfiPkg/Library/EfiCompatLib/EfiCompatLib.inf
  # JsonLib 依赖（edk2-stable202608 实测路径；S03 EDK2 适配器用）
  JsonLib|RedfishPkg/Library/JsonLib/JsonLib.inf
  Ucs2Utf8Lib|RedfishPkg/Library/BaseUcs2Utf8Lib/BaseUcs2Utf8Lib.inf
  RedfishCrtLib|RedfishPkg/PrivateLibrary/RedfishCrtLib/RedfishCrtLib.inf

[BuildOptions]
  GCC:*_*_*_CC_FLAGS = -DUEFI_BUILD

# ---- 模块碎片（各流独占自己的 module.dsc.inc）----
!include EasyEfiPkg/Library/Utf8Lib/module.dsc.inc
!include EasyEfiPkg/Library/EditorModelLib/module.dsc.inc
!include EasyEfiPkg/Library/ConfigCoreLib/module.dsc.inc
!include EasyEfiPkg/Library/ExecPathLib/module.dsc.inc
!include EasyEfiPkg/Library/VolumeModelLib/module.dsc.inc
!include EasyEfiPkg/Library/TuiScreenLib/module.dsc.inc
!include EasyEfiPkg/Library/TuiInputLib/module.dsc.inc
!include EasyEfiPkg/Library/TuiWidgetLib/module.dsc.inc
!include EasyEfiPkg/Library/ExecVolumeLib/module.dsc.inc
!include EasyEfiPkg/Library/ExecImageLib/module.dsc.inc
!include EasyEfiPkg/Application/Launcher/module.dsc.inc
!include EasyEfiPkg/Application/FileManager/module.dsc.inc
!include EasyEfiPkg/Application/Editor/module.dsc.inc
