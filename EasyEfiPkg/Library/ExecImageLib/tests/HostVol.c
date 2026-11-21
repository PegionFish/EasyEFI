/* S10 宿主测试：合成卷集 fixture（TEST-ONLY）。按 S09 契约实现 VolEnumerate/VolFree：
   - VOLUME_INFO 数组与 Dp 为 callee 分配（MemAlloc），VolFree 释放；
   - Fs 实例指针跨调用稳定（契约解释：按卷缓存），VolFree 不销毁 Fs（借用方持有）。
   集成时由真实 ExecVolumeLib 取代。 */
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/FsOps.h"
#include "HostVol.h"

#define HOST_VOL_MAX 16

typedef struct {
  CHAR16 Label[64];
  UINT8  Dp[1024];
  UINTN  DpLen;
  FS_OPS *Fs;
  BOOLEAN Used;
} HOST_VOL;

static HOST_VOL g_vols[HOST_VOL_MAX];

void HostVolReset (VOID) {
  UINTN i;
  UINT8 *p = (UINT8 *)g_vols;
  for (i = 0; i < sizeof (g_vols); i++) p[i] = 0;
}

void HostVolAdd (CONST CHAR16 *Label, CONST UINT8 *Dp, UINTN DpLen, FS_OPS *Fs) {
  UINTN i, j;
  for (i = 0; i < HOST_VOL_MAX; i++) {
    if (!g_vols[i].Used) break;
  }
  if (i == HOST_VOL_MAX) return;
  g_vols[i].Used = TRUE;
  for (j = 0; Label[j] != 0 && j < 63; j++) g_vols[i].Label[j] = Label[j];
  g_vols[i].Label[j] = 0;
  if (DpLen > sizeof (g_vols[i].Dp)) DpLen = sizeof (g_vols[i].Dp);
  for (j = 0; j < DpLen; j++) g_vols[i].Dp[j] = Dp[j];
  g_vols[i].DpLen = DpLen;
  g_vols[i].Fs = Fs;
}

EFI_STATUS VolEnumerate (VOLUME_INFO **Vols, UINTN *Count) {
  UINTN i, n = 0, k = 0, j;
  VOLUME_INFO *Arr;
  for (i = 0; i < HOST_VOL_MAX; i++) if (g_vols[i].Used) n++;
  if (n == 0) return EFI_NOT_FOUND;
  Arr = MemAlloc (n * sizeof (VOLUME_INFO));
  if (Arr == NULL) return EFI_OUT_OF_RESOURCES;
  for (i = 0; i < HOST_VOL_MAX; i++) {
    HOST_VOL *V = &g_vols[i];
    if (!V->Used) continue;
    for (j = 0; V->Label[j] != 0 && j < 63; j++) Arr[k].Label[j] = V->Label[j];
    Arr[k].Label[j] = 0;
    Arr[k].FsName[0] = L'M'; Arr[k].FsName[1] = L'O'; Arr[k].FsName[2] = L'C';
    Arr[k].FsName[3] = L'K'; Arr[k].FsName[4] = 0;
    Arr[k].Capacity = 0;
    Arr[k].Free = 0;
    Arr[k].IsP3 = FALSE;
    Arr[k].Dp = MemAlloc (V->DpLen);
    if (Arr[k].Dp == NULL) {
      while (k > 0) { k--; MemFree (Arr[k].Dp); }
      MemFree (Arr);
      return EFI_OUT_OF_RESOURCES;
    }
    for (j = 0; j < V->DpLen; j++) Arr[k].Dp[j] = V->Dp[j];
    Arr[k].DpLen = V->DpLen;
    Arr[k].Fs = V->Fs;
    k++;
  }
  *Vols = Arr;
  *Count = n;
  return EFI_SUCCESS;
}

VOID VolFree (VOLUME_INFO *Vols, UINTN Count) {
  UINTN i;
  if (Vols == NULL) return;
  for (i = 0; i < Count; i++) MemFree (Vols[i].Dp);   /* Fs 借用，不销毁 */
  MemFree (Vols);
}