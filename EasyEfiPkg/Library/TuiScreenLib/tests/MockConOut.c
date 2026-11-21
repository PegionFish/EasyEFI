/** @file
  S06 mock ConOut 实现（语义见 MockConOut.h）。
  虚拟屏幕滚动行为逐条对齐 edk2-stable202608 GraphicsConsoleDxe：
  行末内部 CRLF + 最后一行 LF 上滚。调用全量记录供序列断言。
**/
#include "MockConOut.h"
#include <string.h>

STATIC UINTN   mModes[MOCK_CONOUT_MAX_MODES][2];
STATIC UINTN   mModeCount = 0;
STATIC UINT32  mFailMask  = 0;
STATIC INTN    mCurrent   = 0;
STATIC UINTN   mCursorColLimit = (UINTN)-1;   /* 默认无限制 */

STATIC TUI_CELL mScreen[MOCK_CONOUT_MAX_ROWS][MOCK_CONOUT_MAX_COLS];
STATIC UINTN    mCol = 0, mRow = 0;
STATIC UINT8    mAttr = 0x07;
STATIC UINTN    mScrolls = 0;

STATIC MOCK_CALL mLog[MOCK_CONOUT_LOG_CAP];
STATIC UINTN     mLogCount = 0;
STATIC BOOLEAN   mOverflow = FALSE;

STATIC VOID CurDims (UINTN *Cols, UINTN *Rows) {
  if (mModeCount == 0 || mCurrent < 0 || (UINTN)mCurrent >= mModeCount) {
    *Cols = 80;   /* 无模式表时按保底 80x25（与核心 fallback 一致） */
    *Rows = 25;
    return;
  }
  *Cols = mModes[mCurrent][0];
  *Rows = mModes[mCurrent][1];
}

STATIC VOID ClearScreen (VOID) {
  UINTN r, c, Cols, Rows;
  CurDims (&Cols, &Rows);
  for (r = 0; r < Rows; r++) {
    for (c = 0; c < Cols; c++) {
      mScreen[r][c].Char = ' ';
      mScreen[r][c].Attr = 0x07;
    }
  }
  mCol = 0;
  mRow = 0;
  mAttr = 0x07;
}

STATIC VOID ScrollUp (VOID) {
  UINTN r, c, Cols, Rows;
  CurDims (&Cols, &Rows);
  for (r = 0; r + 1 < Rows; r++) {
    for (c = 0; c < Cols; c++) {
      mScreen[r][c] = mScreen[r + 1][c];
    }
  }
  for (c = 0; c < Cols; c++) {
    mScreen[Rows - 1][c].Char = ' ';
    mScreen[Rows - 1][c].Attr = mAttr;
  }
  mScrolls++;
}

STATIC MOCK_CALL *Log (MOCK_CALL_TYPE Type) {
  MOCK_CALL *E;
  if (mLogCount >= MOCK_CONOUT_LOG_CAP) {
    mOverflow = TRUE;
    return NULL;
  }
  E = &mLog[mLogCount++];
  memset (E, 0, sizeof (*E));
  E->Type = Type;
  return E;
}

VOID MockConOutReset (VOID) {
  mModeCount = 0;
  mFailMask  = 0;
  mCurrent   = 0;
  mScrolls   = 0;
  mLogCount  = 0;
  mOverflow  = FALSE;
  mCursorColLimit = (UINTN)-1;
  ClearScreen ();
}

VOID MockConOutSetCursorColLimit (UINTN Limit) {
  mCursorColLimit = Limit;
}

VOID MockConOutSetModes (CONST UINTN (*Modes)[2], UINTN Count, UINT32 FailMask) {
  UINTN i;
  if (Count > MOCK_CONOUT_MAX_MODES) {
    Count = MOCK_CONOUT_MAX_MODES;
  }
  for (i = 0; i < Count; i++) {
    mModes[i][0] = Modes[i][0];
    mModes[i][1] = Modes[i][1];
  }
  mModeCount = Count;
  mFailMask  = FailMask;
}

VOID MockConOutSetCurrentMode (INTN Mode) {
  mCurrent = Mode;
}

STATIC EFI_STATUS MockQueryMode (UINTN Mode, UINTN *Cols, UINTN *Rows) {
  MOCK_CALL *E = Log (MOCK_CALL_QUERYMODE);
  EFI_STATUS Status;
  if (E != NULL) { E->A = Mode; }
  if (Mode >= mModeCount || ((mFailMask >> Mode) & 1u) != 0) {
    Status = EFI_UNSUPPORTED;
  } else {
    *Cols = mModes[Mode][0];
    *Rows = mModes[Mode][1];
    Status = EFI_SUCCESS;
  }
  if (E != NULL) { E->Result = Status; }
  return Status;
}

