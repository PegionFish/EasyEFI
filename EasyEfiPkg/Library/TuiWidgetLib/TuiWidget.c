/** @file
  TuiWidgetLib — 共享控件（S08 实现；契约 Include/Contract/TuiWidget.h）。
  列表 / 确认对话框 / 单行输入 / 横幅 / 键位条 / ASCII 画框。

  控件无业务语义：只认 attr/disabled 标志，不知"固件/危险"（任务书禁止项）。
  绘制全部经 TuiScreen 契约（TuiPutCell/TuiFill/TuiPutText/TuiGetSize，越界由
  屏幕层钳制）；按键经 TuiKeyToCommand 抽象命令（不自行解码扫描码）。
  内存走 MemAlloc/MemFree（callee 分配、caller 释放）；双环境同一份代码，
  宿主测试链接 MockTui，UEFI 链接 TuiScreenLib/TuiInputLib。

  行为约定（详见 docs/handoffs/S08.md）：
    - ListHandleKey：导航/数字直选消费按键返回 EFI_SUCCESS；ENTER/ESC/F 键等
      不消费返回 EFI_NOT_READY，由调用方分发（控件不做业务动作）。
    - 列表编号 = 绝对序号 index+1（右对齐，宽 = Count 的位数），数字 '1'-'9'
      直选第 1-9 项、'0' 直选第 10 项；disabled/越界忽略。
    - 选中态属性覆盖：Disabled → ATTR_DISABLED；Selected → ATTR_SELECTED；
      其余用条目基础 Attr。上下/翻页在首尾钳制（不回绕），导航跳过 disabled。
    - DialogConfirm：Y/y/N/n 直答；Enter = DefaultYes；ESC = 取消
      （*Yes=FALSE，返回 EFI_ABORTED）；其它键忽略。调用方仅在
      !EFI_ERROR 且 *Yes==TRUE 时继续。对话框关闭后由调用方重绘底屏
      （TuiScreen 契约无保存/恢复 API）。
    - InputHandleKey：AsciiOnly 拒绝非 ASCII（含控制字符）返回 EFI_UNSUPPORTED，
      容量满返回 EFI_BAD_BUFFER_SIZE——契约未给输入行绘制面，闪烁提示由调用方
      依据返回码渲染。Enter → InputDone；ESC/F 键等不消费（EFI_NOT_READY）。
    - BoxDraw 纯 ASCII +-|（固件字体不保证 CP437 制表符 [design §5.5]）。
**/
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/TuiWidget.h"

/* ------------------------------------------------------------------ */
/* 内部助手                                                            */
/* ------------------------------------------------------------------ */

/* 屏幕尺寸（TuiGetSize 失败时保底 80x25，design §5.5）。 */
STATIC VOID ScreenSize (UINTN *Rows, UINTN *Cols) {
  if (EFI_ERROR (TuiGetSize (Rows, Cols)) || *Rows == 0 || *Cols == 0) {
    *Rows = 25;
    *Cols = 80;
  }
}

/* 写文本，最多 MaxChars 个字符（控件宽度内截断；屏幕边界由 TuiPutCell 钳制）。 */
STATIC VOID PutTextClipped (UINTN Row, UINTN Col, CONST CHAR16 *Text, UINT8 Attr, UINTN MaxChars) {
  UINTN i;
  if (Text == NULL) return;
  for (i = 0; i < MaxChars && Text[i] != 0; i++) {
    TuiPutCell (Row, Col + i, Text[i], Attr);
  }
}

/* 十进制位数（Count>=1）。 */
STATIC UINTN DecDigits (UINTN N) {
  UINTN D = 1;
  while (N >= 10) { D++; N /= 10; }
  return D;
}

/* ------------------------------------------------------------------ */
/* 列表                                                                */
/* ------------------------------------------------------------------ */

struct WIDGET_LIST {
  UINTN      VisibleRows;
  LIST_ITEM  *Items;       /* 深拷贝：Text 指向自有缓冲 */
  UINTN      Count;
  UINTN      Selected;
  UINTN      Top;          /* 视口首项 */
};

STATIC VOID FreeItems (WIDGET_LIST *L) {
  UINTN i;
  if (L->Items != NULL) {
    for (i = 0; i < L->Count; i++) {
      MemFree ((VOID *)L->Items[i].Text);
    }
    MemFree (L->Items);
    L->Items = NULL;
  }
  L->Count = 0;
}

