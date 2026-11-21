/** @file
  EDK2 JsonLib → JSON DOM 适配器（UEFI glue；仅 UEFI 构建编译）。
  契约：Include/Contract/JsonDom.h。核心不认 JsonLib，本文件是唯一 JsonLib 接触面。
  语义：
  - 深拷贝转换；Root 所有权仍归 caller（caller 负责 JsonValueFree）。
  - 浮点（EdkiiJsonTypeReal）→ EFI_INVALID_PARAMETER（DOM 数值仅 INT64；
    design §6 整数 only；与宿主适配器行为一致，T03-15 同构）。
  - 对象键 >63 字节截断至 63（DOM Key[64]；schema 键均短，仅影响未知字段告警显示）。
  - 数组计数用 JsonArrayGetValue 迭代到 NULL（避开 JsonLib/JsonDom 的
    JsonArrayCount 同名冲突，见 ConfigCoreInternal.h）。
  - 深度上限 CFG_MAX_DOM_DEPTH（防御恶意深嵌套耗尽 UEFI 栈）。
**/
#include "ConfigCoreInternal.h"

STATIC EFI_STATUS ConvertEdk2 (EDKII_JSON_VALUE V, UINTN Depth, JSON_NODE **Out);

STATIC EFI_STATUS ConvertObject (EDKII_JSON_VALUE V, UINTN Depth, JSON_NODE *N) {
  UINTN Count = 0, I;
  VOID *Iter;
  EFI_STATUS St;

  for (Iter = JsonObjectIterator (V); Iter != NULL;
       Iter = JsonObjectIteratorNext (V, Iter)) {
    Count++;
  }
  N->Type = JSON_OBJECT;
  N->U.Object.Count = Count;
  if (Count == 0) return EFI_SUCCESS;
  N->U.Object.Members = MemAlloc (Count * sizeof (JSON_MEMBER));
  if (N->U.Object.Members == NULL) return EFI_OUT_OF_RESOURCES;
  I = 0;
  for (Iter = JsonObjectIterator (V); Iter != NULL && I < Count;
       Iter = JsonObjectIteratorNext (V, Iter), I++) {
    CHAR8 *Key = JsonObjectIteratorKey (Iter);
    CfgStrCopy (N->U.Object.Members[I].Key, sizeof (N->U.Object.Members[I].Key),
                (Key != NULL) ? Key : "");
    St = ConvertEdk2 (JsonObjectIteratorValue (Iter), Depth + 1,
                      &N->U.Object.Members[I].Value);
    if (EFI_ERROR (St)) return St;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ConvertArray (EDKII_JSON_VALUE V, UINTN Depth, JSON_NODE *N) {
  EDKII_JSON_ARRAY Arr = JsonValueGetArray (V);
  UINTN Count = 0, I;
  EFI_STATUS St;

  while (JsonArrayGetValue (Arr, Count) != NULL) Count++;
  N->Type = JSON_ARRAY;
  N->U.Array.Count = Count;
  if (Count == 0) return EFI_SUCCESS;
  N->U.Array.Items = MemAlloc (Count * sizeof (JSON_NODE *));
  if (N->U.Array.Items == NULL) return EFI_OUT_OF_RESOURCES;
  for (I = 0; I < Count; I++) {
    St = ConvertEdk2 (JsonArrayGetValue (Arr, I), Depth + 1, &N->U.Array.Items[I]);
    if (EFI_ERROR (St)) return St;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ConvertEdk2 (EDKII_JSON_VALUE V, UINTN Depth, JSON_NODE **Out) {
  JSON_NODE *N;
  EFI_STATUS St = EFI_SUCCESS;
  CONST CHAR8 *S;

  *Out = NULL;
  if (V == NULL) return EFI_INVALID_PARAMETER;
  if (Depth > CFG_MAX_DOM_DEPTH) return EFI_INVALID_PARAMETER;

  N = MemAlloc (sizeof (JSON_NODE));
  if (N == NULL) return EFI_OUT_OF_RESOURCES;

  switch (JsonGetType (V)) {
    case EdkiiJsonTypeObject:
      St = ConvertObject (V, Depth, N);
      break;
    case EdkiiJsonTypeArray:
      St = ConvertArray (V, Depth, N);
      break;
    case EdkiiJsonTypeString:
      N->Type = JSON_STRING;
      S = JsonValueGetAsciiString (V);   /* jansson 字符串值 = UTF-8 字节 */
      if (S == NULL) { St = EFI_INVALID_PARAMETER; break; }
      N->U.Str = MemAlloc (CfgStrLen (S) + 1);
      if (N->U.Str == NULL) { St = EFI_OUT_OF_RESOURCES; break; }
      CfgStrCopy (N->U.Str, CfgStrLen (S) + 1, S);
      break;
    case EdkiiJsonTypeInteger:
      N->Type = JSON_INT;
      N->U.Int = JsonValueGetInteger (V);
      break;
    case EdkiiJsonTypeTrue:
      N->Type = JSON_BOOL;
      N->U.Bool = TRUE;
      break;
    case EdkiiJsonTypeFalse:
      N->Type = JSON_BOOL;
      N->U.Bool = FALSE;
      break;
    case EdkiiJsonTypeNull:
      N->Type = JSON_NULL;
      break;
    case EdkiiJsonTypeReal:
    default:
      /* 浮点/未知类型：DOM 不可表达 → 拒绝（design §6 整数 only）。 */
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

EFI_STATUS JsonDomFromEdk2 (EDKII_JSON_VALUE Root, JSON_NODE **Out) {
  EFI_STATUS St;
  if (Root == NULL || Out == NULL) return EFI_INVALID_PARAMETER;
  *Out = NULL;
  St = ConvertEdk2 (Root, 0, Out);
  if (EFI_ERROR (St)) {
    JsonDomFree (*Out);
    *Out = NULL;
  }
  return St;
}