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

/* ---- 卷 Fs 缓存（契约裁决 main 332553a）----
 * FS_OPS 实例按 SFS 句柄缓存，跨 VolEnumerate/VolFindP3 调用指针稳定；
 * VOLUME_INFO.Fs / VolFindP3 输出均为借用，调用方不得 Destroy；
 * 会话级生命周期（应用退出由固件回收），只增不删——避免悬空借用。 */
typedef struct {
  EFI_HANDLE Handle;
  FS_OPS     *Fs;
} VOL_CACHE_ENTRY;

STATIC VOL_CACHE_ENTRY *mVolCache  = NULL;
STATIC UINTN           mVolCacheN = 0;

STATIC
FS_OPS *
CacheFind (
  EFI_HANDLE Handle
  )
{
  UINTN i;
  for (i = 0; i < mVolCacheN; i++) {
    if (mVolCache[i].Handle == Handle) {
      return mVolCache[i].Fs;
    }
  }
  return NULL;
}

STATIC
EFI_STATUS
CacheAdd (
  EFI_HANDLE Handle,
  FS_OPS     *Fs
  )
{
  VOL_CACHE_ENTRY *ne;

  ne = (VOL_CACHE_ENTRY *)MemRealloc (mVolCache, (mVolCacheN + 1) * sizeof (*ne));
  if (ne == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  mVolCache            = ne;
  mVolCache[mVolCacheN].Handle = Handle;
  mVolCache[mVolCacheN].Fs     = Fs;
  mVolCacheN++;
  return EFI_SUCCESS;
}

FS_OPS *
FsUefiBackend (
  EFI_HANDLE SfsHandle
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
  EFI_FILE_PROTOCOL               *root;
  FS_OPS                          *fs;
  EFI_STATUS                      st;

  fs = CacheFind (SfsHandle);
  if (fs != NULL) {
    return fs;
  }
  st = gBS->HandleProtocol (SfsHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&sfs);
  if (EFI_ERROR (st)) {
    return NULL;
  }
  st = sfs->OpenVolume (sfs, &root);
  if (EFI_ERROR (st)) {
    return NULL;
  }
  fs = FsUefiFromRoot (SfsHandle, root);
  if (fs == NULL) {
    return NULL;
  }
  if (EFI_ERROR (CacheAdd (SfsHandle, fs))) {
    fs->Destroy (fs);               /* 尚未出借，可安全销毁 */
    return NULL;
  }
  return fs;
}

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

EFI_STATUS
VolOwnBootDp (
  UINT8 **Dp,
  UINTN *Len
  )
{
  EFI_LOADED_IMAGE_PROTOCOL *li;
  EFI_DEVICE_PATH_PROTOCOL  *hdp, *full;
  UINTN                     sz;
  UINT8                     *buf;
  EFI_STATUS                st;

  if (Dp == NULL || Len == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  st = gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&li);
  if (EFI_ERROR (st)) {
    return st;
  }
  hdp = DevicePathFromHandle (li->DeviceHandle);
  if (hdp == NULL) {
    return EFI_NOT_FOUND;
  }
  if (li->FilePath != NULL) {
    full = AppendDevicePath (hdp, li->FilePath);   /* AllocatePool 分配 */
    if (full == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
  } else {
    full = hdp;
  }
  sz = GetDevicePathSize (full);
  buf = (UINT8 *)MemAlloc (sz ? sz : 1);
  if (buf == NULL) {
    if (full != hdp) {
      gBS->FreePool (full);
    }
    return EFI_OUT_OF_RESOURCES;
  }
  CopyMem (buf, full, sz);
  if (full != hdp) {
    gBS->FreePool (full);
  }
  *Dp  = buf;
  *Len = sz;
  return EFI_SUCCESS;
}

/** 取自身启动设备路径字节（便利包装）。失败返回 NULL。 */
STATIC
UINT8 *
OwnBootDp (
  UINTN *Len
  )
{
  UINT8 *buf = NULL;

  if (EFI_ERROR (VolOwnBootDp (&buf, Len))) {
    return NULL;
  }
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
    /* 标签/容量/空闲（失败不致命：置空/0）。
     * 健壮 GetInfo：EfiFs/ntfs-3g 尺寸协商非标准（S14 交接已知限制 2）。 */
    st = UefiGetInfoRobust (root, &gEfiFileSystemInfoGuid, (VOID **)&fsi, &sz);
    if (EFI_ERROR (st)) {
      fsi = NULL;
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
    /* Fs：缓存命中则借用（关闭本次查询用的新 root）；未命中则接管 root 并入缓存 */
    v->Fs = CacheFind (handles[i]);
    if (v->Fs != NULL) {
      root->Close (root);
    } else {
      v->Fs = FsUefiFromRoot (handles[i], root);
      if (v->Fs == NULL) {
        MemFree (v->Dp);
        continue;                   /* root 已被 FsUefiFromRoot 失败路径关闭 */
      }
      if (EFI_ERROR (CacheAdd (handles[i], v->Fs))) {
        v->Fs->Destroy (v->Fs);     /* 尚未出借，可安全销毁 */
        v->Fs = NULL;
        MemFree (v->Dp);
        continue;
      }
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
    *P3Fs = vols[idx].Fs;           /* 借用：缓存拥有，caller 不得 Destroy */
  }
  VolFree (vols, n);
  MemFree (ownDp);
  return st;
}
