/* MockTui — TuiScreen/TuiInput 宿主 mock（S00，leader 独占）。
 * 字符矩阵 + 脚本化按键队列；应用 core 宿主测试用。 */
#include "Contract/TuiScreen.h"
#include "Contract/TuiInput.h"
#include "Contract/EfiMem.h"
#include "MockTui.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MOCK_MAX_ROWS 64
#define MOCK_MAX_COLS 200
#define MOCK_KEY_QUEUE 256

static TUI_CELL g_cells[MOCK_MAX_ROWS][MOCK_MAX_COLS];
static UINTN g_rows = 25, g_cols = 80;
static TUI_KEY g_keys[MOCK_KEY_QUEUE];
static UINTN g_key_head = 0, g_key_tail = 0;

void MockTuiSetSize (UINTN Rows, UINTN Cols) {
  g_rows = Rows < MOCK_MAX_ROWS ? Rows : MOCK_MAX_ROWS;
  g_cols = Cols < MOCK_MAX_COLS ? Cols : MOCK_MAX_COLS;
}
void MockTuiPushKey (UINT16 Scan, CHAR16 Uni) {
  if ((g_key_tail + 1) % MOCK_KEY_QUEUE == g_key_head) return;
  g_keys[g_key_tail].ScanCode = Scan;
  g_keys[g_key_tail].UnicodeChar = Uni;
  g_key_tail = (g_key_tail + 1) % MOCK_KEY_QUEUE;
}
TUI_CELL MockTuiGetCell (UINTN Row, UINTN Col) { return g_cells[Row][Col]; }
void MockTuiRowText (UINTN Row, CHAR16 *Out, UINTN Max) {
  UINTN i;
  for (i = 0; i < g_cols && i + 1 < Max; i++) Out[i] = g_cells[Row][i].Char;
  Out[i] = 0;
}
void MockTuiReset (VOID) {
  memset (g_cells, 0, sizeof (g_cells));
  g_key_head = g_key_tail = 0;
}

EFI_STATUS TuiInit (VOID) { memset (g_cells, 0, sizeof (g_cells)); return EFI_SUCCESS; }
EFI_STATUS TuiGetSize (UINTN *Rows, UINTN *Cols) { *Rows = g_rows; *Cols = g_cols; return EFI_SUCCESS; }
VOID TuiPutCell (UINTN Row, UINTN Col, CHAR16 Ch, UINT8 Attr) {
  if (Row >= g_rows || Col >= g_cols) return;
  /* 属性合法性守卫（S08 建议）：真实固件 SetAttribute 拒绝 bit7（背景仅 bit4-6，
     无亮背景）。宿主 mock 在此拦截，使非法属性 bug 宿主测试即暴露（此前 MockTui
     不校验→TUI_ATTR_SELECTED 0xB0 宿主全绿但实机不渲染）。 */
  if (Attr & 0x80) {
    fprintf (stderr, "MockTui: INVALID attr 0x%02x (bit7 set) at (%u,%u) — firmware would reject\n",
             Attr, (unsigned)Row, (unsigned)Col);
    abort ();
  }
  g_cells[Row][Col].Char = Ch; g_cells[Row][Col].Attr = Attr;
}
VOID TuiPutText (UINTN Row, UINTN Col, CONST CHAR16 *Text, UINT8 Attr) {
  UINTN i;
  for (i = 0; Text[i]; i++) TuiPutCell (Row, Col + i, Text[i], Attr);
}
VOID TuiFill (UINTN Row, UINTN Col, UINTN W, UINTN H, CHAR16 Ch, UINT8 Attr) {
  UINTN r, c;
  for (r = Row; r < Row + H; r++) for (c = Col; c < Col + W; c++) TuiPutCell (r, c, Ch, Attr);
}
VOID TuiClear (VOID) { memset (g_cells, 0, sizeof (g_cells)); }
EFI_STATUS TuiFlush (VOID) { return EFI_SUCCESS; }
EFI_STATUS TuiShutdown (VOID) { return EFI_SUCCESS; }

EFI_STATUS TuiWaitKey (TUI_KEY *Key) {
  if (g_key_head == g_key_tail) return EFI_NOT_READY;
  *Key = g_keys[g_key_head];
  g_key_head = (g_key_head + 1) % MOCK_KEY_QUEUE;
  return EFI_SUCCESS;
}
EFI_STATUS TuiPollKey (TUI_KEY *Key) { return TuiWaitKey (Key); }
TUI_COMMAND TuiKeyToCommand (CONST TUI_KEY *Key) {
  if (Key->ScanCode) {
    switch (Key->ScanCode) {
      case SCAN_UP: return CMD_UP;       case SCAN_DOWN: return CMD_DOWN;
      case SCAN_LEFT: return CMD_LEFT;   case SCAN_RIGHT: return CMD_RIGHT;
      case SCAN_PGUP: return CMD_PGUP;   case SCAN_PGDN: return CMD_PGDN;
      case SCAN_HOME: return CMD_HOME;   case SCAN_END: return CMD_END;
      case SCAN_DELETE: return CMD_DELETE;
      case SCAN_ESC: return CMD_ESC;
      case SCAN_F1: return CMD_F1;       case SCAN_F2: return CMD_F2;
      case SCAN_F3: return CMD_F3;       case SCAN_F4: return CMD_F4;
      case SCAN_F5: return CMD_F5;       case SCAN_F6: return CMD_F6;
      case SCAN_F7: return CMD_F7;       case SCAN_F8: return CMD_F8;
      case SCAN_F9: return CMD_F9;       case SCAN_F10: return CMD_F10;
      default: return CMD_NONE;
    }
  }
  switch (Key->UnicodeChar) {
    case 0x0D: return CMD_ENTER;
    case 0x08: return CMD_BACKSPACE;
    case 0x09: return CMD_TAB;
    case 0: return CMD_NONE;
    default: return CMD_CHAR;
  }
}
