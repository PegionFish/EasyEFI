/** @file
  S09 QEMU 冒烟应用（fat 变体，双架构）：
    1) 自身 LoadedImage 设备路径打印；
    2) VolEnumerate：卷数/标签/FsName/容量/isP3/设备路径；
    3) VolFindP3：fat 镜像无 P3/标记 → 断言 EFI_NOT_FOUND（降级路径）；
    4) 启动卷 FAT 读写闭环：MkDir→Open(CREATE)→Write→Flush→Close→重开 Read 比对
       →Enum/Info→Rename→Delete（含非空目录拒绝负例）→清理；
    5) 全部通过打印 [S09] FAT-SMOKE PASS。
  NTFS 变体冒烟待 S14 ntfs.efi 产物就绪后补测（见 docs/handoffs/S09.md）。
**/
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/FsOps.h"
#include "Contract/VolumeModel.h"
#include "ExecVolumeInternal.h"

#define PROBE_SIZE  4096

#define CHECK(cond, msg) do { \
  if (!(cond)) { Print (L"[S09] FAIL %a\n", msg); return EFI_ABORTED; } \
} while (0)

#define CHECK_ST(st, msg) do { \
  EFI_STATUS _s = (st); \
  if (EFI_ERROR (_s)) { Print (L"[S09] FAIL %a status=%r\n", msg, _s); return _s; } \
} while (0)

