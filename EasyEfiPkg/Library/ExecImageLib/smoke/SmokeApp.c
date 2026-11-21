/** @file
  S10 QEMU 冒烟测试应用（TEST-ONLY，fat 变体，挂 2 个 USB 镜像）。
  在 UEFI Shell 下经 startup.nsh 运行。场景：
   1. ExecEfi 测试目标（S10Target.efi：打印标记 + 改 ConOut 模式 + 返回 EFI_ABORTED）
      → 断言 ImageReturn 捕获 + ConOut 模式恢复 + LoadOptions 转发（目标侧打印）。
   2. ExecNsh 测试脚本（s10test.nsh：@echo -off + echo 标记 + exit）经嵌套 Shell
      → 断言执行成功；标记恰好 1 次由 run-smoke.sh 对 serial 日志计数断言（无递归）。
   3. ExecAutoCandidates（第二 USB 镜像含同相对路径）→ 断言候选不含启动盘任何分区
      （启动盘确实含该文件，证明排除真实生效）；0 命中 → EFI_NOT_FOUND。
  卷层/同盘判定用 fixture（S05/S09 契约替身，见 fixture/ 与 smoke/Smoke.dsc）。
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/Shell.h>
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/ExecOps.h"
#include "Contract/FsOps.h"
#include "Contract/VolumeModel.h"

#ifdef MDE_CPU_X64
#define ARCH_NAME  L"x64"
#define BOOT_FILE  L"BOOTX64.EFI"
#define AUTO_REL   L"EFI/BOOT/BOOTX64.EFI"
#else
#define ARCH_NAME  L"aa64"
#define BOOT_FILE  L"BOOTAA64.EFI"
#define AUTO_REL   L"EFI/BOOT/BOOTAA64.EFI"
#endif

/* 在启动 ESP 根创建测试脚本（raw SFS；脚本内容 ASCII + CRLF）。
   首行 @echo -off：嵌套 Shell 默认 echo=ON（UefiShellCommandLib mEchoState=TRUE），
   不关 echo 会把命令行本身也回显 → 标记计数翻倍。 */
