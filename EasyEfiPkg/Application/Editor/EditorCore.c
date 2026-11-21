/** @file
  EditorCore — S13 Editor 核心（纯逻辑；design §7.3 脚本导向 MVP，范围冻结）。

  只 include Contract 契约头 + EditorCore.h；内存 MemAlloc/MemFree；U16/EfiStr* 纪律。
  屏幕/按键经 TuiScreen/TuiInput 契约缝（宿主 MockTui，UEFI S06/S07）；
  文件经 FS_OPS vtable（宿主 MockFs，UEFI S09）；编辑语义全部委托 EditorModelLib（S02）。

  布局（Rows×Cols，保底 80×25）：行号栏 7 列（%6u+空格）+ 文本区；
  倒数第 2 行状态栏（文件名+*+L:C+UTF-8+BOM+光标 U+XXXX/RAW+消息）；末行键位条。
  光标 = 反显单元格（TUI_ATTR_SELECTED；契约无光标定位 API）。
  非 ASCII 占位黄色由 ModelRenderRow 投影（TUI_NONASCII_CHAR+TUI_ATTR_NONASCII）。

  键位（design §7.3 功能集的实现映射，记录于交接）：
    编辑态：可打印字符插入 / Enter 换行 / Backspace / Delete / 方向 / PgUp / PgDn /
            Home / End（ModelMove ±BIG 钳制语义）/
            F2 保存（无文件→另存为）/ F3 查找输入（needle 保留，ENTER=下一命中）/ F4 上一命中 /
            F5 另存为 / F6 跳行 / F7 撤销 / F10 或 ESC 退出（修改→三键确认）。
    查找输入态：字符=needle（含非 ASCII CP）/ TAB=大小写开关 / Enter=执行 / ESC=取消。
    跳行输入态：数字 / Enter / ESC。另存为输入态：ASCII only / Enter / ESC。
    确认态：S=保存并退出 D=放弃退出 ESC=取消。错误页：任意键（致命→退出，否则回编辑）。

  保存流程：ModelSave → Info 拒目录 → Delete（NOT_FOUND 忽略）→ Open(WRITE|CREATE)
  → Write → Flush → Close。三后端 Open 均不截断且契约无 Truncate，故删旧重建保证
  内容精确。写失败置 SaveFailed 标志：显示 Modified = ModelModified || SaveFailed
  （模型保存点已随 ModelSave 前移，磁盘内容却未更新——标志补偿，直到下次成功保存/打开）。

  查找：ModelFind 全量命中（升序）；下一=光标后首个命中（回绕），上一=光标前末个（回绕）；
  任何编辑/撤销使缓存命中失效（需重新 F3）。大小写开关仅 ASCII 折叠（模型语义）。
**/
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/TuiScreen.h"
#include "Contract/TuiInput.h"
#include "Contract/EditorModel.h"
#include "EditorCore.h"

#define ED_GUTTER     7      /* 行号栏宽：%6u + 空格 */
#define ED_MAX_PATH   256
#define ED_MAX_INPUT  256
#define ED_MAX_MSG    128
#define ED_MAX_COLS   256
#define ED_MAX_ROWS   128
#define ED_BIG        0x7FFFFFFF   /* ModelMove 钳制到行首/行尾 */

struct EDITOR_APP {
  FS_OPS       *Fs;
  EDITOR_MODEL *Model;
  UINTN         MaxBytes;
  CHAR16        Path[ED_MAX_PATH];
  BOOLEAN       HasFile;
  ED_STATE      State;
  UINTN         Rows, Cols;
  UINTN         TextRows, TextCols;
  UINTN         TopLine, LeftCol;
  UINT32        Needle[ED_MAX_INPUT];
  UINTN         NeedleLen;
  BOOLEAN       CaseSens;
  MODEL_HIT    *Hits;
  UINTN         HitCount;
  CHAR16        Input[ED_MAX_INPUT];   /* 跳行数字 / 另存为路径 */
  UINTN         InputLen;
  CHAR16        Msg[ED_MAX_MSG];
  BOOLEAN       ErrorFatal;
  BOOLEAN       SaveFailed;            /* 保存失败 → 磁盘与模型保存点不一致 */
  BOOLEAN       QuitAfterSave;         /* 确认态 S → 另存为成功后退出 */
  UINTN         ExitCode;
};

/* ---- 字符串助手（CHAR16 有界；禁 wcs*） ---- */

STATIC VOID StrCopy (CHAR16 *Dst, CONST CHAR16 *Src, UINTN Cap) {
  UINTN i = 0;
  if (Cap == 0) return;
  while (Src[i] && i + 1 < Cap) { Dst[i] = Src[i]; i++; }
  Dst[i] = 0;
}

