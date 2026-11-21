/** @file
  Launcher core — 纯逻辑菜单状态机（S11；宿主可测，UEFI 同码）。
  只依赖 TuiScreen/TuiInput/ExecOps/FsOps/ExecPath/EfiMem（契约 Ops 缝），
  不触 gBS/gRT/Shell/JsonLib；平台动作经 LAUNCHER_PLATFORM_OPS 回调（glue 提供）。
  状态 = {倒计时, 主菜单, 确认, 错误页, boot 子菜单, auto 候选, 帮助}。
  纪律：CHAR16 字面量一律 U16()；字符串操作用 EfiStr* 与自带 bounded 助手（禁 wcs*）。
**/
#include "LauncherCore.h"

/* ---- 内部类型 ---- */

typedef enum {
  ROW_WORK = 0,       /* JSON 工作条目 */
  ROW_SYS_FM,         /* 内置：FileManager */
  ROW_SYS_EDITOR,     /* 内置：Editor */
  ROW_SYS_BOOTMENU,   /* 内置：NVRAM 启动子菜单 */
  ROW_SYS_SHELL,      /* 内置：返回 Shell */
  ROW_SYS_RESET,      /* 内置：重启 */
  ROW_DISCOVERY       /* 内置默认菜单（cfg=NULL）：auto: 外部介质发现 */
} ROW_KIND;

typedef struct {
  UINT8       Kind;        /* ROW_KIND */
  UINTN       ItemIndex;   /* ROW_WORK: Cfg->Items 下标 */
  BOOLEAN     Disabled;
  BOOLEAN     Danger;
  CONST CHAR8 *Reason;     /* 灰显原因 ASCII */
} MENU_ROW;

struct LAUNCHER_CORE {
  LAUNCHER_INIT       Init;         /* 按值拷贝；指针均为借用（glue 持有） */
  LAUNCHER_STATE      State;
  LAUNCHER_EXIT       Exit;
  UINTN               Countdown;
  MENU_ROW            *Rows;
  UINTN               RowCount;
  UINTN               Sel;
  UINTN               Top;          /* 菜单滚动窗口顶 */
  UINTN               PendingRow;   /* CONFIRM 待执行行 */
  CHAR16              ErrMsg[256];
  LAUNCHER_BOOT_ENTRY *BootList;    /* BOOT_MENU（MemAlloc，core 释放） */
  UINTN               BootCount;
  UINTN               BootSel;
  EXEC_CANDIDATE      *Cands;       /* CANDIDATES（ExecOps 分配，ExecFreeCandidates 释放） */
  UINTN               CandCount;
  UINTN               CandSel;
  BOOLEAN             CandIsScript; /* 原始 auto: 请求是否 .nsh（候选执行分流用） */
  CHAR16              CandOpts[256];/* 原始条目 LoadOptions（候选选中后执行用） */
};

#define LABEL_CAP 160
#define PATH_CAP  512

/* ---- CHAR16 bounded 助手（禁 wcs*；宿主 wchar_t 32 位） ---- */

STATIC VOID U16Copy (CHAR16 *Dst, UINTN Cap, CONST CHAR16 *Src) {
  UINTN i;
  for (i = 0; Src[i] != 0 && i + 1 < Cap; i++) {
    Dst[i] = Src[i];
  }
  Dst[i] = 0;
}

STATIC VOID U16Cat (CHAR16 *Dst, UINTN Cap, CONST CHAR16 *Src) {
  UINTN d = EfiStrLen (Dst), i;
  for (i = 0; Src[i] != 0 && d + i + 1 < Cap; i++) {
    Dst[d + i] = Src[i];
  }
  Dst[d + i] = 0;
}

/* ASCII（CHAR8）→ CHAR16 追加；≥0x80 字节防御性替换 '?'（schema 保证 ASCII） */
STATIC VOID U16CatAscii (CHAR16 *Dst, UINTN Cap, CONST CHAR8 *Src) {
  UINTN d = EfiStrLen (Dst), i;
  for (i = 0; Src[i] != 0 && d + i + 1 < Cap; i++) {
    UINT8 b = (UINT8)Src[i];
    Dst[d + i] = (CHAR16)(b < 0x80 ? b : '?');
  }
  Dst[d + i] = 0;
}

STATIC VOID AsciiToU16 (CONST CHAR8 *Src, CHAR16 *Out, UINTN Cap) {
  Out[0] = 0;
  U16CatAscii (Out, Cap, Src);
}

STATIC VOID U16CatUint (CHAR16 *Dst, UINTN Cap, UINTN Val) {
  CHAR16 Tmp[24];
  INTN   i = 0, j;
  do {
    Tmp[i++] = (CHAR16)('0' + (Val % 10));
    Val /= 10;
  } while (Val != 0 && i < 23);
  for (j = i - 1; j >= 0; j--) {
    CHAR16 One[2];
    One[0] = Tmp[j];
    One[1] = 0;
    U16Cat (Dst, Cap, One);
  }
}

STATIC VOID U16CatHex (CHAR16 *Dst, UINTN Cap, UINT64 Val, UINTN Digits) {
  STATIC CONST CHAR16 Hex[] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F',0 };
  INTN i;
  for (i = (INTN)Digits - 1; i >= 0; i--) {
    CHAR16 One[2];
    One[0] = Hex[(Val >> (i * 4)) & 0xF];
    One[1] = 0;
    U16Cat (Dst, Cap, One);
  }
}