WIDGET_LIST *ListNew (UINTN VisibleRows) {
  WIDGET_LIST *L = (WIDGET_LIST *)MemAlloc (sizeof (WIDGET_LIST));
  if (L == NULL) return NULL;
  L->VisibleRows = VisibleRows == 0 ? 1 : VisibleRows;   /* 退化参数钳制为 1 */
  return L;
}

VOID ListFree (WIDGET_LIST *L) {
  if (L == NULL) return;
  FreeItems (L);
  MemFree (L);
}

EFI_STATUS ListSetItems (WIDGET_LIST *L, CONST LIST_ITEM *Items, UINTN Count) {
  LIST_ITEM *Ni = NULL;
  UINTN i, j;

  if (L == NULL) return EFI_INVALID_PARAMETER;
  if (Count > 0 && Items == NULL) return EFI_INVALID_PARAMETER;

  /* 事务式：先完整构建新表，任一失败回滚，旧表保持不变。 */
  if (Count > 0) {
    Ni = (LIST_ITEM *)MemAlloc (Count * sizeof (LIST_ITEM));
    if (Ni == NULL) return EFI_OUT_OF_RESOURCES;
    for (i = 0; i < Count; i++) {
      CONST CHAR16 *T = Items[i].Text != NULL ? Items[i].Text : U16 ("");
      UINTN Len = EfiStrLen (T);
      CHAR16 *Copy = (CHAR16 *)MemAlloc ((Len + 1) * sizeof (CHAR16));
      if (Copy == NULL) {
        for (j = 0; j < i; j++) MemFree ((VOID *)Ni[j].Text);
        MemFree (Ni);
        return EFI_OUT_OF_RESOURCES;
      }
      for (j = 0; j < Len; j++) Copy[j] = T[j];
      Copy[Len]   = 0;
      Ni[i].Text     = Copy;
      Ni[i].Attr     = Items[i].Attr;
      Ni[i].Disabled = Items[i].Disabled;
    }
  }

  FreeItems (L);
  L->Items    = Ni;
  L->Count    = Count;
  L->Selected = 0;
  L->Top      = 0;
  for (i = 0; i < Count; i++) {
    if (!Ni[i].Disabled) { L->Selected = i; break; }
  }
  return EFI_SUCCESS;
}

/* 从 Start（含）沿 Dir 找首个非 disabled；找不到保持原选中。 */
STATIC UINTN ScanEnabled (WIDGET_LIST *L, UINTN Start, INTN Dir) {
  UINTN i = Start;
  for (;;) {
    if (!L->Items[i].Disabled) return i;
    if (Dir > 0) {
      if (i + 1 >= L->Count) break;
      i++;
    } else {
      if (i == 0) break;
      i--;
    }
  }
  return L->Selected;
}

/* 视口跟随选中项；Top 钳制到 [0, max(0, Count-VisibleRows)]。 */
STATIC VOID EnsureVisible (WIDGET_LIST *L) {
  UINTN MaxTop;
  if (L->Selected < L->Top) {
    L->Top = L->Selected;
  } else if (L->Selected >= L->Top + L->VisibleRows) {
    L->Top = L->Selected - L->VisibleRows + 1;
  }
  MaxTop = L->Count > L->VisibleRows ? L->Count - L->VisibleRows : 0;
  if (L->Top > MaxTop) L->Top = MaxTop;
}

EFI_STATUS ListHandleKey (WIDGET_LIST *L, CONST TUI_KEY *Key) {
  TUI_COMMAND Cmd;
  UINTN Target;

  if (L == NULL || Key == NULL) return EFI_INVALID_PARAMETER;
  if (L->Count == 0) return EFI_NOT_READY;

  Cmd = TuiKeyToCommand (Key);
  switch (Cmd) {
    case CMD_UP:
      if (L->Selected == 0) return EFI_SUCCESS;          /* 首端钳制 */
      L->Selected = ScanEnabled (L, L->Selected - 1, -1);
      break;
    case CMD_DOWN:
      if (L->Selected + 1 >= L->Count) return EFI_SUCCESS; /* 尾端钳制 */
      L->Selected = ScanEnabled (L, L->Selected + 1, +1);
      break;
    case CMD_PGUP:
      Target = L->Selected > L->VisibleRows ? L->Selected - L->VisibleRows : 0;
      L->Selected = ScanEnabled (L, Target, -1);
      break;
    case CMD_PGDN:
      Target = L->Selected + L->VisibleRows;
      if (Target >= L->Count) Target = L->Count - 1;
      L->Selected = ScanEnabled (L, Target, +1);
      break;
    case CMD_HOME:
      L->Selected = ScanEnabled (L, 0, +1);
      break;
    case CMD_END:
      L->Selected = ScanEnabled (L, L->Count - 1, -1);
      break;
    case CMD_CHAR:
      if (Key->UnicodeChar >= '1' && Key->UnicodeChar <= '9') {
        Target = (UINTN)(Key->UnicodeChar - '1');
      } else if (Key->UnicodeChar == '0') {
        Target = 9;                                       /* '0' = 第 10 项 */
      } else {
        return EFI_NOT_READY;                             /* 非数字键不消费 */
      }
      if (Target >= L->Count || L->Items[Target].Disabled) return EFI_SUCCESS;
      L->Selected = Target;
      break;
    default:
      return EFI_NOT_READY;   /* ENTER/ESC/F 键等：调用方分发（控件无业务语义） */
  }
  EnsureVisible (L);
  return EFI_SUCCESS;
}

