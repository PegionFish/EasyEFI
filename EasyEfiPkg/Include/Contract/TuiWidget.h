/** @file
  控件契约（S00 冻结；S08 实现）。控件无业务语义：只认 attr/disabled 标志。
  宿主可测：基于 TuiScreen mock 的字符矩阵断言。
**/
#ifndef TUI_WIDGET_H
#define TUI_WIDGET_H

#include "EfiCompat.h"
#include "TuiTheme.h"
#include "TuiScreen.h"
#include "TuiInput.h"

typedef struct {
  CONST CHAR16 *Text;
  UINT8         Attr;       /* 基础属性；选中态由控件叠加 TUI_ATTR_SELECTED */
  BOOLEAN       Disabled;   /* 灰显且导航跳过 */
} LIST_ITEM;

typedef struct WIDGET_LIST WIDGET_LIST;
WIDGET_LIST *ListNew (UINTN VisibleRows);
VOID         ListFree (WIDGET_LIST *L);
EFI_STATUS   ListSetItems (WIDGET_LIST *L, CONST LIST_ITEM *Items, UINTN Count); /* 拷贝文本 */
EFI_STATUS   ListHandleKey (WIDGET_LIST *L, CONST TUI_KEY *Key);  /* 上下/翻页/数字直选/跳过 disabled */
UINTN        ListSelected (WIDGET_LIST *L);
VOID         ListDraw (WIDGET_LIST *L, UINTN Row, UINTN Col, UINTN Width);

/* 确认对话框：danger 场景 DefaultYes=FALSE；ESC=取消（*Yes=FALSE） */
EFI_STATUS DialogConfirm (CONST CHAR16 *Title, CONST CHAR16 *Body, BOOLEAN DefaultYes, BOOLEAN *Yes);

typedef struct WIDGET_INPUT WIDGET_INPUT;
WIDGET_INPUT *InputNew (UINTN MaxLen, BOOLEAN AsciiOnly);
VOID          InputFree (WIDGET_INPUT *I);
EFI_STATUS    InputHandleKey (WIDGET_INPUT *I, CONST TUI_KEY *Key); /* 编辑/退格/Enter 结束 */
BOOLEAN       InputDone (WIDGET_INPUT *I);
CONST CHAR16 *InputText (WIDGET_INPUT *I);

VOID BannerDraw (UINTN Row, CONST CHAR16 *Text, UINT8 Attr);
VOID KeybarDraw (CONST CHAR16 *Text);   /* 底部常驻键位条 */
VOID BoxDraw (UINTN Row, UINTN Col, UINTN W, UINTN H, CONST CHAR16 *Title); /* 纯 ASCII +-| */

#endif
