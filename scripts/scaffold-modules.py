#!/usr/bin/env python3
"""S00: 生成 EasyEfiPkg 模块脚手架（stub .inf + module.dsc.inc + 占位源码）。
流负责人接管目录后以真实实现替换 stub。契约头已冻结于 Include/Contract/。"""
import os, uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(ROOT, "EasyEfiPkg")

LIBS = ["Utf8Lib", "EditorModelLib", "ConfigCoreLib", "ExecPathLib", "VolumeModelLib",
        "TuiScreenLib", "TuiInputLib", "TuiWidgetLib", "ExecVolumeLib", "ExecImageLib"]
APPS = ["Launcher", "FileManager", "Editor"]

LIB_STUB = '''/** @file
  {name} STUB（S00 脚手架）。流负责人以真实实现替换；契约见 Include/Contract/。
**/
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"

/* STUB: 占位实现，仅为让消费流编译链接通过。 */
EFI_STATUS {name}StubProbe (VOID) {{ return EFI_UNSUPPORTED; }}
'''

APP_STUB = '''/** @file
  {name} STUB 应用（S00 脚手架）。打印架构标记后置 EASYEFI_RAN 返回。
**/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Protocol/Shell.h>
#include <Protocol/LoadedImage.h>

EFI_STATUS EFIAPI UefiMain (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{{
  EFI_SHELL_PROTOCOL *Shell;
#if defined(MDE_CPU_X64)
  Print (L"[STUB] {name} x64\\n");
#elif defined(MDE_CPU_AARCH64)
  Print (L"[STUB] {name} aarch64\\n");
#endif
  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiShellProtocolGuid, NULL, (VOID **)&Shell))) {{
    Shell->SetEnv (L"EASYEFI_RAN", L"yes");
  }}
  return EFI_SUCCESS;
}}
'''

LIB_INF = '''## {name} STUB（S00 脚手架；流负责人替换 Sources 为真实实现）
[Defines]
  INF_VERSION    = 0x0001001b
  BASE_NAME      = {name}
  FILE_GUID      = {guid}
  MODULE_TYPE    = BASE
  VERSION_STRING = 0.1
  LIBRARY_CLASS  = {name}|UEFI_APPLICATION UEFI_DRIVER DXE_DRIVER

[Sources]
  {name}Stub.c

[Packages]
  MdePkg/MdePkg.dec
  EasyEfiPkg/EasyEfiPkg.dec

[LibraryClasses]
  EfiCompatLib

[BuildOptions]
  GCC:*_*_*_CC_FLAGS = -DUEFI_BUILD
'''

APP_INF = '''## {name} STUB 应用（S00 脚手架）
[Defines]
  INF_VERSION    = 0x0001001b
  BASE_NAME      = {name}
  FILE_GUID      = {guid}
  MODULE_TYPE    = UEFI_APPLICATION
  VERSION_STRING = 0.1
  ENTRY_POINT    = UefiMain

[Sources]
  {name}Stub.c

[Packages]
  MdePkg/MdePkg.dec
  MdeModulePkg/MdeModulePkg.dec
  EasyEfiPkg/EasyEfiPkg.dec

[LibraryClasses]
  UefiApplicationEntryPoint
  UefiLib
  UefiBootServicesTableLib
  PrintLib

[BuildOptions]
  GCC:*_*_*_CC_FLAGS = -DUEFI_BUILD
'''

LIB_FRAG = '''# {name} 模块碎片（leader 聚合进 EasyEfiPkg.dsc；流负责人只改本文件）
[LibraryClasses]
  {name}|EasyEfiPkg/Library/{name}/{name}.inf

[Components]
  EasyEfiPkg/Library/{name}/{name}.inf
'''

APP_FRAG = '''# {name} 模块碎片
[Components]
  EasyEfiPkg/Application/{name}/{name}.inf
'''

for name in LIBS:
    d = os.path.join(PKG, "Library", name)
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, name + "Stub.c"), "w").write(LIB_STUB.format(name=name))
    open(os.path.join(d, name + ".inf"), "w").write(LIB_INF.format(name=name, guid=str(uuid.uuid4()).upper()))
    open(os.path.join(d, "module.dsc.inc"), "w").write(LIB_FRAG.format(name=name))

for name in APPS:
    d = os.path.join(PKG, "Application", name)
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, name + "Stub.c"), "w").write(APP_STUB.format(name=name))
    open(os.path.join(d, name + ".inf"), "w").write(APP_INF.format(name=name, guid=str(uuid.uuid4()).upper()))
    open(os.path.join(d, "module.dsc.inc"), "w").write(APP_FRAG.format(name=name))

print("scaffolded:", len(LIBS), "libs +", len(APPS), "apps")
