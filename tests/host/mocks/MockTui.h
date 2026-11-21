/* MockTui 测试接口（S00） */
#ifndef MOCK_TUI_H
#define MOCK_TUI_H
#include "Contract/TuiScreen.h"
#include "Contract/TuiInput.h"

void MockTuiSetSize (UINTN Rows, UINTN Cols);
void MockTuiPushKey (UINT16 Scan, CHAR16 Uni);
TUI_CELL MockTuiGetCell (UINTN Row, UINTN Col);
void MockTuiRowText (UINTN Row, CHAR16 *Out, UINTN Max);
void MockTuiReset (VOID);
#endif
