/* T03-17 内存配对计数分配器（替换 EfiMemHost.c 编译进 run-tests-leak）。
   宿主测试框架的 U16 转换一并在此提供（EfiMemHost.c 不参与该变体链接）。 */
#include "Contract/EfiMem.h"
#include <stdlib.h>
#include <string.h>

INTN g_mem_balance = 0;   /* 分配次数 - 释放次数；测试末尾断言 == 0 */

VOID *MemAlloc (UINTN Size) {
  void *P = calloc (1, Size ? Size : 1);
  if (P) g_mem_balance++;
  return P;
}

VOID *MemRealloc (VOID *Ptr, UINTN NewSize) {
  void *P = realloc (Ptr, NewSize ? NewSize : 1);
  if (P && Ptr == NULL) g_mem_balance++;
  return P;
}

VOID MemFree (VOID *Ptr) {
  if (Ptr) {
    g_mem_balance--;
    free (Ptr);
  }
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