/** @file
  S06 TuiScreenLib 宿主测试（T06-1..5 任务书用例 + T06-6..8 补充）。
  mock ConOut 记录调用序列并按 GraphicsConsoleDxe 语义维护虚拟屏幕（含滚动），
  验证：模式选择 / 脏区计算 / 边界钳制 / 缓冲黄金快照 / 模式恢复 / 无滚动保证。
  运行: make -f host.mk test   （ASan/UBSan）
**/
#include "test.h"
#include "MockConOut.h"
#include "TuiScreenInternal.h"

/* 链接期后端注入：核心 TuiScreen.c 经此取得 mock ConOut */
TUI_CONOUT *TuiScreenBackend (VOID) {
  return MockConOutBackend ();
}

STATIC VOID Setup (CONST UINTN (*Modes)[2], UINTN Count, UINT32 FailMask, INTN CurMode) {
  TuiShutdown ();            /* 清理上一用例状态（未初始化则 NOT_READY，忽略） */
  MockConOutReset ();
  MockConOutSetModes (Modes, Count, FailMask);
  MockConOutSetCurrentMode (CurMode);
}

/* T06-1 模式选择：3 种模式集 + 坏模式跳过 + 平局 + 保底 */
TEST (t06_1_mode_select) {
  UINTN Rows, Cols;
  CONST MOCK_CALL *E;
  UINTN ModesA[][2] = { {80, 25}, {80, 50}, {100, 31} };
  UINTN ModesB[][2] = { {80, 25} };
  UINTN ModesE[][2] = { {80, 25}, {40, 50} };   /* 面积平局 2000 */

  /* a) 面积最大 = 80x50（4000 > 3100 > 2000）→ mode 1 */
  Setup (ModesA, 3, 0, 0);
  ASSERT_OK (TuiInit ());
  ASSERT_OK (TuiGetSize (&Rows, &Cols));
  ASSERT_EQ (Rows, 50);
  ASSERT_EQ (Cols, 80);
  ASSERT_EQ (MockConOutCallCount (MOCK_CALL_QUERYMODE), 3);
  E = MockConOutLastOfType (MOCK_CALL_SETMODE);
  ASSERT_TRUE (E != NULL);
  ASSERT_EQ (E->A, 1);
  ASSERT_OK (TuiShutdown ());

  /* b) 仅 80x25 → mode 0 */
  Setup (ModesB, 1, 0, 0);
  ASSERT_OK (TuiInit ());
  ASSERT_OK (TuiGetSize (&Rows, &Cols));
  ASSERT_EQ (Rows, 25);
  ASSERT_EQ (Cols, 80);
  E = MockConOutLastOfType (MOCK_CALL_SETMODE);
  ASSERT_TRUE (E != NULL);
  ASSERT_EQ (E->A, 0);
  ASSERT_OK (TuiShutdown ());

  /* c) 无可查询模式且进入前无有效模式 → 保底 80x25，不 SetMode */
  Setup (NULL, 0, 0, -1);
  ASSERT_OK (TuiInit ());
  ASSERT_OK (TuiGetSize (&Rows, &Cols));
  ASSERT_EQ (Rows, 25);
  ASSERT_EQ (Cols, 80);
  ASSERT_EQ (MockConOutCallCount (MOCK_CALL_SETMODE), 0);
  ASSERT_OK (TuiShutdown ());
  ASSERT_EQ (MockConOutCallCount (MOCK_CALL_SETMODE), 0);   /* orig=-1 不恢复 */

  /* d) mode 1 QueryMode 失败 → 跳过，选 mode 2 (100x31) */
  Setup (ModesA, 3, 0x2, 0);
  ASSERT_OK (TuiInit ());
  ASSERT_OK (TuiGetSize (&Rows, &Cols));
  ASSERT_EQ (Rows, 31);
  ASSERT_EQ (Cols, 100);
  E = MockConOutLastOfType (MOCK_CALL_SETMODE);
  ASSERT_TRUE (E != NULL);
  ASSERT_EQ (E->A, 2);
  ASSERT_OK (TuiShutdown ());

  /* e) 面积平局 → 取最小模式号（确定性） */
  Setup (ModesE, 2, 0, 0);
  ASSERT_OK (TuiInit ());
  ASSERT_OK (TuiGetSize (&Rows, &Cols));
  ASSERT_EQ (Rows, 25);
  ASSERT_EQ (Cols, 80);
  E = MockConOutLastOfType (MOCK_CALL_SETMODE);
  ASSERT_TRUE (E != NULL);
  ASSERT_EQ (E->A, 0);
  ASSERT_OK (TuiShutdown ());
}