STATIC VOID StrCat (CHAR16 *Dst, CONST CHAR16 *Src, UINTN Cap) {
  UINTN n = EfiStrLen (Dst), i = 0;
  while (Src[i] && n + 1 < Cap) { Dst[n++] = Src[i++]; }
  Dst[n] = 0;
}

STATIC VOID StrCatCh (CHAR16 *Dst, CHAR16 Ch, UINTN Cap) {
  UINTN n = EfiStrLen (Dst);
  if (n + 1 < Cap) { Dst[n] = Ch; Dst[n + 1] = 0; }
}

STATIC VOID CatDec (CHAR16 *Dst, UINTN Cap, UINTN Val) {
  CHAR16 Tmp[24];
  UINTN n = 0, i;
  do { Tmp[n++] = (CHAR16)('0' + (Val % 10)); Val /= 10; } while (Val && n < 24);
  for (i = n; i > 0; i--) StrCatCh (Dst, Tmp[i - 1], Cap);
}

STATIC VOID CatHex (CHAR16 *Dst, UINTN Cap, UINTN Val, UINTN MinDigits) {
  CHAR16 Tmp[24];
  UINTN n = 0, i, D;
  do {
    D = Val & 0xF;
    Tmp[n++] = (CHAR16)(D < 10 ? '0' + D : 'A' + D - 10);
    Val >>= 4;
  } while (Val && n < 24);
  while (n < MinDigits && n < 24) Tmp[n++] = (CHAR16)'0';
  for (i = n; i > 0; i--) StrCatCh (Dst, Tmp[i - 1], Cap);
}

STATIC VOID MsgSet (EDITOR_APP *E, CONST CHAR16 *S) {
  StrCopy (E->Msg, S, ED_MAX_MSG);
}

STATIC BOOLEAN IsModified (EDITOR_APP *E) {
  return (BOOLEAN)(ModelModified (E->Model) || E->SaveFailed);
}

STATIC VOID Done (EDITOR_APP *E, UINTN Code) {
  E->State = ED_DONE;
  E->ExitCode = Code;
}

STATIC VOID EdError (EDITOR_APP *E, CONST CHAR16 *What, BOOLEAN Fatal, UINTN Code) {
  MsgSet (E, What);
  E->ErrorFatal = Fatal;
  E->ExitCode = Code;
  E->State = ED_ERROR;
}

STATIC VOID InvalidateHits (EDITOR_APP *E) {
  ModelFreeHits (E->Hits);
  E->Hits = NULL;
  E->HitCount = 0;
}

/* ---- 生命周期 ---- */

EDITOR_APP *EdNew (FS_OPS *Fs, UINTN MaxBytes, UINTN UndoDepth) {
  EDITOR_APP *E;
  UINTN Rows = 25, Cols = 80;
  if (!Fs || MaxBytes == 0) return NULL;
  E = MemAlloc (sizeof (EDITOR_APP));
  if (!E) return NULL;
  E->Fs = Fs;
  E->MaxBytes = MaxBytes;
  E->Model = ModelNew (MaxBytes, UndoDepth);
  if (!E->Model) { MemFree (E); return NULL; }
  E->CaseSens = TRUE;
  E->State = ED_EDIT;
  TuiInit ();
  TuiGetSize (&Rows, &Cols);
  if (Rows < 3) Rows = 3;
  if (Rows > ED_MAX_ROWS) Rows = ED_MAX_ROWS;
  if (Cols < ED_GUTTER + 2) Cols = ED_GUTTER + 2;
  if (Cols > ED_MAX_COLS) Cols = ED_MAX_COLS;
  E->Rows = Rows;
  E->Cols = Cols;
  E->TextRows = Rows - 2;
  E->TextCols = Cols - ED_GUTTER;
  return E;
}

VOID EdFree (EDITOR_APP *E) {
  if (!E) return;
  InvalidateHits (E);
  ModelFree (E->Model);
  TuiShutdown ();
  MemFree (E);
}

/* ---- 文件 ---- */

