/** @file
  EfiMem UEFI 后端（MemoryAllocationLib）。宿主后端见 host/EfiMemHost.c。
**/
#include "Contract/EfiMem.h"
#include <Library/MemoryAllocationLib.h>

VOID *MemAlloc (UINTN Size) { return AllocateZeroPool (Size); }
VOID *MemRealloc (VOID *Ptr, UINTN NewSize) { return ReallocatePool (Ptr, 0, NewSize); }
VOID  MemFree (VOID *Ptr) { FreePool (Ptr); }
