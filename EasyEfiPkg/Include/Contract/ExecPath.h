/** @file
  路径文法（契约，S00 冻结；S04 实现）。design §6 路径文法的唯一解释器。
  data:/<rel>  auto:/<rel>  fs<N>:/<rel>  boot:<XXXX|menu>
  反斜杠规范化为正斜杠；拒绝 .. 穿越/空路径/未知前缀。
**/
#ifndef EXEC_PATH_H
#define EXEC_PATH_H

#include "EfiCompat.h"

#define ARCH_X64_BIT      0x01
#define ARCH_AARCH64_BIT  0x02

typedef enum {
  PATH_DATA = 0,   /* P3 根相对 */
  PATH_AUTO,       /* 外部介质枚举（排除启动盘） */
  PATH_FS,         /* 显式卷号（不推荐） */
  PATH_BOOT        /* NVRAM 启动项 */
} PATH_KIND;

typedef struct {
  PATH_KIND Kind;
  CHAR16    Rel[256];       /* DATA/AUTO/FS 的相对路径（正斜杠，无前导 /） */
  UINTN     FsIndex;        /* PATH_FS 的 N */
  UINT16    BootNumber;     /* PATH_BOOT 的 #### */
  BOOLEAN   BootMenu;       /* boot:menu */
  BOOLEAN   IsScript;       /* .nsh 后缀 */
} RESOLVED_PATH;

EFI_STATUS PathParse (CONST CHAR8 *Utf8Path, RESOLVED_PATH *Out);
BOOLEAN    PathArchMatch (UINT8 ItemArchMask, UINT8 CurrentArch);

#endif
