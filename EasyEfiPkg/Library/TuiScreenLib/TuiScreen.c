/** @file
  TuiScreenLib 核心 — ConOut 文本模式管理 + 单元格缓冲 + 脏区重绘（S06）。
  双环境共享（宿主/UEFI）：只用 Contract 头 + EfiMem + 内部后端接缝，
  ConOut 经 TuiScreenBackend() 链接期注入（UEFI 见 TuiScreenUefi.c）。

  语义（与 tests/host/mocks/MockTui.c 对齐）：
    - PutCell/PutText/Fill 越界写入丢弃（不环绕、不崩溃）；
    - 写入相同值不置脏；TuiClear = 全缓冲 ' ' + TUI_ATTR_NORMAL 并全置脏；
    - Flush 只重绘脏格，按行合并同属性连续脏格为一次 OutputString。

  滚动安全（edk2-stable202608 实证，GraphicsConsoleDxe/GraphicsConsole.c）：
    - OutputString 画到行末（CursorColumn >= MaxColumn）会立即内部输出 CRLF；
      若光标在最后一行，LF 触发整屏上滚。因此：
      1) 每次输出前显式 SetCursorPosition，输出长度不超过行剩余宽度；
      2) 右下角格 (Rows-1, Cols-1) 永不输出——画出即滚动且无法稳定恢复，
         屏幕保留清屏空格，缓冲保留写入值（已知限制，见交接注记）；
      3) Flush 结束把光标停回 (0,0)，不留待卷绕状态。
    - TerminalDxe（serial）光标钳制不滚动，上述策略对其同样安全。
**/
#include "Contract/TuiScreen.h"
#include "Contract/EfiMem.h"
#include "TuiScreenInternal.h"

STATIC TUI_CONOUT *mBe       = NULL;
STATIC TUI_CELL   *mCells    = NULL;   /* ROWS*COLS 单元格矩阵 */
STATIC UINT8      *mDirty    = NULL;   /* ROWS*COLS 脏标记 */
STATIC CHAR16     *mRun      = NULL;   /* COLS+1 输出行缓冲 */
STATIC UINTN       mRows     = 0;
STATIC UINTN       mCols     = 0;
STATIC INTN        mOrigMode = -1;     /* 进入前模式，Shutdown 恢复 */
STATIC BOOLEAN     mReady    = FALSE;

STATIC VOID FreeBuffers (VOID) {
  if (mCells != NULL) { MemFree (mCells); mCells = NULL; }
  if (mDirty != NULL) { MemFree (mDirty); mDirty = NULL; }
  if (mRun   != NULL) { MemFree (mRun);   mRun   = NULL; }
}

/* 候选文本模式（TuiInit 枚举用） */
typedef struct {
  UINTN Mode;
  UINTN Cols;
  UINTN Rows;
  UINTN Area;
} TUI_MODE_CAND;

