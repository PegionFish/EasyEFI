/** @file
  launcher.json schema 校验核心（纯逻辑，只认 JSON DOM）。
  规则权威 = design.md §6；契约 = Include/Contract/ConfigSchema.h（冻结）。
  纪律：不调用 JsonLib/jansson（浮点在适配器边界拒绝）；不 include UEFI 协议头；
  内存走 MemAlloc/MemRealloc/MemFree；path 文法委托 PathParse（S04 契约）。

  语义约定（交接注记同步）：
  - 返回 EFI_SUCCESS = 产出了 CONFIG（尽力而为）；调用方检查 ErrCount 决定横幅/帮助页。
    根非 object / 参数非法 / OOM → 返回错误且 *Out=NULL。
  - 非法项跳过但错误清单保留；每项只记第一个错误（first-error-wins），未知字段警告照收。
  - 顶层字段错误 ItemIndex=(UINTN)-1；title/version 可选（缺省空串），items 必填非空数组。
  - skip_timeout 缺省 = PcdSkipTimeoutDefault（宿主镜像默认 2）；越界钳制 0..30 + RANGE 警告。
  - danger=true 隐含 Confirm=TRUE；options 仅 exec（UTF-8 允许，不做 ASCII 检查）。
  - 浮点：DOM 无 REAL 类型，适配器已拒绝 → 核心整数字段只认 JSON_INT（其余 TYPE）。
**/
#include "ConfigCoreInternal.h"

/* ---- 错误/警告清单（几何增长） ---- */

typedef struct {
  CONFIG_ERROR *V;
  UINTN         Count;
  UINTN         Cap;
} CFG_ERRLIST;

/* 消息 = A + B + C 拼接（B/C 可 NULL），截断至 Msg[127]。避免 printf 族双环境差异。 */
STATIC VOID SetMsg (CONFIG_ERROR *E, CONST CHAR8 *A, CONST CHAR8 *B, CONST CHAR8 *C) {
  UINTN I = 0, J;
  CONST CHAR8 *Parts[3];
  UINTN P;
  Parts[0] = A; Parts[1] = B; Parts[2] = C;
  for (P = 0; P < 3; P++) {
    if (Parts[P] == NULL) continue;
    for (J = 0; Parts[P][J] != '\0' && I + 1 < sizeof (E->Msg); J++) {
      E->Msg[I++] = Parts[P][J];
    }
  }
  E->Msg[I] = '\0';
}

STATIC EFI_STATUS ListAdd (
  CFG_ERRLIST *L, UINTN ItemIndex, CONST CHAR8 *Field, UINT32 Code,
  CONST CHAR8 *A, CONST CHAR8 *B, CONST CHAR8 *C
  )
{
  CONFIG_ERROR *E;
  if (L->Count == L->Cap) {
    UINTN NewCap = (L->Cap != 0) ? L->Cap * 2 : 8;
    CONFIG_ERROR *P = MemRealloc (L->V, NewCap * sizeof (CONFIG_ERROR));
    if (P == NULL) return EFI_OUT_OF_RESOURCES;
    L->V   = P;
    L->Cap = NewCap;
  }
  E = &L->V[L->Count];
  E->ItemIndex = ItemIndex;
  CfgStrCopy (E->Field, sizeof (E->Field), Field);
  E->Code = Code;
  SetMsg (E, A, B, C);
  L->Count++;
  return EFI_SUCCESS;
}

/* ---- 字段检查助手 ---- */

/* 字符串字段：类型/ASCII/长度。OK 返回 0，否则返回 CFG_ERR_CODE。 */
STATIC UINT32 CheckStr (CONST JSON_NODE *V, UINTN MaxLen, BOOLEAN Ascii, CONST CHAR8 **Out) {
  if (V->Type != JSON_STRING) return CFG_ERR_TYPE;
  if (V->U.Str == NULL) return CFG_ERR_TYPE;
  if (Ascii && !CfgIsAscii (V->U.Str)) return CFG_ERR_NON_ASCII;
  if (CfgStrLen (V->U.Str) > MaxLen) return CFG_ERR_TOO_LONG;
  *Out = V->U.Str;
  return 0;
}

STATIC BOOLEAN IsKnownItemKey (CONST CHAR8 *Key) {
  return CfgStrEq (Key, "id")      || CfgStrEq (Key, "name")   ||
         CfgStrEq (Key, "desc")    || CfgStrEq (Key, "arch")   ||
         CfgStrEq (Key, "action")  || CfgStrEq (Key, "path")   ||
         CfgStrEq (Key, "options") || CfgStrEq (Key, "danger") ||
         CfgStrEq (Key, "confirm");
}

