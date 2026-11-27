/** @file
  TuiScreenLib 核心 — ConOut 文本模式管理 + 单元格缓冲 + 脏区差分重绘（S06）。
  双环境共享（宿主/UEFI）：只用 Contract 头 + EfiMem + 内部后端接缝，
  ConOut 经 TuiScreenBackend() 链接期注入（UEFI 见 TuiScreenUefi.c）。

  语义（与 tests/host/mocks/MockTui.c 对齐）：
    - PutCell/PutText/Fill 越界写入丢弃（不环绕、不崩溃）；
    - 写入相同值不置脏；TuiClear = 全缓冲 ' ' + TUI_ATTR_NORMAL 并全置脏；
    - Flush 双缓冲差分：只输出“屏幕未知（valid=0）或与上次成功输出不同”的格，
      按行合并同属性连续格为一次 OutputString。TuiClear + 整帧重绘只重画真正
      变化的格（不再整屏重写；S19 修复 4K 模式下按键逐行爬行）；
    - 外部镜像/嵌套 Shell 会改写屏幕：执行返回后必须调用 TuiInvalidate()，
      清 valid，下次 Flush 全量重绘，随后恢复差分。

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
STATIC TUI_CELL   *mCells    = NULL;   /* ROWS*COLS 目标状态矩阵 */
STATIC UINT8      *mDirty    = NULL;   /* ROWS*COLS 脏标记 */
STATIC TUI_CELL   *mShadow   = NULL;   /* ROWS*COLS 上次成功输出到屏幕的格 */
STATIC UINT8      *mValid    = NULL;   /* ROWS*COLS 0=屏幕内容未知，必输出 */
STATIC CHAR16     *mRun      = NULL;   /* COLS+1 输出行缓冲 */
STATIC UINTN       mRows     = 0;
STATIC UINTN       mCols     = 0;
STATIC INTN        mOrigMode = -1;     /* 进入前模式，Shutdown 恢复 */
STATIC BOOLEAN     mReady    = FALSE;

STATIC VOID FreeBuffers (VOID) {
  if (mCells  != NULL) { MemFree (mCells);  mCells  = NULL; }
  if (mDirty  != NULL) { MemFree (mDirty);  mDirty  = NULL; }
  if (mShadow != NULL) { MemFree (mShadow); mShadow = NULL; }
  if (mValid  != NULL) { MemFree (mValid);  mValid  = NULL; }
  if (mRun    != NULL) { MemFree (mRun);    mRun    = NULL; }
}

/* 候选文本模式（TuiInit 枚举用） */
typedef struct {
  UINTN Mode;
  UINTN Cols;
  UINTN Rows;
  UINTN Area;
} TUI_MODE_CAND;

/* 候选排序：列数升序（字形最宽）→ 行数降序（同列数信息量最大）→ 模式号升序（确定性） */
STATIC BOOLEAN ModeLess (CONST TUI_MODE_CAND *A, CONST TUI_MODE_CAND *B) {
  if (A->Cols != B->Cols) {
    return (BOOLEAN)(A->Cols < B->Cols);
  }
  if (A->Rows != B->Rows) {
    return (BOOLEAN)(A->Rows > B->Rows);
  }
  return (BOOLEAN)(A->Mode < B->Mode);
}

