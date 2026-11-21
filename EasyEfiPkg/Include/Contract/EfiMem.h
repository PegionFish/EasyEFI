/** @file
  内存抽象（契约，S00 冻结）。callee 分配、caller MemFree。
  UEFI 后端 = MemoryAllocationLib；宿主后端 = malloc 系（tests/host 编译）。
**/
#ifndef EFI_MEM_H
#define EFI_MEM_H

#include "EfiCompat.h"

VOID *MemAlloc (UINTN Size);            /* 零初始化 */
VOID *MemRealloc (VOID *Ptr, UINTN NewSize);
VOID  MemFree (VOID *Ptr);

#endif