EFI_STATUS EdOpen (EDITOR_APP *E, CONST CHAR16 *Path) {
  FS_HANDLE *H = NULL;
  UINT8 *Buf = NULL;
  UINTN Cap, Total = 0, N;
  EFI_STATUS St;
  CHAR16 Msg[ED_MAX_MSG];

  if (!E || !Path) return EFI_INVALID_PARAMETER;
  Msg[0] = 0;
  Cap = E->MaxBytes + 1;   /* 多读 1 字节判超限 */
  St = E->Fs->Open (E->Fs, Path, FS_MODE_READ, &H);
  if (EFI_ERROR (St)) {
    StrCopy (Msg, U16 ("OPEN FAILED 0x"), ED_MAX_MSG);
    CatHex (Msg, ED_MAX_MSG, St, 1);
    EdError (E, Msg, TRUE, 1);
    return St;
  }
  Buf = MemAlloc (Cap);
  if (!Buf) {
    E->Fs->Close (E->Fs, H);
    EdError (E, U16 ("OUT OF MEMORY"), TRUE, 1);
    return EFI_OUT_OF_RESOURCES;
  }
  for (;;) {
    N = Cap - Total;
    St = E->Fs->Read (E->Fs, H, Buf + Total, &N);
    if (EFI_ERROR (St)) {
      E->Fs->Close (E->Fs, H);
      MemFree (Buf);
      StrCopy (Msg, U16 ("READ FAILED 0x"), ED_MAX_MSG);
      CatHex (Msg, ED_MAX_MSG, St, 1);
      EdError (E, Msg, TRUE, 1);
      return St;
    }
    Total += N;
    if (Total > E->MaxBytes) {
      E->Fs->Close (E->Fs, H);
      MemFree (Buf);
      EdError (E, U16 ("FILE TOO LARGE - use FM preview"), TRUE, 1);
      return EFI_BAD_BUFFER_SIZE;
    }
    if (N == 0) break;   /* EOF */
  }
  E->Fs->Close (E->Fs, H);
  St = ModelLoad (E->Model, Buf, Total);
  MemFree (Buf);
  if (EFI_ERROR (St)) {
    StrCopy (Msg, U16 ("LOAD FAILED 0x"), ED_MAX_MSG);
    CatHex (Msg, ED_MAX_MSG, St, 1);
    EdError (E, Msg, TRUE, 1);
    return St;
  }
  StrCopy (E->Path, Path, ED_MAX_PATH);
  E->HasFile = TRUE;
  E->SaveFailed = FALSE;
  E->TopLine = 0;
  E->LeftCol = 0;
  InvalidateHits (E);
  return EFI_SUCCESS;
}

/* 保存到 Path。成功 → 更新当前路径/消息；失败 → 非致命错误页 + SaveFailed。 */
STATIC EFI_STATUS SaveTo (EDITOR_APP *E, CONST CHAR16 *Path) {
  UINT8 *Buf = NULL;
  UINTN Len = 0, Wr;
  FS_ENTRY Info;
  FS_HANDLE *H = NULL;
  EFI_STATUS St, StClose;
  CHAR16 Msg[ED_MAX_MSG];

  Msg[0] = 0;
  St = ModelSave (E->Model, &Buf, &Len);
  if (EFI_ERROR (St)) {
    EdError (E, U16 ("SAVE: serialize failed"), FALSE, 0);
    return St;
  }
  St = E->Fs->Info (E->Fs, Path, &Info);
  if (!EFI_ERROR (St) && Info.IsDir) {
    MemFree (Buf);
    E->SaveFailed = TRUE;
    EdError (E, U16 ("SAVE: target is a directory"), FALSE, 0);
    return EFI_ACCESS_DENIED;
  }
  St = E->Fs->Delete (E->Fs, Path);
  if (EFI_ERROR (St) && St != EFI_NOT_FOUND) {
    MemFree (Buf);
    E->SaveFailed = TRUE;
    StrCopy (Msg, U16 ("SAVE: delete failed 0x"), ED_MAX_MSG);
    CatHex (Msg, ED_MAX_MSG, St, 1);
    EdError (E, Msg, FALSE, 0);
    return St;
  }
  St = E->Fs->Open (E->Fs, Path, FS_MODE_WRITE | FS_MODE_CREATE, &H);
  if (EFI_ERROR (St)) {
    MemFree (Buf);
    E->SaveFailed = TRUE;
    StrCopy (Msg, U16 ("SAVE: open failed 0x"), ED_MAX_MSG);
    CatHex (Msg, ED_MAX_MSG, St, 1);
    EdError (E, Msg, FALSE, 0);
    return St;
  }
  Wr = Len;
  St = E->Fs->Write (E->Fs, H, Buf, &Wr);
  if (!EFI_ERROR (St) && Wr != Len) St = EFI_DEVICE_ERROR;
  if (!EFI_ERROR (St)) St = E->Fs->Flush (E->Fs, H);
  StClose = E->Fs->Close (E->Fs, H);
  if (EFI_ERROR (St)) {
    MemFree (Buf);
    E->SaveFailed = TRUE;
    StrCopy (Msg, U16 ("SAVE FAILED 0x"), ED_MAX_MSG);
    CatHex (Msg, ED_MAX_MSG, St, 1);
    EdError (E, Msg, FALSE, 0);
    return St;
  }
  MemFree (Buf);
  if (EFI_ERROR (StClose)) {
    E->SaveFailed = TRUE;
    StrCopy (Msg, U16 ("SAVE: close failed 0x"), ED_MAX_MSG);
    CatHex (Msg, ED_MAX_MSG, StClose, 1);
    EdError (E, Msg, FALSE, 0);
    return StClose;
  }
  StrCopy (E->Path, Path, ED_MAX_PATH);
  E->HasFile = TRUE;
  E->SaveFailed = FALSE;
  MsgSet (E, U16 ("Saved"));
  return EFI_SUCCESS;
}