STATIC EFI_STATUS MockSetMode (UINTN Mode) {
  MOCK_CALL *E = Log (MOCK_CALL_SETMODE);
  EFI_STATUS Status;
  if (E != NULL) { E->A = Mode; }
  if (Mode >= mModeCount) {
    Status = EFI_UNSUPPORTED;
  } else {
    mCurrent = (INTN)Mode;
    ClearScreen ();   /* UEFI 规范：SetMode 清屏 */
    Status = EFI_SUCCESS;
  }
  if (E != NULL) { E->Result = Status; }
  return Status;
}

STATIC EFI_STATUS MockSetCursorPosition (UINTN Col, UINTN Row) {
  MOCK_CALL *E = Log (MOCK_CALL_SETCURSOR);
  UINTN Cols, Rows;
  EFI_STATUS Status;
  if (E != NULL) { E->A = Col; E->B = Row; }
  CurDims (&Cols, &Rows);
  if (Col >= Cols || Row >= Rows || Col >= mCursorColLimit) {
    Status = EFI_UNSUPPORTED;
  } else {
    mCol = Col;
    mRow = Row;
    Status = EFI_SUCCESS;
  }
  if (E != NULL) { E->Result = Status; }
  return Status;
}

STATIC EFI_STATUS MockSetAttribute (UINTN Attr) {
  MOCK_CALL *E = Log (MOCK_CALL_SETATTR);
  if (E != NULL) { E->A = Attr; }
  mAttr = (UINT8)Attr;
  if (E != NULL) { E->Result = EFI_SUCCESS; }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS MockOutputString (CONST CHAR16 *Str) {
  MOCK_CALL *E = Log (MOCK_CALL_OUTPUT);
  UINTN i, Cols, Rows;
  CHAR16 Ch;
  if (E != NULL) {
    for (i = 0; Str[i] != 0 && i + 1 < MOCK_CONOUT_STR_CAP; i++) {
      E->Str[i] = Str[i];
    }
    E->Str[i] = 0;
    E->StrLen = i;
    while (Str[E->StrLen] != 0) { E->StrLen++; }
  }
  CurDims (&Cols, &Rows);
  for (i = 0; Str[i] != 0; i++) {
    Ch = Str[i];
    if (Ch == '\n') {
      if (mRow == Rows - 1) { ScrollUp (); } else { mRow++; }
    } else if (Ch == '\r') {
      mCol = 0;
    } else if (Ch == '\b') {
      if (mCol > 0) { mCol--; }
    } else {
      mScreen[mRow][mCol].Char = Ch;
      mScreen[mRow][mCol].Attr = mAttr;
      mCol++;
      if (mCol >= Cols) {
        /* GraphicsConsoleDxe：画到行末立即内部 CRLF */
        mCol = 0;
        if (mRow == Rows - 1) { ScrollUp (); } else { mRow++; }
      }
    }
  }
  if (E != NULL) { E->Result = EFI_SUCCESS; }
  return EFI_SUCCESS;
}

TUI_CONOUT *MockConOutBackend (VOID) {
  STATIC TUI_CONOUT Be;
  Be.MaxMode         = mModeCount;
  Be.CurrentMode     = mCurrent;
  Be.QueryMode       = MockQueryMode;
  Be.SetMode         = MockSetMode;
  Be.SetCursorPosition = MockSetCursorPosition;
  Be.SetAttribute    = MockSetAttribute;
  Be.OutputString    = MockOutputString;
  return &Be;
}

UINTN MockConOutTotalCalls (VOID) { return mLogCount; }

UINTN MockConOutCallCount (MOCK_CALL_TYPE Type) {
  UINTN i, n = 0;
  for (i = 0; i < mLogCount; i++) {
    if (mLog[i].Type == Type) { n++; }
  }
  return n;
}

CONST MOCK_CALL *MockConOutCall (UINTN Index) {
  return Index < mLogCount ? &mLog[Index] : NULL;
}

CONST MOCK_CALL *MockConOutLastOfType (MOCK_CALL_TYPE Type) {
  UINTN i;
  for (i = mLogCount; i > 0; i--) {
    if (mLog[i - 1].Type == Type) { return &mLog[i - 1]; }
  }
  return NULL;
}

TUI_CELL MockConOutScreen (UINTN Row, UINTN Col) {
  TUI_CELL Cell;
  UINTN Cols, Rows;
  CurDims (&Cols, &Rows);
  if (Row >= Rows || Col >= Cols) {
    Cell.Char = ' ';
    Cell.Attr = 0;
    return Cell;
  }
  return mScreen[Row][Col];
}

UINTN   MockConOutScrolls (VOID)     { return mScrolls; }
UINTN   MockConOutCursorCol (VOID)   { return mCol; }
UINTN   MockConOutCursorRow (VOID)   { return mRow; }
BOOLEAN MockConOutLogOverflow (VOID) { return mOverflow; }
