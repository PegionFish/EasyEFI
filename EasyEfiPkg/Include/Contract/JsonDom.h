/** @file
  最小 JSON DOM（契约，S00 冻结）。ConfigCore 只认 DOM，不认 JsonLib/jansson。
  双适配器：EDK2 JsonLib→DOM（S03 glue，QEMU 测）；宿主 jansson→DOM（S03，宿主测）。
  注意：UEFI JsonLib 不支持浮点（JsonLib.h 原文）→ DOM 数值仅 INT64。
**/
#ifndef JSON_DOM_H
#define JSON_DOM_H

#include "EfiCompat.h"

typedef enum {
  JSON_NULL = 0, JSON_BOOL, JSON_INT, JSON_STRING, JSON_ARRAY, JSON_OBJECT
} JSON_TYPE;

typedef struct JSON_NODE JSON_NODE;
typedef struct {
  CHAR8     Key[64];
  JSON_NODE *Value;
} JSON_MEMBER;

struct JSON_NODE {
  JSON_TYPE Type;
  union {
    BOOLEAN Bool;
    INT64   Int;
    CHAR8   *Str;            /* UTF-8，callee 分配 */
    struct { JSON_NODE **Items; UINTN Count; } Array;
    struct { JSON_MEMBER *Members; UINTN Count; } Object;
  } U;
};

/* 适配器（S03 实现） */
#ifdef UEFI_BUILD
#include <Library/JsonLib.h>
EFI_STATUS JsonDomFromEdk2 (EDKII_JSON_VALUE Root, JSON_NODE **Out);
#endif
EFI_STATUS JsonDomFromHost (CONST CHAR8 *Text, JSON_NODE **Out);  /* 宿主 jansson 适配器 */

VOID       JsonDomFree (JSON_NODE *N);
JSON_NODE *JsonObjectGet (CONST JSON_NODE *Obj, CONST CHAR8 *Key);   /* 无则 NULL */
/* 命名避开 JsonLib.h 的 JsonArrayCount（UEFI 构建符号冲突，S03 上报，leader 批准改名） */
UINTN      JsonDomArrayCount (CONST JSON_NODE *Arr);
JSON_NODE *JsonArrayGet (CONST JSON_NODE *Arr, UINTN Index);

#endif