/* ---- 查找 ---- */

STATIC VOID JumpHit (EDITOR_APP *E, BOOLEAN Forward) {
  MODEL_INFO I;
  UINTN k;
  INTN Found = -1;
  ModelInfo (E->Model, &I);
  if (E->HitCount == 0) return;
  if (Forward) {
    for (k = 0; k < E->HitCount; k++) {
      if (E->Hits[k].Line > I.CursorLine ||
          (E->Hits[k].Line == I.CursorLine && E->Hits[k].Col > I.CursorCol)) {
        Found = (INTN)k;
        break;
      }
    }
    if (Found < 0) Found = 0;   /* 回绕 */
  } else {
    for (k = E->HitCount; k > 0; k--) {
      if (E->Hits[k - 1].Line < I.CursorLine ||
          (E->Hits[k - 1].Line == I.CursorLine && E->Hits[k - 1].Col < I.CursorCol)) {
        Found = (INTN)(k - 1);
        break;
      }
    }
    if (Found < 0) Found = (INTN)E->HitCount - 1;   /* 回绕 */
  }
  ModelSetCursor (E->Model, E->Hits[Found].Line, E->Hits[Found].Col);
  {
    CHAR16 Msg[ED_MAX_MSG];
    Msg[0] = 0;
    StrCat (Msg, U16 ("Hit "), ED_MAX_MSG);
    CatDec (Msg, ED_MAX_MSG, (UINTN)Found + 1);
    StrCatCh (Msg, (CHAR16)'/', ED_MAX_MSG);
    CatDec (Msg, ED_MAX_MSG, E->HitCount);
    MsgSet (E, Msg);
  }
}

STATIC VOID RunFind (EDITOR_APP *E) {
  EFI_STATUS St;
  CHAR16 Msg[ED_MAX_MSG];
  E->State = ED_EDIT;
  if (E->NeedleLen == 0) {
    MsgSet (E, U16 ("Empty needle"));
    return;
  }
  InvalidateHits (E);
  St = ModelFind (E->Model, E->Needle, E->NeedleLen, E->CaseSens, &E->Hits, &E->HitCount);
  if (EFI_ERROR (St)) {
    MsgSet (E, U16 ("Find failed"));
    return;
  }
  if (E->HitCount == 0) {
    MsgSet (E, U16 ("No hits"));
    return;
  }
  JumpHit (E, TRUE);
  Msg[0] = 0;
  CatDec (Msg, ED_MAX_MSG, E->HitCount);
  StrCat (Msg, U16 (" hits"), ED_MAX_MSG);
  MsgSet (E, Msg);
}

/* ---- 键分发 ---- */

STATIC BOOLEAN Printable (CHAR16 C) {
  return (BOOLEAN)(C >= 0x20 && C != 0x7F);
}

