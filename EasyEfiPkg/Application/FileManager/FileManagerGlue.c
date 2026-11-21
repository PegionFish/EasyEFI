/** @file
  FileManager glue（S12）：UEFI 后端装配 + 入口循环。
  core（FmCore.c）只认契约 Ops；glue 负责 VolEnumerate/Shell 映射名/PCD/
  启动 ESP 识别/Editor 与 Shell 路径解析，持有 FM_INIT 并原地更新（Rescan）。
  路径约定与 S11/S10 对齐：ExecEfi/ExecNsh 全限定 fsN:/...，Fs=所属卷。
**/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PcdLib.h>
#include <Protocol/Shell.h>
#include <Protocol/LoadedImage.h>
#include "Contract/TuiScreen.h"
#include "Contract/TuiInput.h"
#include "Contract/FsOps.h"
#include "FmCore.h"

typedef struct {
  EFI_HANDLE          ImageHandle;
  EFI_SHELL_PROTOCOL  *Shell;          /* 可 NULL（固件直启时无 Shell） */
  UINT8               *BootPartDp;     /* 启动 ESP 分区 DP（截断至 HD 节点含） */
  UINTN               BootPartDpLen;
  VOLUME_INFO         *Vols;
  UINTN               VolCount;
  CHAR16              *NameBuf;        /* VolCount*16 连续映射名存储 */
  CONST CHAR16        **FsNames;       /* 指针数组（FM_INIT 借用） */
  CHAR16              BootFsName[16];
  CHAR16              EditorPath[128];
  CHAR16              ShellPath[128];
  FM_INIT             Init;
  FM_PLATFORM_OPS     Ops;
} FM_GLUE;

/* Shell 映射名（fsN:）；无 Shell/无映射 → 空串（S11 同款） */
STATIC VOID MapName (FM_GLUE *G, CONST UINT8 *Dp, CHAR16 *Out, UINTN Cap) {
  EFI_DEVICE_PATH_PROTOCOL *Tmp;
  CONST CHAR16 *Map;
  UINTN i;
  Out[0] = 0;
  if (G->Shell == NULL || Dp == NULL) return;
  Tmp = (EFI_DEVICE_PATH_PROTOCOL *)Dp;
  Map = G->Shell->GetMapFromDevicePath (&Tmp);
  if (Map == NULL) return;
  for (i = 0; Map[i] != 0 && i + 1 < Cap; i++) Out[i] = Map[i];
  Out[i] = 0;
  FreePool ((VOID *)Map);
}

/* 截断至首个 MEDIA_HARDDRIVE_DP（含）→ 分区 DP（S11 同款） */
STATIC UINT8 *TrimToPartition (CONST UINT8 *Dp, UINTN Len, UINTN *OutLen) {
  CONST EFI_DEVICE_PATH_PROTOCOL *Node = (CONST EFI_DEVICE_PATH_PROTOCOL *)Dp;
  UINTN Off = 0;
  *OutLen = 0;
  while (Off + 4 <= Len && !IsDevicePathEnd (Node)) {
    UINTN NodeLen = DevicePathNodeLength (Node);
    if (NodeLen < 4 || Off + NodeLen > Len) break;
    if (DevicePathType (Node) == MEDIA_DEVICE_PATH &&
        DevicePathSubType (Node) == MEDIA_HARDDRIVE_DP) {
      *OutLen = Off + NodeLen;
      return AllocateCopyPool (*OutLen, Dp);
    }
    Off += NodeLen;
    Node = NextDevicePathNode (Node);
  }
  return NULL;
}

STATIC VOID FreeVolumes (FM_GLUE *G) {
  if (G->Vols != NULL) {
    VolFree (G->Vols, G->VolCount);
    G->Vols = NULL;
    G->VolCount = 0;
  }
  MemFree (G->NameBuf);  G->NameBuf = NULL;
  MemFree (G->FsNames);  G->FsNames = NULL;
}

