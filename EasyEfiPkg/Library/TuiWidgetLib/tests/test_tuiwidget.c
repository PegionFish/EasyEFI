/* S08 TuiWidgetLib 宿主测试 — 任务书 T08-1..6（docs/parallel-tasks.md §3 S08）。
 * mock 屏幕字符矩阵断言（tests/host/mocks/MockTui，leader 独占）。
 * 运行：make -f host.mk test（模块目录内；ASan/UBSan）。
 */
#include "test.h"
#include "MockTui.h"
#include "Contract/TuiWidget.h"

/* ---- 助手：稳定文本池（U16 宿主为 8 缓冲旋转，构建条目数组需独立存储） ---- */
static CHAR16 g_pool[64][96];
static UINTN g_pool_n = 0;
static CHAR16 *Txt (const char *S) {
  CHAR16 *B = g_pool[g_pool_n % 64];
  UINTN i;
  g_pool_n++;
  for (i = 0; S[i] != '\0' && i + 1 < 96; i++) B[i] = (CHAR16)(UINT8)S[i];
  B[i] = 0;
  return B;
}

static TUI_KEY KeyChar (CHAR16 C) { TUI_KEY K; K.ScanCode = 0; K.UnicodeChar = C; return K; }
static TUI_KEY KeyScan (UINT16 S) { TUI_KEY K; K.ScanCode = S; K.UnicodeChar = 0; return K; }

/* 行内 Col 起与期望串逐字符相等。 */
static void ExpectRowAt (UINTN Row, UINTN Col, const char *Want) {
  CHAR16 Buf[256];
  CHAR16 *W = Txt (Want);
  UINTN Len = EfiStrLen (W);
  MockTuiRowText (Row, Buf, 256);
  ASSERT_TRUE (EfiStrnCmp (Buf + Col, W, Len) == 0);
}

static void Screen80x25 (void) {
  MockTuiReset ();
  MockTuiSetSize (25, 80);
  ASSERT_OK (TuiInit ());
}

/* T08-1 列表滚动/视口/首尾钳制（编号前缀 = 绝对序号右对齐）。 */
TEST (t08_1_list_scroll_viewport) {
  LIST_ITEM Items[10];
  char Lab[8];
  UINTN i;
  TUI_KEY K;
  WIDGET_LIST *L;

  Screen80x25 ();
  for (i = 0; i < 10; i++) {
    memcpy (Lab, "item", 4);
    Lab[4] = (char)('0' + i);
    Lab[5] = '\0';
    Items[i].Text = Txt (Lab);
    Items[i].Attr = TUI_ATTR_NORMAL;
    Items[i].Disabled = FALSE;
  }
  L = ListNew (3);
  ASSERT_TRUE (L != NULL);
  ASSERT_OK (ListSetItems (L, Items, 10));
  ASSERT_EQ (ListSelected (L), 0);

  ListDraw (L, 2, 0, 40);
  ExpectRowAt (2, 0, " 1. item0");
  ExpectRowAt (3, 0, " 2. item1");
  ExpectRowAt (4, 0, " 3. item2");
  ASSERT_EQ (MockTuiGetCell (2, 0).Attr, TUI_ATTR_SELECTED);
  ASSERT_EQ (MockTuiGetCell (3, 0).Attr, TUI_ATTR_NORMAL);

  /* 下移 3 次：选中 3，视口滚动 Top=1 */
  K = KeyScan (SCAN_DOWN);
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 3);
  ListDraw (L, 2, 0, 40);
  ExpectRowAt (2, 0, " 2. item1");
  ExpectRowAt (4, 0, " 4. item3");
  ASSERT_EQ (MockTuiGetCell (4, 0).Attr, TUI_ATTR_SELECTED);
  ASSERT_EQ (MockTuiGetCell (2, 0).Attr, TUI_ATTR_NORMAL);

  /* 尾端钳制：连续下移停在 9，Top=7 */
  for (i = 0; i < 20; i++) ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 9);
  ListDraw (L, 2, 0, 40);
  ExpectRowAt (2, 0, " 8. item7");
  ExpectRowAt (4, 0, "10. item9");
  ASSERT_EQ (MockTuiGetCell (4, 0).Attr, TUI_ATTR_SELECTED);

  /* 首端钳制：连续上移停在 0，Top=0 */
  K = KeyScan (SCAN_UP);
  for (i = 0; i < 20; i++) ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 0);
  ListDraw (L, 2, 0, 40);
  ExpectRowAt (2, 0, " 1. item0");
  ASSERT_EQ (MockTuiGetCell (2, 0).Attr, TUI_ATTR_SELECTED);

  /* 翻页：PGDN 0→3（Top=1），PGUP 3→0（Top=0） */
  K = KeyScan (SCAN_PGDN);
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 3);
  K = KeyScan (SCAN_PGUP);
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 0);

  /* HOME/END */
  K = KeyScan (SCAN_END);
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 9);
  K = KeyScan (SCAN_HOME);
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 0);

  /* ENTER/ESC 不消费（调用方分发） */
  K = KeyChar (0x0D);
  ASSERT_EQ (ListHandleKey (L, &K), EFI_NOT_READY);
  K = KeyScan (SCAN_ESC);
  ASSERT_EQ (ListHandleKey (L, &K), EFI_NOT_READY);
  ASSERT_EQ (ListSelected (L), 0);

  ListFree (L);
}