STATIC VOID EditKey (EDITOR_APP *E, TUI_COMMAND Cmd, CHAR16 Ch) {
  UINT32 Cp;
  switch (Cmd) {
    case CMD_CHAR:
      if (!Printable (Ch)) return;
      Cp = Ch;
      if (EFI_ERROR (ModelInsert (E->Model, &Cp, 1))) {
        MsgSet (E, U16 ("Insert rejected (size limit?)"));
        return;
      }
      InvalidateHits (E);
      E->Msg[0] = 0;
      break;
    case CMD_ENTER:
      if (EFI_ERROR (ModelNewline (E->Model))) {
        MsgSet (E, U16 ("Newline rejected (size limit?)"));
        return;
      }
      InvalidateHits (E);
      E->Msg[0] = 0;
      break;
    case CMD_BACKSPACE:
      ModelBackspace (E->Model);
      InvalidateHits (E);
      E->Msg[0] = 0;
      break;
    case CMD_DELETE:
      ModelDeleteFwd (E->Model);
      InvalidateHits (E);
      E->Msg[0] = 0;
      break;
    case CMD_UP:    ModelMove (E->Model, 0, -1); break;
    case CMD_DOWN:  ModelMove (E->Model, 0, 1); break;
    case CMD_LEFT:  ModelMove (E->Model, -1, 0); break;
    case CMD_RIGHT: ModelMove (E->Model, 1, 0); break;
    case CMD_PGUP:  ModelMove (E->Model, 0, -(INTN)E->TextRows); break;
    case CMD_PGDN:  ModelMove (E->Model, 0, (INTN)E->TextRows); break;
    case CMD_HOME:  ModelMove (E->Model, -ED_BIG, 0); break;
    case CMD_END:   ModelMove (E->Model, ED_BIG, 0); break;
    case CMD_F2:
      if (E->HasFile) {
        SaveTo (E, E->Path);
      } else {
        E->InputLen = 0;
        E->Input[0] = 0;
        E->State = ED_SAVEAS;
        E->Msg[0] = 0;
      }
      break;
    case CMD_F3:
      /* 查找输入（needle 保留）；ENTER 执行并跳到光标后首个命中 = 下一命中。
         上一命中 = F4（缓存命中；编辑后失效需重查）。 */
      E->State = ED_FIND;
      E->Msg[0] = 0;
      break;
    case CMD_F4:
      if (E->HitCount > 0) JumpHit (E, FALSE);
      break;
    case CMD_F5:
      E->InputLen = 0;
      E->Input[0] = 0;
      if (E->HasFile) {
        StrCopy (E->Input, E->Path, ED_MAX_INPUT);
        E->InputLen = EfiStrLen (E->Input);
      }
      E->State = ED_SAVEAS;
      E->Msg[0] = 0;
      break;
    case CMD_F6:
      E->InputLen = 0;
      E->Input[0] = 0;
      E->State = ED_GOTO;
      E->Msg[0] = 0;
      break;
    case CMD_F7:
      if (EFI_ERROR (ModelUndo (E->Model))) {
        MsgSet (E, U16 ("Nothing to undo"));
      } else {
        InvalidateHits (E);
        E->Msg[0] = 0;
      }
      break;
    case CMD_F10:
    case CMD_ESC:
      if (IsModified (E)) {
        E->State = ED_CONFIRM;
      } else {
        Done (E, 0);
      }
      break;
    default:
      break;   /* TAB 等：MVP 忽略 */
  }
}

EFI_STATUS EdKey (EDITOR_APP *E, CONST TUI_KEY *Key) {
  TUI_COMMAND Cmd;
  CHAR16 Ch;
  if (!E || !Key) return EFI_INVALID_PARAMETER;
  if (E->State == ED_DONE) return EFI_SUCCESS;
  Cmd = TuiKeyToCommand (Key);
  Ch = Key->UnicodeChar;

  switch (E->State) {
    case ED_EDIT:
      EditKey (E, Cmd, Ch);
      break;

    case ED_FIND:
      if (Cmd == CMD_ESC) {
        E->State = ED_EDIT;
      } else if (Cmd == CMD_TAB) {
        E->CaseSens = (BOOLEAN)!E->CaseSens;
      } else if (Cmd == CMD_ENTER) {
        RunFind (E);
      } else if (Cmd == CMD_BACKSPACE) {
        if (E->NeedleLen > 0) E->Needle[--E->NeedleLen] = 0;
      } else if (Cmd == CMD_CHAR && Printable (Ch) && E->NeedleLen < ED_MAX_INPUT) {
        E->Needle[E->NeedleLen++] = Ch;
      }
      break;

    case ED_GOTO:
      if (Cmd == CMD_ESC) {
        E->State = ED_EDIT;
      } else if (Cmd == CMD_ENTER) {
        UINTN Line = 0, i;
        for (i = 0; i < E->InputLen; i++) Line = Line * 10 + (E->Input[i] - (CHAR16)'0');
        ModelGotoLine (E->Model, Line);   /* 0/越界由模型钳制 */
        E->State = ED_EDIT;
        MsgSet (E, U16 ("Jumped"));
      } else if (Cmd == CMD_BACKSPACE) {
        if (E->InputLen > 0) E->Input[--E->InputLen] = 0;
      } else if (Cmd == CMD_CHAR && Ch >= (CHAR16)'0' && Ch <= (CHAR16)'9' && E->InputLen < 8) {
        E->Input[E->InputLen++] = Ch;
        E->Input[E->InputLen] = 0;
      }
      break;

    case ED_SAVEAS:
      if (Cmd == CMD_ESC) {
        E->State = ED_EDIT;
        E->QuitAfterSave = FALSE;
      } else if (Cmd == CMD_ENTER) {
        if (E->InputLen == 0) {
          MsgSet (E, U16 ("Empty path"));
        } else if (!EFI_ERROR (SaveTo (E, E->Input))) {
          E->State = ED_EDIT;
          if (E->QuitAfterSave) {
            E->QuitAfterSave = FALSE;
            Done (E, 0);
          }
        }
        /* SaveTo 失败已置 ED_ERROR */
      } else if (Cmd == CMD_BACKSPACE) {
        if (E->InputLen > 0) E->Input[--E->InputLen] = 0;
      } else if (Cmd == CMD_CHAR) {
        if (Ch >= 0x20 && Ch <= 0x7E) {   /* AsciiOnly（design §7.3） */
          if (E->InputLen + 1 < ED_MAX_INPUT) {
            E->Input[E->InputLen++] = Ch;
            E->Input[E->InputLen] = 0;
          }
        } else if (Printable (Ch)) {
          MsgSet (E, U16 ("ASCII only"));
        }
      }
      break;

    case ED_CONFIRM:
      if (Cmd == CMD_CHAR && (Ch == (CHAR16)'S' || Ch == (CHAR16)'s')) {
        if (E->HasFile) {
          if (!EFI_ERROR (SaveTo (E, E->Path))) Done (E, 0);
        } else {
          E->InputLen = 0;
          E->Input[0] = 0;
          E->QuitAfterSave = TRUE;
          E->State = ED_SAVEAS;
        }
      } else if (Cmd == CMD_CHAR && (Ch == (CHAR16)'D' || Ch == (CHAR16)'d')) {
        Done (E, 0);   /* 放弃修改 */
      } else if (Cmd == CMD_ESC) {
        E->State = ED_EDIT;
      }
      break;

    case ED_ERROR:
      if (E->ErrorFatal) {
        Done (E, E->ExitCode);
      } else {
        E->State = ED_EDIT;
      }
      break;

    default:
      break;
  }
  return EFI_SUCCESS;
}