UINTN ListSelected (WIDGET_LIST *L) {
  return L != NULL ? L->Selected : 0;
}

VOID ListDraw (WIDGET_LIST *L, UINTN Row, UINTN Col, UINTN Width) {
  UINTN i, j, c, Digits, PrefixLen, Num, Pos;
  CHAR16 Pref[24];   /* UINTN 十进制 ≤ 20 位 + ". " */
  UINT8 Attr;

  if (L == NULL || Width == 0) return;
  Digits = DecDigits (L->Count > 0 ? L->Count : 1);
  PrefixLen = Digits + 2;

  for (i = 0; i < L->VisibleRows; i++) {
    UINTN Idx = L->Top + i;
    UINTN R = Row + i;
    if (Idx >= L->Count) {
      /* 空槽：清为空白行（控件自有区域） */
      for (c = 0; c < Width; c++) TuiPutCell (R, Col + c, ' ', TUI_ATTR_NORMAL);
      continue;
    }
    Attr = L->Items[Idx].Disabled ? TUI_ATTR_DISABLED
         : Idx == L->Selected     ? TUI_ATTR_SELECTED
         :                          L->Items[Idx].Attr;

    /* 编号前缀：绝对序号右对齐 + ". " */
    Pos = Digits;
    Num = Idx + 1;
    do { Pref[--Pos] = (CHAR16)('0' + Num % 10); Num /= 10; } while (Num > 0);
    while (Pos > 0) Pref[--Pos] = ' ';
    Pref[Digits]     = '.';
    Pref[Digits + 1] = ' ';

    c = 0;
    for (j = 0; j < PrefixLen && c < Width; j++, c++) {
      TuiPutCell (R, Col + c, Pref[j], Attr);
    }
    for (j = 0; L->Items[Idx].Text[j] != 0 && c < Width; j++, c++) {
      TuiPutCell (R, Col + c, L->Items[Idx].Text[j], Attr);
    }
    for (; c < Width; c++) {
      TuiPutCell (R, Col + c, ' ', Attr);
    }
  }
}

/* ------------------------------------------------------------------ */
/* 确认对话框                                                          */
/* ------------------------------------------------------------------ */