/* options UTF-8 → UCS-2（严格解码；非法字节/星面 → '?'；LoadOptions 短串，独立实现避免跨流依赖） */
STATIC VOID Utf8ToU16 (CONST CHAR8 *S, CHAR16 *Out, UINTN Cap) {
  CONST UINT8 *B = (CONST UINT8 *)S;
  UINTN       i = 0, o = 0;
  while (B[i] != 0 && o + 1 < Cap) {
    UINT8  b0 = B[i];
    UINT32 Cp;
    UINTN  n;
    if (b0 < 0x80) {
      Cp = b0; n = 1;
    } else if (b0 >= 0xC2 && b0 <= 0xDF && (B[i + 1] & 0xC0) == 0x80) {
      Cp = ((UINT32)(b0 & 0x1F) << 6) | (B[i + 1] & 0x3F); n = 2;
    } else if (b0 >= 0xE0 && b0 <= 0xEF && (B[i + 1] & 0xC0) == 0x80 && (B[i + 2] & 0xC0) == 0x80 &&
               (b0 != 0xE0 || B[i + 1] >= 0xA0) && (b0 != 0xED || B[i + 1] <= 0x9F)) {
      Cp = ((UINT32)(b0 & 0x0F) << 12) | ((UINT32)(B[i + 1] & 0x3F) << 6) | (B[i + 2] & 0x3F); n = 3;
    } else if (b0 >= 0xF0 && b0 <= 0xF4 && (B[i + 1] & 0xC0) == 0x80 && (B[i + 2] & 0xC0) == 0x80 &&
               (B[i + 3] & 0xC0) == 0x80 && (b0 != 0xF0 || B[i + 1] >= 0x90) && (b0 != 0xF4 || B[i + 1] <= 0x8F)) {
      Cp = ((UINT32)(b0 & 0x07) << 18) | ((UINT32)(B[i + 1] & 0x3F) << 12) |
           ((UINT32)(B[i + 2] & 0x3F) << 6) | (B[i + 3] & 0x3F);
      n = 4;
      if (Cp > 0xFFFF) {
        Cp = '?';                 /* UCS-2 不可表示星面 */
      }
    } else {
      Cp = '?'; n = 1;
    }
    Out[o++] = (CHAR16)Cp;
    i += n;
  }
  Out[o] = 0;
}

/* fsN: + / + Rel（Rel 无前导 /，S04 约定；反斜杠统一规范化为 /）→ 全限定路径 */
STATIC VOID BuildFull (CHAR16 *Dst, UINTN Cap, CONST CHAR16 *FsName, CONST CHAR16 *Rel) {
  UINTN i;
  U16Copy (Dst, Cap, FsName);
  U16Cat (Dst, Cap, U16("/"));
  for (i = 0; Rel[i] != 0; i++) {
    CHAR16 One[2];
    One[0] = (Rel[i] == '\\') ? '/' : Rel[i];
    One[1] = 0;
    U16Cat (Dst, Cap, One);
  }
}

/* ---- 菜单行构建 ---- */

STATIC CONST CHAR16 *DiscoveryRel (CONST LAUNCHER_CORE *C) {
  return (C->Init.CurrentArch == ARCH_X64_BIT) ? U16("EFI/BOOT/BOOTX64.EFI")
                                               : U16("EFI/BOOT/BOOTAA64.EFI");
}

STATIC VOID RebuildRows (LAUNCHER_CORE *C) {
  CONST CONFIG *Cfg = C->Init.Sess->Cfg;
  UINTN        n, i, k;
  MENU_ROW     *r;

  n = 5;                          /* 系统条目固定 5 */
  n += (Cfg != NULL) ? Cfg->Count : 1;   /* 工作条目；无配置 → +1 外部介质发现 */
  r = MemAlloc (n * sizeof (MENU_ROW));
  if (r == NULL) {
    return;                       /* OOM：保留旧行（降级不崩） */
  }
  k = 0;
  if (Cfg != NULL) {
    for (i = 0; i < Cfg->Count; i++) {
      CONST CONFIG_ITEM *It = &Cfg->Items[i];
      RESOLVED_PATH     P;
      r[k].Kind      = ROW_WORK;
      r[k].ItemIndex = i;
      r[k].Danger    = It->Danger;
      r[k].Disabled  = FALSE;
      r[k].Reason    = "";
      if (!PathArchMatch (It->ArchMask, C->Init.CurrentArch)) {
        r[k].Disabled = TRUE;
        r[k].Reason   = "arch mismatch";
      } else if (It->Action == ACTION_EXEC || It->Action == ACTION_BOOT) {
        /* shell/reset 动作无需 path（schema §6）；仅 exec/boot 校验路径 */
        if (EFI_ERROR (PathParse (It->Path, &P))) {
          r[k].Disabled = TRUE;
          r[k].Reason   = "bad path";
        } else if (P.Kind == PATH_DATA && C->Init.Sess->P3Fs == NULL) {
          r[k].Disabled = TRUE;
          r[k].Reason   = "P3 unavailable";
        }
      }
      k++;
    }
  } else {
    r[k].Kind = ROW_DISCOVERY; r[k].ItemIndex = 0; r[k].Danger = FALSE;
    r[k].Disabled = FALSE; r[k].Reason = "";
    k++;
  }
  r[k].Kind = ROW_SYS_FM;       r[k].ItemIndex = 0; r[k].Danger = FALSE;
  r[k].Disabled = (BOOLEAN)(C->Init.Sess->BootFs == NULL);
  r[k].Reason = "no boot ESP"; k++;
  r[k].Kind = ROW_SYS_EDITOR;   r[k].ItemIndex = 0; r[k].Danger = FALSE;
  r[k].Disabled = (BOOLEAN)(C->Init.Sess->BootFs == NULL);
  r[k].Reason = "no boot ESP"; k++;
  r[k].Kind = ROW_SYS_BOOTMENU; r[k].ItemIndex = 0; r[k].Danger = FALSE; r[k].Disabled = FALSE; r[k].Reason = ""; k++;
  r[k].Kind = ROW_SYS_SHELL;    r[k].ItemIndex = 0; r[k].Danger = FALSE; r[k].Disabled = FALSE; r[k].Reason = ""; k++;
  r[k].Kind = ROW_SYS_RESET;    r[k].ItemIndex = 0; r[k].Danger = FALSE; r[k].Disabled = FALSE; r[k].Reason = ""; k++;

  MemFree (C->Rows);
  C->Rows     = r;
  C->RowCount = k;
  C->Top      = 0;
  if (C->Sel >= k) {
    C->Sel = 0;
  }
  /* 选中行若灰显 → 移到首个可用行 */
  if (C->RowCount > 0 && C->Rows[C->Sel].Disabled) {
    for (i = 0; i < C->RowCount; i++) {
      if (!C->Rows[i].Disabled) {
        C->Sel = i;
        break;
      }
    }
  }
}