/* ---- 渲染 ---- */

STATIC VOID ClampView (EDITOR_APP *E) {
  MODEL_INFO I;
  ModelInfo (E->Model, &I);
  if (I.CursorLine < E->TopLine) E->TopLine = I.CursorLine;
  if (I.CursorLine >= E->TopLine + E->TextRows) {
    E->TopLine = I.CursorLine - E->TextRows + 1;
  }
  if (I.CursorCol < E->LeftCol) E->LeftCol = I.CursorCol;
  if (I.CursorCol >= E->LeftCol + E->TextCols) {
    E->LeftCol = I.CursorCol - E->TextCols + 1;
  }
}

STATIC VOID DrawStatusBar (EDITOR_APP *E) {
  CHAR16 Line[ED_MAX_COLS];
  MODEL_INFO I;
  UINTN r = E->Rows - 2, i, n;
  CONST CHAR16 *Base;
  UTF8_TOKEN Tok;

  ModelInfo (E->Model, &I);
  Line[0] = 0;
  /* 文件名（basename，≤16 字符）+ 修改标记 */
  Base = E->HasFile ? E->Path : U16 ("<new>");
  {
    CONST CHAR16 *p = Base;
    CONST CHAR16 *q;
    for (q = Base; *q; q++) {
      if (*q == (CHAR16)'/' || *q == (CHAR16)'\\') p = q + 1;
    }
    for (i = 0; i < 16 && p[i]; i++) StrCatCh (Line, p[i], ED_MAX_COLS);
  }
  if (IsModified (E)) StrCatCh (Line, (CHAR16)'*', ED_MAX_COLS);
  StrCatCh (Line, (CHAR16)' ', ED_MAX_COLS);
  StrCat (Line, U16 ("L"), ED_MAX_COLS);
  CatDec (Line, ED_MAX_COLS, I.CursorLine + 1);
  StrCatCh (Line, (CHAR16)':', ED_MAX_COLS);
  StrCat (Line, U16 ("C"), ED_MAX_COLS);
  CatDec (Line, ED_MAX_COLS, I.CursorCol + 1);
  StrCat (Line, U16 (" UTF-8"), ED_MAX_COLS);
  if (I.HasBom) StrCat (Line, U16 (" BOM"), ED_MAX_COLS);
  /* 光标处码点（design §7.3：非 ASCII 显示 U+XXXX；RAW 显示原字节） */
  if (!EFI_ERROR (ModelTokenAt (E->Model, I.CursorLine, I.CursorCol, &Tok))) {
    if (Tok.Type == UTF8_TOKEN_CP && Tok.Cp >= 0x80) {
      StrCat (Line, U16 (" U+"), ED_MAX_COLS);
      CatHex (Line, ED_MAX_COLS, Tok.Cp, 4);
    } else if (Tok.Type == UTF8_TOKEN_RAW) {
      StrCat (Line, U16 (" RAW:0x"), ED_MAX_COLS);
      CatHex (Line, ED_MAX_COLS, Tok.Raw, 2);
    }
  }
  if (E->Msg[0]) {
    StrCat (Line, U16 ("  | "), ED_MAX_COLS);
    StrCat (Line, E->Msg, ED_MAX_COLS);
  }
  TuiFill (r, 0, E->Cols, 1, (CHAR16)' ', TUI_ATTR_STATUSBAR);
  n = EfiStrLen (Line);
  if (n > E->Cols) n = E->Cols;
  for (i = 0; i < n; i++) TuiPutCell (r, i, Line[i], TUI_ATTR_STATUSBAR);
}