EFI_STATUS DialogConfirm (CONST CHAR16 *Title, CONST CHAR16 *Body, BOOLEAN DefaultYes, BOOLEAN *Yes) {
  UINTN Rows, Cols, W, H, R, C, Inner;
  UINTN TitleLen, BodyLen, HintLen;
  TUI_KEY Key;
  EFI_STATUS Status;

  if (Yes == NULL) return EFI_INVALID_PARAMETER;
  *Yes = FALSE;   /* 失败安全默认：任何异常路径都不继续 */

  if (Title == NULL) Title = U16 ("");
  if (Body == NULL) Body = U16 ("");
  TitleLen = EfiStrLen (Title);
  BodyLen  = EfiStrLen (Body);
  /* U16 宿主为旋转缓冲：长度与绘制各取一次，避免跨语句生命周期依赖 */
  HintLen  = EfiStrLen (U16 ("Enter=default  ESC=cancel"));

  ScreenSize (&Rows, &Cols);
  W = TitleLen + 4;
  if (BodyLen + 2 > W) W = BodyLen + 2;
  if (HintLen + 2 > W) W = HintLen + 2;
  if (W < 20) W = 20;
  if (W > Cols) W = Cols;
  H = 6;
  if (H > Rows) H = Rows;
  C = Cols > W ? (Cols - W) / 2 : 0;
  R = Rows > H ? (Rows - H) / 2 : 0;

  BoxDraw (R, C, W, H, Title);
  Inner = W >= 2 ? W - 2 : 0;
  if (H >= 3) PutTextClipped (R + 1, C + 1, Body, TUI_ATTR_NORMAL, Inner);
  if (H >= 5) {
    /* 按钮行：[Y]es   [N]o，默认项高亮 */
    UINT8 YAttr = DefaultYes ? TUI_ATTR_SELECTED : TUI_ATTR_NORMAL;
    UINT8 NAttr = DefaultYes ? TUI_ATTR_NORMAL : TUI_ATTR_SELECTED;
    PutTextClipped (R + 3, C + 2, U16 ("[Y]es"), YAttr, Inner);
    PutTextClipped (R + 3, C + 10, U16 ("[N]o"), NAttr, Inner > 8 ? Inner - 8 : 0);
  }
  if (H >= 6) PutTextClipped (R + 4, C + 1, U16 ("Enter=default  ESC=cancel"), TUI_ATTR_DISABLED, Inner);
  TuiFlush ();

  for (;;) {
    Status = TuiWaitKey (&Key);
    if (EFI_ERROR (Status)) return Status;   /* 宿主 mock 空队列=NOT_READY，不死循环 */
    switch (TuiKeyToCommand (&Key)) {
      case CMD_CHAR: {
        CHAR16 Ch = (CHAR16)(Key.UnicodeChar | 0x20);   /* 折叠大小写，仅比较 y/n */
        if (Ch == 'y') { *Yes = TRUE;  return EFI_SUCCESS; }
        if (Ch == 'n') { *Yes = FALSE; return EFI_SUCCESS; }
        break;
      }
      case CMD_ENTER:
        *Yes = DefaultYes;
        return EFI_SUCCESS;
      case CMD_ESC:
        *Yes = FALSE;
        return EFI_ABORTED;
      default:
        break;   /* 其它键忽略，继续等待 */
    }
  }
}

/* ------------------------------------------------------------------ */
/* 单行输入                                                            */
/* ------------------------------------------------------------------ */

struct WIDGET_INPUT {
  UINTN   MaxLen;
  BOOLEAN AsciiOnly;
  CHAR16  *Buf;      /* MaxLen+1 */
  UINTN   Len;
  UINTN   Cursor;    /* 0..Len */
  BOOLEAN Done;
};

WIDGET_INPUT *InputNew (UINTN MaxLen, BOOLEAN AsciiOnly) {
  WIDGET_INPUT *I = (WIDGET_INPUT *)MemAlloc (sizeof (WIDGET_INPUT));
  if (I == NULL) return NULL;
  I->Buf = (CHAR16 *)MemAlloc ((MaxLen + 1) * sizeof (CHAR16));
  if (I->Buf == NULL) { MemFree (I); return NULL; }
  I->MaxLen    = MaxLen;
  I->AsciiOnly = AsciiOnly;
  return I;
}

VOID InputFree (WIDGET_INPUT *I) {
  if (I == NULL) return;
  MemFree (I->Buf);
  MemFree (I);
}

