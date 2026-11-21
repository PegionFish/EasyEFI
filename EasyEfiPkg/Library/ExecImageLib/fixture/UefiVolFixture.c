/** @file
  TEST-ONLY fixture —— S09 ExecVolumeLib 契约（VolEnumerate/VolFree）的 UEFI 测试替身，
  供 S10 QEMU 冒烟使用（worktree 内 ExecVolumeLib 是 S00 stub，未定义契约符号）。
  集成时由真实 ExecVolumeLib 取代（smoke/Smoke.dsc 的 fixture 解析随之移除）。

  行为（按契约 + S10 交接注记的解释）：
  - 枚举全部 SimpleFileSystem 句柄；卷设备路径取自句柄 DevicePath 协议（含 END）。
  - FS_OPS 实例按句柄缓存（指针跨调用稳定）；仅实现 Info（S10 唯一引用的操作），
    其余 vtable 项为 NULL（冒烟不调用）。
  - VolFree 只释放 VOLUME_INFO 数组与 Dp；Fs 实例进程期存活（EXEC_CANDIDATE.Fs 借用）。
**/
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/FsOps.h"

typedef struct {
  FS_OPS     Ops;
  EFI_HANDLE Handle;
  BOOLEAN    Used;
} FIXFS;

#define FIXFS_MAX 32
STATIC FIXFS mFixFs[FIXFS_MAX];

/* Info：SFS 打开文件读 EFI_FILE_INFO。路径规范化为 '\\' 分隔 + 前导 '\\'（SFS 绝对路径）。 */
STATIC EFI_STATUS FixInfo (FS_OPS *Fs, CONST CHAR16 *Path, FS_ENTRY *Out) {
  FIXFS                           *F = (FIXFS *)Fs;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs;
  EFI_FILE_PROTOCOL               *Root = NULL;
  EFI_FILE_PROTOCOL               *File = NULL;
  EFI_FILE_INFO                   *Info = NULL;
  UINTN                           InfoSize = 0;
  EFI_STATUS                      Status;
  CHAR16                          Norm[512];
  UINTN                           i, j;

  if (Fs == NULL || Path == NULL || Out == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  j = 0;
  Norm[j++] = L'\\';
  for (i = 0; Path[i] != 0 && j + 1 < sizeof (Norm) / sizeof (CHAR16); i++) {
    CHAR16 C = (Path[i] == L'/') ? L'\\' : Path[i];
    if (C == L'\\' && Norm[j - 1] == L'\\') {
      continue;
    }
    Norm[j++] = C;
  }
  Norm[j] = 0;

  Status = gBS->HandleProtocol (F->Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Sfs);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Sfs->OpenVolume (Sfs, &Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Root->Open (Root, &File, Norm, EFI_FILE_MODE_READ, 0);
  Root->Close (Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Info = AllocatePool (InfoSize);
    if (Info == NULL) {
      File->Close (File);
      return EFI_OUT_OF_RESOURCES;
    }
    Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  }
  File->Close (File);
  if (EFI_ERROR (Status)) {
    if (Info != NULL) {
      FreePool (Info);
    }
    return Status;
  }
  for (i = 0; Info->FileName[i] != 0 && i < 255; i++) {
    Out->Name[i] = Info->FileName[i];
  }
  Out->Name[i] = 0;
  Out->Size  = Info->FileSize;
  Out->Mtime = 0;
  Out->IsDir = (BOOLEAN)((Info->Attribute & EFI_FILE_DIRECTORY) != 0);
  FreePool (Info);
  return EFI_SUCCESS;
}

STATIC FS_OPS *NewFixFs (EFI_HANDLE Handle) {
  UINTN i;
  for (i = 0; i < FIXFS_MAX; i++) {
    if (mFixFs[i].Used && mFixFs[i].Handle == Handle) {
      return &mFixFs[i].Ops;               /* 缓存命中：指针稳定 */
    }
  }
  for (i = 0; i < FIXFS_MAX; i++) {
    if (!mFixFs[i].Used) {
      mFixFs[i].Used   = TRUE;
      mFixFs[i].Handle = Handle;
      mFixFs[i].Ops.Info = FixInfo;        /* 其余项 NULL：S10 冒烟不调用 */
      return &mFixFs[i].Ops;
    }
  }
  return NULL;
}

STATIC VOID GetLabel (EFI_HANDLE Handle, CHAR16 *Out, UINTN Cap, UINTN Idx) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs;
  EFI_FILE_PROTOCOL               *Root = NULL;
  EFI_FILE_SYSTEM_INFO            *FsInfo = NULL;
  UINTN                           Size = 0;
  EFI_STATUS                      Status;
  UINTN                           i;

  Out[0] = 0;
  Status = gBS->HandleProtocol (Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Sfs);
  if (!EFI_ERROR (Status)) {
    Status = Sfs->OpenVolume (Sfs, &Root);
  }
  if (!EFI_ERROR (Status)) {
    Status = Root->GetInfo (Root, &gEfiFileSystemInfoGuid, &Size, NULL);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      FsInfo = AllocatePool (Size);
      if (FsInfo != NULL) {
        Status = Root->GetInfo (Root, &gEfiFileSystemInfoGuid, &Size, FsInfo);
      }
    }
    Root->Close (Root);
  }
  if (!EFI_ERROR (Status) && FsInfo != NULL) {
    for (i = 0; FsInfo->VolumeLabel[i] != 0 && i + 1 < Cap; i++) {
      Out[i] = FsInfo->VolumeLabel[i];
    }
    Out[i] = 0;
  }
  if (FsInfo != NULL) {
    FreePool (FsInfo);
  }
  if (Out[0] == 0) {
    Out[0] = L'V'; Out[1] = L'O'; Out[2] = L'L';
    Out[3] = (CHAR16)(L'0' + (Idx % 10));
    Out[4] = 0;
  }
}