STATIC VOID DrawInputBar (EDITOR_APP *E, CONST CHAR16 *Prompt) {
  CHAR16 Line[ED_MAX_COLS];
  UINTN r = E->Rows - 2, i, n, CursorAt;
  Line[0] = 0;
  StrCat (Line, Prompt, ED_MAX_COLS);
  if (E->State == ED_FIND) {
    StrCat (Line, U16 ("[TAB case:"), ED_MAX_COLS);
    StrCat (Line, E->CaseSens ? U16 ("ON") : U16 ("OFF"), ED_MAX_COLS);
    StrCat (Line, U16 ("]: "), ED_MAX_COLS);
    for (i = 0; i < E->NeedleLen; i++) {
      StrCatCh (Line, (CHAR16)(E->Needle[i] <= 0xFFFF ? E->Needle[i] : (UINT32)'?'), ED_MAX_COLS);
    }
  } else {
    StrCat (Line, E->Input, ED_MAX_COLS);
  }
  CursorAt = EfiStrLen (Line);   /* 输入光标在输入文本末尾（消息之前） */
  if (E->Msg[0]) {
    StrCat (Line, U16 ("  | "), ED_MAX_COLS);
    StrCat (Line, E->Msg, ED_MAX_COLS);
  }
  TuiFill (r, 0, E->Cols, 1, (CHAR16)' ', TUI_ATTR_STATUSBAR);
  n = EfiStrLen (Line);
  if (n > E->Cols) n = E->Cols;
  for (i = 0; i < n; i++) TuiPutCell (r, i, Line[i], TUI_ATTR_STATUSBAR);
  if (CursorAt < E->Cols) TuiPutCell (r, CursorAt, (CHAR16)' ', TUI_ATTR_SELECTED);
}

STATIC VOID DrawKeybar (EDITOR_APP *E, CONST CHAR16 *Text) {
  UINTN r = E->Rows - 1, i, n;
  TuiFill (r, 0, E->Cols, 1, (CHAR16)' ', TUI_ATTR_KEYBAR);
  n = EfiStrLen (Text);
  if (n > E->Cols) n = E->Cols;
  for (i = 0; i < n; i++) TuiPutCell (r, i, Text[i], TUI_ATTR_KEYBAR);
}

STATIC VOID DrawConfirm (EDITOR_APP *E) {
  /* ASCII 画框居中提示（三键语义） */
  CONST CHAR16 *L1 = U16 ("+------------------------------+");
  CONST CHAR16 *L2 = U16 ("|           Quit?              |");
  CONST CHAR16 *L3 = U16 ("| File modified.               |");
  CONST CHAR16 *L4 = U16 ("| S=Save+Quit D=Discard        |");
  CONST CHAR16 *L5 = U16 ("| ESC=Cancel                   |");
  CONST CHAR16 *L6 = U16 ("+------------------------------+");
  UINTN w = 32, top, col;
  if (w > E->Cols) w = E->Cols;
  top = E->Rows / 2 >= 3 ? E->Rows / 2 - 3 : 0;
  col = E->Cols > w ? (E->Cols - w) / 2 : 0;
  TuiPutText (top,     col, L1, TUI_ATTR_NORMAL);
  TuiPutText (top + 1, col, L2, TUI_ATTR_TITLE);
  TuiPutText (top + 2, col, L3, TUI_ATTR_NORMAL);
  TuiPutText (top + 3, col, L4, TUI_ATTR_NORMAL);
  TuiPutText (top + 4, col, L5, TUI_ATTR_NORMAL);
  TuiPutText (top + 5, col, L6, TUI_ATTR_NORMAL);
}

STATIC VOID DrawError (EDITOR_APP *E) {
  UINTN mid = E->Rows / 2, i, n;
  CONST CHAR16 *Hint = E->ErrorFatal ? U16 ("Any key: exit") : U16 ("Any key: continue");
  TuiFill (0, 0, E->Cols, E->Rows, (CHAR16)' ', TUI_ATTR_BANNER_ERR);
  TuiPutText (mid >= 2 ? mid - 2 : 0, 2, U16 ("ERROR"), TUI_ATTR_BANNER_ERR);
  n = EfiStrLen (E->Msg);
  if (n > E->Cols - 4) n = E->Cols - 4;
  for (i = 0; i < n; i++) TuiPutCell (mid, 2 + i, E->Msg[i], TUI_ATTR_BANNER_ERR);
  TuiPutText (mid + 2 < E->Rows ? mid + 2 : E->Rows - 1, 2, Hint, TUI_ATTR_BANNER_ERR);
}

