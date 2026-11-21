/** @file
  JSON DOM 公共助手（契约 JsonDom.h 的 DOM 侧实现；纯逻辑，只认 DOM）。
  双适配器（JsonDomEdk2.c / JsonDomHost.c）产出的 DOM 均由此释放/查询。
**/
#include "ConfigCoreInternal.h"

VOID JsonDomFree (JSON_NODE *N) {
  UINTN I;
  if (N == NULL) return;
  switch (N->Type) {
    case JSON_STRING:
      MemFree (N->U.Str);
      break;
    case JSON_ARRAY:
      for (I = 0; I < N->U.Array.Count; I++) {
        JsonDomFree (N->U.Array.Items[I]);
      }
      MemFree (N->U.Array.Items);
      break;
    case JSON_OBJECT:
      for (I = 0; I < N->U.Object.Count; I++) {
        JsonDomFree (N->U.Object.Members[I].Value);
      }
      MemFree (N->U.Object.Members);
      break;
    default:
      break;
  }
  MemFree (N);
}

JSON_NODE *JsonObjectGet (CONST JSON_NODE *Obj, CONST CHAR8 *Key) {
  UINTN I;
  if (Obj == NULL || Obj->Type != JSON_OBJECT || Key == NULL) return NULL;
  for (I = 0; I < Obj->U.Object.Count; I++) {
    if (CfgStrEq (Obj->U.Object.Members[I].Key, Key)) {
      return Obj->U.Object.Members[I].Value;
    }
  }
  return NULL;
}

UINTN JsonDomArrayCount (CONST JSON_NODE *Arr) {
  if (Arr == NULL || Arr->Type != JSON_ARRAY) return 0;
  return Arr->U.Array.Count;
}

JSON_NODE *JsonArrayGet (CONST JSON_NODE *Arr, UINTN Index) {
  if (Arr == NULL || Arr->Type != JSON_ARRAY || Index >= Arr->U.Array.Count) {
    return NULL;
  }
  return Arr->U.Array.Items[Index];
}

/* ---- 规范 dumps（内部附加 API，T03-15 同构对比用） ---- */

VOID CfgI64ToA (INT64 V, CHAR8 *Buf, UINTN Cap) {
  CHAR8  Tmp[24];
  UINTN  N = 0, I;
  UINT64 U;
  if (Cap == 0) return;
  if (V < 0) {
    /* INT64_MIN 安全取负 */
    U = (UINT64)(-(V + 1)) + 1u;
  } else {
    U = (UINT64)V;
  }
  do {
    Tmp[N++] = (CHAR8)('0' + (U % 10));
    U /= 10;
  } while (U != 0 && N < sizeof (Tmp));
  I = 0;
  if (V < 0 && I + 1 < Cap) Buf[I++] = '-';
  while (N > 0 && I + 1 < Cap) Buf[I++] = Tmp[--N];
  Buf[I] = '\0';
}

typedef struct {
  CHAR8 *Buf;
  UINTN  Len;
  UINTN  Cap;
} CFG_DUMPBUF;