/* T08-2 disabled 跳过 + 数字直选 + 空表/全 disabled 边界。 */
TEST (t08_2_disabled_digit) {
  LIST_ITEM Items[5];
  LIST_ITEM AllDis[3];
  UINTN i;
  TUI_KEY K;
  WIDGET_LIST *L;

  Screen80x25 ();
  for (i = 0; i < 5; i++) {
    Items[i].Text = Txt (i == 0 ? "alpha" : i == 1 ? "bravo" : i == 2 ? "charlie" : i == 3 ? "delta" : "echo");
    Items[i].Attr = TUI_ATTR_NORMAL;
    Items[i].Disabled = (i == 1 || i == 3) ? TRUE : FALSE;
  }
  L = ListNew (5);
  ASSERT_OK (ListSetItems (L, Items, 5));
  ASSERT_EQ (ListSelected (L), 0);

  K = KeyScan (SCAN_DOWN);
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 2);   /* 跳过 1 */
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 4);   /* 跳过 3 */
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 4);   /* 尾端钳制 */
  K = KeyScan (SCAN_UP);
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 2);
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 0);
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 0);   /* 首端钳制 */

  /* 数字直选：'3'→idx2；'2' disabled 忽略；'5'→idx4；'9'/'0' 越界忽略 */
  K = KeyChar ('3');
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 2);
  K = KeyChar ('2');
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 2);
  K = KeyChar ('5');
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 4);
  K = KeyChar ('9');
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 4);
  K = KeyChar ('0');
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 4);

  /* 渲染属性：disabled=ATTR_DISABLED，选中=ATTR_SELECTED，其余基础 attr */
  ListDraw (L, 0, 0, 40);
  ASSERT_EQ (MockTuiGetCell (1, 0).Attr, TUI_ATTR_DISABLED);
  ASSERT_EQ (MockTuiGetCell (3, 0).Attr, TUI_ATTR_DISABLED);
  ASSERT_EQ (MockTuiGetCell (4, 0).Attr, TUI_ATTR_SELECTED);
  ASSERT_EQ (MockTuiGetCell (0, 0).Attr, TUI_ATTR_NORMAL);

  /* 数字直选触发视口滚动：2 行视口，'9'→idx8，Top=7 */
  {
    LIST_ITEM Ten[10];
    char Lab[8];
    for (i = 0; i < 10; i++) {
      memcpy (Lab, "item", 4); Lab[4] = (char)('0' + i); Lab[5] = '\0';
      Ten[i].Text = Txt (Lab); Ten[i].Attr = TUI_ATTR_NORMAL; Ten[i].Disabled = FALSE;
    }
    ListFree (L);
    L = ListNew (2);
    ASSERT_OK (ListSetItems (L, Ten, 10));
    K = KeyChar ('9');
    ASSERT_OK (ListHandleKey (L, &K));
    ASSERT_EQ (ListSelected (L), 8);
    ListDraw (L, 0, 0, 40);
    ExpectRowAt (0, 0, " 8. item7");
    ExpectRowAt (1, 0, " 9. item8");
    ASSERT_EQ (MockTuiGetCell (1, 0).Attr, TUI_ATTR_SELECTED);
  }

  /* 全 disabled：选中停 0，导航不动，渲染 DISABLED 优先 */
  for (i = 0; i < 3; i++) {
    AllDis[i].Text = Txt ("off"); AllDis[i].Attr = TUI_ATTR_NORMAL; AllDis[i].Disabled = TRUE;
  }
  ASSERT_OK (ListSetItems (L, AllDis, 3));
  ASSERT_EQ (ListSelected (L), 0);
  K = KeyScan (SCAN_DOWN);
  ASSERT_OK (ListHandleKey (L, &K));
  ASSERT_EQ (ListSelected (L), 0);
  ListDraw (L, 0, 0, 40);
  ASSERT_EQ (MockTuiGetCell (0, 0).Attr, TUI_ATTR_DISABLED);

  /* 空表：导航不消费，绘制空白 */
  ASSERT_OK (ListSetItems (L, NULL, 0));
  ASSERT_EQ (ListSelected (L), 0);
  K = KeyScan (SCAN_DOWN);
  ASSERT_EQ (ListHandleKey (L, &K), EFI_NOT_READY);
  ListDraw (L, 0, 0, 40);
  ASSERT_EQ (MockTuiGetCell (0, 0).Char, ' ');
  ASSERT_EQ (MockTuiGetCell (0, 0).Attr, TUI_ATTR_NORMAL);

  /* NULL 防御 */
  ASSERT_ERR (ListHandleKey (NULL, &K));
  ASSERT_ERR (ListSetItems (NULL, Items, 5));
  ASSERT_ERR (ListSetItems (L, NULL, 3));
  ASSERT_EQ (ListSelected (NULL), 0);
  ListDraw (NULL, 0, 0, 40);
  ListFree (NULL);
  ListFree (L);
}

