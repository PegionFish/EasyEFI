/** @file
  ShellRunner — compat 布局的架构分流 trampoline（S15）。
  作为 \EFI\BOOT\BOOT{ARCH}.EFI：加载同目录真实 Shell{Arch}.efi，
  以架构专属脚本为 file-name 参数启动（隐式 -nostartup）。
  架构分流由固件 LoadImage 天然完成——.nsh 内启动异架构应用会中止脚本
  （QEMU 实证），故不在脚本层做 try-both。
**/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/LoadedImage.h>

#if defined(MDE_CPU_X64)
STATIC CONST CHAR16 *mShellName = L"\\EFI\\BOOT\\ShellX64.efi";
STATIC CONST CHAR16 *mLoadOpts  = L"shell.efi \\EFI\\BOOT\\startup-x64.nsh";
#elif defined(MDE_CPU_AARCH64)
STATIC CONST CHAR16 *mShellName = L"\\EFI\\BOOT\\ShellAA64.efi";
STATIC CONST CHAR16 *mLoadOpts  = L"shell.efi \\EFI\\BOOT\\startup-aa64.nsh";
#else
#error "ShellRunner: unsupported architecture"
#endif

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *Li;
  EFI_LOADED_IMAGE_PROTOCOL  *ShellLi;
  EFI_DEVICE_PATH_PROTOCOL   *Node;
  EFI_DEVICE_PATH_PROTOCOL   *Prefix;
  EFI_DEVICE_PATH_PROTOCOL   *FileNode;
  EFI_DEVICE_PATH_PROTOCOL   *ShellDp;
  EFI_HANDLE                 ShellImage;
  UINTN                      PrefixLen;
  UINTN                      NodeSize;

  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // 前缀 = 自身设备路径中最后一个 FILEPATH 节点之前的部分（即所在目录）
  //
  PrefixLen = 0;
  Node      = Li->FilePath;
  while (!IsDevicePathEnd (Node)) {
    if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) &&
        (DevicePathSubType (Node) == MEDIA_FILEPATH_DP)) {
      PrefixLen = (UINTN)((UINT8 *)Node - (UINT8 *)Li->FilePath);
    }
    Node = NextDevicePathNode (Node);
  }

  if (PrefixLen == 0) {
    Print (L"[ShellRunner] no file path node in own device path\n");
    return EFI_NOT_FOUND;
  }

  Prefix = AllocateZeroPool (PrefixLen + sizeof (EFI_DEVICE_PATH_PROTOCOL));
  if (Prefix == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  CopyMem (Prefix, Li->FilePath, PrefixLen);
  SetDevicePathEndNode ((UINT8 *)Prefix + PrefixLen);

  NodeSize = sizeof (FILEPATH_DEVICE_PATH) - sizeof (CHAR16) + StrSize (mShellName);
  FileNode = CreateDeviceNode (MEDIA_DEVICE_PATH, MEDIA_FILEPATH_DP, (UINT16)NodeSize);
  if (FileNode == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  StrCpyS (((FILEPATH_DEVICE_PATH *)FileNode)->PathName,
           (NodeSize - sizeof (EFI_DEVICE_PATH_PROTOCOL)) / sizeof (CHAR16), mShellName);

  ShellDp = AppendDevicePath (Prefix, FileNode);
  FreePool (Prefix);
  FreePool (FileNode);
  if (ShellDp == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->LoadImage (FALSE, ImageHandle, ShellDp, NULL, 0, &ShellImage);
  FreePool (ShellDp);
  if (EFI_ERROR (Status)) {
    Print (L"[ShellRunner] LoadImage %s failed: %r\n", mShellName, Status);
    return Status;
  }

  Status = gBS->HandleProtocol (ShellImage, &gEfiLoadedImageProtocolGuid, (VOID **)&ShellLi);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  ShellLi->LoadOptions     = (VOID *)mLoadOpts;
  ShellLi->LoadOptionsSize = (UINT32)StrSize (mLoadOpts);

  return gBS->StartImage (ShellImage, NULL, NULL);
}