STATIC EFI_STATUS DumpReserve (CFG_DUMPBUF *B, UINTN Extra) {
  UINTN Need = B->Len + Extra + 1;
  UINTN NewCap;
  CHAR8 *P;
  if (Need <= B->Cap) return EFI_SUCCESS;
  NewCap = (B->Cap != 0) ? B->Cap : 256;
  while (NewCap < Need) NewCap *= 2;
  P = MemRealloc (B->Buf, NewCap);
  if (P == NULL) return EFI_OUT_OF_RESOURCES;
  B->Buf = P;
  B->Cap = NewCap;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS DumpPutc (CFG_DUMPBUF *B, CHAR8 C) {
  EFI_STATUS S = DumpReserve (B, 1);
  if (EFI_ERROR (S)) return S;
  B->Buf[B->Len++] = C;
  B->Buf[B->Len]   = '\0';
  return EFI_SUCCESS;
}

STATIC EFI_STATUS DumpPuts (CFG_DUMPBUF *B, CONST CHAR8 *S) {
  UINTN      I;
  EFI_STATUS St;
  for (I = 0; S[I] != '\0'; I++) {
    St = DumpPutc (B, S[I]);
    if (EFI_ERROR (St)) return St;
  }
  return EFI_SUCCESS;
}

/* 字符串/键内容转义（保证单行、无歧义分隔）。 */
STATIC EFI_STATUS DumpEscaped (CFG_DUMPBUF *B, CONST CHAR8 *S) {
  UINTN      I;
  EFI_STATUS St;
  for (I = 0; S[I] != '\0'; I++) {
    CHAR8 C = S[I];
    switch (C) {
      case '\\': St = DumpPuts (B, "\\\\"); break;
      case ')':  St = DumpPuts (B, "\\)");  break;
      case ',':  St = DumpPuts (B, "\\,");  break;
      case '\n': St = DumpPuts (B, "\\n");  break;
      case '\r': St = DumpPuts (B, "\\r");  break;
      case '\t': St = DumpPuts (B, "\\t");  break;
      default:   St = DumpPutc (B, C);      break;
    }
    if (EFI_ERROR (St)) return St;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS DumpNode (CFG_DUMPBUF *B, CONST JSON_NODE *N, UINTN Depth) {
  EFI_STATUS S;
  UINTN      I;
  CHAR8      Num[24];

  if (Depth > CFG_MAX_DOM_DEPTH) return EFI_BAD_BUFFER_SIZE;
  if (N == NULL) return DumpPuts (B, "null");
  switch (N->Type) {
    case JSON_NULL:
      return DumpPuts (B, "null");
    case JSON_BOOL:
      return DumpPuts (B, N->U.Bool ? "true" : "false");
    case JSON_INT:
      CfgI64ToA (N->U.Int, Num, sizeof (Num));
      return DumpPuts (B, Num);
    case JSON_STRING:
      S = DumpPuts (B, "str(");
      if (EFI_ERROR (S)) return S;
      S = DumpEscaped (B, (N->U.Str != NULL) ? N->U.Str : "");
      if (EFI_ERROR (S)) return S;
      return DumpPutc (B, ')');
    case JSON_ARRAY:
      S = DumpPuts (B, "arr[");
      if (EFI_ERROR (S)) return S;
      for (I = 0; I < N->U.Array.Count; I++) {
        if (I != 0) {
          S = DumpPutc (B, ',');
          if (EFI_ERROR (S)) return S;
        }
        S = DumpNode (B, N->U.Array.Items[I], Depth + 1);
        if (EFI_ERROR (S)) return S;
      }
      return DumpPutc (B, ']');
    case JSON_OBJECT:
      S = DumpPuts (B, "obj{");
      if (EFI_ERROR (S)) return S;
      for (I = 0; I < N->U.Object.Count; I++) {
        if (I != 0) {
          S = DumpPutc (B, ',');
          if (EFI_ERROR (S)) return S;
        }
        S = DumpEscaped (B, N->U.Object.Members[I].Key);
        if (EFI_ERROR (S)) return S;
        S = DumpPutc (B, ':');
        if (EFI_ERROR (S)) return S;
        S = DumpNode (B, N->U.Object.Members[I].Value, Depth + 1);
        if (EFI_ERROR (S)) return S;
      }
      return DumpPutc (B, '}');
    default:
      return EFI_INVALID_PARAMETER;
  }
}

EFI_STATUS JsonDomDump (CONST JSON_NODE *N, CHAR8 **Out) {
  CFG_DUMPBUF B;
  EFI_STATUS  S;
  if (Out == NULL) return EFI_INVALID_PARAMETER;
  *Out = NULL;
  B.Buf = NULL; B.Len = 0; B.Cap = 0;
  S = DumpNode (&B, N, 0);
  if (EFI_ERROR (S)) {
    MemFree (B.Buf);
    return S;
  }
  if (B.Buf == NULL) {
    /* 空节点也产出空串缓冲 */
    B.Buf = MemAlloc (1);
    if (B.Buf == NULL) return EFI_OUT_OF_RESOURCES;
  }
  *Out = B.Buf;
  return EFI_SUCCESS;
}