/** @file
  launcher.json schema 校验核心（契约，S00 冻结；S03 实现）。
  规则权威 = design.md §6。非法项跳过但保留错误清单；未知字段警告不报错。
**/
#ifndef CONFIG_SCHEMA_H
#define CONFIG_SCHEMA_H

#include "EfiCompat.h"
#include "JsonDom.h"
#include "ExecPath.h"   /* ARCH_*_BIT */

typedef enum {
  ACTION_EXEC = 0, ACTION_SHELL, ACTION_BOOT, ACTION_RESET
} CONFIG_ACTION;

typedef struct {
  CHAR8   Id[64];
  CHAR8   Name[41];        /* ASCII ≤40 */
  CHAR8   Desc[77];        /* ASCII ≤76，可空 */
  UINT8   ArchMask;        /* ARCH_X64_BIT | ARCH_AARCH64_BIT */
  UINT8   Action;          /* CONFIG_ACTION */
  CHAR8   Path[256];       /* 路径文法见 ExecPath.h */
  CHAR8   Options[256];    /* LoadOptions，可空 */
  BOOLEAN Danger;          /* 隐含 Confirm */
  BOOLEAN Confirm;
} CONFIG_ITEM;

typedef struct {
  CHAR8       Title[41];
  CHAR8       Version[32];
  UINTN       SkipTimeout;   /* 0–30，越界钳制+警告 */
  CONFIG_ITEM *Items;
  UINTN       Count;
} CONFIG;

typedef enum {
  CFG_ERR_ROOT_TYPE = 1, CFG_ERR_MISSING_FIELD, CFG_ERR_NON_ASCII, CFG_ERR_TOO_LONG,
  CFG_ERR_BAD_ENUM, CFG_ERR_BAD_PATH, CFG_ERR_DUP_ID, CFG_ERR_FLOAT, CFG_ERR_RANGE,
  CFG_ERR_TYPE
} CFG_ERR_CODE;

typedef struct {
  UINTN      ItemIndex;    /* 顶层错误 = (UINTN)-1 */
  CHAR8      Field[32];
  UINT32     Code;         /* CFG_ERR_CODE */
  CHAR8      Msg[128];
} CONFIG_ERROR;

EFI_STATUS ConfigValidate (CONST JSON_NODE *Root, CONFIG **Out,
                           CONFIG_ERROR **Errs, UINTN *ErrCount,
                           CONFIG_ERROR **Warns, UINTN *WarnCount);
VOID       ConfigFree (CONFIG *Cfg);
VOID       ConfigFreeErrors (CONFIG_ERROR *Errs);

#endif