/* T08-3 对话框 Y/N/默认值/ESC=取消 + 居中画框快照 + 空队列不死循环。 */
TEST (t08_3_dialog) {
  BOOLEAN Yes;
  CHAR16 Buf[256];

  Screen80x25 ();
  /* W=max(7+4, 19+2, 25+2, 20)=27, H=6, C=(80-27)/2=26, R=(25-6)/2=9 */
  Yes = TRUE;
  MockTuiPushKey (0, 'y');
  ASSERT_OK (DialogConfirm (Txt ("Confirm"), Txt ("Proceed with flash?"), FALSE, &Yes));
  ASSERT_TRUE (Yes);
  ASSERT_EQ (MockTuiGetCell (9, 26).Char, '+');
  ASSERT_EQ (MockTuiGetCell (9, 52).Char, '+');
  ASSERT_EQ (MockTuiGetCell (14, 26).Char, '+');
  ExpectRowAt (10, 27, "Proceed with flash?");
  ExpectRowAt (9, 28, " Confirm ");   /* 标题段 " Confirm "（C+2 起） */
  /* 默认 N：[N]o 高亮（R+3=12 行，C+2=28 / C+10=36） */
  ASSERT_EQ (MockTuiGetCell (12, 28).Attr, TUI_ATTR_NORMAL);
  ASSERT_EQ (MockTuiGetCell (12, 36).Attr, TUI_ATTR_SELECTED);
  ExpectRowAt (12, 28, "[Y]es");
  ExpectRowAt (12, 36, "[N]o");
  ExpectRowAt (13, 27, "Enter=default  ESC=cancel");

  MockTuiPushKey (0, 'n');
  ASSERT_OK (DialogConfirm (Txt ("Confirm"), Txt ("Proceed with flash?"), FALSE, &Yes));
  ASSERT_FALSE (Yes);

  MockTuiPushKey (0, 'N');   /* 大写 N */
  ASSERT_OK (DialogConfirm (Txt ("Confirm"), Txt ("body"), TRUE, &Yes));
  ASSERT_FALSE (Yes);

  /* Enter = 默认值 */
  MockTuiPushKey (0, 0x0D);
  ASSERT_OK (DialogConfirm (Txt ("Confirm"), Txt ("body"), TRUE, &Yes));
  ASSERT_TRUE (Yes);
  /* 默认 Y：[Y]es 高亮 */
  ASSERT_EQ (MockTuiGetCell (12, 28).Attr, TUI_ATTR_SELECTED);
  ASSERT_EQ (MockTuiGetCell (12, 36).Attr, TUI_ATTR_NORMAL);

  MockTuiPushKey (0, 0x0D);
  ASSERT_OK (DialogConfirm (Txt ("Confirm"), Txt ("body"), FALSE, &Yes));
  ASSERT_FALSE (Yes);

  /* ESC = 取消：*Yes=FALSE + EFI_ABORTED */
  MockTuiPushKey (SCAN_ESC, 0);
  ASSERT_EQ (DialogConfirm (Txt ("Confirm"), Txt ("body"), TRUE, &Yes), EFI_ABORTED);
  ASSERT_FALSE (Yes);

  /* 无关键忽略后 'Y' 生效 */
  MockTuiPushKey (SCAN_F1, 0);
  MockTuiPushKey (0, 'x');
  MockTuiPushKey (0, 'Y');
  ASSERT_OK (DialogConfirm (Txt ("Confirm"), Txt ("body"), FALSE, &Yes));
  ASSERT_TRUE (Yes);

  /* 空按键队列：错误传播（NOT_READY），Yes 保持 FALSE，不死循环 */
  ASSERT_EQ (DialogConfirm (Txt ("Confirm"), Txt ("body"), TRUE, &Yes), EFI_NOT_READY);
  ASSERT_FALSE (Yes);

  /* NULL 防御 */
  ASSERT_ERR (DialogConfirm (Txt ("t"), Txt ("b"), FALSE, NULL));
  Yes = TRUE;
  MockTuiPushKey (0, 'y');
  ASSERT_OK (DialogConfirm (NULL, NULL, FALSE, &Yes));   /* NULL 标题/正文按空串 */
  ASSERT_TRUE (Yes);

  /* 长正文截断不越界（80 列内） */
  {
    char Long[200];
    memset (Long, 'B', 190);
    Long[190] = '\0';
    MockTuiPushKey (0, 'n');
    ASSERT_OK (DialogConfirm (Txt ("T"), Txt (Long), FALSE, &Yes));
    ASSERT_FALSE (Yes);
    MockTuiRowText (10, Buf, 256);
    ASSERT_TRUE (EfiStrLen (Buf) <= 80);
  }
}

