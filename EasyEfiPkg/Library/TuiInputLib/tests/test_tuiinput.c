/* S07 宿主测试：TuiKeyToCommand 映射（T07-1..3 + T07-4 常量一致性）。
 * T07-4 的 mock 语义 parity 全量扫描见 test_mock_parity.c（独立二进制）。
 * 运行: make -f host.mk test
 */
#include "test.h"
#include <stddef.h>
#include "Contract/TuiInput.h"
#include "Contract/TuiTheme.h"

/* ---- T07-1 扫描码映射全表（方向/翻页/Home/End/Delete/功能键/ESC） ---- */
TEST (t07_1_scan_table) {
  static CONST struct { UINT16 Scan; TUI_COMMAND Cmd; } Table[] = {
    { SCAN_UP,     CMD_UP     },
    { SCAN_DOWN,   CMD_DOWN   },
    { SCAN_LEFT,   CMD_LEFT   },
    { SCAN_RIGHT,  CMD_RIGHT  },
    { SCAN_PGUP,   CMD_PGUP   },
    { SCAN_PGDN,   CMD_PGDN   },
    { SCAN_HOME,   CMD_HOME   },
    { SCAN_END,    CMD_END    },
    { SCAN_DELETE, CMD_DELETE },
    { SCAN_ESC,    CMD_ESC    },
    { SCAN_F1,     CMD_F1     },
    { SCAN_F2,     CMD_F2     },
    { SCAN_F3,     CMD_F3     },
    { SCAN_F4,     CMD_F4     },
    { SCAN_F5,     CMD_F5     },
    { SCAN_F6,     CMD_F6     },
    { SCAN_F7,     CMD_F7     },
    { SCAN_F8,     CMD_F8     },
    { SCAN_F9,     CMD_F9     },
    { SCAN_F10,    CMD_F10    },
  };
  UINTN i;
  TUI_KEY k;
  for (i = 0; i < sizeof (Table) / sizeof (Table[0]); i++) {
    k.ScanCode = Table[i].Scan;
    k.UnicodeChar = 0;
    ASSERT_EQ (TuiKeyToCommand (&k), Table[i].Cmd);
  }
  /* SCAN_INSERT 未收录于命令表（契约无 CMD_INSERT）→ CMD_NONE */
  k.ScanCode = SCAN_INSERT; k.UnicodeChar = 0;
  ASSERT_EQ (TuiKeyToCommand (&k), CMD_NONE);
}

/* ---- T07-2 可打印字符透传 + 特殊 Unicode 控制键 ---- */
TEST (t07_2_unicode) {
  TUI_KEY k;
  k.ScanCode = 0;
  /* 可打印 ASCII 与非 ASCII：CMD_CHAR 且 UnicodeChar 原样保留 */
  static CONST CHAR16 Chars[] = { L'a', L'Z', L'0', L' ', L'~', 0x4E2D, 0xFFFD };
  UINTN i;
  for (i = 0; i < sizeof (Chars) / sizeof (Chars[0]); i++) {
    k.UnicodeChar = Chars[i];
    ASSERT_EQ (TuiKeyToCommand (&k), CMD_CHAR);
    ASSERT_EQ (k.UnicodeChar, Chars[i]);
  }
  /* 回车/退格/Tab/NUL（与 MockTui 语义一致） */
  k.UnicodeChar = 0x0D; ASSERT_EQ (TuiKeyToCommand (&k), CMD_ENTER);
  k.UnicodeChar = 0x08; ASSERT_EQ (TuiKeyToCommand (&k), CMD_BACKSPACE);
  k.UnicodeChar = 0x09; ASSERT_EQ (TuiKeyToCommand (&k), CMD_TAB);
  k.UnicodeChar = 0x00; ASSERT_EQ (TuiKeyToCommand (&k), CMD_NONE);
  /* 其它控制符按 mock 语义透传为 CHAR（0x0A/0x1B/0x7F 不做特殊解释） */
  k.UnicodeChar = 0x01; ASSERT_EQ (TuiKeyToCommand (&k), CMD_CHAR);
  k.UnicodeChar = 0x0A; ASSERT_EQ (TuiKeyToCommand (&k), CMD_CHAR);
  k.UnicodeChar = 0x1B; ASSERT_EQ (TuiKeyToCommand (&k), CMD_CHAR);
  k.UnicodeChar = 0x7F; ASSERT_EQ (TuiKeyToCommand (&k), CMD_CHAR);
}