/* 重扫卷 + 映射名 + 启动 ESP/Editor/Shell 路径；原地更新 Init（core 借用） */
STATIC EFI_STATUS RefreshVolumes (FM_GLUE *G) {
  UINTN i;
  FS_ENTRY ent;
  FreeVolumes (G);
  if (EFI_ERROR (VolEnumerate (&G->Vols, &G->VolCount))) {
    G->Vols = NULL;
    G->VolCount = 0;
  }
  if (G->VolCount) {
    G->NameBuf = MemAlloc (G->VolCount * 16 * sizeof (CHAR16));
    G->FsNames = MemAlloc (G->VolCount * sizeof (CHAR16 *));
    for (i = 0; i < G->VolCount; i++) {
      G->FsNames[i] = G->NameBuf + i * 16;
      MapName (G, G->Vols[i].Dp, (CHAR16 *)G->FsNames[i], 16);
    }
  }
  G->Init.Vols = G->Vols;
  G->Init.VolCount = G->VolCount;
  G->Init.FsNames = G->FsNames;

  /* 启动 ESP：分区 DP 逐字节匹配 */
  G->BootFsName[0] = 0;
  if (G->BootPartDp != NULL) {
    for (i = 0; i < G->VolCount; i++) {
      if (G->Vols[i].DpLen == G->BootPartDpLen &&
          CompareMem (G->Vols[i].Dp, G->BootPartDp, G->BootPartDpLen) == 0) {
        MapName (G, G->Vols[i].Dp, G->BootFsName, 16);
        break;
      }
    }
  }
  /* Editor：启动 ESP \EFI\Apps\Editor.efi（架构由镜像布局保证，design §7.2） */
  G->EditorPath[0] = 0;
  if (G->BootFsName[0]) {
    UnicodeSPrint (G->EditorPath, sizeof (G->EditorPath), L"%s/EFI/Apps/Editor.efi", G->BootFsName);
  }
  /* Shell：优先 EFI/BOOT/Shell.efi，回退本架构 BOOT 名（测试镜像布局） */
  G->ShellPath[0] = 0;
  if (G->BootFsName[0] && G->BootPartDp != NULL) {
    for (i = 0; i < G->VolCount; i++) {
      if (G->Vols[i].DpLen == G->BootPartDpLen &&
          CompareMem (G->Vols[i].Dp, G->BootPartDp, G->BootPartDpLen) == 0) {
        FS_OPS *Fs = G->Vols[i].Fs;
        if (!EFI_ERROR (Fs->Info (Fs, L"EFI/BOOT/Shell.efi", &ent))) {
          UnicodeSPrint (G->ShellPath, sizeof (G->ShellPath), L"%s/EFI/BOOT/Shell.efi", G->BootFsName);
        } else {
#if defined(MDE_CPU_X64)
          if (!EFI_ERROR (Fs->Info (Fs, L"EFI/BOOT/BOOTX64.EFI", &ent))) {
            UnicodeSPrint (G->ShellPath, sizeof (G->ShellPath), L"%s/EFI/BOOT/BOOTX64.EFI", G->BootFsName);
          }
#elif defined(MDE_CPU_AARCH64)
          if (!EFI_ERROR (Fs->Info (Fs, L"EFI/BOOT/BOOTAA64.EFI", &ent))) {
            UnicodeSPrint (G->ShellPath, sizeof (G->ShellPath), L"%s/EFI/BOOT/BOOTAA64.EFI", G->BootFsName);
          }
#endif
        }
        break;
      }
    }
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS RescanCb (VOID *Ctx) {
  return RefreshVolumes ((FM_GLUE *)Ctx);
}

EFI_STATUS EFIAPI UefiMain (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  FM_GLUE G;
  FM_CORE *C;
  EFI_LOADED_IMAGE_PROTOCOL *Li;
  EFI_STATUS Status;
  TUI_KEY Key;

  ZeroMem (&G, sizeof (G));
  G.ImageHandle = ImageHandle;
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  gBS->LocateProtocol (&gEfiShellProtocolGuid, NULL, (VOID **)&G.Shell);

  /* 启动盘身份：自身 LoadedImage 设备路径截断至分区 */
  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li)) &&
      Li->FilePath != NULL) {
    G.BootPartDp = TrimToPartition ((CONST UINT8 *)Li->FilePath,
                                    GetDevicePathSize (Li->FilePath), &G.BootPartDpLen);
  }

  Print (L"[S12] FileManager start\n");
  Status = TuiInit ();
  if (EFI_ERROR (Status)) {
    Print (L"[S12] FAIL TuiInit=%r\n", Status);
    return Status;
  }

  RefreshVolumes (&G);
  G.Init.EditorPath = G.EditorPath;
  G.Init.ShellPath = G.ShellPath;
  G.Init.PreviewMaxBytes = PcdGet32 (PcdPreviewMaxBytes);
  G.Init.Ops = &G.Ops;
  G.Ops.Ctx = &G;
  G.Ops.Rescan = RescanCb;

  C = FmCoreNew (&G.Init);
  if (C == NULL) {
    TuiShutdown ();
    FreeVolumes (&G);
    return EFI_OUT_OF_RESOURCES;
  }
  for (;;) {
    FmCoreDraw (C);
    if (FmCoreExit (C)) break;
    Status = TuiWaitKey (&Key);
    if (EFI_ERROR (Status)) {
      Print (L"[S12] FAIL WaitKey=%r\n", Status);
      break;
    }
    FmCoreHandleKey (C, &Key);
  }
  FmCoreFree (C);
  TuiShutdown ();
  FreeVolumes (&G);
  MemFree (G.BootPartDp);
  Print (L"[S12] FileManager exit\n");
  return EFI_SUCCESS;
}