STATIC VOID RowLabel (CONST LAUNCHER_CORE *C, UINTN Idx, CHAR16 *Out, UINTN Cap) {
  CONST MENU_ROW *R = &C->Rows[Idx];
  Out[0] = 0;
  U16CatUint (Out, Cap, Idx + 1);
  U16Cat (Out, Cap, U16(". "));
  switch (R->Kind) {
    case ROW_WORK: {
      CONST CONFIG_ITEM *It = &C->Init.Sess->Cfg->Items[R->ItemIndex];
      U16CatAscii (Out, Cap, It->Name);
      if (It->Danger) {
        U16Cat (Out, Cap, U16(" !"));
      }
      break;
    }
    case ROW_SYS_FM:       U16Cat (Out, Cap, U16("FileManager")); break;
    case ROW_SYS_EDITOR:   U16Cat (Out, Cap, U16("Editor")); break;
    case ROW_SYS_BOOTMENU: U16Cat (Out, Cap, U16("NVRAM boot menu")); break;
    case ROW_SYS_SHELL:    U16Cat (Out, Cap, U16("Return to UEFI Shell")); break;
    case ROW_SYS_RESET:    U16Cat (Out, Cap, U16("Reboot")); break;
    default:               /* ROW_DISCOVERY */
      U16Cat (Out, Cap, U16("External media: "));
      U16Cat (Out, Cap, DiscoveryRel (C));
      break;
  }
  if (R->Disabled && R->Reason[0] != 0) {
    U16Cat (Out, Cap, U16(" ["));
    U16CatAscii (Out, Cap, R->Reason);
    U16Cat (Out, Cap, U16("]"));
  }
}

STATIC UINT8 RowAttr (CONST LAUNCHER_CORE *C, UINTN Idx) {
  if (Idx == C->Sel) {
    return TUI_ATTR_SELECTED;
  }
  if (C->Rows[Idx].Disabled) {
    return TUI_ATTR_DISABLED;
  }
  if (C->Rows[Idx].Danger) {
    return TUI_ATTR_DANGER;
  }
  return TUI_ATTR_NORMAL;
}

STATIC VOID RowDetail (CONST LAUNCHER_CORE *C, UINTN Idx, CHAR16 *Out, UINTN Cap) {
  CONST MENU_ROW *R = &C->Rows[Idx];
  Out[0] = 0;
  if (R->Disabled) {
    U16Cat (Out, Cap, U16("["));
    U16CatAscii (Out, Cap, R->Reason);
    U16Cat (Out, Cap, U16("]"));
    return;
  }
  switch (R->Kind) {
    case ROW_WORK: {
      CONST CONFIG_ITEM *It = &C->Init.Sess->Cfg->Items[R->ItemIndex];
      if (It->Desc[0] != 0) {
        U16CatAscii (Out, Cap, It->Desc);
      } else {
        U16CatAscii (Out, Cap, It->Id);
      }
      break;
    }
    case ROW_SYS_FM:       U16Cat (Out, Cap, U16("Browse volumes and execute firmware tools")); break;
    case ROW_SYS_EDITOR:   U16Cat (Out, Cap, U16("Edit scripts (UTF-8 byte-safe)")); break;
    case ROW_SYS_BOOTMENU: U16Cat (Out, Cap, U16("Pick a firmware boot entry (BitLocker: have recovery key ready)")); break;
    case ROW_SYS_SHELL:    U16Cat (Out, Cap, U16("Exit Launcher to the UEFI Shell prompt")); break;
    case ROW_SYS_RESET:    U16Cat (Out, Cap, U16("Cold reset the machine")); break;
    default:               U16Cat (Out, Cap, U16("Search other media for the fallback bootloader")); break;
  }
}

/* ---- 导航 ---- */

STATIC UINTN MenuVis (UINTN Rows) {
  return Rows > 5 ? Rows - 5 : 1;
}

STATIC VOID MoveSel (LAUNCHER_CORE *C, INTN Delta) {
  INTN i    = (INTN)C->Sel;
  INTN Step = (Delta > 0) ? 1 : -1;
  INTN Left = (Delta > 0) ? Delta : -Delta;
  while (Left > 0) {
    INTN j = i + Step;
    while (j >= 0 && j < (INTN)C->RowCount && C->Rows[j].Disabled) {
      j += Step;                  /* 跳过灰显 */
    }
    if (j < 0 || j >= (INTN)C->RowCount) {
      break;                      /* 端点钳制（不环绕） */
    }
    i = j;
    Left--;
  }
  C->Sel = (UINTN)i;
}

STATIC VOID SelEdge (LAUNCHER_CORE *C, BOOLEAN First) {
  UINTN i;
  if (C->RowCount == 0) {
    return;
  }
  if (First) {
    for (i = 0; i < C->RowCount; i++) {
      if (!C->Rows[i].Disabled) { C->Sel = i; return; }
    }
  } else {
    for (i = C->RowCount; i > 0; i--) {
      if (!C->Rows[i - 1].Disabled) { C->Sel = i - 1; return; }
    }
  }
}

/* ---- 状态转移 / 执行 ---- */

STATIC VOID EnterError (LAUNCHER_CORE *C, CONST CHAR16 *Msg) {
  U16Copy (C->ErrMsg, sizeof (C->ErrMsg) / sizeof (CHAR16), Msg);
  C->State = LSTATE_ERROR;
}

STATIC VOID EnterErrorStatus (LAUNCHER_CORE *C, CONST CHAR16 *Prefix, EFI_STATUS S) {
  CHAR16 Msg[256];
  U16Copy (Msg, 256, Prefix);
  U16Cat (Msg, 256, U16(" 0x"));
  U16CatHex (Msg, 256, (UINT64)S, 16);
  EnterError (C, Msg);
}

STATIC VOID EnterConfirm (LAUNCHER_CORE *C, UINTN Idx) {
  C->PendingRow = Idx;
  C->State      = LSTATE_CONFIRM;
}

STATIC VOID DoReset (LAUNCHER_CORE *C) {
  ExecReset ();
  C->Exit = LEXIT_RESET;          /* 真机不返回；mock 返回后 glue 直接退出 */
}

STATIC VOID DoExecEfi (LAUNCHER_CORE *C, FS_OPS *Fs, CONST CHAR16 *Full, CONST CHAR16 *Opts) {
  EFI_STATUS Ret = EFI_SUCCESS;
  EFI_STATUS S   = ExecEfi (Fs, Full, Opts, &Ret);
  if (EFI_ERROR (S)) {
    EnterErrorStatus (C, U16("Exec failed:"), S);
    return;
  }
  if (EFI_ERROR (Ret)) {
    EnterErrorStatus (C, U16("Image returned:"), Ret);
    return;
  }
  C->State = LSTATE_MENU;
}

