/** @file
  ExecVolumeLib 双环境公共逻辑（宿主 + UEFI 均编译）：
  路径规范化 / 卷标记验证 / 卷集排序与 IsP3 标记 / P3 发现算法核心 / VolFree。
  不碰 gBS/gST；卷操作一律经 FsOps vtable，设备路径匹配经 VolumeModel 契约。
**/
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/FsOps.h"
#include "Contract/VolumeModel.h"
#include "ExecVolumeInternal.h"

STATIC INTN
ByteCompare (
  CONST UINT8 *A,
  CONST UINT8 *B,
  UINTN       Len
  )
{
  UINTN i;
  for (i = 0; i < Len; i++) {
    if (A[i] != B[i]) {
      return (INTN)A[i] - (INTN)B[i];
    }
  }
  return 0;
}

EFI_STATUS
FsNormalizePath (
  CONST CHAR16 *In,
  CHAR16       **Out
  )
{
  UINTN  len, i, j;
  CHAR16 *buf;

  if (In == NULL || Out == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  len = EfiStrLen (In);
  if (len == 0) {
    return EFI_INVALID_PARAMETER;
  }
  buf = (CHAR16 *)MemAlloc ((len + 1) * sizeof (CHAR16));
  if (buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  j = 0;
  for (i = 0; i < len; i++) {
    CHAR16 c = (In[i] == (CHAR16)'\\') ? (CHAR16)'/' : In[i];
    if (c == (CHAR16)'/') {
      if (j == 0 || buf[j - 1] == (CHAR16)'/') {
        continue;                   /* 去首 '/' 与折叠重复 '/' */
      }
      buf[j++] = c;
      continue;
    }
    buf[j++] = c;
  }
  if (j > 0 && buf[j - 1] == (CHAR16)'/') {
    j--;                            /* 去尾 '/' */
  }
  buf[j] = 0;

  /* 分量级检查：拒绝 ".."（越出卷根），去掉 "." */
  {
    UINTN s = 0, k, n = 0;
    for (k = 0; k <= j; k++) {
      if (k == j || buf[k] == (CHAR16)'/') {
        UINTN clen = k - s;
        if (clen == 2 && buf[s] == (CHAR16)'.' && buf[s + 1] == (CHAR16)'.') {
          MemFree (buf);
          return EFI_INVALID_PARAMETER;
        }
        if (clen == 1 && buf[s] == (CHAR16)'.') {
          /* 跳过 "." 分量 */
        } else {
          UINTN m;
          if (n != 0) {
            buf[n++] = (CHAR16)'/';
          }
          for (m = s; m < k; m++) {
            buf[n++] = buf[m];
          }
        }
        s = k + 1;
      }
    }
    buf[n] = 0;
  }
  *Out = buf;
  return EFI_SUCCESS;
}

BOOLEAN
FsHasMarker (
  FS_OPS *Fs
  )
{
  FS_ENTRY   ent;
  EFI_STATUS st;

  if (Fs == NULL) {
    return FALSE;
  }
  st = Fs->Info (Fs, U16 (".easyefi"), &ent);
  if (!EFI_ERROR (st)) {
    return TRUE;
  }
  st = Fs->Info (Fs, U16 ("launcher.json"), &ent);
  return (BOOLEAN)(!EFI_ERROR (st));
}

/* 排序比较：IsP3 优先；其次设备路径字节序（长度不等时短前缀在前）。 */
STATIC
INTN
VolCompare (
  CONST VOLUME_INFO *A,
  CONST VOLUME_INFO *B
  )
{
  UINTN n;
  INTN  r;

  if (A->IsP3 != B->IsP3) {
    return A->IsP3 ? -1 : 1;
  }
  n = (A->DpLen < B->DpLen) ? A->DpLen : B->DpLen;
  r = ByteCompare (A->Dp, B->Dp, n);
  if (r != 0) {
    return r;
  }
  if (A->DpLen != B->DpLen) {
    return (A->DpLen < B->DpLen) ? -1 : 1;
  }
  return 0;
}

EFI_STATUS
VolSortMark (
  VOLUME_INFO *Vols,
  UINTN       Count,
  CONST UINT8 *BootDp,
  UINTN       BootLen
  )
{
  UINTN i, j;

  if (Vols == NULL && Count != 0) {
    return EFI_INVALID_PARAMETER;
  }
  for (i = 0; i < Count; i++) {
    Vols[i].IsP3 = FALSE;
    if (BootDp != NULL && BootLen != 0 &&
        VolIsP3 (BootDp, BootLen, Vols[i].Dp, Vols[i].DpLen))
    {
      Vols[i].IsP3 = FsHasMarker (Vols[i].Fs);
    }
  }
  /* 插入排序（稳定；卷数量小） */
  for (i = 1; i < Count; i++) {
    VOLUME_INFO tmp = Vols[i];
    j = i;
    while (j > 0 && VolCompare (&Vols[j - 1], &tmp) > 0) {
      Vols[j] = Vols[j - 1];
      j--;
    }
    Vols[j] = tmp;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
VolFindP3InSet (
  VOLUME_INFO *Vols,
  UINTN       Count,
  CONST UINT8 *BootDp,
  UINTN       BootLen,
  UINTN       *Index
  )
{
  UINTN   i;
  BOOLEAN haveBoot = (BOOLEAN)(BootDp != NULL && BootLen != 0);
  UINTN   fallback = 0;
  BOOLEAN haveFallback = FALSE;

  if (Index == NULL || (Vols == NULL && Count != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  /* 步骤 2–3：同盘 + 分区3 候选 → 标记验证 */
  if (haveBoot) {
    for (i = 0; i < Count; i++) {
      if (VolIsP3 (BootDp, BootLen, Vols[i].Dp, Vols[i].DpLen) &&
          FsHasMarker (Vols[i].Fs))
      {
        *Index = i;
        return EFI_SUCCESS;
      }
    }
  }

  /* 步骤 4：兜底全盘扫标记；多命中优先启动盘，再按卷集顺序取首个 */
  for (i = 0; i < Count; i++) {
    if (!FsHasMarker (Vols[i].Fs)) {
      continue;
    }
    if (haveBoot && VolSameDisk (BootDp, BootLen, Vols[i].Dp, Vols[i].DpLen)) {
      *Index = i;
      return EFI_SUCCESS;
    }
    if (!haveFallback) {
      fallback     = i;
      haveFallback = TRUE;
    }
  }
  if (haveFallback) {
    *Index = fallback;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

VOID
VolFree (
  VOLUME_INFO *Vols,
  UINTN       Count
  )
{
  UINTN i;

  if (Vols == NULL) {
    return;
  }
  /* 契约裁决（main 332553a，S11 悬空 Fs 崩溃根因）：VolFree 只释放数组与 Dp，
   * 不销毁 Fs —— UEFI 侧 FS_OPS 按卷缓存、指针跨调用稳定、调用方借用；
   * 宿主侧 FsPosixBackend 实例由构造者自行 Destroy。 */
  for (i = 0; i < Count; i++) {
    MemFree (Vols[i].Dp);
    Vols[i].Dp = NULL;
    Vols[i].Fs = NULL;
  }
  MemFree (Vols);
}