EFI_STATUS TuiInit (VOID) {
  TUI_CONOUT   *Be;
  TUI_MODE_CAND Cand[TUI_SCREEN_MAX_MODES];
  TUI_MODE_CAND Key;
  UINTN         NCand, Mode, Cols, Rows, i, j;
  BOOLEAN       Found;

  if (mReady) {
    return EFI_SUCCESS;   /* 幂等 */
  }
  Be = TuiScreenBackend ();
  if (Be == NULL) {
    return EFI_NOT_READY;
  }

  /* QueryMode 枚举候选模式（单个失败 → 跳过）。护栏防 MaxMode/尺寸异常。 */
  NCand = 0;
  for (Mode = 0; Mode < Be->MaxMode && Mode < TUI_SCREEN_MODE_PROBE_LIMIT; Mode++) {
    Cols = 0;
    Rows = 0;
    if (EFI_ERROR (Be->QueryMode (Mode, &Cols, &Rows))) {
      continue;
    }
    if (Cols == 0 || Rows == 0 ||
        Cols > TUI_SCREEN_MAX_COLS || Rows > TUI_SCREEN_MAX_ROWS) {
      continue;
    }
    if (NCand >= TUI_SCREEN_MAX_MODES) {
      break;
    }
    Cand[NCand].Mode = Mode;
    Cand[NCand].Cols = Cols;
    Cand[NCand].Rows = Rows;
    Cand[NCand].Area = Cols * Rows;
    NCand++;
  }
  /* 稳定插入排序：面积降序（平局保持模式号升序 → 取最小模式号） */
  for (i = 1; i < NCand; i++) {
    Key = Cand[i];
    j = i;
    while (j > 0 && Cand[j - 1].Area < Key.Area) {
      Cand[j] = Cand[j - 1];
      j--;
    }
    Cand[j] = Key;
  }

  mOrigMode = Be->CurrentMode;
  Found = FALSE;
  /* 取最大“完全可寻址”模式：SetMode 后探测右下角 SetCursorPosition(Cols-1, Rows-1)。
     OVMF ConSplitter 实证（x64）：QueryMode 报告的模式宽度可能大于图形控制台实际
     可寻址宽度（右缘列 SetCursorPosition 返回 EFI_UNSUPPORTED），故须探测，
     失败则降级到次大候选，直至保底 80x25。 */
  for (i = 0; i < NCand; i++) {
    if (EFI_ERROR (Be->SetMode (Cand[i].Mode))) {
      continue;
    }
    if (EFI_ERROR (Be->SetCursorPosition (Cand[i].Cols - 1, Cand[i].Rows - 1))) {
      continue;
    }
    mCols = Cand[i].Cols;
    mRows = Cand[i].Rows;
    Found = TRUE;
    break;
  }
  if (!Found) {
    /* 保底 80x25：无可查询/可寻址模式时不 SetMode，按保底尺寸建缓冲 */
    mCols = TUI_SCREEN_FALLBACK_COLS;
    mRows = TUI_SCREEN_FALLBACK_ROWS;
  }

  mCells = MemAlloc (mRows * mCols * sizeof (TUI_CELL));
  mDirty = MemAlloc (mRows * mCols);
  mRun   = MemAlloc ((mCols + 1) * sizeof (CHAR16));
  if (mCells == NULL || mDirty == NULL || mRun == NULL) {
    FreeBuffers ();
    mRows = 0;
    mCols = 0;
    return EFI_OUT_OF_RESOURCES;
  }

  mBe    = Be;
  mReady = TRUE;
  TuiClear ();   /* 首帧全置脏：Flush 建立确定的整屏状态 */
  return EFI_SUCCESS;
}

