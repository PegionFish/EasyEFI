/** @file
  S14 驱动冒烟探针：枚举 SimpleFileSystem 卷，打印卷数/标签到 ConOut
  （OVMF/ArmVirtQemu 的 ConOut 含 serial），断言 NTFS 卷 TOOLKIT 可见。

  只证卷可见（OpenVolume 成功 + 卷标签可读）；读写持久化由 S09 集成验证。
  纯 UEFI 应用（非宿主纯逻辑），按 build-recipe 允许裸 L""。

  SPDX-License-Identifier: GPL-3.0-or-later
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileSystemVolumeLabelInfo.h>

#if defined (MDE_CPU_X64)
#define PROBE_ARCH  L"X64"
#elif defined (MDE_CPU_AARCH64)
#define PROBE_ARCH  L"AARCH64"
#else
#define PROBE_ARCH  L"UNKNOWN"
#endif

#define EXPECTED_LABEL  L"TOOLKIT"

EFI_STATUS
EFIAPI
NtfsProbeMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                    Status;
  EFI_HANDLE                    *Handles;
  UINTN                         Count;
  UINTN                         Index;
  UINTN                         Mounted;
  BOOLEAN                       Found;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL             *Root;
  EFI_FILE_SYSTEM_VOLUME_LABEL  *Label;
  UINTN                         LabelSize;

  Handles = NULL;
  Count   = 0;
  Mounted = 0;
  Found   = FALSE;

  Print (L"[S14] NtfsProbe arch=%s\n", PROBE_ARCH);

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    Print (L"[S14] LocateHandleBuffer failed: %r\n", Status);
    Print (L"[S14] NTFS TOOLKIT FAIL\n");
    return Status;
  }

  Print (L"[S14] fs_count=%Lu\n", (UINT64)Count);

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->OpenProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Fs,
                    ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Root   = NULL;
    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) {
      Print (L"[S14] vol %Lu: OpenVolume failed: %r\n", (UINT64)Index, Status);
      continue;
    }

    Mounted++;

    // 两阶段 GetInfo：起始给足缓冲，EFI_BUFFER_TOO_SMALL 时按驱动回传尺寸重试。
    // 注意（S14 实测）：ntfs-3g 驱动不支持 NULL 缓冲问尺寸（返回 TOO_SMALL 但不回写
    // 尺寸），且缓冲"过大"时也返回 TOO_SMALL + 精确尺寸——标准两阶段重试两者兼容。
    LabelSize = 256;
    Label     = AllocateZeroPool (LabelSize);
    if (Label == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
    } else {
      Status = Root->GetInfo (Root, &gEfiFileSystemVolumeLabelInfoIdGuid, &LabelSize, Label);
      if ((Status == EFI_BUFFER_TOO_SMALL) && (LabelSize > 0)) {
        FreePool (Label);
        Label = AllocateZeroPool (LabelSize);
        if (Label != NULL) {
          Status = Root->GetInfo (Root, &gEfiFileSystemVolumeLabelInfoIdGuid, &LabelSize, Label);
        } else {
          Status = EFI_OUT_OF_RESOURCES;
        }
      }
    }

    if (!EFI_ERROR (Status) && (Label != NULL)) {
      Print (L"[S14] vol %Lu label=%s\n", (UINT64)Index, Label->VolumeLabel);
      if (StrCmp (Label->VolumeLabel, EXPECTED_LABEL) == 0) {
        Found = TRUE;
      }
    } else {
      Print (L"[S14] vol %Lu label=<unreadable: %r>\n", (UINT64)Index, Status);
    }

    if (Label != NULL) {
      FreePool (Label);
    }

    Root->Close (Root);
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }

  Print (L"[S14] mounted=%Lu\n", (UINT64)Mounted);
  Print (L"[S14] NTFS TOOLKIT %s\n", Found ? L"PASS" : L"FAIL");
  return Found ? EFI_SUCCESS : EFI_NOT_FOUND;
}