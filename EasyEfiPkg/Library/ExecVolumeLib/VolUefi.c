/** @file
  卷层 UEFI 实现（仅 UEFI 编译，进 .inf）：
  VolEnumerate — 枚举 SimpleFileSystem 句柄，取卷标签/容量/空闲（FileSystem Info）、
                 设备路径、FsName（PartitionInfo 的 GPT 分区类型 GUID 启发式）。
  VolFindP3    — design §5.2 五步：调用方传入自身 LoadedImage 设备路径（BootDp=NULL
                 时自动取 gImageHandle 的 LoadedImage），同盘+分区3+标记 → 兜底全盘
                 扫标记 → EFI_NOT_FOUND（Launcher 据此降级 DEGRADED）。
  不加载驱动（S14/S15 职责）；不做磁盘/分区级写（MVP 冻结）。
**/
#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PartitionInfo.h>
#include <Guid/FileSystemInfo.h>
#include <Guid/Gpt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/FsOps.h"
#include "Contract/VolumeModel.h"
#include "ExecVolumeInternal.h"

/* Microsoft Basic Data GUID（design §3.1 P3 类型；MdePkg 未提供常量） */
STATIC CONST EFI_GUID mMsBasicDataGuid = {
  0xEBD0A0A2, 0xB9E5, 0x4433, { 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 }
};

STATIC
VOID
CopyStrTrunc (
  CHAR16       *Dst,
  CONST CHAR16 *Src,
  UINTN        Cap
  )
{
  UINTN i;
  for (i = 0; Src[i] != 0 && i + 1 < Cap; i++) {
    Dst[i] = Src[i];
  }
  Dst[i] = 0;
}

/**
  FsName 启发式（显示用途）：GPT 分区类型 GUID → ESP="FAT32"、
  Microsoft Basic Data="NTFS"（本镜像 P3 由 ntfs-3g 驱动绑定）、其余="UNKNOWN"。
  注：Basic Data 分区理论上可为其它 FS；MVP 镜像布局下即 NTFS（交接注记已注明）。
**/
STATIC
VOID
DeriveFsName (
  EFI_HANDLE Handle,
  CHAR16     *Out,
  UINTN      Cap
  )
{
  EFI_PARTITION_INFO_PROTOCOL *pi;
  EFI_STATUS                  st;
  CONST CHAR16                *name = U16 ("UNKNOWN");

  st = gBS->HandleProtocol (Handle, &gEfiPartitionInfoProtocolGuid, (VOID **)&pi);
  if (!EFI_ERROR (st) && pi->Type == PARTITION_TYPE_GPT) {
    if (CompareGuid (&pi->Info.Gpt.PartitionTypeGUID, &gEfiPartTypeSystemPartGuid)) {
      name = U16 ("FAT32");
    } else if (CompareGuid (&pi->Info.Gpt.PartitionTypeGUID, &mMsBasicDataGuid)) {
      name = U16 ("NTFS");
    }
  }
  CopyStrTrunc (Out, name, Cap);
}

/** 取自身 LoadedImage 设备路径字节（callee 分配）。失败返回 NULL。 */
STATIC
UINT8 *
OwnBootDp (
  UINTN *Len
  )
{
  EFI_LOADED_IMAGE_PROTOCOL *li;
  EFI_DEVICE_PATH_PROTOCOL  *dp;
  UINTN                     sz;
  UINT8                     *buf;
  EFI_STATUS                st;

  st = gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&li);
  if (EFI_ERROR (st) || li->FilePath == NULL) {
    return NULL;
  }
  dp = li->FilePath;
  sz = GetDevicePathSize (dp);
  if (sz == 0) {
    return NULL;
  }
  buf = (UINT8 *)MemAlloc (sz);
  if (buf == NULL) {
    return NULL;
  }
  CopyMem (buf, dp, sz);
  *Len = sz;
  return buf;
}

