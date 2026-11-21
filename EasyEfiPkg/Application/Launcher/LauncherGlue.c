/** @file
  Launcher glue — UEFI 装配（S11；UEFI-only，不参与宿主编译）。
  职责（任务书 S11 glue 规格）：入口 SetWatchdogTimer(0)；自身 LoadedImage 设备路径 →
  VolFindP3；读 launcher.json（FsOps）→ JsonDomFromEdk2 → ConfigValidate；
  失败 → 内置菜单 + 错误横幅；读 SecureBoot/SetupMode 变量显示；
  任何正常退出先 SetEnv EASYEFI_RAN=yes（Shell 协议可用时，§3.4）。
  core（LauncherCore.c）纯逻辑宿主可测；本文件只装配后端与驱动循环。
  纯 UEFI glue 允许裸 L""（编码纪律 1）。
**/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PcdLib.h>
#include <Library/JsonLib.h>
#include <Protocol/Shell.h>
#include <Protocol/LoadedImage.h>
#include <Guid/GlobalVariable.h>
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/TuiScreen.h"
#include "Contract/TuiInput.h"
#include "Contract/FsOps.h"
#include "Contract/ExecOps.h"
#include "Contract/ConfigSchema.h"
#include "Contract/JsonDom.h"
#include "Contract/VolumeModel.h"
#include "LauncherCore.h"

typedef struct {
  EFI_HANDLE          ImageHandle;
  EFI_SHELL_PROTOCOL  *Shell;         /* 可 NULL（固件直启 Launcher 时无 Shell） */
  UINT8               *BootDp;        /* 自身 LoadedImage 设备路径（启动盘身份） */
  UINTN               BootDpLen;
  UINT8               *BootPartDp;    /* 启动 ESP 分区 DP（截断至 HD 节点含） */
  UINTN               BootPartDpLen;
  VOLUME_INFO         *Vols;
  UINTN               VolCount;
  LAUNCHER_SESSION    Sess;
  CONFIG              *Cfg;           /* glue 持有生命周期（core 借用） */
  CONFIG_ERROR        *Errs;
  UINTN               ErrCount;
  CONFIG_ERROR        *Warns;
  UINTN               WarnCount;
  CHAR16              ShellRel[64];
  CHAR16              FmRel[64];
  CHAR16              EdRel[64];
  CHAR8               SbStatus[24];
  UINT8               Arch;
} GLUE_CTX;

/* ---- 设备路径 / 映射名 ---- */

