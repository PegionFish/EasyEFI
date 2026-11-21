/** @file
  S08 QEMU 冒烟测试应用（仅 UEFI，不进宿主）。

  经真实 TuiScreen/TuiInput 后端渲染 横幅+列表+键位条+对话框+输入行，
  QMP 注入按键驱动控件导航，serial 打印选中序号/对话框结果/输入文本标记，
  并对照编译期期望序列自检。序列与 smoke/keys-*.txt 及 smoke/expect.txt 一致：
    列表: DOWN DOWN DOWN UP '5' '2' HOME END  → sel 2 4 4 2 4 4 0 4
    对话: 'y'(默认N) RET(默认Y) ESC(默认Y)    → yes=1/1/0(abort)
    输入: 'a' 'b' 'c' BACKSPACE RET           → "ab" done=1
  全部匹配 → [S08] PASS；任何偏差 → [S08] FAIL ...。
  纯 UEFI glue：允许裸 L""（build-recipe 编码纪律 1）。
**/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include "Contract/TuiWidget.h"

STATIC CONST UINTN mExpectedSel[] = { 2, 4, 4, 2, 4, 4, 0, 4 };
#define NAV_KEYS  (sizeof (mExpectedSel) / sizeof (mExpectedSel[0]))

STATIC VOID DrawFrame (WIDGET_LIST *L) {
  BannerDraw (0, L"EasyEFI S08 widget smoke", TUI_ATTR_TITLE);
  ListDraw (L, 2, 2, 60);
  KeybarDraw (L"Up/Dn/0-9 select  Enter exec  ESC back");
}

EFI_STATUS EFIAPI UefiMain (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  LIST_ITEM   Items[5];
  WIDGET_LIST *L;
  WIDGET_INPUT *I;
  TUI_KEY     Key;
  EFI_STATUS  Status;
  BOOLEAN     Yes;
  BOOLEAN     Failed = FALSE;
  UINTN       i;

  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  Status = TuiInit ();
  if (EFI_ERROR (Status)) {
    Print (L"[S08] FAIL TuiInit=%r\n", Status);
    return Status;
  }

  Items[0].Text = L"alpha";   Items[0].Attr = TUI_ATTR_NORMAL; Items[0].Disabled = FALSE;
  Items[1].Text = L"bravo";   Items[1].Attr = TUI_ATTR_NORMAL; Items[1].Disabled = TRUE;
  Items[2].Text = L"charlie"; Items[2].Attr = TUI_ATTR_NORMAL; Items[2].Disabled = FALSE;
  Items[3].Text = L"delta";   Items[3].Attr = TUI_ATTR_NORMAL; Items[3].Disabled = TRUE;
  Items[4].Text = L"echo";    Items[4].Attr = TUI_ATTR_NORMAL; Items[4].Disabled = FALSE;

  L = ListNew (5);
  if (L == NULL || EFI_ERROR (ListSetItems (L, Items, 5))) {
    Print (L"[S08] FAIL list-setup\n");
    TuiShutdown ();
    return EFI_ABORTED;
  }
  DrawFrame (L);
  TuiFlush ();
  Print (L"[S08] ready\n");
  Print (L"[S08] sel %u\n", (UINTN)ListSelected (L));   /* 初始 0 */

  /* ---- 列表导航（8 键，跳过 disabled + 数字直选 + 首尾钳制） ---- */
  for (i = 0; i < NAV_KEYS; i++) {
    Status = TuiWaitKey (&Key);
    if (EFI_ERROR (Status)) {
      Print (L"[S08] FAIL waitkey=%r\n", Status);
      Failed = TRUE;
      break;
    }
    Status = ListHandleKey (L, &Key);
    if (EFI_ERROR (Status)) {
      Print (L"[S08] FAIL listkey=%r\n", Status);
      Failed = TRUE;
    }
    DrawFrame (L);
    TuiFlush ();
    Print (L"[S08] sel %u\n", (UINTN)ListSelected (L));
    if (ListSelected (L) != mExpectedSel[i]) {
      Print (L"[S08] FAIL seq=%u want=%u\n", (UINTN)i, (UINTN)mExpectedSel[i]);
      Failed = TRUE;
    }
  }

  /* ---- 对话框（Y/N/默认/ESC；关闭后调用方重绘底屏） ---- */
  Status = DialogConfirm (L"Confirm", L"Proceed (default N)?", FALSE, &Yes);
  Print (L"[S08] dlg1 yes=%u st=%u\n", (UINTN)Yes, (UINTN)(Status == EFI_SUCCESS));
  if (Yes != TRUE || EFI_ERROR (Status)) Failed = TRUE;
  DrawFrame (L); TuiFlush ();

  Status = DialogConfirm (L"Confirm", L"Proceed (default Y)?", TRUE, &Yes);
  Print (L"[S08] dlg2 yes=%u st=%u\n", (UINTN)Yes, (UINTN)(Status == EFI_SUCCESS));
  if (Yes != TRUE || EFI_ERROR (Status)) Failed = TRUE;
  DrawFrame (L); TuiFlush ();

  Status = DialogConfirm (L"Confirm", L"Cancel via ESC?", TRUE, &Yes);
  Print (L"[S08] dlg3 yes=%u abort=%u\n", (UINTN)Yes, (UINTN)(Status == EFI_ABORTED));
  if (Yes != FALSE || Status != EFI_ABORTED) Failed = TRUE;
  DrawFrame (L); TuiFlush ();

  /* ---- 输入行（契约无绘制面：glue 自行渲染 InputText） ---- */
  I = InputNew (8, TRUE);
  if (I == NULL) {
    Print (L"[S08] FAIL input-setup\n");
    Failed = TRUE;
  } else {
    for (i = 0; i < 5; i++) {
      Status = TuiWaitKey (&Key);
      if (EFI_ERROR (Status)) {
        Print (L"[S08] FAIL waitkey=%r\n", Status);
        Failed = TRUE;
        break;
      }
      InputHandleKey (I, &Key);
      TuiFill (9, 2, 20, 1, ' ', TUI_ATTR_NORMAL);
      TuiPutText (9, 2, InputText (I), TUI_ATTR_NORMAL);
      TuiFlush ();
    }
    Print (L"[S08] input=%s done=%u\n", InputText (I), (UINTN)InputDone (I));
    if (EfiStrCmp (InputText (I), L"ab") != 0 || !InputDone (I)) Failed = TRUE;
    InputFree (I);
  }

  ListFree (L);
  TuiShutdown ();
  if (!Failed) {
    Print (L"[S08] PASS\n");
    return EFI_SUCCESS;
  }
  Print (L"[S08] FAIL total\n");
  return EFI_ABORTED;
}