STATIC VOID DoExecNsh (LAUNCHER_CORE *C, CONST CHAR16 *ScriptFull, CONST CHAR16 *Opts) {
  EFI_STATUS S;
  /* S10 约定：ShellFullPath = 启动卷根相对（ExecNsh 内部经自身 LoadedImage 定位启动卷，
     BuildFileDp 拒绝 ':'）；ScriptFullPath = fsN: 全限定（LoadOptions 用）。 */
  if (C->Init.ShellRel == NULL || C->Init.ShellRel[0] == 0) {
    EnterError (C, U16("Boot ESP shell path unknown"));
    return;
  }
  S = ExecNsh (C->Init.ShellRel, ScriptFull, Opts);
  if (EFI_ERROR (S)) {
    EnterErrorStatus (C, U16("Nested shell failed:"), S);
    return;
  }
  C->State = LSTATE_MENU;
}

STATIC VOID ExecCandidate (LAUNCHER_CORE *C, CONST EXEC_CANDIDATE *Cand, CONST CHAR16 *Opts, BOOLEAN IsScript) {
  FS_OPS *Fs = Cand->Fs;            /* 宿主 mock 路径：候选 Fs 有效 */
  CHAR16 FsName[16];
  CHAR16 Full[PATH_CAP];
  FsName[0] = 0;
  /* UEFI：S10 候选 Fs 借用自已 VolFree 的枚举集（悬空）→ 经 glue 在活 Vols 中解析 */
  if (C->Init.Ops != NULL && C->Init.Ops->AutoVol != NULL) {
    if (EFI_ERROR (C->Init.Ops->AutoVol (C->Init.Ops->Ctx, Cand->FullPath, Cand->Display,
                                         &Fs, FsName, 16))) {
      EnterError (C, U16("Candidate volume no longer available"));
      return;
    }
  }
  if (IsScript) {
    /* ExecNsh 需 fsN: 全限定：候选 FullPath 已含 ':' 直接使用；否则 FsName + 根相对 */
    if (EfiStrChr (Cand->FullPath, ':') != NULL) {
      DoExecNsh (C, Cand->FullPath, Opts);
    } else if (FsName[0] != 0) {
      CONST CHAR16 *Rel = Cand->FullPath;
      while (*Rel == '/' || *Rel == '\\') {
        Rel++;
      }
      BuildFull (Full, PATH_CAP, FsName, Rel);
      DoExecNsh (C, Full, Opts);
    } else {
      EnterError (C, U16("Candidate volume mapping unknown"));
    }
    return;
  }
  /* ExecEfi = 活 Fs + 根相对路径（S10 约定）；全限定形式仅宿主 mock 使用 */
  DoExecEfi (C, Fs, Cand->FullPath, Opts);
}

STATIC VOID ExecAuto (LAUNCHER_CORE *C, CONST CHAR16 *Rel, CONST CHAR16 *Opts, BOOLEAN IsScript) {
  EXEC_CANDIDATE *L = NULL;
  UINTN          N = 0;
  EFI_STATUS     S;
  S = ExecAutoCandidates (Rel, C->Init.BootDp, C->Init.BootDpLen, &L, &N);
  if (EFI_ERROR (S) || N == 0) {
    EnterError (C, U16("No external media found with that file"));
    return;
  }
  if (N == 1) {
    EXEC_CANDIDATE One = L[0];
    ExecFreeCandidates (L, N);
    ExecCandidate (C, &One, Opts, IsScript);
    return;
  }
  C->Cands       = L;
  C->CandCount   = N;
  C->CandSel     = 0;
  C->CandIsScript = IsScript;
  U16Copy (C->CandOpts, 256, Opts);
  C->State       = LSTATE_CANDIDATES;
}

STATIC VOID EnterBootMenu (LAUNCHER_CORE *C) {
  LAUNCHER_BOOT_ENTRY *L = NULL;
  UINTN               N = 0;
  if (C->Init.Ops == NULL || C->Init.Ops->BootEntries == NULL ||
      EFI_ERROR (C->Init.Ops->BootEntries (C->Init.Ops->Ctx, &L, &N)) || N == 0) {
    EnterError (C, U16("No active NVRAM boot entries"));
    return;
  }
  C->BootList  = L;
  C->BootCount = N;
  C->BootSel   = 0;
  C->State     = LSTATE_BOOT_MENU;
}

STATIC VOID ExecResolved (LAUNCHER_CORE *C, CONST RESOLVED_PATH *P, CONST CHAR16 *Opts) {
  CHAR16 Full[PATH_CAP];
  switch (P->Kind) {
    case PATH_DATA:
      if (C->Init.Sess->P3Fs == NULL) {
        EnterError (C, U16("P3 data volume unavailable"));
        return;
      }
      if (P->IsScript) {
        if (C->Init.Sess->P3FsName[0] == 0) {
          EnterError (C, U16("P3 volume mapping unknown"));
          return;
        }
        BuildFull (Full, PATH_CAP, C->Init.Sess->P3FsName, P->Rel);
        DoExecNsh (C, Full, Opts);
      } else {
        DoExecEfi (C, C->Init.Sess->P3Fs, P->Rel, Opts);   /* 活 Fs + 根相对（S10 约定） */
      }
      return;
    case PATH_FS:
      if (P->IsScript) {
        U16Copy (Full, PATH_CAP, U16("fs"));
        U16CatUint (Full, PATH_CAP, P->FsIndex);
        U16Cat (Full, PATH_CAP, U16(":/"));
        U16Cat (Full, PATH_CAP, P->Rel);
        DoExecNsh (C, Full, Opts);
      } else {
        FS_OPS *Fs = NULL;
        if (C->Init.Ops == NULL || C->Init.Ops->FsByIndex == NULL ||
            EFI_ERROR (C->Init.Ops->FsByIndex (C->Init.Ops->Ctx, P->FsIndex, &Fs)) || Fs == NULL) {
          EnterError (C, U16("Explicit fs volume not found"));
          return;
        }
        DoExecEfi (C, Fs, P->Rel, Opts);
      }
      return;
    case PATH_AUTO:
      ExecAuto (C, P->Rel, Opts, P->IsScript);
      return;
    default:                        /* PATH_BOOT */
      if (P->BootMenu) {
        EnterBootMenu (C);
      } else {
        ExecBootNext (P->BootNumber);
        C->Exit = LEXIT_RESET;
      }
      return;
  }
}