/* T06-2 脏区计算：改 1 格只输出 1 格；同值写入不置脏 */
TEST (t06_2_dirty_region) {
  UINTN Modes[][2] = { {10, 5} };
  UINTN n, i;
  CONST MOCK_CALL *E;
  Setup (Modes, 1, 0, 0);
  ASSERT_OK (TuiInit ());
  ASSERT_OK (TuiFlush ());   /* 首帧整屏：每行一段，共 5 次输出 */
  ASSERT_EQ (MockConOutCallCount (MOCK_CALL_OUTPUT), 5);
  ASSERT_EQ (MockConOutScrolls (), 0);

  TuiPutCell (2, 3, 'X', TUI_ATTR_SELECTED);
  ASSERT_OK (TuiFlush ());
  ASSERT_EQ (MockConOutCallCount (MOCK_CALL_OUTPUT), 6);   /* 只多 1 次 */
  E = MockConOutLastOfType (MOCK_CALL_OUTPUT);
  ASSERT_TRUE (E != NULL);
  ASSERT_EQ (E->StrLen, 1);
  ASSERT_EQ (E->Str[0], 'X');
  ASSERT_EQ (MockConOutScreen (2, 3).Char, 'X');
  ASSERT_EQ (MockConOutScreen (2, 3).Attr, TUI_ATTR_SELECTED);

  /* 输出前序列 = SetCursorPosition(3,2) → SetAttribute(SELECTED) */
  for (i = MockConOutTotalCalls (); i > 0; i--) {
    if (MockConOutCall (i - 1)->Type == MOCK_CALL_OUTPUT) { break; }
  }
  ASSERT_TRUE (i >= 3);
  E = MockConOutCall (i - 2);
  ASSERT_EQ (E->Type, MOCK_CALL_SETATTR);
  ASSERT_EQ (E->A, TUI_ATTR_SELECTED);
  E = MockConOutCall (i - 3);
  ASSERT_EQ (E->Type, MOCK_CALL_SETCURSOR);
  ASSERT_EQ (E->A, 3);
  ASSERT_EQ (E->B, 2);

  /* 同值重写 → 不置脏 → 无输出 */
  TuiPutCell (2, 3, 'X', TUI_ATTR_SELECTED);
  n = MockConOutCallCount (MOCK_CALL_OUTPUT);
  ASSERT_OK (TuiFlush ());
  ASSERT_EQ (MockConOutCallCount (MOCK_CALL_OUTPUT), n);
  ASSERT_OK (TuiShutdown ());
}

