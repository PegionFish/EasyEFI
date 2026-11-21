/* S07 宿主测试 T07-4b：与 S00 MockTui 映射的语义 parity（全量扫描）。
 *
 * 手法：把 leader 独占的 MockTui.c 以重命名符号 include 进本翻译单元，
 * 再 include 真实 TuiInput.c（宿主分支只含映射函数），逐键对比两者输出。
 * 不修改 mock 源码；覆盖全部 65536 扫描码 × 全部 65536 Unicode 及交叉组合。
 */
#include "test.h"

#define TuiKeyToCommand MockTuiKeyToCommand
#include "MockTui.c"
#undef TuiKeyToCommand

#include "../TuiInput.c"   /* 宿主分支：仅 TuiKeyToCommand（无 UEFI_BUILD） */

/* 全扫描码空间（UnicodeChar=0） */
TEST (t07_4_parity_scan_sweep) {
  TUI_KEY k;
  UINT32 scan;
  k.UnicodeChar = 0;
  for (scan = 0; scan <= 0xFFFF; scan++) {
    k.ScanCode = (UINT16)scan;
    ASSERT_EQ (TuiKeyToCommand (&k), MockTuiKeyToCommand (&k));
  }
}

/* 全 Unicode 空间（ScanCode=0） */
TEST (t07_4_parity_unicode_sweep) {
  TUI_KEY k;
  UINT32 uni;
  k.ScanCode = 0;
  for (uni = 0; uni <= 0xFFFF; uni++) {
    k.UnicodeChar = (CHAR16)uni;
    ASSERT_EQ (TuiKeyToCommand (&k), MockTuiKeyToCommand (&k));
  }
}

/* 交叉组合：已映射/未映射扫描码 × 代表性 Unicode（验证扫描码优先级一致） */
TEST (t07_4_parity_cross) {
  static CONST UINT16 Scans[] = {
    0, SCAN_UP, SCAN_DOWN, SCAN_LEFT, SCAN_RIGHT, SCAN_HOME, SCAN_END,
    SCAN_INSERT, SCAN_DELETE, SCAN_PGUP, SCAN_PGDN,
    SCAN_F1, SCAN_F2, SCAN_F3, SCAN_F4, SCAN_F5,
    SCAN_F6, SCAN_F7, SCAN_F8, SCAN_F9, SCAN_F10, SCAN_ESC,
    0x15, 0x16, 0x18, 0xFF, 0x100, 0xFFFF
  };
  static CONST CHAR16 Unis[] = { 0, 0x01, 0x08, 0x09, 0x0A, 0x0D, 0x1B, 0x20, L'a', 0x7F, 0x4E2D, 0xFFFF };
  TUI_KEY k;
  UINTN i, j;
  for (i = 0; i < sizeof (Scans) / sizeof (Scans[0]); i++) {
    for (j = 0; j < sizeof (Unis) / sizeof (Unis[0]); j++) {
      k.ScanCode = Scans[i];
      k.UnicodeChar = Unis[j];
      ASSERT_EQ (TuiKeyToCommand (&k), MockTuiKeyToCommand (&k));
    }
  }
}

int main (void) {
  RUN_TEST (t07_4_parity_scan_sweep);
  RUN_TEST (t07_4_parity_unicode_sweep);
  RUN_TEST (t07_4_parity_cross);
  RUN_TESTS_END ();
}
