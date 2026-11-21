/* EfiMem 宿主后端 + U16 字面量转换（tests/host 编译用，不进 .inf）。
   MemRealloc 语义与 UEFI 后端一致：calloc+copy+free（扩展区零初始化）。 */
#include "Contract/EfiMem.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  UINT64 Size;
  UINT64 Canary;
} MEM_HDR;

#define MEM_CANARY 0x456173794546494DULL

VOID *MemAlloc (UINTN Size) {
  MEM_HDR *H = (MEM_HDR *)calloc (1, Size + sizeof (MEM_HDR));
  if (H == NULL) return NULL;
  H->Size = Size;
  H->Canary = MEM_CANARY;
  return (VOID *)(H + 1);
}

VOID MemFree (VOID *Ptr) {
  if (Ptr == NULL) return;
  MEM_HDR *H = ((MEM_HDR *)Ptr) - 1;
  if (H->Canary != MEM_CANARY) abort ();
  free (H);
}

VOID *MemRealloc (VOID *Ptr, UINTN NewSize) {
  if (Ptr == NULL) return MemAlloc (NewSize);
  MEM_HDR *H = ((MEM_HDR *)Ptr) - 1;
  if (H->Canary != MEM_CANARY) abort ();
  VOID *New = MemAlloc (NewSize);
  if (New == NULL) return NULL;
  memcpy (New, Ptr, H->Size < NewSize ? (size_t)H->Size : (size_t)NewSize);
  MemFree (Ptr);
  return New;
}

#define U16_BUFS 8
#define U16_CAP 512
static CHAR16 g_u16[U16_BUFS][U16_CAP];
static int g_u16_next = 0;

CONST CHAR16 *EfiAsciiToU16 (CONST CHAR8 *S) {
  CHAR16 *buf = g_u16[g_u16_next];
  UINTN i;
  g_u16_next = (g_u16_next + 1) % U16_BUFS;
  for (i = 0; S[i] && i + 1 < U16_CAP; i++) buf[i] = (CHAR16)(UINT8)S[i];
  buf[i] = 0;
  return buf;
}
