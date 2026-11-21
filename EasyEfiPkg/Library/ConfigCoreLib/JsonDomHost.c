/** @file
  宿主 jansson → JSON DOM 适配器（仅宿主测试编译，不进 .inf）。
  jansson 源码 = 共享 edk2 子模块（与 UEFI JsonLib 同版本 2.13.1，同解析器 → T03-15 同构）。
  语义与 JsonDomEdk2.c 逐条对齐：
  - JSON_REJECT_DUPLICATES（design §5.3）；根仅 object/array（与 JsonLoadString 一致，
    不传 JSON_DECODE_ANY；标量根 → 解析失败）。
  - JSON_REAL → EFI_INVALID_PARAMETER（DOM 数值仅 INT64；design §6 整数 only）。
  - 对象键 >63 字节截断；深度上限 CFG_MAX_DOM_DEPTH；callee 分配，caller JsonDomFree。
**/
#include "ConfigCoreInternal.h"

/* jansson 公开枚举 JSON_OBJECT/JSON_ARRAY/JSON_STRING/JSON_NULL 与 DOM 的
   JSON_TYPE 同名（契约冻结，不改 DOM 侧）。仅在 include jansson.h 期间改名，
   本文件 switch 用 JANSSON_* 别名。UEFI 侧无此问题（JsonLib 类型不透明）。 */
#define JSON_OBJECT  JANSSON_OBJECT
#define JSON_ARRAY   JANSSON_ARRAY
#define JSON_STRING  JANSSON_STRING
#define JSON_INTEGER JANSSON_INTEGER
#define JSON_REAL    JANSSON_REAL
#define JSON_TRUE    JANSSON_TRUE
#define JSON_FALSE   JANSSON_FALSE
#define JSON_NULL    JANSSON_NULL
#include <jansson.h>
#undef JSON_OBJECT
#undef JSON_ARRAY
#undef JSON_STRING
#undef JSON_INTEGER
#undef JSON_REAL
#undef JSON_TRUE
#undef JSON_FALSE
#undef JSON_NULL

STATIC EFI_STATUS ConvertHost (json_t *J, UINTN Depth, JSON_NODE **Out) {
  JSON_NODE *N;
  EFI_STATUS St = EFI_SUCCESS;
  UINTN I, Count;
  CONST CHAR8 *S;

  *Out = NULL;
  if (J == NULL) return EFI_INVALID_PARAMETER;
  if (Depth > CFG_MAX_DOM_DEPTH) return EFI_INVALID_PARAMETER;

  N = MemAlloc (sizeof (JSON_NODE));
  if (N == NULL) return EFI_OUT_OF_RESOURCES;

  switch (json_typeof (J)) {
    case JANSSON_OBJECT:
      N->Type = JSON_OBJECT;
      Count = json_object_size (J);
      N->U.Object.Count = Count;
      if (Count != 0) {
        json_t *Val;
        const char *Key;
        N->U.Object.Members = MemAlloc (Count * sizeof (JSON_MEMBER));
        if (N->U.Object.Members == NULL) { St = EFI_OUT_OF_RESOURCES; break; }
        I = 0;
        json_object_foreach (J, Key, Val) {
          if (I >= Count) break;
          CfgStrCopy (N->U.Object.Members[I].Key, sizeof (N->U.Object.Members[I].Key), Key);
          St = ConvertHost (Val, Depth + 1, &N->U.Object.Members[I].Value);
          if (EFI_ERROR (St)) break;
          I++;
        }
      }
      break;
    case JANSSON_ARRAY:
      N->Type = JSON_ARRAY;
      Count = json_array_size (J);
      N->U.Array.Count = Count;
      if (Count != 0) {
        N->U.Array.Items = MemAlloc (Count * sizeof (JSON_NODE *));
        if (N->U.Array.Items == NULL) { St = EFI_OUT_OF_RESOURCES; break; }
        for (I = 0; I < Count; I++) {
          St = ConvertHost (json_array_get (J, I), Depth + 1, &N->U.Array.Items[I]);
          if (EFI_ERROR (St)) break;
        }
      }
      break;
    case JANSSON_STRING:
      N->Type = JSON_STRING;
      S = json_string_value (J);   /* UTF-8 字节 */
      if (S == NULL) { St = EFI_INVALID_PARAMETER; break; }
      N->U.Str = MemAlloc (CfgStrLen (S) + 1);
      if (N->U.Str == NULL) { St = EFI_OUT_OF_RESOURCES; break; }
      CfgStrCopy (N->U.Str, CfgStrLen (S) + 1, S);
      break;
    case JANSSON_INTEGER:
      N->Type = JSON_INT;
      N->U.Int = (INT64)json_integer_value (J);
      break;
    case JANSSON_TRUE:
      N->Type = JSON_BOOL;
      N->U.Bool = TRUE;
      break;
    case JANSSON_FALSE:
      N->Type = JSON_BOOL;
      N->U.Bool = FALSE;
      break;
    case JANSSON_NULL:
      N->Type = JSON_NULL;
      break;
    case JANSSON_REAL:
    default:
      /* 浮点：DOM 不可表达 → 拒绝（与 EDK2 适配器一致）。 */
      St = EFI_INVALID_PARAMETER;
      break;
  }

  if (EFI_ERROR (St)) {
    JsonDomFree (N);
    return St;
  }
  *Out = N;
  return EFI_SUCCESS;
}

EFI_STATUS JsonDomFromHost (CONST CHAR8 *Text, JSON_NODE **Out) {
  json_error_t Err;
  json_t *Root;
  EFI_STATUS St;

  if (Text == NULL || Out == NULL) return EFI_INVALID_PARAMETER;
  *Out = NULL;
  Root = json_loads (Text, JSON_REJECT_DUPLICATES, &Err);
  if (Root == NULL) return EFI_INVALID_PARAMETER;
  St = ConvertHost (Root, 0, Out);
  json_decref (Root);
  if (EFI_ERROR (St)) {
    JsonDomFree (*Out);
    *Out = NULL;
  }
  return St;
}