/* T06-3 边界写入钳制：越界丢弃 / 右缘截断不环绕 / Fill 钳制防回绕 */
TEST (t06_3_clamp) {
  UINTN Modes[][2] = { {10, 5} };
  UINTN r, c;
  CONST TUI_CELL *Cell;
  Setup (Modes, 1, 0, 0);
  ASSERT_OK (TuiInit ());

  TuiPutCell (5, 0, 'A', 0x07);            /* 行越界 */
  TuiPutCell (0, 10, 'A', 0x07);           /* 列越界 */
  TuiPutCell ((UINTN)-1, (UINTN)-1, 'A', 0x07);
  ASSERT_TRUE (TuiScreenPeekCell (5, 0) == NULL);
  for (r = 0; r < 5; r++) {
    for (c = 0; c < 10; c++) {
      Cell = TuiScreenPeekCell (r, c);
      ASSERT_TRUE (Cell != NULL);
      ASSERT_EQ (Cell->Char, ' ');
      ASSERT_EQ (Cell->Attr, TUI_ATTR_NORMAL);
    }
  }

  /* 右缘截断：col 7 起只有 3 格可用，DE 丢弃，不环绕到下一行 */
  TuiPutText (1, 7, U16 ("ABCDE"), TUI_ATTR_TITLE);
  ASSERT_EQ (TuiScreenPeekCell (1, 7)->Char, 'A');
  ASSERT_EQ (TuiScreenPeekCell (1, 8)->Char, 'B');
  ASSERT_EQ (TuiScreenPeekCell (1, 9)->Char, 'C');
  ASSERT_EQ (TuiScreenPeekCell (2, 0)->Char, ' ');
  TuiPutText (0, 10, U16 ("X"), 0x07);     /* 起点越界 */
  TuiPutText (5, 0, U16 ("X"), 0x07);
  TuiPutText (0, 0, NULL, 0x07);           /* NULL 安全 */
  ASSERT_EQ (TuiScreenPeekCell (0, 0)->Char, ' ');

  /* Fill 越界钳制 */
  TuiFill (3, 8, 1000, 1000, '#', TUI_ATTR_DANGER);
  ASSERT_EQ (TuiScreenPeekCell (3, 8)->Char, '#');
  ASSERT_EQ (TuiScreenPeekCell (3, 9)->Char, '#');
  ASSERT_EQ (TuiScreenPeekCell (4, 8)->Char, '#');
  ASSERT_EQ (TuiScreenPeekCell (4, 9)->Char, '#');
  ASSERT_EQ (TuiScreenPeekCell (2, 8)->Char, ' ');
  /* SIZE_MAX 宽高不崩溃（钳制防回绕） */
  TuiFill (0, 0, (UINTN)-1, (UINTN)-1, '@', 0x07);
  for (r = 0; r < 5; r++) {
    for (c = 0; c < 10; c++) {
      ASSERT_EQ (TuiScreenPeekCell (r, c)->Char, '@');
    }
  }
  TuiFill (5, 0, 1, 1, 'Z', 0x07);
  TuiFill (0, 10, 1, 1, 'Z', 0x07);
  TuiFill (0, 0, 0, 5, 'Z', 0x07);
  ASSERT_EQ (TuiScreenPeekCell (0, 0)->Char, '@');
  ASSERT_OK (TuiFlush ());
  ASSERT_EQ (MockConOutScrolls (), 0);
  ASSERT_OK (TuiShutdown ());
}

/* T06-4 缓冲快照黄金对比（含属性）+ 虚拟屏幕一致性（右下角已知限制） */
TEST (t06_4_golden) {
  UINTN Modes[][2] = { {12, 6} };
  STATIC CONST struct {
    CONST CHAR8 *Chars;
    UINT8 Attrs[12];
  } Expected[6] = {
    { "            ", {0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07} },
    { " +- S06 --+ ", {0x07,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x07} },
    { " | Hi     | ", {0x07,0x0B,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x0B,0x07} },
    { " |        | ", {0x07,0x0B,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x0B,0x07} },
    { " +--------+ ", {0x07,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x07} },
    { "============", {0x70,0x70,0x70,0x70,0x70,0x70,0x70,0x70,0x70,0x70,0x70,0x70} },
  };
  UINTN r, c;
  CONST TUI_CELL *Cell;
  Setup (Modes, 1, 0, 0);
  ASSERT_OK (TuiInit ());

  /* ASCII 画框（+-|）+ 标题 + 文本 + 底部状态栏（覆盖右下角） */
  TuiFill (1, 1, 10, 4, ' ', TUI_ATTR_NORMAL);
  for (c = 2; c <= 9; c++) {
    TuiPutCell (1, c, '-', TUI_ATTR_TITLE);
    TuiPutCell (4, c, '-', TUI_ATTR_TITLE);
  }
  for (r = 2; r <= 3; r++) {
    TuiPutCell (r, 1, '|', TUI_ATTR_TITLE);
    TuiPutCell (r, 10, '|', TUI_ATTR_TITLE);
  }
  TuiPutCell (1, 1, '+', TUI_ATTR_TITLE);
  TuiPutCell (1, 10, '+', TUI_ATTR_TITLE);
  TuiPutCell (4, 1, '+', TUI_ATTR_TITLE);
  TuiPutCell (4, 10, '+', TUI_ATTR_TITLE);
  TuiPutText (1, 3, U16 (" S06 "), TUI_ATTR_TITLE);
  TuiPutText (2, 3, U16 ("Hi"), TUI_ATTR_NORMAL);
  TuiFill (5, 0, 12, 1, '=', TUI_ATTR_STATUSBAR);

  /* 缓冲逐格黄金对比 */
  for (r = 0; r < 6; r++) {
    for (c = 0; c < 12; c++) {
      Cell = TuiScreenPeekCell (r, c);
      ASSERT_TRUE (Cell != NULL);
      ASSERT_EQ (Cell->Char, (CHAR16)(UINT8)Expected[r].Chars[c]);
      ASSERT_EQ (Cell->Attr, Expected[r].Attrs[c]);
    }
  }

  ASSERT_OK (TuiFlush ());
  /* 段数黄金：row0=1, row1=3, row2=5, row3=5, row4=3, row5=1（右下角除外）= 18 */
  ASSERT_EQ (MockConOutCallCount (MOCK_CALL_OUTPUT), 18);
  ASSERT_EQ (MockConOutScrolls (), 0);
  /* 虚拟屏幕与缓冲一致，唯一例外：右下角 (5,11) 永不输出，保留清屏空格 */
  for (r = 0; r < 6; r++) {
    for (c = 0; c < 12; c++) {
      if (r == 5 && c == 11) {
        ASSERT_EQ (MockConOutScreen (r, c).Char, ' ');
        ASSERT_EQ (MockConOutScreen (r, c).Attr, 0x07);
      } else {
        ASSERT_EQ (MockConOutScreen (r, c).Char, (CHAR16)(UINT8)Expected[r].Chars[c]);
        ASSERT_EQ (MockConOutScreen (r, c).Attr, Expected[r].Attrs[c]);
      }
    }
  }
  ASSERT_OK (TuiShutdown ());
}