/* T08-4 输入行编辑/退格/上限/ASCII 过滤/光标导航/Enter 结束。 */
TEST (t08_4_input) {
  WIDGET_INPUT *I;
  TUI_KEY K;

  I = InputNew (8, TRUE);
  ASSERT_TRUE (I != NULL);
  ASSERT_FALSE (InputDone (I));
  ASSERT_TRUE (EfiStrCmp (InputText (I), U16 ("")) == 0);

  K = KeyChar ('a'); ASSERT_OK (InputHandleKey (I, &K));
  K = KeyChar ('b'); ASSERT_OK (InputHandleKey (I, &K));
  K = KeyChar ('c'); ASSERT_OK (InputHandleKey (I, &K));
  ASSERT_TRUE (EfiStrCmp (InputText (I), U16 ("abc")) == 0);

  K = KeyChar (0x08);   /* Backspace */
  ASSERT_OK (InputHandleKey (I, &K));
  ASSERT_TRUE (EfiStrCmp (InputText (I), U16 ("ab")) == 0);

  /* 光标导航 + 中间插入 + 前向删除 */
  K = KeyScan (SCAN_LEFT);  ASSERT_OK (InputHandleKey (I, &K));   /* cursor 1 */
  K = KeyChar ('X');        ASSERT_OK (InputHandleKey (I, &K));   /* aXb, cursor 2 */
  ASSERT_TRUE (EfiStrCmp (InputText (I), U16 ("aXb")) == 0);
  K = KeyScan (SCAN_DELETE); ASSERT_OK (InputHandleKey (I, &K));  /* 删 'b' → aX */
  ASSERT_TRUE (EfiStrCmp (InputText (I), U16 ("aX")) == 0);
  K = KeyScan (SCAN_HOME);  ASSERT_OK (InputHandleKey (I, &K));
  K = KeyChar ('Z');        ASSERT_OK (InputHandleKey (I, &K));   /* 行首插入 → ZaX */
  ASSERT_TRUE (EfiStrCmp (InputText (I), U16 ("ZaX")) == 0);
  K = KeyScan (SCAN_END);   ASSERT_OK (InputHandleKey (I, &K));
  K = KeyChar ('1');        ASSERT_OK (InputHandleKey (I, &K));
  K = KeyChar ('2');        ASSERT_OK (InputHandleKey (I, &K));
  K = KeyChar ('3');        ASSERT_OK (InputHandleKey (I, &K));
  K = KeyChar ('4');        ASSERT_OK (InputHandleKey (I, &K));
  K = KeyChar ('5');        ASSERT_OK (InputHandleKey (I, &K));   /* ZaX12345 = 8 满 */
  ASSERT_TRUE (EfiStrCmp (InputText (I), U16 ("ZaX12345")) == 0);

  /* 上限：第 9 个字符拒绝，文本不变 */
  K = KeyChar ('9');
  ASSERT_EQ (InputHandleKey (I, &K), EFI_BAD_BUFFER_SIZE);
  ASSERT_TRUE (EfiStrCmp (InputText (I), U16 ("ZaX12345")) == 0);

  /* ASCII 过滤：非 ASCII / 控制字符拒绝（调用方据返回码闪提示） */
  K = KeyChar (0x4E2D);
  ASSERT_EQ (InputHandleKey (I, &K), EFI_UNSUPPORTED);
  K = KeyChar (0x01);
  ASSERT_EQ (InputHandleKey (I, &K), EFI_UNSUPPORTED);
  K = KeyChar (0x7F);
  ASSERT_EQ (InputHandleKey (I, &K), EFI_UNSUPPORTED);
  ASSERT_TRUE (EfiStrCmp (InputText (I), U16 ("ZaX12345")) == 0);

  /* Enter 结束；结束后不再消费按键 */
  ASSERT_FALSE (InputDone (I));
  K = KeyChar (0x0D);
  ASSERT_OK (InputHandleKey (I, &K));
  ASSERT_TRUE (InputDone (I));
  K = KeyChar ('q');
  ASSERT_EQ (InputHandleKey (I, &K), EFI_NOT_READY);
  ASSERT_TRUE (EfiStrCmp (InputText (I), U16 ("ZaX12345")) == 0);
  InputFree (I);

  /* 非 AsciiOnly：BMP 非 ASCII 接受，控制字符仍拒绝 */
  I = InputNew (4, FALSE);
  K = KeyChar (0x4E2D);
  ASSERT_OK (InputHandleKey (I, &K));
  ASSERT_EQ (InputText (I)[0], 0x4E2D);
  ASSERT_EQ (EfiStrLen (InputText (I)), 1);
  K = KeyChar (0x01);
  ASSERT_EQ (InputHandleKey (I, &K), EFI_UNSUPPORTED);
  InputFree (I);

  /* MaxLen=0：任何字符都满，Enter 仍可结束 */
  I = InputNew (0, TRUE);
  K = KeyChar ('a');
  ASSERT_EQ (InputHandleKey (I, &K), EFI_BAD_BUFFER_SIZE);
  K = KeyChar (0x0D);
  ASSERT_OK (InputHandleKey (I, &K));
  ASSERT_TRUE (InputDone (I));
  InputFree (I);

  /* ESC/TAB 不消费（调用方分发取消等） */
  I = InputNew (8, TRUE);
  K = KeyScan (SCAN_ESC);
  ASSERT_EQ (InputHandleKey (I, &K), EFI_NOT_READY);
  K = KeyChar (0x09);
  ASSERT_EQ (InputHandleKey (I, &K), EFI_NOT_READY);
  ASSERT_FALSE (InputDone (I));
  InputFree (I);

  /* NULL 防御 */
  K = KeyChar ('a');
  ASSERT_ERR (InputHandleKey (NULL, &K));
  ASSERT_FALSE (InputDone (NULL));
  ASSERT_TRUE (EfiStrCmp (InputText (NULL), U16 ("")) == 0);
  InputFree (NULL);
}

