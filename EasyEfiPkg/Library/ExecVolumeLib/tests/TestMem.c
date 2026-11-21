/* S09 宿主测试内存垫片：EfiMem 契约宿主实现 + 分配配对计数（T09-12）。
 * 替代 EfiCompatLib/host/EfiMemHost.c（语义相同，另加 live 计数与双释放守卫），
 * 并同样提供 U16() 所需的 EfiAsciiToU16。仅测试二进制链接，不进 .inf。 */
#include "Contract/EfiMem.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
  UINTN Size;
  UINTN Magic;
} TM_HDR;

#define TM_MAGIC 0x53394D454DULL   /* "S9MEM" */

static UINTN g_live_blocks = 0;
static UINTN g_live_bytes  = 0;

UINTN TestMemLiveBlocks (void) { return g_live_blocks; }
UINTN TestMemLiveBytes  (void) { return g_live_bytes; }

VOID *MemAlloc (UINTN Size) {
  TM_HDR *h;
  UINTN   n = Size ? Size : 1;
  h = (TM_HDR *)calloc (1, sizeof (TM_HDR) + n);
  if (h == NULL) return NULL;
  h->Size = n;
  h->Magic = TM_MAGIC;
  g_live_blocks++;
  g_live_bytes += n;
  return (VOID *)(h + 1);
}

VOID *MemRealloc (VOID *Ptr, UINTN NewSize) {
  TM_HDR *h, *nh;
  UINTN   n = NewSize ? NewSize : 1;
  if (Ptr == NULL) return MemAlloc (NewSize);
  h = (TM_HDR *)Ptr - 1;
  if (h->Magic != TM_MAGIC) { fprintf (stderr, "TestMem: bad realloc %p\n", Ptr); abort (); }
  g_live_bytes -= h->Size;
  nh = (TM_HDR *)realloc (h, sizeof (TM_HDR) + n);
  if (nh == NULL) { g_live_bytes += h->Size; return NULL; }
  nh->Size = n;
  g_live_bytes += n;
  return (VOID *)(nh + 1);
}

VOID MemFree (VOID *Ptr) {
  TM_HDR *h;
  if (Ptr == NULL) return;
  h = (TM_HDR *)Ptr - 1;
  if (h->Magic != TM_MAGIC) { fprintf (stderr, "TestMem: bad/double free %p\n", Ptr); abort (); }
  g_live_blocks--;
  g_live_bytes -= h->Size;
  h->Magic = 0;
  free (h);
}

/* 与 EfiMemHost.c 相同：U16() 宿主运行时转换（旋转静态缓冲） */
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
