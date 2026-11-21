/** @file
  S03 QEMU 冒烟应用（UEFI glue，双架构）。
  读 FAT 上的 good.json / bad.json / syntax.json：
    JsonLoadBuffer(REJECT_DUPLICATES) → JsonDomFromEdk2 → ConfigValidate
  serial 打印校验结果标记 + 黄金 DOM 规范 dumps（T03-15 EDK2 侧同构对比）。
  纯 UEFI glue：可用裸 L""/gBS（纪律豁免）；核心逻辑全在 ConfigCoreLib。
  PathParse = S04 ExecPathLib 真实实现（main 已合并）。
**/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

#include "../ConfigCoreInternal.h"

STATIC EFI_STATUS
ReadRootFile (
  IN  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs,
  IN  CHAR16                           *Name,
  OUT UINT8                            **Buf,
  OUT UINTN                            *Len
  )
{
  EFI_FILE_PROTOCOL  *Root = NULL;
  EFI_FILE_PROTOCOL  *File = NULL;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  EFI_STATUS         St;

  *Buf = NULL;
  *Len = 0;
  St = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (St)) return St;
  St = Root->Open (Root, &File, Name, EFI_FILE_MODE_READ, 0);
  Root->Close (Root);
  if (EFI_ERROR (St)) return St;
  InfoSize = sizeof (EFI_FILE_INFO) + 256;
  Info = AllocateZeroPool (InfoSize);
  if (Info == NULL) { File->Close (File); return EFI_OUT_OF_RESOURCES; }
  St = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (St)) goto Done;
  *Len = (UINTN)Info->FileSize;
  *Buf = AllocatePool (*Len + 1);
  if (*Buf == NULL) { St = EFI_OUT_OF_RESOURCES; goto Done; }
  St = File->Read (File, Len, *Buf);
  if (!EFI_ERROR (St)) (*Buf)[*Len] = 0;
Done:
  FreePool (Info);
  File->Close (File);
  if (EFI_ERROR (St)) { FreePool (*Buf); *Buf = NULL; *Len = 0; }
  return St;
}

/* 在 FAT 上找 good.json 所在的卷（QEMU 仅 USB 启动盘一个 SimpleFileSystem）。 */
STATIC EFI_STATUS
FindSampleFs (
  OUT EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  **FsOut,
  OUT UINT8                            **GoodBuf,
  OUT UINTN                            *GoodLen
  )
{
  EFI_HANDLE  *Handles = NULL;
  UINTN       Count = 0, I;
  EFI_STATUS  St;

  St = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                NULL, &Count, &Handles);
  if (EFI_ERROR (St)) return St;
  for (I = 0; I < Count; I++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
    St = gBS->HandleProtocol (Handles[I], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (St)) continue;
    St = ReadRootFile (Fs, L"good.json", GoodBuf, GoodLen);
    if (!EFI_ERROR (St)) {
      *FsOut = Fs;
      FreePool (Handles);
      return EFI_SUCCESS;
    }
  }
  FreePool (Handles);
  return EFI_NOT_FOUND;
}