STATIC BOOLEAN IsKnownTopKey (CONST CHAR8 *Key) {
  return CfgStrEq (Key, "title") || CfgStrEq (Key, "version") ||
         CfgStrEq (Key, "skip_timeout") || CfgStrEq (Key, "items");
}

/* arch 数组 → 掩码。OK 返回 0。 */
STATIC UINT32 CheckArch (CONST JSON_NODE *V, UINT8 *Mask, CONST CHAR8 **BadVal) {
  UINTN I, N;
  UINT8 M = 0;
  if (V->Type != JSON_ARRAY) return CFG_ERR_TYPE;
  N = JsonArrayCount (V);
  for (I = 0; I < N; I++) {
    CONST JSON_NODE *E = JsonArrayGet (V, I);
    CONST CHAR8 *S;
    if (E == NULL || E->Type != JSON_STRING || E->U.Str == NULL) {
      *BadVal = NULL;
      return CFG_ERR_TYPE;
    }
    S = E->U.Str;
    if (CfgStrEq (S, "x64")) {
      M |= ARCH_X64_BIT;
    } else if (CfgStrEq (S, "aarch64")) {
      M |= ARCH_AARCH64_BIT;
    } else {
      *BadVal = S;
      return CFG_ERR_BAD_ENUM;
    }
  }
  *Mask = M;
  return 0;
}

/* ---- 单项校验 ---- */

/* 返回 EFI_SUCCESS（无论项是否合法，错误已入清单）；OOM → EFI_OUT_OF_RESOURCES。
   *Valid=TRUE 时 Out 已填充。 */