/* T06-5 恢复模式调用 + 释放后状态 + 可重入初始化 */
TEST (t06_5_restore) {
  UINTN Modes[][2] = { {80, 25}, {80, 50}, {100, 31} };
  UINTN Rows, Cols;
  CONST MOCK_CALL *E;
  Setup (Modes, 3, 0, 2);            /* 进入前模式 = 2 */
  ASSERT_OK (TuiInit ());            /* 选中 mode 1 (80x50) */
  E = MockConOutLastOfType (MOCK_CALL_SETMODE);
  ASSERT_TRUE (E != NULL);
  ASSERT_EQ (E->A, 1);
  ASSERT_OK (TuiShutdown ());
  E = MockConOutLastOfType (MOCK_CALL_SETMODE);
  ASSERT_TRUE (E != NULL);
  ASSERT_EQ (E->A, 2);               /* 恢复进入前模式 */

  ASSERT_EQ (TuiGetSize (&Rows, &Cols), EFI_NOT_READY);
  ASSERT_EQ (TuiFlush (), EFI_NOT_READY);
  ASSERT_EQ (TuiShutdown (), EFI_NOT_READY);
  ASSERT_TRUE (TuiScreenPeekCell (0, 0) == NULL);

  /* 可再次初始化（此时进入前模式已是 2） */
  ASSERT_OK (TuiInit ());
  ASSERT_OK (TuiGetSize (&Rows, &Cols));
  ASSERT_EQ (Rows, 50);
  ASSERT_EQ (Cols, 80);
  ASSERT_OK (TuiShutdown ());
  E = MockConOutLastOfType (MOCK_CALL_SETMODE);
  ASSERT_TRUE (E != NULL);
  ASSERT_EQ (E->A, 2);
}

