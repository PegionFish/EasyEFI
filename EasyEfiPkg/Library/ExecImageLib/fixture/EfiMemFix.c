/** @file
  TEST-ONLY fixture —— EfiCompatLib（EfiMem UEFI 后端）冒烟替身。
  背景：worktree 基线（S00 a06e66a）的 EfiMem.c 有 ReallocatePool 参数序 bug
  （S01 上报，leader 已在 main 84c6f4c 修复为尺寸跟踪头）。EfiCompatLib 不在
  S10 写权限内，故冒烟 DSC 解析到本替身；内容与 main 修复版语义一致。
  集成后（main 已含修复）把 Smoke.dsc 的 EfiCompatLib 解析换回真实 .inf 即可。
**/
#include "Contract/EfiMem.h"
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>

typedef struct {
  UINT64 Size;
  UINT64 Canary;
} MEM_HDR;

#define MEM_CANARY 0x456173794546494DULL  /* "EasyEFIM" */

VOID *MemAlloc (UINTN Size) {
  MEM_HDR *H = (MEM_HDR *)AllocateZeroPool (Size + sizeof (MEM_HDR));
  if (H == NULL) {
    return NULL;
  }
  H->Size   = Size;
  H->Canary = MEM_CANARY;
  return (VOID *)(H + 1);
}

VOID MemFree (VOID *Ptr) {
  if (Ptr == NULL) {
    return;
  }
  MEM_HDR *H = ((MEM_HDR *)Ptr) - 1;
  ASSERT (H->Canary == MEM_CANARY);
  FreePool (H);
}

VOID *MemRealloc (VOID *Ptr, UINTN NewSize) {
  if (Ptr == NULL) {
    return MemAlloc (NewSize);
  }
  MEM_HDR *H   = ((MEM_HDR *)Ptr) - 1;
  VOID    *New = MemAlloc (NewSize);
  if (New == NULL) {
    return NULL;
  }
  CopyMem (New, Ptr, (H->Size < NewSize) ? (UINTN)H->Size : NewSize);
  MemFree (Ptr);
  return New;
}