STATIC VOID ExecWorkItem (LAUNCHER_CORE *C, CONST CONFIG_ITEM *It) {
  RESOLVED_PATH P;
  CHAR16        Opts[256];
  /* shell/reset 无 path（schema §6），先分流动作再解析路径 */
  if (It->Action == ACTION_SHELL) {
    C->Exit = LEXIT_SHELL;
    return;
  }
  if (It->Action == ACTION_RESET) {
    DoReset (C);
    return;
  }
  if (EFI_ERROR (PathParse (It->Path, &P))) {
    EnterError (C, U16("Invalid path"));
    return;
  }
  Utf8ToU16 (It->Options, Opts, 256);
  if (It->Action == ACTION_BOOT) {
    if (P.BootMenu) {
      EnterBootMenu (C);
    } else {
      ExecBootNext (P.BootNumber);
      C->Exit = LEXIT_RESET;
    }
    return;
  }
  ExecResolved (C, &P, Opts);     /* ACTION_EXEC */
}

STATIC VOID ExecApp (LAUNCHER_CORE *C, CONST CHAR16 *AppRel) {
  if (C->Init.Sess->BootFs == NULL || AppRel == NULL) {
    EnterError (C, U16("Boot ESP volume unknown"));
    return;
  }
  DoExecEfi (C, C->Init.Sess->BootFs, AppRel, U16(""));   /* 活 Fs + 根相对（S10 约定） */
}

STATIC VOID Activate (LAUNCHER_CORE *C, UINTN Idx) {
  MENU_ROW *R;
  if (Idx >= C->RowCount) {
    return;
  }
  R = &C->Rows[Idx];
  if (R->Disabled) {
    return;
  }
  switch (R->Kind) {
    case ROW_SYS_SHELL:
      C->Exit = LEXIT_SHELL;
      return;
    case ROW_SYS_RESET:
      EnterConfirm (C, Idx);
      return;
    case ROW_SYS_BOOTMENU:
      EnterBootMenu (C);
      return;
    case ROW_SYS_FM:
      ExecApp (C, C->Init.FileManagerRel);
      return;
    case ROW_SYS_EDITOR:
      ExecApp (C, C->Init.EditorRel);
      return;
    case ROW_DISCOVERY: {
      CHAR16 Opts[2];
      Opts[0] = 0;
      ExecAuto (C, DiscoveryRel (C), Opts, FALSE);
      return;
    }
    default: {                      /* ROW_WORK */
      CONST CONFIG_ITEM *It = &C->Init.Sess->Cfg->Items[R->ItemIndex];
      if (It->Danger || It->Confirm || It->Action == ACTION_RESET) {
        EnterConfirm (C, Idx);      /* danger 隐含 confirm；reset 动作强制确认（§5.6） */
      } else {
        ExecWorkItem (C, It);
      }
      return;
    }
  }
}

STATIC VOID ExecutePending (LAUNCHER_CORE *C) {
  MENU_ROW *R = &C->Rows[C->PendingRow];
  if (R->Kind == ROW_SYS_RESET) {
    DoReset (C);
  } else {
    ExecWorkItem (C, &C->Init.Sess->Cfg->Items[R->ItemIndex]);
  }
}

STATIC VOID DoReload (LAUNCHER_CORE *C) {
  if (C->Init.Ops == NULL || C->Init.Ops->Reload == NULL) {
    EnterError (C, U16("Reload unavailable"));
    return;
  }
  if (EFI_ERROR (C->Init.Ops->Reload (C->Init.Ops->Ctx))) {
    EnterError (C, U16("Reload failed"));
    return;
  }
  RebuildRows (C);                  /* Sess 由 glue 原地更新 */
  C->State = LSTATE_MENU;
}

/* ---- 按键处理 ---- */

STATIC VOID HandleMenuKey (LAUNCHER_CORE *C, TUI_COMMAND Cmd, CHAR16 Ch) {
  UINTN Vis, Rows = 25, Cols = 80;
  TuiGetSize (&Rows, &Cols);
  Vis = MenuVis (Rows);
  switch (Cmd) {
    case CMD_UP:    MoveSel (C, -1); return;
    case CMD_DOWN:  MoveSel (C, 1);  return;
    case CMD_PGUP:  MoveSel (C, -(INTN)Vis); return;
    case CMD_PGDN:  MoveSel (C, (INTN)Vis);  return;
    case CMD_HOME:  SelEdge (C, TRUE);  return;
    case CMD_END:   SelEdge (C, FALSE); return;
    case CMD_ENTER: Activate (C, C->Sel); return;
    case CMD_ESC:   C->Exit = LEXIT_SHELL; return;
    case CMD_CHAR:
      if (Ch >= '1' && Ch <= '9') {
        UINTN Idx = (UINTN)(Ch - '1');
        if (Idx < C->RowCount && !C->Rows[Idx].Disabled) {
          C->Sel = Idx;
        }
        return;
      }
      if (Ch == 's' || Ch == 'S' || Ch == 'q' || Ch == 'Q') {
        C->Exit = LEXIT_SHELL;
        return;
      }
      if (Ch == 'r' || Ch == 'R') {
        DoReload (C);
        return;
      }
      if (Ch == '?') {
        C->State = LSTATE_HELP;
        return;
      }
      return;
    default:
      return;
  }
}

STATIC VOID HandleConfirmKey (LAUNCHER_CORE *C, TUI_COMMAND Cmd, CHAR16 Ch) {
  if (Cmd == CMD_CHAR && (Ch == 'y' || Ch == 'Y')) {
    ExecutePending (C);
    return;
  }
  if (Cmd == CMD_ESC || Cmd == CMD_ENTER ||
      (Cmd == CMD_CHAR && (Ch == 'n' || Ch == 'N'))) {
    C->State = LSTATE_MENU;         /* 默认 N */
  }
}

STATIC VOID HandleBootKey (LAUNCHER_CORE *C, TUI_COMMAND Cmd, CHAR16 Ch) {
  if (Cmd == CMD_UP && C->BootSel > 0) {
    C->BootSel--;
  } else if (Cmd == CMD_DOWN && C->BootSel + 1 < C->BootCount) {
    C->BootSel++;
  } else if (Cmd == CMD_CHAR && Ch >= '1' && Ch <= '9') {
    UINTN Idx = (UINTN)(Ch - '1');
    if (Idx < C->BootCount) {
      C->BootSel = Idx;
    }
  } else if (Cmd == CMD_ENTER) {
    UINT16 Num = C->BootList[C->BootSel].BootNumber;
    MemFree (C->BootList);
    C->BootList  = NULL;
    C->BootCount = 0;
    ExecBootNext (Num);
    C->Exit = LEXIT_RESET;
  } else if (Cmd == CMD_ESC) {
    MemFree (C->BootList);
    C->BootList  = NULL;
    C->BootCount = 0;
    C->State     = LSTATE_MENU;
  }
}