/* ---- T07-3 未知扫描码 → CMD_NONE 不崩；扫描码优先于 UnicodeChar ---- */
TEST (t07_3_unknown_scan) {
  TUI_KEY k;
  static CONST UINT16 Unknown[] = { 0x15, 0x16, 0x18, 0x1F, 0x7F, 0xFF, 0x100, 0x8000, 0xFFFF };
  UINTN i;
  for (i = 0; i < sizeof (Unknown) / sizeof (Unknown[0]); i++) {
    k.ScanCode = Unknown[i];
    k.UnicodeChar = 0;
    ASSERT_EQ (TuiKeyToCommand (&k), CMD_NONE);
    /* 未知扫描码 + 可打印字符：仍 NONE（扫描码路径忽略 Unicode） */
    k.UnicodeChar = L'x';
    ASSERT_EQ (TuiKeyToCommand (&k), CMD_NONE);
  }
  /* 已知扫描码 + 非零 Unicode：扫描码映射优先 */
  k.ScanCode = SCAN_UP; k.UnicodeChar = L'x';
  ASSERT_EQ (TuiKeyToCommand (&k), CMD_UP);
  k.ScanCode = SCAN_ESC; k.UnicodeChar = 0x0D;
  ASSERT_EQ (TuiKeyToCommand (&k), CMD_ESC);
}

/* ---- T07-4a 契约常量一致性（TuiTheme 命令枚举 / UEFI Spec 2.10 §12.2 扫描码） ---- */
TEST (t07_4_theme_constants) {
  /* 命令枚举基值：CMD_NONE=0，CHAR 为末项（TuiTheme.h 冻结顺序） */
  ASSERT_EQ (CMD_NONE, 0);
  ASSERT_TRUE (CMD_CHAR > CMD_F10);
  /* 扫描码常量值 = UEFI Spec 2.10 §12.2（契约头冻结值防漂移） */
  ASSERT_EQ (SCAN_UP, 0x01);    ASSERT_EQ (SCAN_DOWN, 0x02);
  ASSERT_EQ (SCAN_RIGHT, 0x03); ASSERT_EQ (SCAN_LEFT, 0x04);
  ASSERT_EQ (SCAN_HOME, 0x05);  ASSERT_EQ (SCAN_END, 0x06);
  ASSERT_EQ (SCAN_INSERT, 0x07); ASSERT_EQ (SCAN_DELETE, 0x08);
  ASSERT_EQ (SCAN_PGUP, 0x09);  ASSERT_EQ (SCAN_PGDN, 0x0A);
  ASSERT_EQ (SCAN_F1, 0x0B);    ASSERT_EQ (SCAN_F10, 0x14);
  ASSERT_EQ (SCAN_ESC, 0x17);
  /* TUI_KEY 布局：ScanCode 在前，UnicodeChar 在后（与 EFI_INPUT_KEY 同构） */
  ASSERT_EQ (sizeof (TUI_KEY), 4);
  ASSERT_EQ (offsetof (TUI_KEY, ScanCode), 0);
  ASSERT_EQ (offsetof (TUI_KEY, UnicodeChar), 2);
}

int main (void) {
  RUN_TEST (t07_1_scan_table);
  RUN_TEST (t07_2_unicode);
  RUN_TEST (t07_3_unknown_scan);
  RUN_TEST (t07_4_theme_constants);
  RUN_TESTS_END ();
}