VOID EdRender (EDITOR_APP *E) {
  TUI_CELL Cells[ED_MAX_COLS];
  MODEL_INFO I;
  UINTN r, c, Line;
  CHAR16 Gutter[ED_GUTTER + 1];
  UINTN v, n, i;
  CHAR16 Ch;
  UTF8_TOKEN Tok;

  if (!E) return;
  TuiClear ();
  ModelInfo (E->Model, &I);

  if (E->State == ED_ERROR) {
    DrawError (E);
    TuiFlush ();
    return;
  }

  ClampView (E);
  for (r = 0; r < E->TextRows; r++) {
    Line = E->TopLine + r;
    if (Line >= I.LineCount) break;
    /* 行号（1 基，右对齐 6 位 + 空格） */
    v = Line + 1;
    for (i = 0; i < ED_GUTTER; i++) Gutter[i] = (CHAR16)' ';
    Gutter[ED_GUTTER] = 0;
    n = 0;
    {
      CHAR16 Tmp[24];
      UINTN t = 0, k;
      do { Tmp[t++] = (CHAR16)('0' + (v % 10)); v /= 10; } while (v && t < 24);
      n = t;
      if (n > ED_GUTTER - 1) n = ED_GUTTER - 1;
      for (k = 0; k < n; k++) Gutter[ED_GUTTER - 2 - k] = Tmp[k];
    }
    TuiPutText (r, 0, Gutter, TUI_ATTR_DISABLED);
    /* 文本投影（非 ASCII 占位 + 属性由模型给出） */
    ModelRenderRow (E->Model, Line, E->LeftCol, E->TextCols, Cells);
    for (c = 0; c < E->TextCols; c++) {
      TuiPutCell (r, ED_GUTTER + c, Cells[c].Char, Cells[c].Attr);
    }
  }
  /* 光标 = 反显单元格 */
  if (I.CursorLine >= E->TopLine && I.CursorLine < E->TopLine + E->TextRows &&
      I.CursorCol >= E->LeftCol && I.CursorCol < E->LeftCol + E->TextCols) {
    Ch = (CHAR16)' ';
    if (!EFI_ERROR (ModelTokenAt (E->Model, I.CursorLine, I.CursorCol, &Tok))) {
      if (Tok.Type == UTF8_TOKEN_CP && Tok.Cp < 0x80) {
        Ch = (CHAR16)Tok.Cp;
      } else {
        Ch = TUI_NONASCII_CHAR;
      }
    }
    TuiPutCell (I.CursorLine - E->TopLine, ED_GUTTER + (I.CursorCol - E->LeftCol),
                Ch, TUI_ATTR_SELECTED);
  }

  if (E->State == ED_FIND || E->State == ED_GOTO || E->State == ED_SAVEAS) {
    DrawInputBar (E, E->State == ED_FIND ? U16 ("Find ") :
                     E->State == ED_GOTO ? U16 ("Goto line: ") : U16 ("Save as: "));
    DrawKeybar (E, E->State == ED_FIND ? U16 ("ENTER Find  TAB Case  ESC Cancel") :
                   E->State == ED_GOTO ? U16 ("ENTER Jump  ESC Cancel") :
                                         U16 ("ENTER Save  ESC Cancel (ASCII path)"));
  } else {
    DrawStatusBar (E);
    DrawKeybar (E, U16 ("F2 Save F3 Find F4 Prev F5 SaveAs F6 Goto F7 Undo F10 Quit"));
  }
  if (E->State == ED_CONFIRM) DrawConfirm (E);
  TuiFlush ();
}

VOID EdRun (EDITOR_APP *E) {
  TUI_KEY Key;
  if (!E) return;
  for (;;) {
    EdRender (E);
    if (E->State == ED_DONE) break;
    if (EFI_ERROR (TuiWaitKey (&Key))) break;   /* 宿主 mock 队列空 → 脚本结束 */
    EdKey (E, &Key);
  }
}

/* ---- 查询 ---- */

ED_STATE EdState (CONST EDITOR_APP *E) { return E ? E->State : ED_DONE; }
EDITOR_MODEL *EdModel (EDITOR_APP *E) { return E ? E->Model : NULL; }
CONST CHAR16 *EdPath (CONST EDITOR_APP *E) { return E ? E->Path : NULL; }
UINTN EdExitCode (CONST EDITOR_APP *E) { return E ? E->ExitCode : 1; }
CONST CHAR16 *EdMsg (CONST EDITOR_APP *E) { return E ? E->Msg : NULL; }