STATIC EFI_STATUS ValidateItem (
  CONST JSON_NODE *Item, UINTN Index, CONFIG_ITEM *Out,
  CFG_ERRLIST *Errs, CFG_ERRLIST *Warns, BOOLEAN *Valid
  )
{
  CONST JSON_NODE *V;
  CONST CHAR8     *S;
  UINT32           Code;
  UINTN            I;
  CONFIG_ACTION    Action = ACTION_EXEC;
  EFI_STATUS       St;

  *Valid = FALSE;

  if (Item == NULL || Item->Type != JSON_OBJECT) {
    return ListAdd (Errs, Index, "(item)", CFG_ERR_TYPE, "item must be an object", NULL, NULL);
  }

  /* 未知字段 → 警告（先于错误收集；非法项的未知字段同样告警）。 */
  for (I = 0; I < Item->U.Object.Count; I++) {
    CONST CHAR8 *Key = Item->U.Object.Members[I].Key;
    if (!IsKnownItemKey (Key)) {
      St = ListAdd (Warns, Index, Key, CFG_WARN_UNKNOWN, "unknown field ignored: ", Key, NULL);
      if (EFI_ERROR (St)) return St;
    }
  }

  /* id：必填、字符串、ASCII、非空、≤63。 */
  V = JsonObjectGet (Item, "id");
  if (V == NULL) {
    return ListAdd (Errs, Index, "id", CFG_ERR_MISSING_FIELD, "missing required field", NULL, NULL);
  }
  Code = CheckStr (V, sizeof (Out->Id) - 1, TRUE, &S);
  if (Code != 0) {
    return ListAdd (Errs, Index, "id", Code, "id: ",
                    (Code == CFG_ERR_TYPE) ? "expected string" :
                    (Code == CFG_ERR_NON_ASCII) ? "ASCII required" : "too long (max 63)", NULL);
  }
  if (S[0] == '\0') {
    return ListAdd (Errs, Index, "id", CFG_ERR_MISSING_FIELD, "id is empty", NULL, NULL);
  }
  CfgStrCopy (Out->Id, sizeof (Out->Id), S);

  /* name：必填、字符串、ASCII、≤40。 */
  V = JsonObjectGet (Item, "name");
  if (V == NULL) {
    return ListAdd (Errs, Index, "name", CFG_ERR_MISSING_FIELD, "missing required field", NULL, NULL);
  }
  Code = CheckStr (V, sizeof (Out->Name) - 1, TRUE, &S);
  if (Code != 0) {
    return ListAdd (Errs, Index, "name", Code, "name: ",
                    (Code == CFG_ERR_TYPE) ? "expected string" :
                    (Code == CFG_ERR_NON_ASCII) ? "ASCII required (ConOut has no CJK glyphs)" :
                    "too long (max 40)", NULL);
  }
  CfgStrCopy (Out->Name, sizeof (Out->Name), S);

  /* desc：可选、字符串、ASCII、≤76。 */
  V = JsonObjectGet (Item, "desc");
  if (V != NULL) {
    Code = CheckStr (V, sizeof (Out->Desc) - 1, TRUE, &S);
    if (Code != 0) {
      return ListAdd (Errs, Index, "desc", Code, "desc: ",
                      (Code == CFG_ERR_TYPE) ? "expected string" :
                      (Code == CFG_ERR_NON_ASCII) ? "ASCII required" : "too long (max 76)", NULL);
    }
    CfgStrCopy (Out->Desc, sizeof (Out->Desc), S);
  }

  /* action：必填枚举（先于 path 检查，决定 path 是否必需）。 */
  V = JsonObjectGet (Item, "action");
  if (V == NULL) {
    return ListAdd (Errs, Index, "action", CFG_ERR_MISSING_FIELD, "missing required field", NULL, NULL);
  }
  if (V->Type != JSON_STRING || V->U.Str == NULL) {
    return ListAdd (Errs, Index, "action", CFG_ERR_TYPE, "action: expected string", NULL, NULL);
  }
  S = V->U.Str;
  if      (CfgStrEq (S, "exec"))  Action = ACTION_EXEC;
  else if (CfgStrEq (S, "shell")) Action = ACTION_SHELL;
  else if (CfgStrEq (S, "boot"))  Action = ACTION_BOOT;
  else if (CfgStrEq (S, "reset")) Action = ACTION_RESET;
  else {
    return ListAdd (Errs, Index, "action", CFG_ERR_BAD_ENUM, "unknown action '", S,
                    "' (exec/shell/boot/reset)");
  }
  Out->Action = (UINT8)Action;

  /* arch：可选数组，缺省 = 双架构。 */
  V = JsonObjectGet (Item, "arch");
  if (V != NULL) {
    CONST CHAR8 *BadVal = NULL;
    Code = CheckArch (V, &Out->ArchMask, &BadVal);
    if (Code == CFG_ERR_BAD_ENUM) {
      if (BadVal != NULL && CfgStrEq (BadVal, "arm64")) {
        return ListAdd (Errs, Index, "arch", Code, "unknown arch 'arm64'; use 'aarch64'", NULL, NULL);
      }
      return ListAdd (Errs, Index, "arch", Code, "unknown arch '", BadVal, "' (x64/aarch64)");
    }
    if (Code != 0) {
      return ListAdd (Errs, Index, "arch", Code, "arch: expected array of strings", NULL, NULL);
    }
  } else {
    Out->ArchMask = ARCH_X64_BIT | ARCH_AARCH64_BIT;
  }

  /* path：exec/boot 必填；shell/reset 不需要（出现则警告忽略）。文法委托 PathParse。 */
  V = JsonObjectGet (Item, "path");
  if (Action == ACTION_EXEC || Action == ACTION_BOOT) {
    RESOLVED_PATH Rp;
    if (V == NULL) {
      return ListAdd (Errs, Index, "path", CFG_ERR_MISSING_FIELD,
                      "missing required field for this action", NULL, NULL);
    }
    Code = CheckStr (V, sizeof (Out->Path) - 1, FALSE, &S);
    if (Code != 0) {
      return ListAdd (Errs, Index, "path", Code, "path: ",
                      (Code == CFG_ERR_TYPE) ? "expected string" : "too long (max 255)", NULL);
    }
    if (!EFI_ERROR (PathParse (S, &Rp))) {
      CfgStrCopy (Out->Path, sizeof (Out->Path), S);
    } else {
      return ListAdd (Errs, Index, "path", CFG_ERR_BAD_PATH, "invalid path grammar: ", S, NULL);
    }
  } else if (V != NULL) {
    St = ListAdd (Warns, Index, "path", CFG_WARN_UNKNOWN,
                  "path ignored for shell/reset action", NULL, NULL);
    if (EFI_ERROR (St)) return St;
  }

  /* options：可选，仅 exec；UTF-8 允许（glue 侧 UTF-8→UCS-2）。 */
  V = JsonObjectGet (Item, "options");
  if (V != NULL) {
    if (Action != ACTION_EXEC) {
      St = ListAdd (Warns, Index, "options", CFG_WARN_UNKNOWN,
                    "options only applies to exec action", NULL, NULL);
      if (EFI_ERROR (St)) return St;
    } else {
      Code = CheckStr (V, sizeof (Out->Options) - 1, FALSE, &S);
      if (Code != 0) {
        return ListAdd (Errs, Index, "options", Code, "options: ",
                        (Code == CFG_ERR_TYPE) ? "expected string" : "too long (max 255)", NULL);
      }
      CfgStrCopy (Out->Options, sizeof (Out->Options), S);
    }
  }

  /* danger / confirm：可选 bool；danger 隐含 confirm。 */
  V = JsonObjectGet (Item, "danger");
  if (V != NULL) {
    if (V->Type != JSON_BOOL) {
      return ListAdd (Errs, Index, "danger", CFG_ERR_TYPE, "danger: expected boolean", NULL, NULL);
    }
    Out->Danger = V->U.Bool;
  }
  V = JsonObjectGet (Item, "confirm");
  if (V != NULL) {
    if (V->Type != JSON_BOOL) {
      return ListAdd (Errs, Index, "confirm", CFG_ERR_TYPE, "confirm: expected boolean", NULL, NULL);
    }
    Out->Confirm = V->U.Bool;
  }
  if (Out->Danger) {
    Out->Confirm = TRUE;
  }

  *Valid = TRUE;
  return EFI_SUCCESS;
}

