/* EfiMem 宿主后端 + U16 字面量转换（tests/host 编译用，不进 .inf） */
#include "Contract/EfiMem.h"
#include <stdlib.h>
#include <string.h>

VOID *MemAlloc (UINTN Size) { return calloc (1, Size ? Size : 1); }
VOID *MemRealloc (VOID *Ptr, UINTN NewSize) { return realloc (Ptr, NewSize ? NewSize : 1); }
VOID  MemFree (VOID *Ptr) { free (Ptr); }

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