/* T06-6 无滚动保证：整屏逐行异属性 + 右下角写入，Flush 不触发滚动 */
TEST (t06_6_no_scroll) {
  UINTN Modes[][2] = { {10, 5} };
  UINTN r, c, n;
  Setup (Modes, 1, 0, 0);
  ASSERT_OK (TuiInit ());
  for (r = 0; r < 5; r++) {
    TuiFill (r, 0, 10, 1, (CHAR16)('0' + r), (UINT8)(0x07 + r * 0x10));
  }
  TuiPutCell (4, 9, 'Z', TUI_ATTR_DANGER);   /* 右下角 */
  ASSERT_OK (TuiFlush ());
  ASSERT_EQ (MockConOutScrolls (), 0);
  ASSERT_EQ (MockConOutCursorCol (), 0);     /* 光标停回可视区 (0,0) */
  ASSERT_EQ (MockConOutCursorRow (), 0);
  for (r = 0; r < 5; r++) {
    for (c = 0; c < 10; c++) {
      if (r == 4 && c == 9) {
        ASSERT_EQ (MockConOutScreen (r, c).Char, ' ');   /* 右下角不输出 */
      } else {
        ASSERT_EQ (MockConOutScreen (r, c).Char, (CHAR16)('0' + r));
        ASSERT_EQ (MockConOutScreen (r, c).Attr, (UINT8)(0x07 + r * 0x10));
      }
    }
  }
  ASSERT_EQ (TuiScreenPeekCell (4, 9)->Char, 'Z');   /* 缓冲保留写入值 */
  ASSERT_EQ (TuiScreenPeekCell (4, 9)->Attr, TUI_ATTR_DANGER);
  /* 右下角脏标记已清：再次 Flush 无输出 */
  n = MockConOutCallCount (MOCK_CALL_OUTPUT);
  ASSERT_OK (TuiFlush ());
  ASSERT_EQ (MockConOutCallCount (MOCK_CALL_OUTPUT), n);
  ASSERT_FALSE (MockConOutLogOverflow ());
  ASSERT_OK (TuiShutdown ());
}

/* T06-7 未初始化/NULL 参数安全 */
TEST (t06_7_uninit_safe) {
  UINTN Rows, Cols;
  TuiShutdown ();
  MockConOutReset ();
  ASSERT_EQ (TuiGetSize (&Rows, &Cols), EFI_NOT_READY);
  ASSERT_EQ (TuiFlush (), EFI_NOT_READY);
  ASSERT_EQ (TuiShutdown (), EFI_NOT_READY);
  TuiPutCell (0, 0, 'X', 0x07);
  TuiPutText (0, 0, U16 ("X"), 0x07);
  TuiFill (0, 0, 10, 10, 'X', 0x07);
  TuiClear ();
  ASSERT_TRUE (TuiScreenPeekCell (0, 0) == NULL);
  ASSERT_EQ (MockConOutTotalCalls (), 0);

  {
    UINTN Modes[][2] = { {10, 5} };
    Setup (Modes, 1, 0, 0);
    ASSERT_OK (TuiInit ());
    ASSERT_EQ (TuiGetSize (NULL, &Cols), EFI_INVALID_PARAMETER);
    ASSERT_EQ (TuiGetSize (&Rows, NULL), EFI_INVALID_PARAMETER);
    TuiPutText (0, 0, NULL, 0x07);   /* 不崩溃 */
    ASSERT_OK (TuiShutdown ());
  }
}

