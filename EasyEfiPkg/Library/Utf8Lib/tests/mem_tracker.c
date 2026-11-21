/* S01 T01-10 配对计数后端（仅 host.mk test-leaks 目标链接，替代 EfiMemHost.c）。
 * 语义与 EfiMemHost.c 完全一致：MemAlloc 零初始化、MemRealloc=realloc（不零化扩展区）、
 * MemFree=free（NULL 安全）。附加：存活分配计数 + 前后 canary + 退出时零残留断言。
 * 本库测试不用 U16/CHAR16，故无需提供 EfiAsciiToU16。
 */
#include "Contract/EfiMem.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

typedef struct { UINTN Size; UINT64 Canary; } TRACK_HDR;  /* 16 字节，保持最大对齐 */
#define TRACK_MAGIC 0xE4F1A5C3DEADBEEFULL
#define TRACK_TAIL  0xA5
#define TAIL_BYTES  8

static long g_live = 0;

long MemTrackerLive (void) { return g_live; }

static void die (const char *msg, void *user) {
  fprintf (stderr, "mem_tracker: %s (user ptr %p)\n", msg, user);
  abort ();
}

static void check (TRACK_HDR *h, void *user) {
  UINTN i;
  CONST UINT8 *tail;
  if (h->Canary != TRACK_MAGIC) {
    die ("header canary corrupted (underflow/UAF)", user);
  }
  tail = (CONST UINT8 *)(h + 1) + h->Size;
  for (i = 0; i < TAIL_BYTES; i++) {
    if (tail[i] != TRACK_TAIL) {
      die ("tail canary corrupted (overflow)", user);
    }
  }
}

VOID *MemAlloc (UINTN Size) {
  UINTN     n = Size ? Size : 1;
  TRACK_HDR *h = malloc (sizeof (TRACK_HDR) + n + TAIL_BYTES);
  if (h == NULL) {
    return NULL;
  }
  h->Size   = n;
  h->Canary = TRACK_MAGIC;
  memset (h + 1, 0, n);
  memset ((UINT8 *)(h + 1) + n, TRACK_TAIL, TAIL_BYTES);
  g_live++;
  return h + 1;
}

VOID *MemRealloc (VOID *Ptr, UINTN NewSize) {
  TRACK_HDR *h, *nh;
  UINTN     n = NewSize ? NewSize : 1;
  if (Ptr == NULL) {
    return MemAlloc (NewSize);
  }
  h = (TRACK_HDR *)Ptr - 1;
  check (h, Ptr);
  nh = realloc (h, sizeof (TRACK_HDR) + n + TAIL_BYTES);
  if (nh == NULL) {
    return NULL;
  }
  nh->Size = n;                               /* 扩展区不零化，与 realloc 语义一致 */
  memset ((UINT8 *)(nh + 1) + n, TRACK_TAIL, TAIL_BYTES);
  return nh + 1;
}

VOID MemFree (VOID *Ptr) {
  TRACK_HDR *h;
  if (Ptr == NULL) {
    return;
  }
  h = (TRACK_HDR *)Ptr - 1;
  check (h, Ptr);
  g_live--;
  free (h);
}

/* 退出兜底：任何残留分配都视为泄漏失败（T01-10 之外还有进程级断言）。 */
__attribute__((destructor)) static void tracker_fini (void) {
  if (g_live != 0) {
    fprintf (stderr, "mem_tracker: LEAK — %ld block(s) still live at exit\n", g_live);
    _exit (1);
  }
}
