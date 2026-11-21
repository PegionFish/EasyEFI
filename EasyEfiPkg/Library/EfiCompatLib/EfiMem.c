/** @file
  EfiMem UEFI 后端（尺寸跟踪头实现）。
  edk2 ReallocatePool 需要 OldSize，契约 MemRealloc(Ptr,NewSize) 无 OldSize，
  故用隐藏头记录尺寸（S01 上报 bug 后的修复方案，契约签名不变）。
  头 16 字节（UINT64×2）保持 8 字节对齐；MemFree(NULL) 安全（FreePool(NULL) 会 ASSERT）。
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
