/** @file
  S06 临时 shim — 修正 S00 EfiCompatLib/EfiMem.c 的 ReallocatePool 参数序 bug。
  仅因 S00 EfiMem.c 为 leader 独占且阻塞链接而设；上游修复后本目录整体删除。

  与宿主 EfiMemHost.c 语义对齐：MemAlloc 零初始化；MemRealloc 保留内容；MemFree 配对。
  UEFI ReallocatePool 需要 OldSize，而 EfiMem 契约不传旧尺寸 → 用尺寸头部记录，
  保证 realloc 内容保留（S00 原实现即使修正参数序，OldSize=0 也会丢内容）。
**/
#include "Contract/EfiMem.h"
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>

#define MEM_HDR_SIZE  sizeof (UINTN)

VOID *MemAlloc (UINTN Size) {
  UINTN *Hdr;
  if (Size == 0) {
    Size = 1;
  }
  Hdr = AllocateZeroPool (MEM_HDR_SIZE + Size);
  if (Hdr == NULL) {
    return NULL;
  }
  *Hdr = Size;
  return (VOID *)(Hdr + 1);
}

VOID *MemRealloc (VOID *Ptr, UINTN NewSize) {
  UINTN *Hdr;
  UINTN  OldSize;
  VOID  *New;
  if (NewSize == 0) {
    NewSize = 1;
  }
  if (Ptr == NULL) {
    return MemAlloc (NewSize);
  }
  Hdr     = ((UINTN *)Ptr) - 1;
  OldSize = *Hdr;
  New = MemAlloc (NewSize);
  if (New == NULL) {
    return NULL;
  }
  CopyMem (New, Ptr, OldSize < NewSize ? OldSize : NewSize);
  MemFree (Ptr);
  return New;
}

VOID MemFree (VOID *Ptr) {
  if (Ptr != NULL) {
    FreePool (((UINTN *)Ptr) - 1);
  }
}