/* ---- 顶层校验 ---- */

EFI_STATUS ConfigValidate (
  CONST JSON_NODE *Root, CONFIG **Out,
  CONFIG_ERROR **Errs, UINTN *ErrCount,
  CONFIG_ERROR **Warns, UINTN *WarnCount
  )
{
  CONFIG       *Cfg = NULL;
  CFG_ERRLIST   E = { NULL, 0, 0 };
  CFG_ERRLIST   W = { NULL, 0, 0 };
  CONST JSON_NODE *V;
  CONST CHAR8  *S;
  UINT32        Code;
  UINTN         I, N, Accepted;
  EFI_STATUS    St;

  if (Root == NULL || Out == NULL || Errs == NULL || ErrCount == NULL ||
      Warns == NULL || WarnCount == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Out = NULL; *Errs = NULL; *ErrCount = 0; *Warns = NULL; *WarnCount = 0;

  if (Root->Type != JSON_OBJECT) {
    St = ListAdd (&E, CFG_TOP_INDEX, "root", CFG_ERR_ROOT_TYPE, "root must be a JSON object", NULL, NULL);
    if (EFI_ERROR (St)) goto Fail;
    *Errs = E.V; *ErrCount = E.Count;
    return EFI_INVALID_PARAMETER;
  }

  Cfg = MemAlloc (sizeof (CONFIG));
  if (Cfg == NULL) { St = EFI_OUT_OF_RESOURCES; goto Fail; }

  /* 未知顶层字段 → 警告。 */
  for (I = 0; I < Root->U.Object.Count; I++) {
    CONST CHAR8 *Key = Root->U.Object.Members[I].Key;
    if (!IsKnownTopKey (Key)) {
      St = ListAdd (&W, CFG_TOP_INDEX, Key, CFG_WARN_UNKNOWN, "unknown field ignored: ", Key, NULL);
      if (EFI_ERROR (St)) goto Fail;
    }
  }

  /* title：可选，ASCII ≤40。 */
  V = JsonObjectGet (Root, "title");
  if (V != NULL) {
    Code = CheckStr (V, sizeof (Cfg->Title) - 1, TRUE, &S);
    if (Code != 0) {
      St = ListAdd (&E, CFG_TOP_INDEX, "title", Code, "title: ",
                    (Code == CFG_ERR_TYPE) ? "expected string" :
                    (Code == CFG_ERR_NON_ASCII) ? "ASCII required" : "too long (max 40)", NULL);
      if (EFI_ERROR (St)) goto Fail;
    } else {
      CfgStrCopy (Cfg->Title, sizeof (Cfg->Title), S);
    }
  }

  /* version：可选，ASCII ≤31（仅展示/日志）。 */
  V = JsonObjectGet (Root, "version");
  if (V != NULL) {
    Code = CheckStr (V, sizeof (Cfg->Version) - 1, TRUE, &S);
    if (Code != 0) {
      St = ListAdd (&E, CFG_TOP_INDEX, "version", Code, "version: ",
                    (Code == CFG_ERR_TYPE) ? "expected string" :
                    (Code == CFG_ERR_NON_ASCII) ? "ASCII required" : "too long (max 31)", NULL);
      if (EFI_ERROR (St)) goto Fail;
    } else {
      CfgStrCopy (Cfg->Version, sizeof (Cfg->Version), S);
    }
  }

  /* skip_timeout：可选整数，缺省 PCD；越界钳制 + 警告。 */
  Cfg->SkipTimeout = CFG_SKIP_TIMEOUT_DEFAULT;
  V = JsonObjectGet (Root, "skip_timeout");
  if (V != NULL) {
    if (V->Type != JSON_INT) {
      St = ListAdd (&E, CFG_TOP_INDEX, "skip_timeout", CFG_ERR_TYPE,
                    "expected integer (floats rejected at adapter)", NULL, NULL);
      if (EFI_ERROR (St)) goto Fail;
    } else if (V->U.Int < 0) {
      Cfg->SkipTimeout = 0;
      St = ListAdd (&W, CFG_TOP_INDEX, "skip_timeout", CFG_ERR_RANGE,
                    "below 0; clamped to 0", NULL, NULL);
      if (EFI_ERROR (St)) goto Fail;
    } else if (V->U.Int > (INT64)CFG_SKIP_TIMEOUT_MAX) {
      Cfg->SkipTimeout = CFG_SKIP_TIMEOUT_MAX;
      St = ListAdd (&W, CFG_TOP_INDEX, "skip_timeout", CFG_ERR_RANGE,
                    "above 30; clamped to 30", NULL, NULL);
      if (EFI_ERROR (St)) goto Fail;
    } else {
      Cfg->SkipTimeout = (UINTN)V->U.Int;
    }
  }

  /* items：必填非空数组；非法项跳过，错误保留。 */
  V = JsonObjectGet (Root, "items");
  if (V == NULL) {
    St = ListAdd (&E, CFG_TOP_INDEX, "items", CFG_ERR_MISSING_FIELD, "missing required field", NULL, NULL);
    if (EFI_ERROR (St)) goto Fail;
  } else if (V->Type != JSON_ARRAY) {
    St = ListAdd (&E, CFG_TOP_INDEX, "items", CFG_ERR_TYPE, "items: expected array", NULL, NULL);
    if (EFI_ERROR (St)) goto Fail;
  } else if (JsonArrayCount (V) == 0) {
    St = ListAdd (&E, CFG_TOP_INDEX, "items", CFG_ERR_MISSING_FIELD, "items is empty", NULL, NULL);
    if (EFI_ERROR (St)) goto Fail;
  } else {
    N = JsonArrayCount (V);
    Cfg->Items = MemAlloc (N * sizeof (CONFIG_ITEM));
    if (Cfg->Items == NULL) { St = EFI_OUT_OF_RESOURCES; goto Fail; }
    Accepted = 0;
    for (I = 0; I < N; I++) {
      BOOLEAN Valid = FALSE;
      CONFIG_ITEM *Slot = &Cfg->Items[Accepted];
      UINT8 *Bytes = (UINT8 *)Slot;
      UINTN B;
      /* 槽位复用（非法项不占位）：先清零，避免上一项残留字段泄入下一项。 */
      for (B = 0; B < sizeof (CONFIG_ITEM); B++) Bytes[B] = 0;
      St = ValidateItem (JsonArrayGet (V, I), I, Slot, &E, &W, &Valid);
      if (EFI_ERROR (St)) goto Fail;
      if (!Valid) continue;
      /* id 唯一（对已接受的项查重）。 */
      {
        UINTN J;
        BOOLEAN Dup = FALSE;
        for (J = 0; J < Accepted; J++) {
          if (CfgStrEq (Cfg->Items[J].Id, Slot->Id)) { Dup = TRUE; break; }
        }
        if (Dup) {
          St = ListAdd (&E, I, "id", CFG_ERR_DUP_ID, "duplicate id '", Slot->Id, "'");
          if (EFI_ERROR (St)) goto Fail;
          continue;
        }
      }
      Accepted++;
    }
    Cfg->Count = Accepted;
  }

  *Out      = Cfg;
  *Errs     = E.V;  *ErrCount  = E.Count;
  *Warns    = W.V;  *WarnCount = W.Count;
  return EFI_SUCCESS;

Fail:
  MemFree (E.V);
  MemFree (W.V);
  if (Cfg != NULL) {
    MemFree (Cfg->Items);
    MemFree (Cfg);
  }
  return St;
}

VOID ConfigFree (CONFIG *Cfg) {
  if (Cfg == NULL) return;
  MemFree (Cfg->Items);
  MemFree (Cfg);
}

VOID ConfigFreeErrors (CONFIG_ERROR *Errs) {
  MemFree (Errs);
}