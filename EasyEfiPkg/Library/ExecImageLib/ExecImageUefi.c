/** @file
  ExecImageLib UEFI glue —— 内部缝（ExecImageSeam.h）的真实 gBS/gRT/gST 后端。
  仅 UEFI 构建编译（.inf Sources）；宿主测试以 tests/HostSeam.c 替代。

  已核实的 EDK2 行为（edk2-stable202608）：
  - gBS->LoadImage 不接受 LoadOptions 参数 → 加载后经 LoadedImage 协议注入
    （MdeModulePkg BmBoot 同款做法）；Options 缓冲在 StartImage 返回前保持有效。
  - gBS->StartImage 返回值 = 目标镜像 entry point 返回值 / Exit() 状态
    （Core/Dxe/Image/Image.c：Image->Status = EntryPoint(...); return Image->Status）。
  - UEFI 无事件枚举 API：EventsClose 只能关闭本库自己创建的事件（MVP 无）。
    调用方应用（S11 Launcher）自己的倒计时定时器须在调 ExecEfi/ExecNsh 前自行停止
    ——见 docs/handoffs/S10.md 集成注记。
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "ExecImageSeam.h"

/* ---------------- 控制台保存/恢复 ---------------- */

STATIC UINTN   mSavedMode      = 0;
STATIC BOOLEAN mSavedModeValid = FALSE;

STATIC EFI_STATUS UefiConsoleSave (VOID) {
  if (gST == NULL || gST->ConOut == NULL || gST->ConOut->Mode == NULL) {
    return EFI_UNSUPPORTED;
  }
  mSavedMode      = gST->ConOut->Mode->Mode;
  mSavedModeValid = TRUE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS UefiConsoleRestore (VOID) {
  if (!mSavedModeValid || gST == NULL || gST->ConOut == NULL) {
    return EFI_UNSUPPORTED;
  }
  mSavedModeValid = FALSE;
  if (gST->ConOut->Mode->Mode != mSavedMode) {
    /* SetMode 会清屏；调用方 TUI 负责重绘 */
    return gST->ConOut->SetMode (gST->ConOut, mSavedMode);
  }
  return EFI_SUCCESS;
}

/* ---------------- 自身事件清理 ---------------- */

#define EXEC_MAX_OWN_EVENTS 8
STATIC EFI_EVENT mOwnEvents[EXEC_MAX_OWN_EVENTS];
STATIC UINTN     mOwnEventCount = 0;

/* MVP 的本库不创建任何事件，注册表为空；保留真实关闭循环，未来 glue 若创建
   定时器事件（经 mOwnEvents 登记）即在 StartImage 前全部取消并关闭。 */
STATIC EFI_STATUS UefiEventsClose (VOID) {
  UINTN i;
  for (i = 0; i < mOwnEventCount; i++) {
    if (mOwnEvents[i] != NULL) {
      gBS->SetTimer (mOwnEvents[i], TimerCancel, 0);
      gBS->CloseEvent (mOwnEvents[i]);
      mOwnEvents[i] = NULL;
    }
  }
  mOwnEventCount = 0;
  return EFI_SUCCESS;
}

/* ---------------- LoadImage / StartImage ---------------- */

STATIC EFI_STATUS UefiLoadImage (CONST UINT8 *Dp, UINTN DpLen, CONST CHAR16 *Options,
                                 UINTN OptionsSize, VOID **ImageHandle) {
  EFI_STATUS                Status;
  EFI_HANDLE                Image = NULL;
  EFI_LOADED_IMAGE_PROTOCOL *Li;

  if (Dp == NULL || DpLen < 4 || ImageHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Status = gBS->LoadImage (
                  FALSE,                   /* BootPolicy：直接加载，非启动项 */
                  gImageHandle,            /* ParentImageHandle */
                  (EFI_DEVICE_PATH_PROTOCOL *)Dp,
                  NULL,                    /* SourceBuffer：从设备路径加载 */
                  0,
                  &Image
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Options != NULL && OptionsSize != 0) {
    Status = gBS->HandleProtocol (Image, &gEfiLoadedImageProtocolGuid, (VOID **)&Li);
    if (EFI_ERROR (Status)) {
      gBS->UnloadImage (Image);
      return Status;
    }
    Li->LoadOptions     = (VOID *)Options;
    Li->LoadOptionsSize = (UINT32)OptionsSize;
  }
  *ImageHandle = Image;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS UefiStartImage (VOID *ImageHandle, EFI_STATUS *ImageReturn) {
  EFI_STATUS Status;
  UINTN      ExitDataSize = 0;
  CHAR16     *ExitData    = NULL;

  Status = gBS->StartImage ((EFI_HANDLE)ImageHandle, &ExitDataSize, &ExitData);
  if (ExitData != NULL) {
    FreePool (ExitData);
  }
  if (ImageReturn != NULL) {
    *ImageReturn = Status;
  }
  return Status;
}

/* ---------------- 变量 / 复位 ---------------- */

STATIC EFI_STATUS UefiSetVariable (CONST CHAR16 *Name, CONST UINT8 *Guid16,
                                   UINT32 Attributes, CONST VOID *Data, UINTN Size) {
  EFI_GUID Guid;
  CopyMem (&Guid, Guid16, sizeof (Guid));
  return gRT->SetVariable ((CHAR16 *)Name, &Guid, Attributes, Size, (VOID *)Data);
}

STATIC VOID UefiResetSystem (UINT32 ResetType) {
  gRT->ResetSystem ((EFI_RESET_TYPE)ResetType, EFI_SUCCESS, 0, NULL);
  CpuDeadLoop ();                          /* 仅当固件 ResetSystem 异常返回时到达 */
}

/* ---------------- 启动卷设备路径 ---------------- */

STATIC EFI_STATUS UefiBootVolumeDp (UINT8 **Dp, UINTN *DpLen) {
  EFI_STATUS                Status;
  EFI_LOADED_IMAGE_PROTOCOL *Li;
  EFI_DEVICE_PATH_PROTOCOL  *VolDp;
  UINTN                     Len;
  UINT8                     *Copy;

  if (Dp == NULL || DpLen == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Status = gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  /* DeviceHandle = 加载来源卷（分区）句柄；其 DevicePath 协议 = 卷设备路径（含 END） */
  Status = gBS->HandleProtocol (Li->DeviceHandle, &gEfiDevicePathProtocolGuid, (VOID **)&VolDp);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Len = GetDevicePathSize (VolDp);
  if (Len < 4) {
    return EFI_NOT_FOUND;
  }
  Copy = (UINT8 *)MemAlloc (Len);
  if (Copy == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  CopyMem (Copy, VolDp, Len);
  *Dp    = Copy;
  *DpLen = Len;
  return EFI_SUCCESS;
}

/* ---------------- 缝实例 ---------------- */

STATIC CONST EXEC_SEAM mUefiSeam = {
  UefiLoadImage,
  UefiStartImage,
  UefiConsoleSave,
  UefiEventsClose,
  UefiConsoleRestore,
  UefiSetVariable,
  UefiResetSystem,
  UefiBootVolumeDp
};

CONST EXEC_SEAM *gExecSeam = &mUefiSeam;