EFI_STATUS InputHandleKey (WIDGET_INPUT *I, CONST TUI_KEY *Key) {
  TUI_COMMAND Cmd;
  UINTN j;

  if (I == NULL || Key == NULL) return EFI_INVALID_PARAMETER;
  if (I->Done) return EFI_NOT_READY;   /* 已结束：不再消费按键 */

  Cmd = TuiKeyToCommand (Key);
  switch (Cmd) {
    case CMD_CHAR: {
      CHAR16 Ch = Key->UnicodeChar;
      /* 控制字符永远拒绝；AsciiOnly 另拒非 ASCII。闪烁提示由调用方按返回码渲染。 */
      if (Ch < 0x20 || Ch == 0x7F || (I->AsciiOnly && Ch > 0x7E)) {
        return EFI_UNSUPPORTED;
      }
      if (I->Len >= I->MaxLen) return EFI_BAD_BUFFER_SIZE;
      for (j = I->Len; j > I->Cursor; j--) I->Buf[j] = I->Buf[j - 1];
      I->Buf[I->Cursor] = Ch;
      I->Len++;
      I->Cursor++;
      I->Buf[I->Len] = 0;
      return EFI_SUCCESS;
    }
    case CMD_BACKSPACE:
      if (I->Cursor > 0) {
        for (j = I->Cursor - 1; j + 1 < I->Len; j++) I->Buf[j] = I->Buf[j + 1];
        I->Len--;
        I->Cursor--;
        I->Buf[I->Len] = 0;
      }
      return EFI_SUCCESS;
    case CMD_DELETE:
      if (I->Cursor < I->Len) {
        for (j = I->Cursor; j + 1 < I->Len; j++) I->Buf[j] = I->Buf[j + 1];
        I->Len--;
        I->Buf[I->Len] = 0;
      }
      return EFI_SUCCESS;
    case CMD_LEFT:
      if (I->Cursor > 0) I->Cursor--;
      return EFI_SUCCESS;
    case CMD_RIGHT:
      if (I->Cursor < I->Len) I->Cursor++;
      return EFI_SUCCESS;
    case CMD_HOME:
      I->Cursor = 0;
      return EFI_SUCCESS;
    case CMD_END:
      I->Cursor = I->Len;
      return EFI_SUCCESS;
    case CMD_ENTER:
      I->Done = TRUE;
      return EFI_SUCCESS;
    default:
      return EFI_NOT_READY;   /* ESC/TAB/F 键等：调用方分发 */
  }
}

BOOLEAN InputDone (WIDGET_INPUT *I) {
  return (I != NULL && I->Done) ? TRUE : FALSE;
}

CONST CHAR16 *InputText (WIDGET_INPUT *I) {
  return I != NULL ? I->Buf : U16 ("");
}

/* ------------------------------------------------------------------ */
/* 横幅 / 键位条 / 画框                                                */
/* ------------------------------------------------------------------ */

VOID BannerDraw (UINTN Row, CONST CHAR16 *Text, UINT8 Attr) {
  UINTN Rows, Cols;
  ScreenSize (&Rows, &Cols);
  TuiFill (Row, 0, Cols, 1, ' ', Attr);
  PutTextClipped (Row, 0, Text, Attr, Cols);
}

VOID KeybarDraw (CONST CHAR16 *Text) {
  UINTN Rows, Cols;
  ScreenSize (&Rows, &Cols);
  if (Rows == 0) return;
  TuiFill (Rows - 1, 0, Cols, 1, ' ', TUI_ATTR_KEYBAR);
  PutTextClipped (Rows - 1, 0, Text, TUI_ATTR_KEYBAR, Cols);
}

VOID BoxDraw (UINTN Row, UINTN Col, UINTN W, UINTN H, CONST CHAR16 *Title) {
  UINTN i;

  if (W < 2 || H < 2) return;

  /* 内部空白 + 左右竖边 */
  TuiFill (Row + 1, Col + 1, W - 2, H - 2, ' ', TUI_ATTR_NORMAL);
  for (i = 1; i + 1 < H; i++) {
    TuiPutCell (Row + i, Col,         '|', TUI_ATTR_NORMAL);
    TuiPutCell (Row + i, Col + W - 1, '|', TUI_ATTR_NORMAL);
  }
  /* 上下横边 */
  TuiFill (Row,         Col + 1, W - 2, 1, '-', TUI_ATTR_NORMAL);
  TuiFill (Row + H - 1, Col + 1, W - 2, 1, '-', TUI_ATTR_NORMAL);
  TuiPutCell (Row,         Col,         '+', TUI_ATTR_NORMAL);
  TuiPutCell (Row,         Col + W - 1, '+', TUI_ATTR_NORMAL);
  TuiPutCell (Row + H - 1, Col,         '+', TUI_ATTR_NORMAL);
  TuiPutCell (Row + H - 1, Col + W - 1, '+', TUI_ATTR_NORMAL);

  /* 标题段 " Title "（TITLE 属性）：标题字符预算 W-6（前后空格各 1、两侧各留 1 格边框） */
  if (Title != NULL && Title[0] != 0 && W >= 6) {
    UINTN Budget = W - 6;
    UINTN Len = EfiStrLen (Title);
    UINTN Take = Len < Budget ? Len : Budget;
    TuiPutCell (Row, Col + 2, ' ', TUI_ATTR_TITLE);
    PutTextClipped (Row, Col + 3, Title, TUI_ATTR_TITLE, Take);
    TuiPutCell (Row, Col + 3 + Take, ' ', TUI_ATTR_TITLE);
  }
}