/* T08-5 画框 + 键位条 + 横幅黄金快照（纯 ASCII +-|，属性语义）。 */
TEST (t08_5_box_keybar_golden) {
  char Want[64];

  Screen80x25 ();
  BoxDraw (2, 5, 30, 6, Txt ("Title"));
  /* 顶边：'+' '-' " Title " + 20×'-' '+'（W=30） */
  Want[0] = '+'; Want[1] = '-';
  memcpy (Want + 2, " Title ", 7);
  memset (Want + 9, '-', 20);
  Want[29] = '+'; Want[30] = '\0';
  ExpectRowAt (2, 5, Want);
  /* 中间行：'|' + 28 空格 + '|' */
  Want[0] = '|';
  memset (Want + 1, ' ', 28);
  Want[29] = '|'; Want[30] = '\0';
  ExpectRowAt (3, 5, Want);
  ExpectRowAt (6, 5, Want);
  /* 底边：'+' + 28×'-' + '+' */
  Want[0] = '+';
  memset (Want + 1, '-', 28);
  Want[29] = '+'; Want[30] = '\0';
  ExpectRowAt (7, 5, Want);
  /* 属性：边框 NORMAL，标题段 TITLE */
  ASSERT_EQ (MockTuiGetCell (2, 5).Char, '+');
  ASSERT_EQ (MockTuiGetCell (2, 5).Attr, TUI_ATTR_NORMAL);
  ASSERT_EQ (MockTuiGetCell (2, 8).Char, 'T');
  ASSERT_EQ (MockTuiGetCell (2, 8).Attr, TUI_ATTR_TITLE);
  ASSERT_EQ (MockTuiGetCell (3, 5).Char, '|');
  ASSERT_EQ (MockTuiGetCell (3, 6).Char, ' ');
  ASSERT_EQ (MockTuiGetCell (3, 6).Attr, TUI_ATTR_NORMAL);

  /* 无标题小框 */
  BoxDraw (10, 0, 4, 3, NULL);
  ExpectRowAt (10, 0, "+--+");
  ExpectRowAt (11, 0, "|  |");
  ExpectRowAt (12, 0, "+--+");

  /* 退化尺寸 no-op */
  BoxDraw (20, 0, 1, 5, Txt ("x"));
  ASSERT_EQ (MockTuiGetCell (20, 0).Char, 0);
  BoxDraw (20, 0, 5, 1, Txt ("x"));
  ASSERT_EQ (MockTuiGetCell (20, 0).Char, 0);

  /* 键位条：底行全宽 KEYBAR 属性 */
  KeybarDraw (Txt ("Enter=Select  ESC=Back"));
  ExpectRowAt (24, 0, "Enter=Select  ESC=Back");
  ASSERT_EQ (MockTuiGetCell (24, 0).Attr, TUI_ATTR_KEYBAR);
  ASSERT_EQ (MockTuiGetCell (24, 79).Attr, TUI_ATTR_KEYBAR);
  ASSERT_EQ (MockTuiGetCell (24, 79).Char, ' ');
  ASSERT_EQ (MockTuiGetCell (23, 0).Attr, 0);   /* 未触及上一行 */

  /* 横幅：指定行全宽指定属性 */
  BannerDraw (0, Txt ("ERROR: something"), TUI_ATTR_BANNER_ERR);
  ExpectRowAt (0, 0, "ERROR: something");
  ASSERT_EQ (MockTuiGetCell (0, 0).Attr, TUI_ATTR_BANNER_ERR);
  ASSERT_EQ (MockTuiGetCell (0, 79).Attr, TUI_ATTR_BANNER_ERR);
  ASSERT_EQ (MockTuiGetCell (0, 79).Char, ' ');
}