/* T06-8 调用序列黄金：异属性分段 + SetCursor/SetAttr/Output 顺序 + 光标停靠 */
TEST (t06_8_call_sequence) {
  UINTN Modes[][2] = { {4, 2} };
  UINTN Base, i;
  CONST MOCK_CALL *E;
  Setup (Modes, 1, 0, 0);
  ASSERT_OK (TuiInit ());
  /* 初始化序列：QueryMode(0) → SetMode(0) → 可寻址性探测 SetCursor(3,1) */
  ASSERT_EQ (MockConOutTotalCalls (), 3);
  ASSERT_EQ (MockConOutCall (0)->Type, MOCK_CALL_QUERYMODE);
  ASSERT_EQ (MockConOutCall (0)->A, 0);
  ASSERT_EQ (MockConOutCall (1)->Type, MOCK_CALL_SETMODE);
  ASSERT_EQ (MockConOutCall (1)->A, 0);
  ASSERT_EQ (MockConOutCall (2)->Type, MOCK_CALL_SETCURSOR);
  ASSERT_EQ (MockConOutCall (2)->A, 3);
  ASSERT_EQ (MockConOutCall (2)->B, 1);
  ASSERT_OK (TuiFlush ());           /* 首帧整屏，清空脏标记 */

  TuiPutCell (0, 1, 'A', TUI_ATTR_SELECTED);
  TuiPutCell (0, 2, 'B', TUI_ATTR_SELECTED);
  TuiPutCell (0, 3, 'C', TUI_ATTR_DANGER);
  Base = MockConOutTotalCalls ();
  ASSERT_OK (TuiFlush ());
  /* 尾序列：SETCURSOR(1,0) SETATTR(SELECTED) OUTPUT("AB")
             SETCURSOR(3,0) SETATTR(DANGER) OUTPUT("C") SETCURSOR(0,0) */
  ASSERT_EQ (MockConOutTotalCalls () - Base, 7);
  E = MockConOutCall (Base + 0);
  ASSERT_EQ (E->Type, MOCK_CALL_SETCURSOR); ASSERT_EQ (E->A, 1); ASSERT_EQ (E->B, 0);
  E = MockConOutCall (Base + 1);
  ASSERT_EQ (E->Type, MOCK_CALL_SETATTR); ASSERT_EQ (E->A, TUI_ATTR_SELECTED);
  E = MockConOutCall (Base + 2);
  ASSERT_EQ (E->Type, MOCK_CALL_OUTPUT); ASSERT_EQ (E->StrLen, 2);
  ASSERT_EQ (E->Str[0], 'A'); ASSERT_EQ (E->Str[1], 'B');
  E = MockConOutCall (Base + 3);
  ASSERT_EQ (E->Type, MOCK_CALL_SETCURSOR); ASSERT_EQ (E->A, 3); ASSERT_EQ (E->B, 0);
  E = MockConOutCall (Base + 4);
  ASSERT_EQ (E->Type, MOCK_CALL_SETATTR); ASSERT_EQ (E->A, TUI_ATTR_DANGER);
  E = MockConOutCall (Base + 5);
  ASSERT_EQ (E->Type, MOCK_CALL_OUTPUT); ASSERT_EQ (E->StrLen, 1); ASSERT_EQ (E->Str[0], 'C');
  E = MockConOutCall (Base + 6);
  ASSERT_EQ (E->Type, MOCK_CALL_SETCURSOR); ASSERT_EQ (E->A, 0); ASSERT_EQ (E->B, 0);
  ASSERT_EQ (MockConOutScrolls (), 0);
  ASSERT_EQ (MockConOutScreen (0, 3).Char, 'C');
  ASSERT_EQ (MockConOutScreen (0, 3).Attr, TUI_ATTR_DANGER);
  ASSERT_EQ (MockConOutScreen (1, 3).Char, ' ');   /* 右下角保持空格 */
  for (i = 0; i < MockConOutTotalCalls (); i++) {
    ASSERT_FALSE (EFI_ERROR (MockConOutCall (i)->Result));
  }
  ASSERT_OK (TuiShutdown ());
}

/* T06-9 可寻址性探测降级：模拟 OVMF ConSplitter 报告宽度 > 图形实际可寻址宽度 */
TEST (t06_9_addressability_fallback) {
  UINTN Modes[][2] = { {80, 25}, {100, 31} };
  UINTN Rows, Cols;
  CONST MOCK_CALL *E;
  Setup (Modes, 2, 0, 0);
  MockConOutSetCursorColLimit (98);   /* 100 宽模式右缘 2 列不可寻址（OVMF x64 实证） */
  ASSERT_OK (TuiInit ());
  /* 100x31 探测 (99,30) 失败 → 降级 80x25 探测 (79,24) 成功 */
  ASSERT_OK (TuiGetSize (&Rows, &Cols));
  ASSERT_EQ (Rows, 25);
  ASSERT_EQ (Cols, 80);
  E = MockConOutLastOfType (MOCK_CALL_SETMODE);
  ASSERT_TRUE (E != NULL);
  ASSERT_EQ (E->A, 0);
  /* 降级后整屏 Flush 不受列限影响（80 < 98），无错误 */
  ASSERT_OK (TuiFlush ());
  ASSERT_EQ (MockConOutScrolls (), 0);
  ASSERT_OK (TuiShutdown ());
}

int main (void) {
  RUN_TEST (t06_1_mode_select);
  RUN_TEST (t06_2_dirty_region);
  RUN_TEST (t06_3_clamp);
  RUN_TEST (t06_4_golden);
  RUN_TEST (t06_5_restore);
  RUN_TEST (t06_6_no_scroll);
  RUN_TEST (t06_7_uninit_safe);
  RUN_TEST (t06_8_call_sequence);
  RUN_TEST (t06_9_addressability_fallback);
  RUN_TESTS_END ();
}
