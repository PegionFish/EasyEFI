/** @file
  TuiInputLib 真实后端（S07）：ConIn 按键读取 + 扫描码→抽象命令映射。

  - TuiKeyToCommand：纯逻辑，宿主/UEFI 双环境编译同一份代码；
    语义与 S00 宿主 mock（tests/host/mocks/MockTui.c）逐条一致，
    宿主 parity 测试全量扫描验证（tests/test_mock_parity.c）。
  - TuiWaitKey/TuiPollKey：UEFI 胶水（gBS/gST），仅 UEFI_BUILD 编译。
  全键盘（design D7）：不碰 SimplePointer/鼠标。库内不做业务分发。
**/
#ifdef UEFI_BUILD
/* AutoGen.h 强制先引入 MdePkg <Uefi.h> → Protocol/SimpleTextIn.h，其 SCAN_*
   宏与冻结契约 TuiInput.h 重名（值逐条相同，UEFI Spec 2.10 §12.2；仅拼写
   0x01 vs 0x0001 差异触发 -Werror 重定义）。契约不可改：此处 undef 让位，
   由契约头重新定义。SCAN_PGUP/PGDN 为契约独有名（MdePkg 用 SCAN_PAGE_*），
   无需处理。消费流（S08/S11-S13）UEFI 侧 include TuiInput.h 前需同样处理，
   或待契约变更提案（见 docs/handoffs/S07.md）。 */
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
#endif
#include "Contract/TuiInput.h"

#ifdef UEFI_BUILD
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SimpleTextIn.h>
#endif

/**
  扫描码→抽象命令映射（契约 TuiInput.h；键位表 design §5.5/§7.2）。

  ScanCode 非 0：查扫描码表（方向/翻页/Home/End/Delete/ESC/F1..F10），
  未收录扫描码（含 SCAN_INSERT）→ CMD_NONE；UnicodeChar 被忽略。
  ScanCode 为 0：回车 0x0D→ENTER，退格 0x08→BACKSPACE，Tab 0x09→TAB，
  NUL→CMD_NONE，其余（含其它控制符）→ CMD_CHAR 透传 UnicodeChar。

  @param[in] Key  按键（ScanCode + UnicodeChar）。

  @return 抽象命令枚举；CHAR 命令的字符见 Key->UnicodeChar。
**/
TUI_COMMAND TuiKeyToCommand (CONST TUI_KEY *Key) {
  if (Key->ScanCode) {
    switch (Key->ScanCode) {
      case SCAN_UP:    return CMD_UP;
      case SCAN_DOWN:  return CMD_DOWN;
      case SCAN_LEFT:  return CMD_LEFT;
      case SCAN_RIGHT: return CMD_RIGHT;
      case SCAN_PGUP:  return CMD_PGUP;
      case SCAN_PGDN:  return CMD_PGDN;
      case SCAN_HOME:  return CMD_HOME;
      case SCAN_END:   return CMD_END;
      case SCAN_DELETE: return CMD_DELETE;
      case SCAN_ESC:   return CMD_ESC;
      case SCAN_F1:    return CMD_F1;
      case SCAN_F2:    return CMD_F2;
      case SCAN_F3:    return CMD_F3;
      case SCAN_F4:    return CMD_F4;
      case SCAN_F5:    return CMD_F5;
      case SCAN_F6:    return CMD_F6;
      case SCAN_F7:    return CMD_F7;
      case SCAN_F8:    return CMD_F8;
      case SCAN_F9:    return CMD_F9;
      case SCAN_F10:   return CMD_F10;
      default:         return CMD_NONE;
    }
  }
  switch (Key->UnicodeChar) {
    case 0x0D: return CMD_ENTER;
    case 0x08: return CMD_BACKSPACE;
    case 0x09: return CMD_TAB;
    case 0:    return CMD_NONE;
    default:   return CMD_CHAR;
  }
}

#ifdef UEFI_BUILD

/**
  阻塞读键：WaitForEvent(ConIn.WaitForKey) 后 ReadKeyStroke。

  WaitForEvent 返回后 ReadKeyStroke 仍可能 EFI_NOT_READY（事件被其它
  消费者抢走/伪唤醒），此时回到事件等待重试，不向上层吐 NOT_READY。

  @param[out] Key  读到的按键。

  @retval EFI_SUCCESS           读到一键。
  @retval EFI_INVALID_PARAMETER Key 为 NULL。
  @retval 其它                  WaitForEvent/ReadKeyStroke 的设备错误。
**/
EFI_STATUS TuiWaitKey (TUI_KEY *Key) {
  EFI_STATUS    Status;
  UINTN         Index;
  EFI_INPUT_KEY EfiKey;

  if (Key == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  for (;;) {
    Status = gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &EfiKey);
    if (Status == EFI_NOT_READY) {
      continue;
    }
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Key->ScanCode    = EfiKey.ScanCode;
    Key->UnicodeChar = EfiKey.UnicodeChar;
    return EFI_SUCCESS;
  }
}

/**
  非阻塞读键：直接 ReadKeyStroke，无键返回 EFI_NOT_READY。

  @param[out] Key  读到的按键（仅 EFI_SUCCESS 时有效）。

  @retval EFI_SUCCESS           读到一键。
  @retval EFI_NOT_READY         当前无键。
  @retval EFI_INVALID_PARAMETER Key 为 NULL。
  @retval 其它                  ConIn 设备错误。
**/
EFI_STATUS TuiPollKey (TUI_KEY *Key) {
  EFI_STATUS    Status;
  EFI_INPUT_KEY EfiKey;

  if (Key == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &EfiKey);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Key->ScanCode    = EfiKey.ScanCode;
  Key->UnicodeChar = EfiKey.UnicodeChar;
  return EFI_SUCCESS;
}

#endif /* UEFI_BUILD */