EFI_STATUS
VolEnumerate (
  VOLUME_INFO **Vols,
  UINTN       *Count
  )
{
  EFI_HANDLE                    *handles = NULL;
  UINTN                         n = 0, i, filled = 0;
  VOLUME_INFO                   *vols;
  UINT8                         *bootDp = NULL;
  UINTN                         bootLen = 0;
  EFI_STATUS                    st;

  if (Vols == NULL || Count == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Vols  = NULL;
  *Count = 0;

  st = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &n, &handles);
  if (st == EFI_NOT_FOUND) {
    return EFI_SUCCESS;             /* 无卷：空集 */
  }
  if (EFI_ERROR (st)) {
    return st;
  }
  vols = (VOLUME_INFO *)MemAlloc (n * sizeof (VOLUME_INFO));
  if (vols == NULL) {
    gBS->FreePool (handles);
    return EFI_OUT_OF_RESOURCES;
  }
  bootDp = OwnBootDp (&bootLen);

  for (i = 0; i < n; i++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
    EFI_FILE_PROTOCOL               *root = NULL;
    EFI_FILE_SYSTEM_INFO            *fsi = NULL;
    EFI_DEVICE_PATH_PROTOCOL        *dp;
    UINTN                           sz = 0, dpSz;
    VOLUME_INFO                     *v = &vols[filled];

    st = gBS->HandleProtocol (handles[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&sfs);
    if (EFI_ERROR (st)) {
      continue;
    }
    st = sfs->OpenVolume (sfs, &root);
    if (EFI_ERROR (st)) {
      continue;
    }
    /* 标签/容量/空闲（失败不致命：置空/0） */
    st = root->GetInfo (root, &gEfiFileSystemInfoGuid, &sz, NULL);
    if (st == EFI_BUFFER_TOO_SMALL) {
      fsi = (EFI_FILE_SYSTEM_INFO *)MemAlloc (sz);
      if (fsi != NULL) {
        st = root->GetInfo (root, &gEfiFileSystemInfoGuid, &sz, fsi);
        if (EFI_ERROR (st)) {
          MemFree (fsi);
          fsi = NULL;
        }
      }
    }
    ZeroMem (v, sizeof (*v));
    if (fsi != NULL) {
      CopyStrTrunc (v->Label, fsi->VolumeLabel, 64);
      v->Capacity = fsi->VolumeSize;
      v->Free     = fsi->FreeSpace;
      MemFree (fsi);
    }
    DeriveFsName (handles[i], v->FsName, 16);
    /* 设备路径字节（含 END 节点） */
    dp = DevicePathFromHandle (handles[i]);
    if (dp != NULL) {
      dpSz = GetDevicePathSize (dp);
      v->Dp = (UINT8 *)MemAlloc (dpSz ? dpSz : 1);
      if (v->Dp != NULL) {
        CopyMem (v->Dp, dp, dpSz);
        v->DpLen = dpSz;
      }
    }
    /* Fs 接管 root 所有权 */
    v->Fs = FsUefiFromRoot (handles[i], root);
    if (v->Fs == NULL) {
      MemFree (v->Dp);
      continue;                     /* root 已被 FsUefiFromRoot 失败路径关闭 */
    }
    filled++;
  }
  gBS->FreePool (handles);

  if (filled == 0) {
    MemFree (bootDp);
    MemFree (vols);
    return EFI_SUCCESS;
  }
  VolSortMark (vols, filled, bootDp, bootLen);
  MemFree (bootDp);
  *Vols  = vols;
  *Count = filled;
  return EFI_SUCCESS;
}

EFI_STATUS
VolFindP3 (
  CONST UINT8 *BootDp,
  UINTN       BootLen,
  FS_OPS      **P3Fs
  )
{
  VOLUME_INFO *vols = NULL;
  UINTN       n = 0, idx = 0;
  UINT8       *ownDp = NULL;
  UINTN       ownLen = 0;
  EFI_STATUS  st;

  if (P3Fs == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *P3Fs = NULL;
  if (BootDp == NULL || BootLen == 0) {
    ownDp  = OwnBootDp (&ownLen);   /* 便利路径：自动取自身 LoadedImage */
    BootDp = ownDp;
    BootLen = ownLen;
  }
  st = VolEnumerate (&vols, &n);
  if (EFI_ERROR (st)) {
    MemFree (ownDp);
    return st;
  }
  st = VolFindP3InSet (vols, n, BootDp, BootLen, &idx);
  if (!EFI_ERROR (st)) {
    *P3Fs = vols[idx].Fs;
    vols[idx].Fs = NULL;            /* 交接所有权：caller 经 Destroy 释放 */
  }
  VolFree (vols, n);
  MemFree (ownDp);
  return st;
}
