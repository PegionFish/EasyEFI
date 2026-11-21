/** @file
  EditorGlue — S13 Editor UEFI 入口与后端装配（纯 UEFI 胶水，可用裸 L""/gBS）。

  职责（core/glue 纪律）：
  - LoadedImage->LoadOptions = 待打开文件全路径（FM F4 传入；可空 = 未命名新文件）。
  - fsN: 前缀 → Shell GetDevicePathFromMap → 与 VolEnumerate 卷设备路径逐字节匹配 → 该卷 FS_OPS；
    无 Shell/无匹配 → 各卷 Info 探测兜底；无路径 → VolFindP3（P3 优先）或首卷。
  - PCD 阈值传 core：PcdEditorMaxFileSize / PcdUndoDepth（配置优先，不硬编码）。
  - VolFree 会 Destroy 卷 FS_OPS → 卷集必须在整个编辑会话期间持有，退出时释放。
    VolFindP3 转移 P3 Fs 所有权 → 单独 Destroy。
  - 正常退出置 EASYEFI_RAN=yes（Shell 协议可用时，与 S00 stub 行为一致）。
  - 退出码：0 正常 → EFI_SUCCESS；非 0（致命打开错误等）→ EFI_LOAD_ERROR。
  - serial 标记（冒烟断言用）：[S13] Editor start / [S13] Editor exit=<n>。
**/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/PcdLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/Shell.h>
#include <Protocol/LoadedImage.h>

#include "Contract/EfiCompat.h"
#include "Contract/FsOps.h"
#include "EditorCore.h"

/* 去掉 LoadOptions 首尾空白与引号 → 静态缓冲路径（UCS-2）。 */
STATIC CONST CHAR16 *
TrimPath (
  CONST CHAR16 *In,
  CHAR16       *Buf,
  UINTN         Cap
  )
{
  UINTN n, s = 0, e, i;
  if (In == NULL) {
    return NULL;
  }
  n = StrLen (In);
  while (s < n && (In[s] == L' ' || In[s] == L'"')) {
    s++;
  }
  e = n;
  while (e > s && (In[e - 1] == L' ' || In[e - 1] == L'"')) {
    e--;
  }
  if (e == s) {
    return NULL;
  }
  n = e - s;
  if (n >= Cap) {
    n = Cap - 1;
  }
  for (i = 0; i < n; i++) {
    Buf[i] = In[s + i];
  }
  Buf[n] = 0;
  return Buf;
}

/* 解析 fsN: 前缀（不区分大小写）。命中 → MapName 填 "fsN"，*Rel 指向前缀后。 */
STATIC BOOLEAN
ParseFsPrefix (
  CONST CHAR16  *Path,
  CHAR16        *MapName,
  CONST CHAR16 **Rel
  )
{
  CHAR16 c0, c1;
  if (Path == NULL || StrLen (Path) < 4) {
    return FALSE;
  }
  c0 = Path[0];
  c1 = Path[1];
  if ((c0 == L'f' || c0 == L'F') && (c1 == L's' || c1 == L'S') &&
      Path[2] >= L'0' && Path[2] <= L'9' && Path[3] == L':') {
    MapName[0] = L'f';
    MapName[1] = L's';
    MapName[2] = Path[2];
    MapName[3] = 0;
    *Rel = Path + 4;
    while (**Rel == L'/' || **Rel == L'\\') {
      (*Rel)++;
    }
    return TRUE;
  }
  return FALSE;
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                    St;
  EFI_LOADED_IMAGE_PROTOCOL    *Li = NULL;
  EFI_SHELL_PROTOCOL           *Shell = NULL;
  VOLUME_INFO                  *Vols = NULL;
  UINTN                         VolCount = 0, i;
  FS_OPS                       *Fs = NULL;
  FS_OPS                       *P3Fs = NULL;
  BOOLEAN                       P3Owned = FALSE;
  CONST CHAR16                 *Path, *Rel = NULL;
  CHAR16                        PathBuf[256];
  CHAR16                        MapName[8];
  EDITOR_APP                   *E;
  UINTN                         Code;
  FS_ENTRY                      Probe;

  Print (L"[S13] Editor start\n");

  /* 1. 命令行参数 = 待打开文件全路径 */
  Path = NULL;
  St = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li);
  if (!EFI_ERROR (St) && Li != NULL && Li->LoadOptions != NULL) {
    Path = TrimPath ((CONST CHAR16 *)Li->LoadOptions, PathBuf, sizeof (PathBuf) / sizeof (CHAR16));
  }
  Rel = Path;

  gBS->LocateProtocol (&gEfiShellProtocolGuid, NULL, (VOID **)&Shell);

  /* 2. 卷集（整个会话持有；VolFree 会 Destroy 各 Fs） */
  St = VolEnumerate (&Vols, &VolCount);
  if (EFI_ERROR (St) || VolCount == 0) {
    Print (L"[S13] Editor exit=1 (no volumes)\n");
    return EFI_LOAD_ERROR;
  }

  /* 3. 路径 → 卷 FS_OPS（GetDevicePathFromMap 返回 Shell 内部指针，不可释放） */
  if (Path != NULL && ParseFsPrefix (Path, MapName, &Rel) && Shell != NULL) {
    CONST EFI_DEVICE_PATH_PROTOCOL *MapDp = Shell->GetDevicePathFromMap (MapName);
    if (MapDp != NULL) {
      UINTN MapSize = GetDevicePathSize (MapDp);
      for (i = 0; i < VolCount; i++) {
        if (Vols[i].Dp != NULL && Vols[i].DpLen == MapSize &&
            CompareMem (Vols[i].Dp, MapDp, MapSize) == 0) {
          Fs = Vols[i].Fs;
          break;
        }
      }
    }
  }
  if (Fs == NULL && Path != NULL) {
    /* 兜底：各卷 Info 探测（前缀已剥离） */
    for (i = 0; i < VolCount; i++) {
      if (!EFI_ERROR (Vols[i].Fs->Info (Vols[i].Fs, Rel, &Probe)) && !Probe.IsDir) {
        Fs = Vols[i].Fs;
        break;
      }
    }
  }
  if (Fs == NULL) {
    /* 未命名/未命中：P3 优先（所有权转移），否则首卷 */
    if (!EFI_ERROR (VolFindP3 (NULL, 0, &P3Fs)) && P3Fs != NULL) {
      Fs = P3Fs;
      P3Owned = TRUE;
    } else {
      Fs = Vols[0].Fs;
    }
  }

  /* 4. core 运行（PCD 阈值传入） */
  E = EdNew (Fs, FixedPcdGet32 (PcdEditorMaxFileSize), FixedPcdGet32 (PcdUndoDepth));
  if (E == NULL) {
    if (P3Owned) {
      P3Fs->Destroy (P3Fs);
    }
    VolFree (Vols, VolCount);
    Print (L"[S13] Editor exit=1 (oom)\n");
    return EFI_LOAD_ERROR;
  }
  if (Path != NULL) {
    EdOpen (E, Rel);   /* 失败 → core 错误页（致命），EdRun 任意键退出 */
  }
  EdRun (E);
  Code = EdExitCode (E);
  EdFree (E);

  /* 5. 清理 + 退出约定 */
  if (P3Owned) {
    P3Fs->Destroy (P3Fs);
  }
  VolFree (Vols, VolCount);
  if (Shell != NULL) {
    Shell->SetEnv (L"EASYEFI_RAN", L"yes", TRUE);
  }
  Print (L"[S13] Editor exit=%u\n", (UINT32)Code);
  return (Code == 0) ? EFI_SUCCESS : EFI_LOAD_ERROR;
}