EFI_STATUS TuiInit (VOID) {
  TUI_CONOUT    *Be;
  TUI_MODE_CAND  Cand[TUI_SCREEN_MAX_MODES];
  TUI_MODE_CAND  Key;
  BOOLEAN        Tried[TUI_SCREEN_MAX_MODES];
  UINTN          NCand, Mode, Cols, Rows, i, j;
  BOOLEAN        Found;

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
  /* 稳定插入排序：可读性优先（见 ModeLess）。高 DPI/4K 固件常把 320x75 之类的
     “最大面积”模式当默认，实际字形仅十几像素宽、不可读；TUI 布局只保证 80x25，
     故优先列数最少（字形最宽）的模式：4K 屏会落到 80x25/128x40 一类，字放大数倍。 */
  for (i = 1; i < NCand; i++) {
    Key = Cand[i];
    j = i;
    while (j > 0 && ModeLess (&Key, &Cand[j - 1])) {
      Cand[j] = Cand[j - 1];
      j--;
    }
    Cand[j] = Key;
  }

  mOrigMode = Be->CurrentMode;
  Found = FALSE;
  for (i = 0; i < NCand; i++) {
    Tried[i] = FALSE;
  }
  /* 第一轮（可读性优先）：满足布局基线 80x25 的候选中按 ModeLess 顺序取第一个。
     SetMode 后探测右下角 SetCursorPosition(Cols-1, Rows-1)：OVMF ConSplitter 实证
     查询宽度可能大于图形控制台实际可寻址宽度（右缘列返回 EFI_UNSUPPORTED），
     失败换下一个候选。 */
  for (i = 0; i < NCand; i++) {
    if (Cand[i].Cols < TUI_SCREEN_FALLBACK_COLS || Cand[i].Rows < TUI_SCREEN_FALLBACK_ROWS) {
      continue;
    }
    Tried[i] = TRUE;
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
  /* 第二轮（异常固件：无 >=80x25 的可寻址模式）：按面积最大逐个探测，维持旧行为。 */
  while (!Found) {
    UINTN Best = (UINTN)-1;
    for (i = 0; i < NCand; i++) {
      if (!Tried[i] && (Best == (UINTN)-1 || Cand[i].Area > Cand[Best].Area)) {
        Best = i;
      }
    }
    if (Best == (UINTN)-1) {
      break;
    }
    Tried[Best] = TRUE;
    if (EFI_ERROR (Be->SetMode (Cand[Best].Mode))) {
      continue;
    }
    if (EFI_ERROR (Be->SetCursorPosition (Cand[Best].Cols - 1, Cand[Best].Rows - 1))) {
      continue;
    }
    mCols = Cand[Best].Cols;
    mRows = Cand[Best].Rows;
    Found = TRUE;
  }
  if (!Found) {
    /* 保底 80x25：无可查询/可寻址模式时不 SetMode，按保底尺寸建缓冲 */
    mCols = TUI_SCREEN_FALLBACK_COLS;
    mRows = TUI_SCREEN_FALLBACK_ROWS;
  }

  mCells  = MemAlloc (mRows * mCols * sizeof (TUI_CELL));
  mDirty  = MemAlloc (mRows * mCols);
  mShadow = MemAlloc (mRows * mCols * sizeof (TUI_CELL));
  mValid  = MemAlloc (mRows * mCols);
  mRun    = MemAlloc ((mCols + 1) * sizeof (CHAR16));
  if (mCells == NULL || mDirty == NULL || mShadow == NULL || mValid == NULL || mRun == NULL) {
    FreeBuffers ();
    mRows = 0;
    mCols = 0;
    return EFI_OUT_OF_RESOURCES;
  }
  for (i = 0; i < mRows * mCols; i++) {
    mValid[i] = 0;   /* 首帧屏幕内容未知：Flush 全量重绘 */
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

/* 屏幕内容被外部程序改写（子镜像执行、嵌套 Shell、SetMode 等）：
   清 valid，下次 Flush 全量重绘，随后恢复基于 shadow 的差分。 */
VOID TuiInvalidate (VOID) {
  UINTN Idx, N;
  if (!mReady) {
    return;
  }
  N = mRows * mCols;
  for (Idx = 0; Idx < N; Idx++) {
    mValid[Idx] = 0;
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

/* 该格是否需要输出：屏幕内容未知（valid=0），或缓冲与上次成功输出不同。
   右下角格恒不输出（画出即滚动，见文件头注释），且不参与判断——否则每次
   Flush 都会因它重复尝试，使“无变化帧零输出”不成立。 */
STATIC BOOLEAN CellNeeds (UINTN Row, UINTN Col) {
  UINTN Idx = Row * mCols + Col;
  if (Row == mRows - 1 && Col == mCols - 1) {
    return FALSE;
  }
  if (mValid[Idx] == 0) {
    return TRUE;
  }
  if (mDirty[Idx] == 0) {
    return FALSE;
  }
  return (BOOLEAN)(mCells[Idx].Char != mShadow[Idx].Char ||
                   mCells[Idx].Attr != mShadow[Idx].Attr);
}

EFI_STATUS TuiFlush (VOID) {
  UINTN      Row, Col, First, Last, Start, End, Len;
  UINT8      Attr;
  EFI_STATUS Status, Ret;

  if (!mReady) {
    return EFI_NOT_READY;
  }
  Ret = EFI_SUCCESS;
  for (Row = 0; Row < mRows; Row++) {
    /* 行内需求范围 [First, Last]：本行首个/末个需输出格。范围内无需输出的格
       （典型：词间空格）也随段输出——保证序列化文本连续（QEMU expect 断言整行
       文案），同时不把未变区域扩进输出（性能：按键只重绘真正变化的行段）。 */
    First = mCols;
    Last  = 0;
    for (Col = 0; Col < mCols; Col++) {
      if (CellNeeds (Row, Col)) {
        if (First == mCols) {
          First = Col;
        }
        Last = Col;
      } else {
        mDirty[Row * mCols + Col] = 0;
      }
    }
    if (First == mCols) {
      continue;                       /* 本行无变化：零输出 */
    }
    Col = First;
    while (Col <= Last) {
      Attr  = mCells[Row * mCols + Col].Attr;
      Start = Col;
      End   = Col;
      while (End <= Last && mCells[Row * mCols + End].Attr == Attr) {
        End++;
      }
      Len = End - Start;
      Status = OutputRun (Row, Start, Len, Attr);
      if (EFI_ERROR (Status)) {
        Ret = Status;      /* 输出失败：保留脏位/valid，下次 Flush 重试 */
        Col = End;
        continue;
      }
      for (Col = Start; Col < End; Col++) {
        mShadow[Row * mCols + Col] = mCells[Row * mCols + Col];
        mValid[Row * mCols + Col]  = 1;
        mDirty[Row * mCols + Col]  = 0;
      }
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