EFI_STATUS VolEnumerate (VOLUME_INFO **Vols, UINTN *Count) {
  EFI_HANDLE  *Handles = NULL;
  UINTN       N = 0, i;
  EFI_STATUS  Status;
  VOLUME_INFO *Arr;

  if (Vols == NULL || Count == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Vols = NULL;
  *Count = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &N,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Arr = (VOLUME_INFO *)MemAlloc (N * sizeof (VOLUME_INFO));
  if (Arr == NULL) {
    FreePool (Handles);
    return EFI_OUT_OF_RESOURCES;
  }
  for (i = 0; i < N; i++) {
    EFI_DEVICE_PATH_PROTOCOL *Dp = NULL;
    UINTN                    Len;

    Arr[i].Fs = NewFixFs (Handles[i]);
    Status = gBS->HandleProtocol (Handles[i], &gEfiDevicePathProtocolGuid, (VOID **)&Dp);
    if (EFI_ERROR (Status) || Dp == NULL) {
      Arr[i].Dp    = NULL;
      Arr[i].DpLen = 0;
    } else {
      Len = GetDevicePathSize (Dp);
      Arr[i].Dp = (UINT8 *)MemAlloc (Len);
      if (Arr[i].Dp != NULL) {
        CopyMem (Arr[i].Dp, Dp, Len);
        Arr[i].DpLen = Len;
      } else {
        Arr[i].DpLen = 0;
      }
    }
    GetLabel (Handles[i], Arr[i].Label, sizeof (Arr[i].Label) / sizeof (CHAR16), i);
    Arr[i].FsName[0] = L'F'; Arr[i].FsName[1] = L'A'; Arr[i].FsName[2] = L'T';
    Arr[i].FsName[3] = 0;
    Arr[i].Capacity = 0;                   /* S10 不引用 */
    Arr[i].Free     = 0;
    Arr[i].IsP3     = FALSE;
  }
  FreePool (Handles);
  *Vols  = Arr;
  *Count = N;
  return EFI_SUCCESS;
}

VOID VolFree (VOLUME_INFO *Vols, UINTN Count) {
  UINTN i;
  if (Vols == NULL) {
    return;
  }
  for (i = 0; i < Count; i++) {
    MemFree (Vols[i].Dp);                  /* Fs 实例借用存活，不销毁 */
  }
  MemFree (Vols);
}