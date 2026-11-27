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
#include "Contract/VolumeModel.h"
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
  FS_OPS              *BootFs;         /* 启动 ESP 卷（FM_INIT 借用，F4 编辑器执行） */
  CHAR16              EditorPath[128];
  CHAR16              ShellPath[128];
  FM_INIT             Init;
  FM_PLATFORM_OPS     Ops;
} FM_GLUE;

/* Shell 映射名（fsN:）；无 Shell/无映射 → 空串。
   注意（S12 冒烟实证）：GetMapFromDevicePath 返回值经 AddBufferToFreeList
   注册进 Shell 的延迟释放链表——**调用方不得 FreePool**（提前释放会让 Shell
   脚本清理 FreeBufferList 踩 0xAF 毒化内存 → Synchronous Exception）。
   返回串可能含别名（"FS0:;HD0a0b:;BLK1:"），取首个 ';' 前的主映射名。 */
STATIC VOID MapName (FM_GLUE *G, CONST UINT8 *Dp, CHAR16 *Out, UINTN Cap) {
  EFI_DEVICE_PATH_PROTOCOL *Tmp;
  CONST CHAR16 *Map;
  UINTN i;
  Out[0] = 0;
  if (G->Shell == NULL || Dp == NULL) return;
  Tmp = (EFI_DEVICE_PATH_PROTOCOL *)Dp;
  Map = G->Shell->GetMapFromDevicePath (&Tmp);
  if (Map == NULL) return;
  for (i = 0; Map[i] != 0 && Map[i] != L';' && i + 1 < Cap; i++) Out[i] = Map[i];
  Out[i] = 0;
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
    /* S09 VolSortMark 的 OwnBootDp 取 LoadedImage->FilePath——Shell 启动的应用
       FilePath 可能仅文件节点（无 HD）→ IsP3 标记失效（S12 冒烟实证）。
       glue 用 DeviceHandle 来源卷 DP 重标记（S10 实证同款身份）；S09 修复后
       此段为冗余 no-op。标记文件判定与 S09 FsHasMarker 一致（.easyefi/launcher.json）。 */
    if (G->BootPartDp != NULL) {
      for (i = 0; i < G->VolCount; i++) {
        FS_ENTRY ent;
        BOOLEAN Marker = FALSE;
        if (!EFI_ERROR (G->Vols[i].Fs->Info (G->Vols[i].Fs, L".easyefi", &ent)) ||
            !EFI_ERROR (G->Vols[i].Fs->Info (G->Vols[i].Fs, L"launcher.json", &ent))) {
          Marker = TRUE;
        }
        G->Vols[i].IsP3 = (BOOLEAN)(VolIsP3 (G->BootPartDp, G->BootPartDpLen,
                                             G->Vols[i].Dp, G->Vols[i].DpLen) && Marker);
      }
      /* P3 置首（对齐 S09 VolSortMark 意图；重标记在其排序之后，需补排序）。
         同盘分区3唯一 → 至多一个 P3，单次交换即稳定。 */
      for (i = 1; i < G->VolCount; i++) {
        if (G->Vols[i].IsP3 && !G->Vols[0].IsP3) {
          VOLUME_INFO Tv = G->Vols[0];
          CONST CHAR16 *Tn = G->FsNames[0];
          G->Vols[0] = G->Vols[i];
          G->FsNames[0] = G->FsNames[i];
          G->Vols[i] = Tv;
          G->FsNames[i] = Tn;
          break;
        }
      }
    }
  }
  G->Init.Vols = G->Vols;
  G->Init.VolCount = G->VolCount;
  G->Init.FsNames = G->FsNames;

  /* 启动 ESP：分区 DP 逐字节匹配 */
  G->BootFsName[0] = 0;
  G->BootFs = NULL;
  if (G->BootPartDp != NULL) {
    for (i = 0; i < G->VolCount; i++) {
      if (G->Vols[i].DpLen == G->BootPartDpLen &&
          CompareMem (G->Vols[i].Dp, G->BootPartDp, G->BootPartDpLen) == 0) {
        G->BootFs = G->Vols[i].Fs;
        MapName (G, G->Vols[i].Dp, G->BootFsName, 16);
        break;
      }
    }
  }
  /* Editor：卷相对路径，按布局探测（规范 EFI/Apps/Editor.efi → 旧集成布局
     EFI/Editor/Editor.efi → 架构后缀），由 FmCore 用 BootFs 执行。 */
  G->EditorPath[0] = 0;
  if (G->BootFs != NULL) {
#if defined(MDE_CPU_X64)
    CONST CHAR16 *Suffix = L"X64";
#elif defined(MDE_CPU_AARCH64)
    CONST CHAR16 *Suffix = L"AA64";
#else
    CONST CHAR16 *Suffix = L"";
#endif
    FS_ENTRY      ent;
    CHAR16        SuffixPath[64];
    CONST CHAR16 *Cands[3];
    UINTN         k;

    UnicodeSPrint (SuffixPath, sizeof (SuffixPath), L"EFI/Apps/Editor%s.efi", Suffix);
    Cands[0] = L"EFI/Apps/Editor.efi";
    Cands[1] = L"EFI/Editor/Editor.efi";
    Cands[2] = SuffixPath;
    for (k = 0; k < 3; k++) {
      if (!EFI_ERROR (G->BootFs->Info (G->BootFs, Cands[k], &ent))) {
        StrCpyS (G->EditorPath, 128, Cands[k]);
        break;
      }
    }
    if (G->EditorPath[0] == 0) {
      StrCpyS (G->EditorPath, 128, Cands[0]);
    }
  }
  G->Init.BootFs = G->BootFs;
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

  /* 启动盘身份 = 自身 LoadedImage 来源卷（DeviceHandle）的设备路径（S10 实证同款）。
     注意：Shell 启动的应用 LoadedImage->FilePath 可能仅含文件节点
     （\EFI\Apps\x.efi，无 HD 节点，S12 冒烟实证）——不可用作启动盘身份。 */
  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li)) &&
      Li->DeviceHandle != NULL) {
    EFI_DEVICE_PATH_PROTOCOL *SrcDp = NULL;
    if (!EFI_ERROR (gBS->HandleProtocol (Li->DeviceHandle, &gEfiDevicePathProtocolGuid, (VOID **)&SrcDp)) &&
        SrcDp != NULL) {
      G.BootPartDp = TrimToPartition ((CONST UINT8 *)SrcDp, GetDevicePathSize (SrcDp), &G.BootPartDpLen);
    }
  }

  Print (L"[S12] FileManager start\n");
  Status = TuiInit ();
  if (EFI_ERROR (Status)) {
    Print (L"[S12] FAIL TuiInit=%r\n", Status);
    return Status;
  }

  RefreshVolumes (&G);
  {
    UINTN i;
    for (i = 0; i < G.VolCount; i++) {
      Print (L"[S12] vol %u label=%s P3=%u map=%s\n", i, G.Vols[i].Label,
             (UINTN)G.Vols[i].IsP3, G.FsNames ? G.FsNames[i] : L"");
    }
  }
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
  FreePool (G.BootPartDp);   /* TrimToPartition 用 AllocateCopyPool（裸池）——须 FreePool，
                                不可 MemFree（EfiMem 隐藏头会偏移 16 字节 → 池头损坏） */
  Print (L"[S12] FileManager exit\n");
  return EFI_SUCCESS;
}
