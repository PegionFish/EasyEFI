/** @file
  黄金 DOM 规范 dumps 生成器（一次性工具，非测试用例）。
  用法: cc <同 host.mk 的 include flags> -o gen_golden gen_golden.c \
        ../ConfigCore.c ../JsonDom.c ../JsonDomHost.c <jansson srcs> \
        ../../../EfiCompatLib/host/EfiMemHost.c
        ./gen_golden <launcher.example.json> > golden-dom.txt
  产物 golden-dom.txt 同时驱动：T03-15 宿主断言 + QEMU 冒烟 expect（EDK2 侧同构对比）。
**/
#include "ConfigCoreInternal.h"
#include <stdio.h>
#include <stdlib.h>

int main (int argc, char **argv) {
  FILE *F;
  long Sz;
  char *Buf;
  JSON_NODE *Dom = NULL;
  CHAR8 *Dump = NULL;
  if (argc != 2) { fprintf (stderr, "usage: gen_golden <json>\n"); return 2; }
  F = fopen (argv[1], "rb");
  if (!F) { perror ("open"); return 1; }
  fseek (F, 0, SEEK_END); Sz = ftell (F); fseek (F, 0, SEEK_SET);
  Buf = malloc ((size_t)Sz + 1);
  if (fread (Buf, 1, (size_t)Sz, F) != (size_t)Sz) { return 1; }
  Buf[Sz] = '\0';
  fclose (F);
  if (EFI_ERROR (JsonDomFromHost (Buf, &Dom))) { fprintf (stderr, "parse fail\n"); return 1; }
  if (EFI_ERROR (JsonDomDump (Dom, &Dump))) { fprintf (stderr, "dump fail\n"); return 1; }
  printf ("%s\n", Dump);
  MemFree (Dump);
  JsonDomFree (Dom);
  free (Buf);
  return 0;
}