STATIC EFI_STATUS CreateTestScript (EFI_HANDLE BootVolHandle) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs;
  EFI_FILE_PROTOCOL               *Root = NULL;
  EFI_FILE_PROTOCOL               *File = NULL;
  EFI_STATUS                      Status;
  CHAR8                           Script[] = "@echo -off\r\necho [S10-NSH] marker\r\nexit\r\n";
  UINTN                           Len = sizeof (Script) - 1;

  Status = gBS->HandleProtocol (BootVolHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Sfs);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Sfs->OpenVolume (Sfs, &Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Root->Open (Root, &File, L"\\s10test.nsh",
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  Root->Close (Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = File->Write (File, &Len, Script);
  if (!EFI_ERROR (Status)) {
    Status = File->Flush (File);
  }
  File->Close (File);
  return Status;
}

EFI_STATUS EFIAPI UefiMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS                Status;
  EFI_LOADED_IMAGE_PROTOCOL *Li = NULL;
  EFI_DEVICE_PATH_PROTOCOL  *BootDpProto = NULL;
  UINT8                     *BootDp = NULL;
  UINTN                     BootLen = 0;
  VOLUME_INFO               *Vols = NULL;
  UINTN                     VolN = 0, i;
  FS_OPS                    *BootFs = NULL;
  UINTN                     ModeBefore;
  EFI_STATUS                Ret;
  EXEC_CANDIDATE            *Cands = NULL;
  UINTN                     CandN = 0;
  UINTN                     Failures = 0;

  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  Print (L"[S10] smoke start arch=%s\n", ARCH_NAME);

  /* 启动盘身份 = 自身 LoadedImage 来源卷设备路径 */
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li);
  if (EFI_ERROR (Status)) {
    Print (L"[S10] FATAL LoadedImage %r\n", Status);
    return Status;
  }
  Status = gBS->HandleProtocol (Li->DeviceHandle, &gEfiDevicePathProtocolGuid, (VOID **)&BootDpProto);
  if (EFI_ERROR (Status)) {
    Print (L"[S10] FATAL BootDp %r\n", Status);
    return Status;
  }
  BootLen = GetDevicePathSize (BootDpProto);
  BootDp  = (UINT8 *)MemAlloc (BootLen);
  if (BootDp == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  CopyMem (BootDp, BootDpProto, BootLen);

  Status = VolEnumerate (&Vols, &VolN);
  if (EFI_ERROR (Status)) {
    Print (L"[S10] FATAL VolEnumerate %r\n", Status);
    MemFree (BootDp);
    return Status;
  }
  for (i = 0; i < VolN; i++) {
    if (Vols[i].DpLen == BootLen && CompareMem (Vols[i].Dp, BootDp, BootLen) == 0) {
      BootFs = Vols[i].Fs;
      break;
    }
  }
  if (BootFs == NULL) {
    Print (L"[S10] FATAL boot volume not in enumeration\n");
    Failures++;
    goto Cleanup;
  }
  Print (L"[S10] volumes=%u boot-found\n", VolN);

  /* ---- 场景 1：ExecEfi 目标 .efi（返回非零 + 改模式） ---- */
  ModeBefore = gST->ConOut->Mode->Mode;
  Ret = EFI_SUCCESS;
  Status = ExecEfi (BootFs, L"\\EFI\\Apps\\S10Target.efi", L"hello options", &Ret);
  Print (L"[S10] EFI status=0x%lx ret=0x%lx\n", (UINT64)Status, (UINT64)Ret);
  if (Status == EFI_ABORTED && Ret == EFI_ABORTED) {
    Print (L"[S10] EFI-CAPTURE PASS\n");
  } else {
    Print (L"[S10] EFI-CAPTURE FAIL\n");
    Failures++;
  }
  if (gST->ConOut->Mode->Mode == ModeBefore) {
    Print (L"[S10] MODE-RESTORE PASS\n");
  } else {
    Print (L"[S10] MODE-RESTORE FAIL\n");
    Failures++;
  }

  /* ---- 场景 2：ExecNsh 嵌套 Shell 执行 .nsh ---- */
  Status = CreateTestScript (Li->DeviceHandle);
  if (EFI_ERROR (Status)) {
    Print (L"[S10] NSH-CREATE FAIL %r\n", Status);
    Failures++;
  } else {
    EFI_SHELL_PROTOCOL       *Shell = NULL;
    CONST CHAR16             *MapName = NULL;
    EFI_DEVICE_PATH_PROTOCOL *DpPtr = BootDpProto;

    /* 运行时 fsN 经 Shell 协议解析（冒烟在 Shell 下运行，协议必可用） */
    Status = gBS->LocateProtocol (&gEfiShellProtocolGuid, NULL, (VOID **)&Shell);
    if (!EFI_ERROR (Status) && Shell != NULL) {
      MapName = Shell->GetMapFromDevicePath (&DpPtr);
    }
    if (MapName == NULL) {
      Print (L"[S10] NSH-MAP FAIL\n");
      Failures++;
    } else {
      CHAR16 ScriptFull[256];
      CHAR16 MapFirst[64];
      UINTN  mi;
      /* GetMapFromDevicePath 返回 ';' 分隔的全部别名（FS0:;HD0a0b:;BLK1:，
         已核实 ShellProtocol.c）；取首段 FS 映射名 */
      for (mi = 0; MapName[mi] != 0 && MapName[mi] != L';' && mi < 63; mi++) {
        MapFirst[mi] = MapName[mi];
      }
      MapFirst[mi] = 0;
      UnicodeSPrint (ScriptFull, sizeof (ScriptFull), L"%s\\s10test.nsh", MapFirst);
      FreePool ((VOID *)MapName);
      Print (L"[S10] nsh script=%s\n", ScriptFull);
      Status = ExecNsh (L"\\EFI\\BOOT\\" BOOT_FILE, ScriptFull, NULL);
      Print (L"[S10] NSH status=0x%lx\n", (UINT64)Status);
      if (!EFI_ERROR (Status)) {
        Print (L"[S10] NSH-OK PASS\n");
      } else {
        Print (L"[S10] NSH-OK FAIL\n");
        Failures++;
      }
    }
  }

  /* ---- 场景 3：auto: 候选排除启动盘 ---- */
  Status = ExecAutoCandidates (AUTO_REL, BootDp, BootLen, &Cands, &CandN);
  if (EFI_ERROR (Status)) {
    Print (L"[S10] AUTO FAIL status=%r\n", Status);
    Failures++;
  } else {
    BOOLEAN  AllExternal = TRUE;
    BOOLEAN  BootHasFile;
    FS_ENTRY Probe;

    for (i = 0; i < CandN; i++) {
      UINTN j;
      Print (L"[S10] cand[%u]=%s\n", i, Cands[i].Display);
      for (j = 0; j < VolN; j++) {
        if (Vols[j].Fs == Cands[i].Fs) {
          if (VolSameDisk (BootDp, BootLen, Vols[j].Dp, Vols[j].DpLen)) {
            AllExternal = FALSE;
          }
          break;
        }
      }
    }
    BootHasFile = (BOOLEAN)!EFI_ERROR (BootFs->Info (BootFs, AUTO_REL, &Probe));
    if (CandN >= 1 && AllExternal && BootHasFile) {
      Print (L"[S10] AUTO n=%u boot-excluded PASS\n", CandN);
    } else {
      Print (L"[S10] AUTO n=%u boot-excluded FAIL (allExt=%d bootHas=%d)\n",
             CandN, AllExternal, BootHasFile);
      Failures++;
    }
    ExecFreeCandidates (Cands, CandN);
    Cands = NULL;
  }
  Status = ExecAutoCandidates (L"NO/SUCH/FILE.EFI", BootDp, BootLen, &Cands, &CandN);
  if (Status == EFI_NOT_FOUND) {
    Print (L"[S10] AUTO-NONE PASS\n");
  } else {
    Print (L"[S10] AUTO-NONE FAIL status=%r\n", Status);
    Failures++;
  }

Cleanup:
  VolFree (Vols, VolN);
  MemFree (BootDp);
  if (Failures == 0) {
    Print (L"[S10] DONE\n");
    return EFI_SUCCESS;
  }
  Print (L"[S10] DONE failures=%u\n", Failures);
  return EFI_ABORTED;
}