/* 截断至首个 MEDIA_HARDDRIVE_DP（含）→ 分区 DP；无 HD 节点 → NULL */
STATIC UINT8 *TrimToPartition (CONST UINT8 *Dp, UINTN Len, UINTN *OutLen) {
  CONST EFI_DEVICE_PATH_PROTOCOL *Node = (CONST EFI_DEVICE_PATH_PROTOCOL *)Dp;
  UINTN Off = 0;
  *OutLen = 0;
  while (Off + 4 <= Len && !IsDevicePathEnd (Node)) {
    UINTN NodeLen = DevicePathNodeLength (Node);
    if (NodeLen < 4 || Off + NodeLen > Len) {
      break;                      /* 畸形防护 */
    }
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

/* Shell 映射名（fsN:）；无 Shell/无映射 → 空串。GetMapFromDevicePath 返回 callee 分配串 */
STATIC VOID MapName (GLUE_CTX *G, CONST UINT8 *Dp, CHAR16 *Out, UINTN Cap) {
  EFI_DEVICE_PATH_PROTOCOL *Tmp;
  CONST CHAR16             *Map;
  UINTN                    i;
  Out[0] = 0;
  if (G->Shell == NULL || Dp == NULL) {
    return;
  }
  Tmp = (EFI_DEVICE_PATH_PROTOCOL *)Dp;
  Map = G->Shell->GetMapFromDevicePath (&Tmp);
  if (Map == NULL) {
    return;
  }
  for (i = 0; Map[i] != 0 && i + 1 < Cap; i++) {
    Out[i] = Map[i];
  }
  Out[i] = 0;
  FreePool ((VOID *)Map);
}

/* ---- 卷 / P3 / 启动 ESP ---- */

STATIC EFI_STATUS RefreshVolumes (GLUE_CTX *G) {
  FS_OPS *P3 = NULL;
  UINTN  i;
  if (G->Vols != NULL) {
    VolFree (G->Vols, G->VolCount);
    G->Vols     = NULL;
    G->VolCount = 0;
  }
  G->Sess.P3Fs   = NULL;
  G->Sess.BootFs = NULL;
  G->Sess.P3FsName[0]   = 0;
  G->Sess.BootFsName[0] = 0;
  if (EFI_ERROR (VolEnumerate (&G->Vols, &G->VolCount))) {
    return EFI_NOT_FOUND;
  }
  /* P3 发现（任务书：VolFindP3；§5.2 同盘+分区3+.easyefi 标记+兜底） */
  if (!EFI_ERROR (VolFindP3 (G->BootDp, G->BootDpLen, &P3))) {
    G->Sess.P3Fs = P3;
    for (i = 0; i < G->VolCount; i++) {
      if (VolIsP3 (G->BootDp, G->BootDpLen, G->Vols[i].Dp, G->Vols[i].DpLen)) {
        MapName (G, G->Vols[i].Dp, G->Sess.P3FsName, 16);
        break;
      }
    }
  }
  /* 启动 ESP：分区 DP 逐字节匹配 */
  if (G->BootPartDp != NULL) {
    for (i = 0; i < G->VolCount; i++) {
      if (G->Vols[i].DpLen == G->BootPartDpLen &&
          CompareMem (G->Vols[i].Dp, G->BootPartDp, G->BootPartDpLen) == 0) {
        G->Sess.BootFs = G->Vols[i].Fs;
        MapName (G, G->Vols[i].Dp, G->Sess.BootFsName, 16);
        break;
      }
    }
  }
  return EFI_SUCCESS;
}

/* apps 路径：严格布局优先；不存在 → 兼容布局架构后缀名（§5.4） */
STATIC VOID ResolveAppPaths (GLUE_CTX *G) {
  FS_ENTRY   E;
  FS_OPS     *Fs = G->Sess.BootFs;
  CONST CHAR16 *Plain, *Suffix;
#if defined(MDE_CPU_X64)
  Suffix = L"X64";
#elif defined(MDE_CPU_AARCH64)
  Suffix = L"AA64";
#else
  Suffix = L"";
#endif
  Plain = L"EFI/Apps/FileManager.efi";
  if (Fs != NULL && !EFI_ERROR (Fs->Info (Fs, Plain, &E))) {
    StrCpyS (G->FmRel, 64, Plain);
  } else {
    UnicodeSPrint (G->FmRel, sizeof (G->FmRel), L"EFI/Apps/FileManager%s.efi", Suffix);
  }
  Plain = L"EFI/Apps/Editor.efi";
  if (Fs != NULL && !EFI_ERROR (Fs->Info (Fs, Plain, &E))) {
    StrCpyS (G->EdRel, 64, Plain);
  } else {
    UnicodeSPrint (G->EdRel, sizeof (G->EdRel), L"EFI/Apps/Editor%s.efi", Suffix);
  }
}

/* ---- launcher.json 加载 ---- */

STATIC VOID FreeConfig (GLUE_CTX *G) {
  if (G->Cfg != NULL) {
    ConfigFree (G->Cfg);
    G->Cfg = NULL;
  }
  if (G->Errs != NULL) {
    ConfigFreeErrors (G->Errs);
    G->Errs     = NULL;
    G->ErrCount = 0;
  }
  if (G->Warns != NULL) {
    ConfigFreeErrors (G->Warns);
    G->Warns     = NULL;
    G->WarnCount = 0;
  }
  G->Sess.Cfg      = NULL;
  G->Sess.Errs     = NULL;
  G->Sess.ErrCount = 0;
  G->Sess.Warns    = NULL;
  G->Sess.WarnCount = 0;
}

STATIC EFI_STATUS LoadConfig (GLUE_CTX *G) {
  FS_ENTRY         Info;
  FS_HANDLE        *H = NULL;
  UINT8            *Buf = NULL;
  UINTN            Size, Off, Got;
  EFI_STATUS       S;
  EDKII_JSON_ERROR JErr;
  EDKII_JSON_VALUE Root;
  JSON_NODE        *Dom = NULL;
  FS_OPS           *Fs;

  FreeConfig (G);
  G->Sess.Banner[0] = 0;
  Fs = G->Sess.P3Fs;
  if (Fs == NULL) {
    AsciiSPrint (G->Sess.Banner, sizeof (G->Sess.Banner),
                 "launcher.json: P3 data volume unavailable (DEGRADED)");
    return EFI_NOT_FOUND;
  }
  if (EFI_ERROR (Fs->Info (Fs, L"launcher.json", &Info))) {
    AsciiSPrint (G->Sess.Banner, sizeof (G->Sess.Banner),
                 "launcher.json: not found on P3 - builtin menu");
    return EFI_NOT_FOUND;
  }
  if (Info.Size == 0 || Info.Size > LAUNCHER_CONFIG_MAX_BYTES) {
    AsciiSPrint (G->Sess.Banner, sizeof (G->Sess.Banner),
                 "launcher.json: bad size %u bytes", (UINTN)Info.Size);
    return EFI_BAD_BUFFER_SIZE;
  }
  Size = (UINTN)Info.Size;
  Buf = AllocateZeroPool (Size + 1);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  S = Fs->Open (Fs, L"launcher.json", FS_MODE_READ, &H);
  if (EFI_ERROR (S)) {
    FreePool (Buf);
    AsciiSPrint (G->Sess.Banner, sizeof (G->Sess.Banner), "launcher.json: open failed");
    return S;
  }
  Off = 0;
  while (Off < Size) {
    Got = Size - Off;
    S = Fs->Read (Fs, H, Buf + Off, &Got);
    if (EFI_ERROR (S) || Got == 0) {
      break;
    }
    Off += Got;
  }
  Fs->Close (Fs, H);
  if (Off == 0) {
    FreePool (Buf);
    AsciiSPrint (G->Sess.Banner, sizeof (G->Sess.Banner), "launcher.json: empty read");
    return EFI_LOAD_ERROR;
  }
  ZeroMem (&JErr, sizeof (JErr));
  Root = JsonLoadBuffer ((CHAR8 *)Buf, Off, EDKII_JSON_REJECT_DUPLICATES, &JErr);
  FreePool (Buf);
  if (Root == NULL) {
    AsciiSPrint (G->Sess.Banner, sizeof (G->Sess.Banner),
                 "launcher.json: %a (line %d)", JErr.Text, JErr.Line);
    return EFI_LOAD_ERROR;
  }
  S = JsonDomFromEdk2 (Root, &Dom);
  JsonValueFree (Root);
  if (EFI_ERROR (S)) {
    AsciiSPrint (G->Sess.Banner, sizeof (G->Sess.Banner), "launcher.json: DOM adapter failed");
    return S;
  }
  S = ConfigValidate (Dom, &G->Cfg, &G->Errs, &G->ErrCount, &G->Warns, &G->WarnCount);
  JsonDomFree (Dom);
  if (EFI_ERROR (S)) {
    AsciiSPrint (G->Sess.Banner, sizeof (G->Sess.Banner), "launcher.json: validation failed");
    FreeConfig (G);
    return S;
  }
  G->Sess.Cfg       = G->Cfg;
  G->Sess.Errs      = G->Errs;
  G->Sess.ErrCount  = G->ErrCount;
  G->Sess.Warns     = G->Warns;
  G->Sess.WarnCount = G->WarnCount;
  if (G->Cfg->Count == 0) {
    /* 无有效条目 → 内置默认菜单（S03 交接约定：Count==0 → 内置菜单） */
    AsciiSPrint (G->Sess.Banner, sizeof (G->Sess.Banner),
                 "launcher.json: no valid items - builtin menu");
    FreeConfig (G);
    return EFI_SUCCESS;
  }
  if (G->ErrCount > 0) {
    AsciiSPrint (G->Sess.Banner, sizeof (G->Sess.Banner),
                 "config: %u item(s) skipped - press ? for details", (UINTN)G->ErrCount);
  }
  return EFI_SUCCESS;
}

/* ---- SecureBoot / SetupMode ---- */

STATIC VOID ReadSbStatus (GLUE_CTX *G) {
  UINT8      Sb = 0, Sm = 0;
  UINTN      Sz;
  EFI_STATUS S1, S2;
  Sz = 1;
  S1 = gRT->GetVariable (L"SecureBoot", &gEfiGlobalVariableGuid, NULL, &Sz, &Sb);
  Sz = 1;
  S2 = gRT->GetVariable (L"SetupMode", &gEfiGlobalVariableGuid, NULL, &Sz, &Sm);
  if (EFI_ERROR (S1)) {
    AsciiSPrint (G->SbStatus, sizeof (G->SbStatus), "SB:?");
  } else if (!EFI_ERROR (S2) && Sm == 1) {
    AsciiSPrint (G->SbStatus, sizeof (G->SbStatus), "SB:%a(setup)", Sb ? "ON" : "OFF");
  } else {
    AsciiSPrint (G->SbStatus, sizeof (G->SbStatus), "SB:%a", Sb ? "ON" : "OFF");
  }
}

/* ---- core 平台回调 ---- */

STATIC EFI_STATUS ReloadCb (VOID *Ctx) {
  GLUE_CTX *G = (GLUE_CTX *)Ctx;
  RefreshVolumes (G);               /* 重扫外部介质 + P3 */
  LoadConfig (G);                   /* 重载 launcher.json（失败 → 横幅+内置菜单） */
  return EFI_SUCCESS;               /* 降级经 Sess 表达，core 据此渲染 */
}

STATIC EFI_STATUS BootEntriesCb (VOID *Ctx, LAUNCHER_BOOT_ENTRY **Out, UINTN *Count) {
  UINTN               Sz = 0, N, i, Found = 0;
  UINT16              *Order = NULL;
  LAUNCHER_BOOT_ENTRY *L = NULL;
  EFI_STATUS          S;

  *Out   = NULL;
  *Count = 0;
  S = gRT->GetVariable (L"BootOrder", &gEfiGlobalVariableGuid, NULL, &Sz, NULL);
  if (EFI_ERROR (S) || Sz == 0 || Sz % 2 != 0) {
    return EFI_NOT_FOUND;
  }
  Order = AllocatePool (Sz);
  if (Order == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  S = gRT->GetVariable (L"BootOrder", &gEfiGlobalVariableGuid, NULL, &Sz, Order);
  if (EFI_ERROR (S)) {
    FreePool (Order);
    return S;
  }
  N = Sz / 2;
  L = AllocateZeroPool (N * sizeof (LAUNCHER_BOOT_ENTRY));
  if (L == NULL) {
    FreePool (Order);
    return EFI_OUT_OF_RESOURCES;
  }
  for (i = 0; i < N; i++) {
    CHAR16 Name[16];
    UINT8  *Opt = NULL;
    UINTN  OptSz = 0, MaxChars, j;
    EFI_LOAD_OPTION *Lo;
    UnicodeSPrint (Name, sizeof (Name), L"Boot%04x", Order[i]);
    S = gRT->GetVariable (Name, &gEfiGlobalVariableGuid, NULL, &OptSz, NULL);
    if (EFI_ERROR (S) || OptSz < 6) {
      continue;
    }
    Opt = AllocatePool (OptSz);
    if (Opt == NULL) {
      continue;
    }
    S = gRT->GetVariable (Name, &gEfiGlobalVariableGuid, NULL, &OptSz, Opt);
    if (EFI_ERROR (S)) {
      FreePool (Opt);
      continue;
    }
    Lo = (EFI_LOAD_OPTION *)Opt;    /* packed；AllocatePool 缓冲对齐 */
    if ((Lo->Attributes & LOAD_OPTION_ACTIVE) != 0) {
      CHAR16 *Desc = (CHAR16 *)(Opt + 6);   /* Attributes(4)+FilePathListLength(2) 之后 */
      L[Found].BootNumber = Order[i];
      MaxChars = (OptSz - 6) / 2;
      for (j = 0; j < MaxChars && j < 127 && Desc[j] != 0; j++) {
        L[Found].Description[j] = Desc[j];
      }
      L[Found].Description[j] = 0;
      Found++;
    }
    FreePool (Opt);
  }
  FreePool (Order);
  if (Found == 0) {
    FreePool (L);
    return EFI_NOT_FOUND;
  }
  *Out   = L;
  *Count = Found;
  return EFI_SUCCESS;
}

/* ---- 清理 / 入口 ---- */

STATIC VOID GlueCleanup (GLUE_CTX *G) {
  FreeConfig (G);
  if (G->Vols != NULL) {
    VolFree (G->Vols, G->VolCount);
  }
  if (G->BootDp != NULL) {
    FreePool (G->BootDp);
  }
  if (G->BootPartDp != NULL) {
    FreePool (G->BootPartDp);
  }
  FreePool (G);
}

EFI_STATUS EFIAPI UefiMain (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
  GLUE_CTX                *G;
  EFI_LOADED_IMAGE_PROTOCOL *Li;
  LAUNCHER_PLATFORM_OPS   Ops;
  LAUNCHER_INIT           Init;
  LAUNCHER_CORE           *C;
  UINTN                   i;

  G = AllocateZeroPool (sizeof (GLUE_CTX));
  if (G == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  G->ImageHandle = ImageHandle;
  gBS->SetWatchdogTimer (0, 0, 0, NULL);    /* 防固件看门狗在菜单停留时复位（§5.1） */
  gBS->LocateProtocol (&gEfiShellProtocolGuid, NULL, (VOID **)&G->Shell);  /* 可失败 */

#if defined(MDE_CPU_X64)
  G->Arch = ARCH_X64_BIT;
  StrCpyS (G->ShellRel, 64, L"EFI/BOOT/BOOTX64.EFI");
#elif defined(MDE_CPU_AARCH64)
  G->Arch = ARCH_AARCH64_BIT;
  StrCpyS (G->ShellRel, 64, L"EFI/BOOT/BOOTAA64.EFI");
#else
  G->Arch = ARCH_X64_BIT;
  StrCpyS (G->ShellRel, 64, L"EFI/BOOT/BOOTX64.EFI");
#endif

  /* 自身 LoadedImage 设备路径 → 启动盘身份 + 启动 ESP 分区 DP */
  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li)) &&
      Li->FilePath != NULL) {
    UINTN Sz = GetDevicePathSize (Li->FilePath);
    G->BootDp = AllocateCopyPool (Sz, Li->FilePath);
    if (G->BootDp != NULL) {
      G->BootDpLen  = Sz;
      G->BootPartDp = TrimToPartition (G->BootDp, Sz, &G->BootPartDpLen);
    }
  }

  ReadSbStatus (G);
  RefreshVolumes (G);
  ResolveAppPaths (G);
  LoadConfig (G);                   /* 失败 → Sess.Banner + Cfg=NULL（内置菜单） */

  if (EFI_ERROR (TuiInit ())) {
    Print (L"[Launcher] TuiInit failed - cannot render menu\n");
    if (G->Shell != NULL) {
      G->Shell->SetEnv (L"EASYEFI_RAN", L"yes", TRUE);
    }
    GlueCleanup (G);
    return EFI_SUCCESS;
  }

  ZeroMem (&Ops, sizeof (Ops));
  Ops.Ctx         = G;
  Ops.Reload      = ReloadCb;
  Ops.BootEntries = BootEntriesCb;
  ZeroMem (&Init, sizeof (Init));
  Init.Sess           = &G->Sess;
  Init.BootDp         = G->BootDp;
  Init.BootDpLen      = G->BootDpLen;
  Init.CurrentArch    = G->Arch;
  Init.SbStatus       = G->SbStatus;
  Init.FileManagerRel = G->FmRel;
  Init.EditorRel      = G->EdRel;
  Init.ShellRel       = G->ShellRel;
  Init.Ops            = &Ops;
  Init.SkipTimeout    = (G->Cfg != NULL) ? G->Cfg->SkipTimeout
                                         : PcdGet32 (PcdSkipTimeoutDefault);
  C = LauncherCoreNew (&Init);
  if (C == NULL) {
    TuiShutdown ();
    if (G->Shell != NULL) {
      G->Shell->SetEnv (L"EASYEFI_RAN", L"yes", TRUE);
    }
    GlueCleanup (G);
    return EFI_OUT_OF_RESOURCES;
  }

  LauncherCoreDraw (C);
  while (LauncherCoreExit (C) == LEXIT_NONE) {
    TUI_KEY K;
    if (LauncherCoreState (C) == LSTATE_COUNTDOWN) {
      BOOLEAN Got = FALSE;
      for (i = 0; i < 10; i++) {    /* 1s = 10 x 100ms：PollKey + Stall */
        if (!EFI_ERROR (TuiPollKey (&K))) {
          Got = TRUE;
          break;
        }
        gBS->Stall (100000);
      }
      if (Got) {
        LauncherCoreHandleKey (C, &K);
      } else {
        LauncherCoreTick (C);
      }
    } else {
      if (EFI_ERROR (TuiWaitKey (&K))) {
        break;                      /* 输入后端异常 → 正常退出路径 */
      }
      LauncherCoreHandleKey (C, &K);
    }
    LauncherCoreDraw (C);
  }

  LauncherCoreFree (C);
  TuiShutdown ();
  /* 任何正常退出先置 EASYEFI_RAN=yes（兼容布局架构探测依赖，§3.4） */
  if (G->Shell != NULL) {
    G->Shell->SetEnv (L"EASYEFI_RAN", L"yes", TRUE);
  }
  GlueCleanup (G);
  return EFI_SUCCESS;
}
