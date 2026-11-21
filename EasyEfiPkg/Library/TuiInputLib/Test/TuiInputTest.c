/** @file
  S07 QEMU 冒烟测试应用（仅 UEFI，不进宿主）。

  循环 TuiWaitKey，把每个抽象命令名打印到 ConOut（OVMF/ArmVirtQemu 含 serial），
  并对照编译期注入序列自检；ESC 结束。随后 TuiPollKey 必须 EFI_NOT_READY
  （队列已空）。序列与 smoke/keys-*.txt 一一对应：
    DOWN UP LEFT RIGHT PGDN PGUP HOME END ENTER 'a' BACKSPACE TAB DELETE F4 ESC
  全部匹配 → serial 打 [S07] PASS keys=15；任何偏差 → [S07] FAIL ...。
**/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>

/* MdePkg SimpleTextIn.h 的 SCAN_* 与冻结契约重名（同值）；让位后由契约定义。
   详见 TuiInput.c 顶部注释与 docs/handoffs/S07.md。 */
#undef SCAN_UP
#undef SCAN_DOWN
#undef SCAN_RIGHT
#undef SCAN_LEFT
#undef SCAN_HOME
#undef SCAN_END
#undef SCAN_INSERT
#undef SCAN_DELETE
#undef SCAN_F1
#undef SCAN_F2
#undef SCAN_F3
#undef SCAN_F4
#undef SCAN_F5
#undef SCAN_F6
#undef SCAN_F7
#undef SCAN_F8
#undef SCAN_F9
#undef SCAN_F10
#undef SCAN_ESC
#include "Contract/TuiInput.h"
#include "Contract/TuiTheme.h"

STATIC CONST CHAR16 *mCmdNames[] = {
  L"NONE",
  L"UP", L"DOWN", L"LEFT", L"RIGHT",
  L"PGUP", L"PGDN", L"HOME", L"END",
  L"ENTER", L"ESC", L"BACKSPACE", L"DELETE", L"TAB",
  L"F1", L"F2", L"F3", L"F4", L"F5",
  L"F6", L"F7", L"F8", L"F9", L"F10",
  L"CHAR"
};

STATIC CONST TUI_COMMAND mExpected[] = {
  CMD_DOWN, CMD_UP, CMD_LEFT, CMD_RIGHT, CMD_PGDN, CMD_PGUP, CMD_HOME, CMD_END,
  CMD_ENTER, CMD_CHAR, CMD_BACKSPACE, CMD_TAB, CMD_DELETE, CMD_F4, CMD_ESC
};
#define EXPECTED_N     (sizeof (mExpected) / sizeof (mExpected[0]))
#define EXPECTED_CHAR  L'a'

EFI_STATUS EFIAPI UefiMain (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  TUI_KEY     Key;
  TUI_COMMAND Cmd;
  EFI_STATUS  Status;
  UINTN       i;
  BOOLEAN     Failed;

  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  Print (L"[S07] ready\n");

  i = 0;
  Failed = FALSE;
  for (;;) {
    Status = TuiWaitKey (&Key);
    if (EFI_ERROR (Status)) {
      Print (L"[S07] FAIL waitkey=%r\n", Status);
      return Status;
    }
    Cmd = TuiKeyToCommand (&Key);
    Print (L"[S07] key %s", mCmdNames[Cmd]);
    if (Cmd == CMD_CHAR) {
      Print (L" U+%04x", (UINTN)Key.UnicodeChar);
    }
    Print (L"\n");

    if (i >= EXPECTED_N || Cmd != mExpected[i] ||
        (Cmd == CMD_CHAR && Key.UnicodeChar != EXPECTED_CHAR)) {
      Print (L"[S07] FAIL seq=%u got=%s\n", i, mCmdNames[Cmd]);
      Failed = TRUE;
    }
    i++;
    if (Cmd == CMD_ESC) {
      break;
    }
  }

  /* 序列耗尽后 ConIn 队列必须已空：验证 TuiPollKey 非阻塞 NOT_READY 路径 */
  Status = TuiPollKey (&Key);
  if (Status == EFI_NOT_READY) {
    Print (L"[S07] poll-empty\n");
  } else {
    Print (L"[S07] FAIL poll=%r\n", Status);
    Failed = TRUE;
  }

  if (!Failed && i == EXPECTED_N) {
    Print (L"[S07] PASS keys=%u\n", i);
    return EFI_SUCCESS;
  }
  Print (L"[S07] FAIL total\n");
  return EFI_ABORTED;
}