STATIC
EFI_STATUS
RunFatIo (
  FS_OPS *Fs
  )
{
  FS_HANDLE  *h = NULL;
  FS_ENTRY   *ents = NULL, ent;
  UINTN      n = 0, len, i;
  UINT8      *wbuf, *rbuf;
  EFI_STATUS st;

  wbuf = (UINT8 *)MemAlloc (PROBE_SIZE);
  rbuf = (UINT8 *)MemAlloc (PROBE_SIZE);
  if (wbuf == NULL || rbuf == NULL) {
    MemFree (wbuf);
    MemFree (rbuf);
    return EFI_OUT_OF_RESOURCES;
  }
  for (i = 0; i < PROBE_SIZE; i++) {
    wbuf[i] = (UINT8)(i * 7 + 13);
  }

  CHECK_ST (Fs->MkDir (Fs, U16 ("s09test")), "mkdir");
  CHECK_ST (Fs->Open (Fs, U16 ("s09test\\probe.bin"), FS_MODE_WRITE | FS_MODE_CREATE, &h), "open-create");
  len = PROBE_SIZE;
  CHECK_ST (Fs->Write (Fs, h, wbuf, &len), "write");
  CHECK (len == PROBE_SIZE, "write-len");
  CHECK_ST (Fs->Flush (Fs, h), "flush");
  CHECK_ST (Fs->Close (Fs, h), "close");
  h = NULL;

  CHECK_ST (Fs->Open (Fs, U16 ("s09test/probe.bin"), FS_MODE_READ, &h), "open-read");
  len = PROBE_SIZE;
  CHECK_ST (Fs->Read (Fs, h, rbuf, &len), "read");
  CHECK (len == PROBE_SIZE, "read-len");
  CHECK (CompareMem (rbuf, wbuf, PROBE_SIZE) == 0, "readback-mismatch");
  /* EOF：再读返回 0 */
  len = 16;
  CHECK_ST (Fs->Read (Fs, h, rbuf, &len), "read-eof");
  CHECK (len == 0, "eof-len");
  CHECK_ST (Fs->Close (Fs, h), "close-read");
  h = NULL;

  CHECK_ST (Fs->Enum (Fs, U16 ("s09test"), &ents, &n), "enum");
  CHECK (n == 1, "enum-count");
  CHECK (EfiStrCmp (ents[0].Name, U16 ("probe.bin")) == 0, "enum-name");
  CHECK (ents[0].Size == PROBE_SIZE, "enum-size");
  CHECK (!ents[0].IsDir, "enum-isdir");
  MemFree (ents);
  ents = NULL;

  CHECK_ST (Fs->Info (Fs, U16 ("s09test/probe.bin"), &ent), "info");
  CHECK (ent.Size == PROBE_SIZE, "info-size");
  CHECK (!ent.IsDir, "info-isdir");

  /* 负例：非空目录删除拒绝；不存在的文件打开失败 */
  st = Fs->Delete (Fs, U16 ("s09test"));
  CHECK (st == EFI_ACCESS_DENIED, "nonempty-dir-delete-should-fail");
  st = Fs->Open (Fs, U16 ("s09test/no-such-file"), FS_MODE_READ, &h);
  CHECK (st == EFI_NOT_FOUND, "missing-open-should-fail");

  CHECK_ST (Fs->Rename (Fs, U16 ("s09test/probe.bin"), U16 ("s09test/probe2.bin")), "rename");
  CHECK_ST (Fs->Delete (Fs, U16 ("s09test/probe2.bin")), "delete-file");
  CHECK_ST (Fs->Delete (Fs, U16 ("s09test")), "delete-empty-dir");

  MemFree (wbuf);
  MemFree (rbuf);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
UefiMain (
  EFI_HANDLE        ImageHandle,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_LOADED_IMAGE_PROTOCOL *li;
  EFI_DEVICE_PATH_PROTOCOL  *dp;
  UINT8                     *bootDp;
  UINTN                     bootLen, i, n = 0;
  VOLUME_INFO               *vols = NULL;
  FS_OPS                    *p3 = NULL, *bootFs = NULL;
  EFI_STATUS                st;

  (VOID)SystemTable;
  gBS->SetWatchdogTimer (0, 0, 0, NULL);

  st = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&li);
  CHECK_ST (st, "loaded-image");
  dp      = li->FilePath;
  bootLen = GetDevicePathSize (dp);
  bootDp  = (UINT8 *)MemAlloc (bootLen);
  CHECK (bootDp != NULL, "bootdp-alloc");
  CopyMem (bootDp, dp, bootLen);
  Print (L"[S09] boot-dp: %s\n", ConvertDevicePathToText (dp, TRUE, FALSE));

  /* 卷枚举 */
  st = VolEnumerate (&vols, &n);
  CHECK_ST (st, "vol-enumerate");
  Print (L"[S09] volumes=%u\n", (UINT32)n);
  CHECK (n >= 1, "no-volumes");
  for (i = 0; i < n; i++) {
    Print (L"[S09] vol%u label=%s fs=%s cap=%llu free=%llu isP3=%u dp=%s\n",
           (UINT32)i, vols[i].Label, vols[i].FsName,
           vols[i].Capacity, vols[i].Free, (UINT32)vols[i].IsP3,
           ConvertDevicePathToText ((EFI_DEVICE_PATH_PROTOCOL *)vols[i].Dp, TRUE, FALSE));
    if (VolSameDisk (bootDp, bootLen, vols[i].Dp, vols[i].DpLen) &&
        EfiStrCmp (vols[i].FsName, U16 ("FAT32")) == 0)
    {
      bootFs = vols[i].Fs;
    }
  }
  CHECK (bootFs != NULL, "boot-fat-volume-not-found");

  /* P3 发现：fat 镜像无 P3/标记 → NOT_FOUND（DEGRADED 降级路径） */
  st = VolFindP3 (bootDp, bootLen, &p3);
  Print (L"[S09] findp3=%r\n", st);
  CHECK (st == EFI_NOT_FOUND, "findp3-expected-not-found");
  CHECK (p3 == NULL, "findp3-handle-should-be-null");

  /* FAT 读写闭环 */
  st = RunFatIo (bootFs);
  if (EFI_ERROR (st)) {
    MemFree (bootDp);
    VolFree (vols, n);
    return st;
  }
  Print (L"[S09] FAT-SMOKE PASS\n");

  MemFree (bootDp);
  VolFree (vols, n);
  return EFI_SUCCESS;
}