/* T08-6 80x25 小屏布局不溢出 + 超宽/超高钳制 + 极小屏对话框。 */
TEST (t08_6_small_screen) {
  LIST_ITEM Items[3];
  char Long[128];
  CHAR16 Buf[256];
  WIDGET_LIST *L;
  BOOLEAN Yes;
  UINTN i;

  Screen80x25 ();
  memset (Long, 'L', 100);
  Long[100] = '\0';
  for (i = 0; i < 3; i++) {
    Items[i].Text = Txt (Long);
    Items[i].Attr = TUI_ATTR_NORMAL;
    Items[i].Disabled = FALSE;
  }
  L = ListNew (30);   /* 视口高于屏幕 */
  ASSERT_OK (ListSetItems (L, Items, 3));
  ListDraw (L, 0, 0, 200);   /* 宽度超屏幕 */
  for (i = 0; i < 25; i++) {
    MockTuiRowText (i, Buf, 256);
    ASSERT_TRUE (EfiStrLen (Buf) <= 80);
  }
  ASSERT_EQ (MockTuiGetCell (0, 79).Attr, TUI_ATTR_SELECTED);  /* 行属性铺满到边 */
  ASSERT_EQ (MockTuiGetCell (2, 0).Attr, TUI_ATTR_NORMAL);     /* 未选中条目基础 attr */
  ASSERT_EQ (MockTuiGetCell (3, 0).Char, ' ');                 /* 空槽空白 */
  ASSERT_EQ (MockTuiGetCell (3, 0).Attr, TUI_ATTR_NORMAL);
  ListDraw (L, 0, 0, 0);   /* Width=0 no-op */
  ListFree (L);

  /* 超宽/超高画框：钳制不崩 */
  BoxDraw (0, 0, 200, 60, Txt ("Big"));
  ASSERT_EQ (MockTuiGetCell (0, 0).Char, '+');
  ASSERT_EQ (MockTuiGetCell (0, 79).Char, '-');
  KeybarDraw (Txt ("K"));
  ASSERT_EQ (MockTuiGetCell (24, 0).Attr, TUI_ATTR_KEYBAR);

  /* 极小屏 40x10：对话框宽度钳制、按键仍可用 */
  MockTuiReset ();
  MockTuiSetSize (10, 40);
  ASSERT_OK (TuiInit ());
  MockTuiPushKey (0, 'n');
  ASSERT_OK (DialogConfirm (Txt ("Title"), Txt ("A very long body text that exceeds forty columns"), FALSE, &Yes));
  ASSERT_FALSE (Yes);
  MockTuiRowText (0, Buf, 256);
  ASSERT_TRUE (EfiStrLen (Buf) <= 40);
  BoxDraw (0, 0, 100, 50, Txt ("X"));
  KeybarDraw (Txt ("KB"));
  ASSERT_EQ (MockTuiGetCell (9, 0).Attr, TUI_ATTR_KEYBAR);
  BannerDraw (0, Txt ("B"), TUI_ATTR_BANNER_ERR);
  ASSERT_EQ (MockTuiGetCell (0, 39).Attr, TUI_ATTR_BANNER_ERR);

  /* 极小屏列表：1 行视口 */
  L = ListNew (1);
  ASSERT_OK (ListSetItems (L, Items, 3));
  ListDraw (L, 0, 0, 40);
  ASSERT_EQ (MockTuiGetCell (0, 0).Attr, TUI_ATTR_SELECTED);
  ListFree (L);
}

int main (void) {
  RUN_TEST (t08_1_list_scroll_viewport);
  RUN_TEST (t08_2_disabled_digit);
  RUN_TEST (t08_3_dialog);
  RUN_TEST (t08_4_input);
  RUN_TEST (t08_5_box_keybar_golden);
  RUN_TEST (t08_6_small_screen);
  RUN_TESTS_END ();
}