STATIC BOOLEAN
ValidateAndReport (
  IN CONST CHAR8  *Label,
  IN UINT8        *Buf,
  IN UINTN        Len,
  IN UINTN        ExpItems,
  IN UINTN        ExpErrs,
  IN UINTN        ExpWarns,
  IN BOOLEAN      DumpDom
  )
{
  EDKII_JSON_ERROR  JErr;
  EDKII_JSON_VALUE  JRoot;
  JSON_NODE         *Dom = NULL;
  CONFIG            *Cfg = NULL;
  CONFIG_ERROR      *Errs = NULL, *Warns = NULL;
  UINTN             ErrCount = 0, WarnCount = 0, I;
  EFI_STATUS        St;
  BOOLEAN           Pass;

  ZeroMem (&JErr, sizeof (JErr));
  JRoot = JsonLoadBuffer ((CONST CHAR8 *)Buf, Len, EDKII_JSON_REJECT_DUPLICATES, &JErr);
  if (JRoot == NULL) {
    AsciiPrint ("[S03] %a PARSEFAIL line=%d text=%a\n", Label, (INT32)JErr.Line, JErr.Text);
    return FALSE;
  }
  St = JsonDomFromEdk2 (JRoot, &Dom);
  JsonValueFree (JRoot);
  if (EFI_ERROR (St)) {
    AsciiPrint ("[S03] %a ADAPTFAIL st=%u\n", Label, (UINT32)St);
    return FALSE;
  }
  St = ConfigValidate (Dom, &Cfg, &Errs, &ErrCount, &Warns, &WarnCount);
  AsciiPrint ("[S03] %a items=%u errs=%u warns=%u st=%u\n",
              Label, (UINT32)(Cfg ? Cfg->Count : 0), (UINT32)ErrCount,
              (UINT32)WarnCount, (UINT32)St);
  for (I = 0; I < ErrCount; I++) {
    AsciiPrint ("[S03] %a err idx=%d field=%a code=%u msg=%a\n", Label,
                (INT32)(INTN)Errs[I].ItemIndex, Errs[I].Field,
                (UINT32)Errs[I].Code, Errs[I].Msg);
  }
  for (I = 0; I < WarnCount; I++) {
    AsciiPrint ("[S03] %a warn idx=%d field=%a code=%u msg=%a\n", Label,
                (INT32)(INTN)Warns[I].ItemIndex, Warns[I].Field,
                (UINT32)Warns[I].Code, Warns[I].Msg);
  }
  Pass = !EFI_ERROR (St) && Cfg != NULL &&
         Cfg->Count == ExpItems && ErrCount == ExpErrs && WarnCount == ExpWarns;
  if (DumpDom && Pass) {
    CHAR8 *Dump = NULL;
    if (!EFI_ERROR (JsonDomDump (Dom, &Dump))) {
      AsciiPrint ("[S03] DOM %a\n", Dump);
      MemFree (Dump);
    } else {
      Pass = FALSE;
    }
  }
  AsciiPrint ("[S03] %a %a\n", Label, Pass ? "PASS" : "FAIL");
  ConfigFree (Cfg);
  ConfigFreeErrors (Errs);
  ConfigFreeErrors (Warns);
  JsonDomFree (Dom);
  return Pass;
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs = NULL;
  UINT8       *GoodBuf = NULL, *BadBuf = NULL, *SynBuf = NULL;
  UINTN       GoodLen = 0, BadLen = 0, SynLen = 0;
  EFI_STATUS  St;
  BOOLEAN     AllPass = TRUE;
  BOOLEAN     DangerOk = FALSE;

  AsciiPrint ("[S03] ConfigSmoke start arch=%a\n",
#if defined (MDE_CPU_X64)
              "x64"
#elif defined (MDE_CPU_AARCH64)
              "aarch64"
#else
              "unknown"
#endif
              );

  St = FindSampleFs (&Fs, &GoodBuf, &GoodLen);
  if (EFI_ERROR (St)) {
    AsciiPrint ("[S03] SAMPLES NOT FOUND st=%u\n", (UINT32)St);
    AsciiPrint ("[S03] SMOKE FAIL\n");
    return EFI_SUCCESS;
  }
  St = ReadRootFile (Fs, L"bad.json", &BadBuf, &BadLen);
  if (EFI_ERROR (St)) AllPass = FALSE;
  St = ReadRootFile (Fs, L"syntax.json", &SynBuf, &SynLen);
  if (EFI_ERROR (St)) AllPass = FALSE;

  /* 好样例：黄金夹具 9 项 0 错 0 警 + DOM 规范 dumps（T03-15 EDK2 侧）。 */
  if (GoodBuf != NULL) {
    if (!ValidateAndReport ("GOOD", GoodBuf, GoodLen, 9, 0, 0, TRUE)) AllPass = FALSE;
  } else {
    AllPass = FALSE;
  }

  /* 坏样例：6 错 3 警 2 项（详见 TestApp/bad.json 注释与 expect 断言）。 */
  if (BadBuf != NULL) {
    if (!ValidateAndReport ("BAD", BadBuf, BadLen, 2, 6, 3, FALSE)) AllPass = FALSE;
    /* danger 隐含 confirm 复核：独立解析坏样例取 "ok" 项。 */
    {
      EDKII_JSON_ERROR  JErr;
      EDKII_JSON_VALUE  JRoot;
      JSON_NODE         *Dom = NULL;
      CONFIG            *Cfg = NULL;
      CONFIG_ERROR      *E = NULL, *W = NULL;
      UINTN             EN = 0, WN = 0, I;
      ZeroMem (&JErr, sizeof (JErr));
      JRoot = JsonLoadBuffer ((CONST CHAR8 *)BadBuf, BadLen, EDKII_JSON_REJECT_DUPLICATES, &JErr);
      if (JRoot != NULL &&
          !EFI_ERROR (JsonDomFromEdk2 (JRoot, &Dom)) &&
          !EFI_ERROR (ConfigValidate (Dom, &Cfg, &E, &EN, &W, &WN)) && Cfg != NULL) {
        for (I = 0; I < Cfg->Count; I++) {
          if (AsciiStrCmp (Cfg->Items[I].Id, "ok") == 0 &&
              Cfg->Items[I].Danger && Cfg->Items[I].Confirm) {
            DangerOk = TRUE;
          }
        }
      }
      if (JRoot != NULL) JsonValueFree (JRoot);
      ConfigFree (Cfg);
      ConfigFreeErrors (E);
      ConfigFreeErrors (W);
      JsonDomFree (Dom);
    }
    AsciiPrint ("[S03] DANGER-CONFIRM %a\n", DangerOk ? "PASS" : "FAIL");
    if (!DangerOk) AllPass = FALSE;
  } else {
    AllPass = FALSE;
  }

  /* 语法错误样例：JsonLoadBuffer 必须失败（解析失败路径不挂死）。 */
  if (SynBuf != NULL) {
    EDKII_JSON_ERROR  JErr;
    EDKII_JSON_VALUE  JRoot;
    ZeroMem (&JErr, sizeof (JErr));
    JRoot = JsonLoadBuffer ((CONST CHAR8 *)SynBuf, SynLen, EDKII_JSON_REJECT_DUPLICATES, &JErr);
    if (JRoot == NULL) {
      AsciiPrint ("[S03] SYNTAX PASS line=%d\n", (INT32)JErr.Line);
    } else {
      JsonValueFree (JRoot);
      AsciiPrint ("[S03] SYNTAX FAIL\n");
      AllPass = FALSE;
    }
  } else {
    AllPass = FALSE;
  }

  FreePool (GoodBuf);
  FreePool (BadBuf);
  FreePool (SynBuf);
  AsciiPrint ("[S03] SMOKE %a\n", AllPass ? "PASS" : "FAIL");
  return EFI_SUCCESS;
}