STATIC VOID HandleCandKey (LAUNCHER_CORE *C, TUI_COMMAND Cmd, CHAR16 Ch) {
  if (Cmd == CMD_UP && C->CandSel > 0) {
    C->CandSel--;
  } else if (Cmd == CMD_DOWN && C->CandSel + 1 < C->CandCount) {
    C->CandSel++;
  } else if (Cmd == CMD_CHAR && Ch >= '1' && Ch <= '9') {
    UINTN Idx = (UINTN)(Ch - '1');
    if (Idx < C->CandCount) {
      C->CandSel = Idx;
    }
  } else if (Cmd == CMD_ENTER) {
    EXEC_CANDIDATE One = C->Cands[C->CandSel];
    ExecFreeCandidates (C->Cands, C->CandCount);
    C->Cands     = NULL;
    C->CandCount = 0;
    /* 候选脚本判定按原始请求路径后缀（ExecAuto 传入）；候选 FullPath 已全限定 */
    ExecCandidate (C, &One, C->CandOpts, C->CandIsScript);
  } else if (Cmd == CMD_ESC) {
    ExecFreeCandidates (C->Cands, C->CandCount);
    C->Cands     = NULL;
    C->CandCount = 0;
    C->State     = LSTATE_MENU;
  }
}

EFI_STATUS LauncherCoreHandleKey (LAUNCHER_CORE *C, CONST TUI_KEY *Key) {
  TUI_COMMAND Cmd;
  if (C == NULL || Key == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Cmd = TuiKeyToCommand (Key);
  switch (C->State) {
    case LSTATE_COUNTDOWN:
      if (Cmd == CMD_ESC) {
        C->Exit = LEXIT_SHELL;
      } else if (Cmd != CMD_NONE) {
        C->State = LSTATE_MENU;     /* 任意其他键跳过倒计时 */
      }
      break;
    case LSTATE_MENU:
      HandleMenuKey (C, Cmd, Key->UnicodeChar);
      break;
    case LSTATE_CONFIRM:
      HandleConfirmKey (C, Cmd, Key->UnicodeChar);
      break;
    case LSTATE_ERROR:
      if (Cmd != CMD_NONE) {
        C->State = LSTATE_MENU;
      }
      break;
    case LSTATE_HELP:
      if (Cmd == CMD_ESC || Cmd == CMD_ENTER ||
          (Cmd == CMD_CHAR && Key->UnicodeChar == '?')) {
        C->State = LSTATE_MENU;
      }
      break;
    case LSTATE_BOOT_MENU:
      HandleBootKey (C, Cmd, Key->UnicodeChar);
      break;
    case LSTATE_CANDIDATES:
      HandleCandKey (C, Cmd, Key->UnicodeChar);
      break;
    default:
      break;
  }
  return EFI_SUCCESS;
}

EFI_STATUS LauncherCoreTick (LAUNCHER_CORE *C) {
  if (C == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (C->State == LSTATE_COUNTDOWN) {
    if (C->Countdown > 0) {
      C->Countdown--;
    }
    if (C->Countdown == 0) {
      C->State = LSTATE_MENU;
    }
  }
  return EFI_SUCCESS;
}

/* ---- 渲染 ---- */

STATIC VOID DrawStatusBar (LAUNCHER_CORE *C, UINTN Rows, UINTN Cols) {
  CHAR16 Sb[LABEL_CAP];
  CONST CONFIG *Cfg = C->Init.Sess->Cfg;
  Sb[0] = 0;
  U16Cat (Sb, LABEL_CAP, U16("EasyEFI v"));
  U16CatAscii (Sb, LABEL_CAP, (Cfg != NULL && Cfg->Version[0] != 0) ? Cfg->Version : "1.0");
  U16Cat (Sb, LABEL_CAP, U16(" | "));
  U16Cat (Sb, LABEL_CAP, (C->Init.CurrentArch == ARCH_X64_BIT) ? U16("x64") : U16("AA64"));
  U16Cat (Sb, LABEL_CAP, U16(" | "));
  U16CatAscii (Sb, LABEL_CAP, C->Init.SbStatus != NULL ? C->Init.SbStatus : "SB:?");
  U16Cat (Sb, LABEL_CAP, U16(" | P3:"));
  U16Cat (Sb, LABEL_CAP, C->Init.Sess->P3Fs != NULL ? U16("OK") : U16("DEGRADED"));
  U16Cat (Sb, LABEL_CAP, U16(" | "));
  U16CatUint (Sb, LABEL_CAP, Cols);
  U16Cat (Sb, LABEL_CAP, U16("x"));
  U16CatUint (Sb, LABEL_CAP, Rows);
  TuiFill (0, 0, Cols, 1, ' ', TUI_ATTR_STATUSBAR);
  TuiPutText (0, 0, Sb, TUI_ATTR_STATUSBAR);
}

STATIC VOID DrawTitleBanner (LAUNCHER_CORE *C, UINTN Cols) {
  CHAR16       T[LABEL_CAP];
  CONST CONFIG *Cfg = C->Init.Sess->Cfg;
  CONST CHAR8  *Ban;
  AsciiToU16 ((Cfg != NULL && Cfg->Title[0] != 0) ? Cfg->Title : "EasyEFI Launcher", T, LABEL_CAP);
  TuiPutText (1, 0, T, TUI_ATTR_TITLE);
  Ban = C->Init.Sess->Banner;
  if (Ban[0] == 0 && C->Init.Sess->P3Fs == NULL) {
    Ban = "DATA VOLUME UNAVAILABLE - NTFS driver failed or P3 missing";
  }
  if (Ban[0] != 0) {
    CHAR16 B[256];
    AsciiToU16 (Ban, B, 256);
    TuiFill (2, 0, Cols, 1, ' ', TUI_ATTR_BANNER_ERR);
    TuiPutText (2, 0, B, TUI_ATTR_BANNER_ERR);
  }
}

STATIC VOID DrawKeybar (UINTN Rows, UINTN Cols, CONST CHAR16 *Text) {
  if (Rows == 0) {
    return;
  }
  TuiFill (Rows - 1, 0, Cols, 1, ' ', TUI_ATTR_KEYBAR);
  TuiPutText (Rows - 1, 0, Text, TUI_ATTR_KEYBAR);
}

STATIC VOID DrawMenu (LAUNCHER_CORE *C, UINTN Rows, UINTN Cols) {
  UINTN  Vis, i, Row;
  CHAR16 Label[LABEL_CAP];
  DrawTitleBanner (C, Cols);
  Vis = MenuVis (Rows);
  if (C->RowCount > Vis) {
    UINTN Want = (C->Sel > Vis / 2) ? C->Sel - Vis / 2 : 0;
    C->Top = (Want + Vis <= C->RowCount) ? Want : C->RowCount - Vis;
  } else {
    C->Top = 0;
  }
  for (i = 0; i < Vis && C->Top + i < C->RowCount; i++) {
    UINTN Idx = C->Top + i;
    UINT8 Attr = RowAttr (C, Idx);
    Row = 3 + i;
    if (Row + 2 >= Rows) {
      break;                      /* 小屏保护：不侵/detail/keybar */
    }
    if (Attr == TUI_ATTR_SELECTED) {
      TuiFill (Row, 0, Cols, 1, ' ', Attr);
    }
    RowLabel (C, Idx, Label, LABEL_CAP);
    TuiPutText (Row, 1, Label, Attr);
  }
  if (Rows >= 2 && C->RowCount > 0) {
    CHAR16 D[LABEL_CAP];
    RowDetail (C, C->Sel, D, LABEL_CAP);
    TuiPutText (Rows - 2, 1, D, C->Rows[C->Sel].Disabled ? TUI_ATTR_DISABLED : TUI_ATTR_NORMAL);
  }
  DrawKeybar (Rows, Cols, U16("Up/Dn/1-9:Select Enter:Run S:Shell R:Reload ?:Help"));
}

STATIC VOID DrawCountdown (LAUNCHER_CORE *C, UINTN Rows, UINTN Cols) {
  CHAR16 T[LABEL_CAP];
  DrawTitleBanner (C, Cols);
  T[0] = 0;
  U16Cat (T, LABEL_CAP, U16("Auto-start in "));
  U16CatUint (T, LABEL_CAP, C->Countdown);
  U16Cat (T, LABEL_CAP, U16(" s"));
  TuiPutText (4, 2, T, TUI_ATTR_TITLE);
  TuiPutText (6, 2, U16("ESC = UEFI Shell    any other key = menu"), TUI_ATTR_NORMAL);
  DrawKeybar (Rows, Cols, U16("ESC:Shell  any key:Menu"));
}

STATIC VOID PendingName (LAUNCHER_CORE *C, CHAR16 *Out, UINTN Cap) {
  MENU_ROW *R = &C->Rows[C->PendingRow];
  Out[0] = 0;
  if (R->Kind == ROW_WORK) {
    U16CatAscii (Out, Cap, C->Init.Sess->Cfg->Items[R->ItemIndex].Name);
  } else {
    U16Cat (Out, Cap, U16("Reboot"));
  }
}

STATIC VOID DrawConfirm (LAUNCHER_CORE *C, UINTN Rows, UINTN Cols) {
  CHAR16   Name[LABEL_CAP];
  MENU_ROW *R = &C->Rows[C->PendingRow];
  BOOLEAN  Danger = (R->Kind == ROW_WORK) ? C->Init.Sess->Cfg->Items[R->ItemIndex].Danger : FALSE;
  PendingName (C, Name, LABEL_CAP);
  TuiPutText (1, 0, U16("CONFIRM"), TUI_ATTR_TITLE);
  TuiPutText (4, 2, Name, Danger ? TUI_ATTR_DANGER : TUI_ATTR_NORMAL);
  if (Danger) {
    TuiPutText (6, 2, U16("This action is marked DANGEROUS."), TUI_ATTR_DANGER);
    TuiPutText (7, 2, U16("It may write firmware or change boot state."), TUI_ATTR_NORMAL);
  } else if (R->Kind == ROW_SYS_RESET) {
    TuiPutText (6, 2, U16("Cold reset the machine?"), TUI_ATTR_NORMAL);
  } else {
    TuiPutText (6, 2, U16("Execute this action?"), TUI_ATTR_NORMAL);
  }
  TuiPutText (9, 2, U16("[Y] Yes    [N] No    (default: N)"), TUI_ATTR_NORMAL);
  DrawKeybar (Rows, Cols, U16("Y:Yes  N/ESC/Enter:Cancel (default N)"));
}

STATIC VOID DrawError (LAUNCHER_CORE *C, UINTN Rows, UINTN Cols) {
  TuiPutText (1, 0, U16("ERROR"), TUI_ATTR_DANGER);
  TuiPutText (4, 2, C->ErrMsg, TUI_ATTR_DANGER);
  TuiPutText (6, 2, U16("Press any key to return to menu."), TUI_ATTR_NORMAL);
  DrawKeybar (Rows, Cols, U16("any key:Menu"));
}

STATIC VOID DrawBootMenu (LAUNCHER_CORE *C, UINTN Rows, UINTN Cols) {
  UINTN  Vis, i;
  CHAR16 Line[LABEL_CAP];
  TuiPutText (1, 0, U16("NVRAM Boot Entries"), TUI_ATTR_TITLE);
  Vis = MenuVis (Rows);
  for (i = 0; i < Vis && i < C->BootCount; i++) {
    UINTN Row = 3 + i;
    UINT8 Attr = (i == C->BootSel) ? TUI_ATTR_SELECTED : TUI_ATTR_NORMAL;
    if (Row + 2 >= Rows) {
      break;
    }
    Line[0] = 0;
    U16CatHex (Line, LABEL_CAP, C->BootList[i].BootNumber, 4);
    U16Cat (Line, LABEL_CAP, U16(" "));
    U16Cat (Line, LABEL_CAP, C->BootList[i].Description);
    if (Attr == TUI_ATTR_SELECTED) {
      TuiFill (Row, 0, Cols, 1, ' ', Attr);
    }
    TuiPutText (Row, 1, Line, Attr);
  }
  DrawKeybar (Rows, Cols, U16("Up/Dn/1-9:Select Enter:Boot ESC:Back"));
}

STATIC VOID DrawCandidates (LAUNCHER_CORE *C, UINTN Rows, UINTN Cols) {
  UINTN Vis, i;
  TuiPutText (1, 0, U16("Select external media"), TUI_ATTR_TITLE);
  Vis = MenuVis (Rows);
  for (i = 0; i < Vis && i < C->CandCount; i++) {
    UINTN Row = 3 + i;
    UINT8 Attr = (i == C->CandSel) ? TUI_ATTR_SELECTED : TUI_ATTR_NORMAL;
    if (Row + 2 >= Rows) {
      break;
    }
    if (Attr == TUI_ATTR_SELECTED) {
      TuiFill (Row, 0, Cols, 1, ' ', Attr);
    }
    TuiPutText (Row, 1, C->Cands[i].Display, Attr);
  }
  DrawKeybar (Rows, Cols, U16("Up/Dn:Select Enter:Run ESC:Back"));
}

STATIC VOID DrawHelp (LAUNCHER_CORE *C, UINTN Rows, UINTN Cols) {
  UINTN  Row = 3, i;
  CHAR16 Line[256];
  TuiPutText (1, 0, U16("Help"), TUI_ATTR_TITLE);
  TuiPutText (Row++, 1, U16("Keys: Up/Dn/1-9 select  Enter run  S/Q/ESC Shell  R reload"), TUI_ATTR_NORMAL);
  if (Row + 1 >= Rows) { DrawKeybar (Rows, Cols, U16("ESC:Return")); return; }
  TuiPutText (Row++, 1, U16("Warnings:"), TUI_ATTR_TITLE);
  TuiPutText (Row++, 1, U16(" - fsN: volume numbers may drift (prefer data:/auto:)"), TUI_ATTR_NORMAL);
  if (Row + 1 >= Rows) { DrawKeybar (Rows, Cols, U16("ESC:Return")); return; }
  TuiPutText (Row++, 1, U16(" - boot: may trigger BitLocker recovery; have key ready"), TUI_ATTR_NORMAL);
  if (C->Init.Sess->ErrCount > 0 && Row + 1 < Rows) {
    TuiPutText (Row++, 1, U16("Skipped config items:"), TUI_ATTR_TITLE);
    for (i = 0; i < C->Init.Sess->ErrCount && Row + 1 < Rows; i++) {
      CONST CONFIG_ERROR *E = &C->Init.Sess->Errs[i];
      Line[0] = 0;
      U16Cat (Line, 256, U16(" - "));
      if (E->ItemIndex == (UINTN)-1) {
        U16Cat (Line, 256, U16("top"));
      } else {
        U16Cat (Line, 256, U16("item "));
        U16CatUint (Line, 256, E->ItemIndex);
      }
      U16Cat (Line, 256, U16("."));
      U16CatAscii (Line, 256, E->Field);
      U16Cat (Line, 256, U16(": "));
      U16CatAscii (Line, 256, E->Msg);
      TuiPutText (Row++, 1, Line, TUI_ATTR_NORMAL);
    }
  }
  if (C->Init.Sess->WarnCount > 0 && Row + 1 < Rows) {
    TuiPutText (Row++, 1, U16("Config warnings:"), TUI_ATTR_TITLE);
    for (i = 0; i < C->Init.Sess->WarnCount && Row + 1 < Rows; i++) {
      CONST CONFIG_ERROR *W = &C->Init.Sess->Warns[i];
      Line[0] = 0;
      U16Cat (Line, 256, U16(" - "));
      U16CatAscii (Line, 256, W->Field);
      U16Cat (Line, 256, U16(": "));
      U16CatAscii (Line, 256, W->Msg);
      TuiPutText (Row++, 1, Line, TUI_ATTR_NORMAL);
    }
  }
  DrawKeybar (Rows, Cols, U16("ESC/Enter:Return"));
}

EFI_STATUS LauncherCoreDraw (LAUNCHER_CORE *C) {
  UINTN Rows = 0, Cols = 0;
  if (C == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  TuiGetSize (&Rows, &Cols);
  TuiClear ();
  DrawStatusBar (C, Rows, Cols);
  switch (C->State) {
    case LSTATE_COUNTDOWN:  DrawCountdown (C, Rows, Cols);  break;
    case LSTATE_MENU:       DrawMenu (C, Rows, Cols);       break;
    case LSTATE_CONFIRM:    DrawConfirm (C, Rows, Cols);    break;
    case LSTATE_ERROR:      DrawError (C, Rows, Cols);      break;
    case LSTATE_BOOT_MENU:  DrawBootMenu (C, Rows, Cols);   break;
    case LSTATE_CANDIDATES: DrawCandidates (C, Rows, Cols); break;
    case LSTATE_HELP:       DrawHelp (C, Rows, Cols);       break;
    default:                                                break;
  }
  return TuiFlush ();
}

/* ---- 生命周期 / 查询 ---- */

LAUNCHER_CORE *LauncherCoreNew (CONST LAUNCHER_INIT *Init) {
  LAUNCHER_CORE *C;
  if (Init == NULL || Init->Sess == NULL) {
    return NULL;
  }
  C = MemAlloc (sizeof (LAUNCHER_CORE));
  if (C == NULL) {
    return NULL;
  }
  C->Init      = *Init;
  C->Countdown = Init->SkipTimeout;
  C->State     = Init->SkipTimeout > 0 ? LSTATE_COUNTDOWN : LSTATE_MENU;
  RebuildRows (C);
  return C;
}

VOID LauncherCoreFree (LAUNCHER_CORE *C) {
  if (C == NULL) {
    return;
  }
  MemFree (C->Rows);
  MemFree (C->BootList);
  if (C->Cands != NULL) {
    ExecFreeCandidates (C->Cands, C->CandCount);
  }
  MemFree (C);
}

LAUNCHER_STATE LauncherCoreState (CONST LAUNCHER_CORE *C) {
  return C != NULL ? C->State : LSTATE_MENU;
}

LAUNCHER_EXIT LauncherCoreExit (CONST LAUNCHER_CORE *C) {
  return C != NULL ? C->Exit : LEXIT_NONE;
}

UINTN LauncherCoreSelected (CONST LAUNCHER_CORE *C) {
  return C != NULL ? C->Sel : 0;
}

UINTN LauncherCoreCountdown (CONST LAUNCHER_CORE *C) {
  return C != NULL ? C->Countdown : 0;
}
