/** @file
  ShellRunner — compat 布局的架构分流 trampoline（S15）。
  作为 \EFI\BOOT\BOOT{ARCH}.EFI：枚举 SimpleFileSystem 卷，加载 \EFI\BOOT\Shell{Arch}.efi，
  以架构专属脚本为 file-name 参数启动（隐式 -nostartup），并预加载 NTFS 驱动。

  实证教训（QEMU ArmVirtQemu）：
  - 短格式 USB 引导展开时 LoadedImage->FilePath 可能不含 FILEPATH 节点，
    不能依赖自身设备路径做前缀手术 → 改用卷枚举 + FileDevicePath。
  - 脚本内启动异架构应用会中止 .nsh → 架构分流必须在 UEFI 层（本 trampoline）完成。
  - 嵌套 Shell（file-name 模式）不设置 %homefilesystem% → 驱动由本 trampoline 预加载，
    架构脚本只做 map -r + noautorun + Launcher。
  - Shell LoadOptions 首 token 被当镜像名跳过 → 必须带 "shell.efi " 前缀。
**/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>

#if defined(MDE_CPU_X64)
STATIC CONST CHAR16 *mShellName = L"\\EFI\\BOOT\\ShellX64.efi";
STATIC CONST CHAR16 *mLoadOpts  = L"shell.efi \\EFI\\BOOT\\startup-x64.nsh";
STATIC CONST CHAR16 *mDriver    = L"\\EFI\\Drivers\\ntfs_x64.efi";
#elif defined(MDE_CPU_AARCH64)
STATIC CONST CHAR16 *mShellName = L"\\EFI\\BOOT\\ShellAA64.efi";
STATIC CONST CHAR16 *mLoadOpts  = L"shell.efi \\EFI\\BOOT\\startup-aa64.nsh";
STATIC CONST CHAR16 *mDriver    = L"\\EFI\\Drivers\\ntfs_aa64.efi";
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
  EFI_HANDLE                 *Handles;
  UINTN                      Count;
  UINTN                      Index;
  EFI_HANDLE                 ShellImage;
  EFI_HANDLE                 BootVol;
  EFI_LOADED_IMAGE_PROTOCOL  *ShellLi;
  EFI_DEVICE_PATH_PROTOCOL   *Dp;

  ShellImage = NULL;
  BootVol    = NULL;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    Print (L"[ShellRunner] no SimpleFileSystem volumes: %r\n", Status);
    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    Dp = FileDevicePath (Handles[Index], mShellName);
    if (Dp == NULL) {
      continue;
    }
    Status = gBS->LoadImage (FALSE, ImageHandle, Dp, NULL, 0, &ShellImage);
    FreePool (Dp);
    if (!EFI_ERROR (Status)) {
      BootVol = Handles[Index];
      break;
    }
    ShellImage = NULL;
  }
  FreePool (Handles);

  if (ShellImage == NULL) {
    Print (L"[ShellRunner] %s not found on any volume\n", mShellName);
    return EFI_NOT_FOUND;
  }

  //
  // 预加载 NTFS 驱动（失败不致命——降级归 Launcher）。绑定需显式 ConnectController。
  //
  Dp = FileDevicePath (BootVol, mDriver);
  if (Dp != NULL) {
    EFI_HANDLE  DrvImage;
    EFI_STATUS  DrvStatus;

    DrvStatus = gBS->LoadImage (FALSE, ImageHandle, Dp, NULL, 0, &DrvImage);
    FreePool (Dp);
    if (!EFI_ERROR (DrvStatus)) {
      if (!EFI_ERROR (gBS->StartImage (DrvImage, NULL, NULL))) {
        gBS->ConnectController (NULL, NULL, NULL, TRUE);
        Print (L"[ShellRunner] NTFS driver preloaded\n");
      }
    }
  }

  Status = gBS->HandleProtocol (ShellImage, &gEfiLoadedImageProtocolGuid, (VOID **)&ShellLi);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  ShellLi->LoadOptions     = (VOID *)mLoadOpts;
  ShellLi->LoadOptionsSize = (UINT32)StrSize (mLoadOpts);

  return gBS->StartImage (ShellImage, NULL, NULL);
}