EFI_STATUS TuiGetSize (UINTN *Rows, UINTN *Cols) {
  if (!mReady) {
    return EFI_NOT_READY;
  }
  if (Rows == NULL || Cols == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Rows = mRows;
  *Cols = mCols;
  return EFI_SUCCESS;
}

VOID TuiPutCell (UINTN Row, UINTN Col, CHAR16 Ch, UINT8 Attr) {
  UINTN Idx;
  if (!mReady || Row >= mRows || Col >= mCols) {
    return;   /* 越界丢弃 */
  }
  Idx = Row * mCols + Col;
  if (mCells[Idx].Char != Ch || mCells[Idx].Attr != Attr) {
    mCells[Idx].Char = Ch;
    mCells[Idx].Attr = Attr;
    mDirty[Idx] = 1;
  }
}

VOID TuiPutText (UINTN Row, UINTN Col, CONST CHAR16 *Text, UINT8 Attr) {
  UINTN i, Limit;
  if (!mReady || Text == NULL || Row >= mRows || Col >= mCols) {
    return;
  }
  Limit = mCols - Col;   /* 右缘截断，不环绕；防 Col+i 回绕 */
  for (i = 0; i < Limit && Text[i] != 0; i++) {
    TuiPutCell (Row, Col + i, Text[i], Attr);
  }
}

VOID TuiFill (UINTN Row, UINTN Col, UINTN W, UINTN H, CHAR16 Ch, UINT8 Attr) {
  UINTN r, c;
  if (!mReady || Row >= mRows || Col >= mCols || W == 0 || H == 0) {
    return;
  }
  if (W > mCols - Col) { W = mCols - Col; }   /* 钳制矩形，防 W/H 回绕 */
  if (H > mRows - Row) { H = mRows - Row; }
  for (r = Row; r < Row + H; r++) {
    for (c = Col; c < Col + W; c++) {
      TuiPutCell (r, c, Ch, Attr);
    }
  }
}

VOID TuiClear (VOID) {
  UINTN Idx, N;
  if (!mReady) {
    return;
  }
  N = mRows * mCols;
  for (Idx = 0; Idx < N; Idx++) {
    mCells[Idx].Char = ' ';
    mCells[Idx].Attr = TUI_ATTR_NORMAL;
    mDirty[Idx] = 1;
  }
}

/* 输出一段连续同属性单元格（Len >= 1，Start+Len <= mCols）。
   调用方保证：最后一行的段不含右下角格（不会画到行末触发 CRLF 滚动）。 */
STATIC EFI_STATUS OutputRun (UINTN Row, UINTN Start, UINTN Len, UINT8 Attr) {
  EFI_STATUS Status;
  UINTN      i;
  for (i = 0; i < Len; i++) {
    mRun[i] = mCells[Row * mCols + Start + i].Char;
  }
  mRun[Len] = 0;
  Status = mBe->SetCursorPosition (Start, Row);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = mBe->SetAttribute (Attr);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return mBe->OutputString (mRun);
}

EFI_STATUS TuiFlush (VOID) {
  UINTN      Row, Col, Start, End, Len;
  UINT8      Attr;
  EFI_STATUS Status, Ret;

  if (!mReady) {
    return EFI_NOT_READY;
  }
  Ret = EFI_SUCCESS;
  for (Row = 0; Row < mRows; Row++) {
    Col = 0;
    while (Col < mCols) {
      if (mDirty[Row * mCols + Col] == 0) {
        Col++;
        continue;
      }
      /* 极大连续同属性脏段 [Start, End) */
      Start = Col;
      Attr  = mCells[Row * mCols + Col].Attr;
      End   = Col + 1;
      while (End < mCols &&
             mDirty[Row * mCols + End] != 0 &&
             mCells[Row * mCols + End].Attr == Attr) {
        End++;
      }
      Len = End - Start;
      if (Row == mRows - 1 && End == mCols) {
        /* 右下角格永不输出（画出即滚动，见文件头注释）；清脏保留缓冲值 */
        Len--;
        mDirty[Row * mCols + (mCols - 1)] = 0;
      }
      if (Len > 0) {
        Status = OutputRun (Row, Start, Len, Attr);
        if (EFI_ERROR (Status)) {
          Ret = Status;
        }
      }
      for (Col = Start; Col < End; Col++) {
        mDirty[Row * mCols + Col] = 0;
      }
      Col = End;
    }
  }
  /* 光标停回可视区内，避免遗留待卷绕位置导致后续输出滚动 */
  Status = mBe->SetCursorPosition (0, 0);
  if (EFI_ERROR (Status)) {
    Ret = Status;
  }
  return Ret;
}

EFI_STATUS TuiShutdown (VOID) {
  EFI_STATUS Status;
  if (!mReady) {
    return EFI_NOT_READY;
  }
  Status = EFI_SUCCESS;
  if (mOrigMode >= 0) {
    Status = mBe->SetMode ((UINTN)mOrigMode);   /* 恢复进入前模式 */
  }
  FreeBuffers ();
  mRows     = 0;
  mCols     = 0;
  mOrigMode = -1;
  mBe       = NULL;
  mReady    = FALSE;
  return Status;
}

CONST TUI_CELL *TuiScreenPeekCell (UINTN Row, UINTN Col) {
  if (!mReady || Row >= mRows || Col >= mCols) {
    return NULL;
  }
  return &mCells[Row * mCols + Col];
}
