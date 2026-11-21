/* S13 Editor 宿主测试（编号用例 T13-1..12，任务书 DoD 依据）。
 * 运行: make -f host.mk test（模块目录内）。
 * 缝：MockTui（屏幕+脚本按键）+ MockFs（平面内存 FS）+ 真实 EditorModelLib/Utf8Lib。
 * 键位：F2 保存 / F3 查找输入(ENTER=下一命中) / F4 上一命中 / F5 另存为 / F6 跳行 /
 *       F7 撤销 / F10|ESC 退出（修改→S 保存退出 D 放弃 ESC 取消）；查找态 TAB=大小写。
 */
#include "test.h"
#include "MockTui.h"
#include "MockFs.h"
#include "EditorCore.h"

/* ---- 助手 ---- */

static void PushChar (CHAR16 C) { MockTuiPushKey (0, C); }
static void PushScan (UINT16 S) { MockTuiPushKey (S, 0); }
static void PushStr (const char *S) { for (; *S; S++) MockTuiPushKey (0, (CHAR16)(UINT8)*S); }
static void PushBackspaces (UINTN N) { UINTN i; for (i = 0; i < N; i++) MockTuiPushKey (0, 0x08); }

static EDITOR_APP *NewEd (FS_OPS *Fs, UINTN MaxBytes) {
  EDITOR_APP *E;
  MockTuiReset ();
  MockTuiSetSize (25, 80);
  E = EdNew (Fs, MaxBytes, 256);
  ASSERT_TRUE (E != NULL);
  return E;
}

static void ReadFileBytes (FS_OPS *Fs, CONST CHAR16 *Path, UINT8 *Buf, UINTN *Len) {
  FS_HANDLE *H = NULL;
  UINTN Cap = *Len, N, Total = 0;
  ASSERT_OK (Fs->Open (Fs, Path, FS_MODE_READ, &H));
  for (;;) {
    N = Cap - Total;
    ASSERT_OK (Fs->Read (Fs, H, Buf + Total, &N));
    Total += N;
    if (N == 0) break;
  }
  ASSERT_OK (Fs->Close (Fs, H));
  *Len = Total;
}

/* 屏幕行文本是否含 ASCII 子串（状态栏/键位条断言用） */
static BOOLEAN RowHas (UINTN Row, const char *Needle) {
  CHAR16 W[256];
  char A[256];
  UINTN i;
  MockTuiRowText (Row, W, 256);
  for (i = 0; W[i] && i < 255; i++) A[i] = (char)(W[i] & 0xFF);
  A[i] = 0;
  return (BOOLEAN)(strstr (A, Needle) != NULL);
}

static void CursorAt (EDITOR_APP *E, UINTN Line, UINTN Col) {
  MODEL_INFO I;
  ASSERT_OK (ModelInfo (EdModel (E), &I));
  ASSERT_EQ (I.CursorLine, Line);
  ASSERT_EQ (I.CursorCol, Col);
}

/* ---- T13-1 中文注释脚本：编辑 ASCII → 保存 → 非 ASCII 字节逐字节相等 ---- */

TEST (t13_1_utf8_roundtrip) {
  FS_OPS *Fs = MockFsNew ();
  const UINT8 Orig[] = "# \xE4\xB8\xAD\xE6\x96\x87\xE6\xB3\xA8\xE9\x87\x8A\r\nfs0:\\fpt.efi\r\n";
  const UINT8 Exp[]  = "# \xE4\xB8\xAD\xE6\x96\x87\xE6\xB3\xA8\xE9\x87\x8A-OK\r\nfs0:\\fpt.efi\r\n";
  UINT8 Buf[512];
  UINTN Len = sizeof (Buf);
  EDITOR_APP *E;
  MockFsAddFile (Fs, U16 ("fs0:/fpt.nsh"), Orig, sizeof (Orig) - 1);
  E = NewEd (Fs, 262144);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/fpt.nsh")));
  ASSERT_EQ (EdState (E), ED_EDIT);
  PushScan (SCAN_END);      /* 行尾（中文注释之后） */
  PushStr ("-OK");          /* ASCII 编辑 */
  PushScan (SCAN_F2);       /* 保存 */
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_EDIT);
  ASSERT_FALSE (ModelModified (EdModel (E)));
  ReadFileBytes (Fs, U16 ("fs0:/fpt.nsh"), Buf, &Len);
  ASSERT_EQ (Len, sizeof (Exp) - 1);
  if (Len == sizeof (Exp) - 1) ASSERT_MEM_EQ (Buf, Exp, sizeof (Exp) - 1);
  EdFree (E);
  Fs->Destroy (Fs);
}

/* ---- T13-2 BOM/CRLF 保留 ---- */

TEST (t13_2_bom_crlf) {
  FS_OPS *Fs = MockFsNew ();
  const UINT8 Orig[] = { 0xEF, 0xBB, 0xBF, 'a', '\r', '\n', 'b', '\r', '\n' };
  const UINT8 Exp[]  = { 0xEF, 0xBB, 0xBF, 'X', 'a', '\r', '\n', 'b', '\r', '\n' };
  UINT8 Buf[64];
  UINTN Len = sizeof (Buf);
  MODEL_INFO I;
  EDITOR_APP *E;
  MockFsAddFile (Fs, U16 ("fs0:/bom.nsh"), Orig, sizeof (Orig));
  E = NewEd (Fs, 262144);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/bom.nsh")));
  ASSERT_OK (ModelInfo (EdModel (E), &I));
  ASSERT_TRUE (I.HasBom);
  ASSERT_EQ (I.DominantEol, 1);
  PushStr ("X");            /* 文件头插入 */
  PushScan (SCAN_F2);
  EdRun (E);
  ReadFileBytes (Fs, U16 ("fs0:/bom.nsh"), Buf, &Len);
  ASSERT_EQ (Len, sizeof (Exp));
  if (Len == sizeof (Exp)) ASSERT_MEM_EQ (Buf, Exp, sizeof (Exp));
  /* 新行随主约定 CRLF */
  PushScan (SCAN_END);
  PushChar ((CHAR16)0x0D);   /* ENTER */
  PushScan (SCAN_F2);
  EdRun (E);
  Len = sizeof (Buf);
  ReadFileBytes (Fs, U16 ("fs0:/bom.nsh"), Buf, &Len);
  {
    const UINT8 Exp2[] = { 0xEF, 0xBB, 0xBF, 'X', 'a', '\r', '\n', '\r', '\n', 'b', '\r', '\n' };
    ASSERT_EQ (Len, sizeof (Exp2));
    if (Len == sizeof (Exp2)) ASSERT_MEM_EQ (Buf, Exp2, sizeof (Exp2));
  }
  EdFree (E);
  Fs->Destroy (Fs);
}

/* ---- T13-3 修改标记生命周期（撤销回保存点消失） ---- */

TEST (t13_3_modified_lifecycle) {
  FS_OPS *Fs = MockFsNew ();
  EDITOR_APP *E;
  MockFsAddFile (Fs, U16 ("fs0:/m.nsh"), (CONST UINT8 *)"abc\n", 4);
  E = NewEd (Fs, 262144);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/m.nsh")));
  EdRender (E);
  ASSERT_TRUE (RowHas (23, "m.nsh"));
  ASSERT_FALSE (RowHas (23, "*"));
  PushStr ("Z");
  EdRun (E);
  ASSERT_TRUE (ModelModified (EdModel (E)));
  ASSERT_TRUE (RowHas (23, "*"));
  PushScan (SCAN_F7);       /* 撤销回保存点 */
  EdRun (E);
  ASSERT_FALSE (ModelModified (EdModel (E)));
  ASSERT_FALSE (RowHas (23, "*"));
  PushStr ("Q");
  PushScan (SCAN_F2);       /* 再编辑并保存 → 标记消失 */
  EdRun (E);
  ASSERT_FALSE (ModelModified (EdModel (E)));
  ASSERT_FALSE (RowHas (23, "*"));
  EdFree (E);
  Fs->Destroy (Fs);
}

/* ---- T13-4 退出三键（保存/放弃/取消）各路径 ---- */

TEST (t13_4_exit_three_keys) {
  FS_OPS *Fs = MockFsNew ();
  UINT8 Buf[64];
  UINTN Len;
  EDITOR_APP *E;

  /* (a) 放弃：修改后 F10 → D → 退出，文件不变 */
  MockFsAddFile (Fs, U16 ("fs0:/q.nsh"), (CONST UINT8 *)"data\n", 5);
  E = NewEd (Fs, 262144);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/q.nsh")));
  PushStr ("X");
  PushScan (SCAN_F10);
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_CONFIRM);
  PushChar ((CHAR16)'D');
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_DONE);
  ASSERT_EQ (EdExitCode (E), 0);
  Len = sizeof (Buf);
  ReadFileBytes (Fs, U16 ("fs0:/q.nsh"), Buf, &Len);
  ASSERT_EQ (Len, 5);
  if (Len == 5) ASSERT_MEM_EQ (Buf, "data\n", 5);
  EdFree (E);

  /* (b) 保存退出：F10 → S */
  E = NewEd (Fs, 262144);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/q.nsh")));
  PushStr ("Y");
  PushScan (SCAN_F10);
  PushChar ((CHAR16)'S');
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_DONE);
  ASSERT_EQ (EdExitCode (E), 0);
  Len = sizeof (Buf);
  ReadFileBytes (Fs, U16 ("fs0:/q.nsh"), Buf, &Len);
  ASSERT_EQ (Len, 6);
  if (Len == 6) ASSERT_MEM_EQ (Buf, "Ydata\n", 6);
  EdFree (E);

  /* (c) 取消：F10 → ESC → 回编辑；再 F10 → D 退出 */
  MockFsAddFile (Fs, U16 ("fs0:/q.nsh"), (CONST UINT8 *)"data\n", 5);
  E = NewEd (Fs, 262144);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/q.nsh")));
  PushStr ("Z");
  PushScan (SCAN_F10);
  PushScan (SCAN_ESC);
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_EDIT);
  ASSERT_TRUE (ModelModified (EdModel (E)));
  PushScan (SCAN_F10);
  PushChar ((CHAR16)'d');   /* 小写亦接受 */
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_DONE);
  Len = sizeof (Buf);
  ReadFileBytes (Fs, U16 ("fs0:/q.nsh"), Buf, &Len);
  ASSERT_EQ (Len, 5);
  EdFree (E);

  /* (d) 未修改 F10 直接退出 */
  E = NewEd (Fs, 262144);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/q.nsh")));
  PushScan (SCAN_F10);
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_DONE);
  ASSERT_EQ (EdExitCode (E), 0);
  EdFree (E);

  /* (e) 未命名新文件：修改 → F10 → S → 另存为输入 → 保存并退出 */
  E = NewEd (Fs, 262144);
  EdRender (E);
  ASSERT_TRUE (RowHas (23, "<new>"));
  PushStr ("new");
  PushScan (SCAN_F10);
  PushChar ((CHAR16)'S');
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_SAVEAS);
  PushStr ("fs0:/fresh.nsh");
  PushChar ((CHAR16)0x0D);   /* ENTER */
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_DONE);
  ASSERT_EQ (EdExitCode (E), 0);
  Len = sizeof (Buf);
  ReadFileBytes (Fs, U16 ("fs0:/fresh.nsh"), Buf, &Len);
  ASSERT_EQ (Len, 3);
  if (Len == 3) ASSERT_MEM_EQ (Buf, "new", 3);
  EdFree (E);
  Fs->Destroy (Fs);
}

/* ---- T13-5 查找：命中数/大小写/上一/下一 ---- */

TEST (t13_5_find_navigation) {
  FS_OPS *Fs = MockFsNew ();
  EDITOR_APP *E;
  MockFsAddFile (Fs, U16 ("fs0:/f.nsh"), (CONST UINT8 *)"abc ABC\nabcx\n", 13);
  E = NewEd (Fs, 262144);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/f.nsh")));
  /* 大小写敏感：命中 (0,0),(1,0)；光标 (0,0) → 下一 = (1,0) */
  PushScan (SCAN_F3);
  PushStr ("abc");
  PushChar ((CHAR16)0x0D);   /* ENTER */
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_EDIT);
  CursorAt (E, 1, 0);
  ASSERT_TRUE (RowHas (23, "2 hits"));
  /* 下一（needle 保留，F3+ENTER）：回绕 (0,0) */
  PushScan (SCAN_F3);
  PushChar ((CHAR16)0x0D);   /* ENTER */
  EdRun (E);
  CursorAt (E, 0, 0);
  /* 上一（F4）：光标前无命中 → 回绕末个 (1,0) */
  PushScan (SCAN_F4);
  EdRun (E);
  CursorAt (E, 1, 0);
  /* 大小写不敏感（TAB 切换）：3 命中，从 (1,0) 回绕 (0,0) */
  PushScan (SCAN_F3);
  PushChar ((CHAR16)0x09);   /* TAB → CaseSens OFF */
  PushChar ((CHAR16)0x0D);   /* ENTER */
  EdRun (E);
  CursorAt (E, 0, 0);
  ASSERT_TRUE (RowHas (23, "3 hits"));
  /* 下一 → (0,4)（"ABC"） */
  PushScan (SCAN_F3);
  PushChar ((CHAR16)0x0D);   /* ENTER */
  EdRun (E);
  CursorAt (E, 0, 4);
  /* 无命中 */
  PushScan (SCAN_F3);
  PushBackspaces (3);
  PushStr ("zzz");
  PushChar ((CHAR16)0x0D);   /* ENTER */
  EdRun (E);
  ASSERT_TRUE (RowHas (23, "No hits"));
  /* 空 needle */
  PushScan (SCAN_F3);
  PushBackspaces (3);
  PushChar ((CHAR16)0x0D);   /* ENTER */
  EdRun (E);
  ASSERT_TRUE (RowHas (23, "Empty needle"));
  /* ESC 取消输入 */
  PushScan (SCAN_F3);
  PushScan (SCAN_ESC);
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_EDIT);
  EdFree (E);
  Fs->Destroy (Fs);
}

/* ---- T13-6 跳行 ---- */

TEST (t13_6_goto_line) {
  FS_OPS *Fs = MockFsNew ();
  EDITOR_APP *E;
  MockFsAddFile (Fs, U16 ("fs0:/g.nsh"), (CONST UINT8 *)"l1\nl2\nl3\nl4\n", 12);
  E = NewEd (Fs, 262144);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/g.nsh")));
  PushScan (SCAN_F6);
  PushStr ("3");
  PushChar ((CHAR16)0x0D);   /* ENTER */
  EdRun (E);
  CursorAt (E, 2, 0);
  /* 越界钳制到末行（尾换行 → 第 5 行空行） */
  PushScan (SCAN_F6);
  PushStr ("999");
  PushChar ((CHAR16)0x0D);   /* ENTER */
  EdRun (E);
  CursorAt (E, 4, 0);
  /* 0 钳制到第 1 行 */
  PushScan (SCAN_F6);
  PushStr ("0");
  PushChar ((CHAR16)0x0D);   /* ENTER */
  EdRun (E);
  CursorAt (E, 0, 0);
  /* ESC 取消 */
  PushScan (SCAN_F6);
  PushStr ("2");
  PushScan (SCAN_ESC);
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_EDIT);
  CursorAt (E, 0, 0);
  EdFree (E);
  Fs->Destroy (Fs);
}

/* ---- T13-7 撤销 ---- */

TEST (t13_7_undo) {
  FS_OPS *Fs = MockFsNew ();
  UINT8 Buf[64];
  UINTN Len;
  EDITOR_APP *E;
  MockFsAddFile (Fs, U16 ("fs0:/u.nsh"), (CONST UINT8 *)"base\n", 5);
  E = NewEd (Fs, 262144);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/u.nsh")));
  PushStr ("ABC");          /* 连续插入 = 1 步 */
  PushScan (SCAN_F7);
  EdRun (E);
  ASSERT_FALSE (ModelModified (EdModel (E)));
  /* 多步：X / 换行 / Y 各独立 */
  PushStr ("X");
  PushChar ((CHAR16)0x0D);   /* ENTER */
  PushStr ("Y");
  PushScan (SCAN_F7);       /* 撤 Y */
  PushScan (SCAN_F7);       /* 撤换行 */
  PushScan (SCAN_F7);       /* 撤 X */
  PushScan (SCAN_F2);
  EdRun (E);
  ASSERT_FALSE (ModelModified (EdModel (E)));
  Len = sizeof (Buf);
  ReadFileBytes (Fs, U16 ("fs0:/u.nsh"), Buf, &Len);
  ASSERT_EQ (Len, 5);
  if (Len == 5) ASSERT_MEM_EQ (Buf, "base\n", 5);
  /* 空栈撤销提示不崩 */
  PushScan (SCAN_F7);
  EdRun (E);
  ASSERT_TRUE (RowHas (23, "Nothing to undo"));
  EdFree (E);
  Fs->Destroy (Fs);
}

/* ---- T13-8 超限拒绝 ---- */

TEST (t13_8_size_limit) {
  FS_OPS *Fs = MockFsNew ();
  UINT8 Big[65];
  EDITOR_APP *E;
  memset (Big, 'a', sizeof (Big));
  MockFsAddFile (Fs, U16 ("fs0:/big.nsh"), Big, sizeof (Big));
  E = NewEd (Fs, 64);       /* MaxBytes=64（PCD 值由 glue 传入，core 参数化） */
  ASSERT_EQ (EdOpen (E, U16 ("fs0:/big.nsh")), EFI_BAD_BUFFER_SIZE);
  ASSERT_EQ (EdState (E), ED_ERROR);
  EdRender (E);
  ASSERT_TRUE (RowHas (12, "TOO LARGE"));
  PushChar ((CHAR16)'x');   /* 致命错误页：任意键退出 */
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_DONE);
  ASSERT_EQ (EdExitCode (E), 1);
  EdFree (E);
  /* 恰好 MaxBytes 可打开 */
  E = NewEd (Fs, 65);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/big.nsh")));
  ASSERT_EQ (EdState (E), ED_EDIT);
  EdFree (E);
  Fs->Destroy (Fs);
}

/* ---- T13-9 另存为新文件 ---- */

TEST (t13_9_save_as) {
  FS_OPS *Fs = MockFsNew ();
  UINT8 Buf[64];
  UINTN Len;
  EDITOR_APP *E;
  MockFsAddFile (Fs, U16 ("fs0:/a.nsh"), (CONST UINT8 *)"content\n", 8);
  E = NewEd (Fs, 262144);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/a.nsh")));
  PushScan (SCAN_F5);       /* 输入行预填当前路径 */
  PushBackspaces (11);      /* 清空 "fs0:/a.nsh" */
  PushStr ("fs0:/b.nsh");
  PushChar ((CHAR16)0x0D);   /* ENTER */
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_EDIT);
  ASSERT_TRUE (EfiStrCmp (EdPath (E), U16 ("fs0:/b.nsh")) == 0);
  Len = sizeof (Buf);
  ReadFileBytes (Fs, U16 ("fs0:/b.nsh"), Buf, &Len);
  ASSERT_EQ (Len, 8);
  if (Len == 8) ASSERT_MEM_EQ (Buf, "content\n", 8);
  /* 后续 F2 保存到新路径；原文件不变 */
  PushStr ("Z");
  PushScan (SCAN_F2);
  EdRun (E);
  Len = sizeof (Buf);
  ReadFileBytes (Fs, U16 ("fs0:/b.nsh"), Buf, &Len);
  ASSERT_EQ (Len, 9);
  if (Len == 9) ASSERT_MEM_EQ (Buf, "Zcontent\n", 9);
  Len = sizeof (Buf);
  ReadFileBytes (Fs, U16 ("fs0:/a.nsh"), Buf, &Len);
  ASSERT_EQ (Len, 8);
  /* 非 ASCII 文件名拒绝（AsciiOnly） */
  PushScan (SCAN_F5);
  PushBackspaces (11);
  PushChar ((CHAR16)0x4E2D);
  EdRun (E);
  ASSERT_TRUE (RowHas (23, "ASCII only"));
  PushScan (SCAN_ESC);
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_EDIT);
  EdFree (E);

  /* 未命名新文件 F2 → 另存为输入 */
  E = NewEd (Fs, 262144);
  PushStr ("hi");
  PushScan (SCAN_F2);
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_SAVEAS);
  PushStr ("fs0:/c.nsh");
  PushChar ((CHAR16)0x0D);   /* ENTER */
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_EDIT);
  ASSERT_TRUE (EfiStrCmp (EdPath (E), U16 ("fs0:/c.nsh")) == 0);
  Len = sizeof (Buf);
  ReadFileBytes (Fs, U16 ("fs0:/c.nsh"), Buf, &Len);
  ASSERT_EQ (Len, 2);
  if (Len == 2) ASSERT_MEM_EQ (Buf, "hi", 2);
  EdFree (E);
  Fs->Destroy (Fs);
}

/* ---- T13-10 状态栏 U+XXXX / RAW 显示 ---- */

TEST (t13_10_status_codepoint) {
  FS_OPS *Fs = MockFsNew ();
  const UINT8 Data[] = { 0xE4, 0xB8, 0xAD, 0xFF, 'x', '\n' };   /* 中 + RAW FF + x */
  EDITOR_APP *E;
  MockFsAddFile (Fs, U16 ("fs0:/cp.nsh"), Data, sizeof (Data));
  E = NewEd (Fs, 262144);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/cp.nsh")));
  EdRender (E);
  ASSERT_TRUE (RowHas (23, "U+4E2D"));    /* 光标在 中 */
  PushScan (SCAN_RIGHT);
  EdRun (E);
  ASSERT_TRUE (RowHas (23, "RAW:0xFF"));  /* 光标在非法字节 */
  PushScan (SCAN_RIGHT);
  EdRun (E);
  ASSERT_FALSE (RowHas (23, "U+"));       /* ASCII 不显示码点 */
  ASSERT_FALSE (RowHas (23, "RAW:"));
  PushScan (SCAN_END);
  EdRun (E);
  ASSERT_FALSE (RowHas (23, "U+"));       /* 行尾插入位无 token */
  EdFree (E);
  Fs->Destroy (Fs);
}

/* ---- T13-11 80×25 渲染快照 ---- */

TEST (t13_11_render_snapshot) {
  FS_OPS *Fs = MockFsNew ();
  EDITOR_APP *E;
  MockFsAddFile (Fs, U16 ("fs0:/r.nsh"), (CONST UINT8 *)"alpha\nbeta\n", 11);
  E = NewEd (Fs, 262144);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/r.nsh")));
  EdRender (E);
  /* 行号栏（7 列：%6u+空格）+ 文本 */
  ASSERT_EQ (MockTuiGetCell (0, 5).Char, (CHAR16)'1');
  ASSERT_EQ (MockTuiGetCell (0, 6).Char, (CHAR16)' ');
  ASSERT_EQ (MockTuiGetCell (0, 7).Char, (CHAR16)'a');
  ASSERT_TRUE (RowHas (0, "alpha"));
  ASSERT_EQ (MockTuiGetCell (1, 5).Char, (CHAR16)'2');
  ASSERT_TRUE (RowHas (1, "beta"));
  ASSERT_EQ (MockTuiGetCell (2, 5).Char, (CHAR16)'3');   /* 尾换行空行 */
  ASSERT_EQ (MockTuiGetCell (3, 0).Char, 0);             /* 超出 EOF 不画 */
  /* 光标反显 */
  ASSERT_EQ (MockTuiGetCell (0, 7).Attr, TUI_ATTR_SELECTED);
  /* 状态栏 / 键位条 */
  ASSERT_TRUE (RowHas (23, "r.nsh"));
  ASSERT_TRUE (RowHas (23, "L1:C1"));
  ASSERT_TRUE (RowHas (23, "UTF-8"));
  ASSERT_FALSE (RowHas (23, "*"));
  ASSERT_EQ (MockTuiGetCell (23, 0).Attr, TUI_ATTR_STATUSBAR);
  ASSERT_TRUE (RowHas (24, "F2 Save"));
  ASSERT_TRUE (RowHas (24, "F10 Quit"));
  ASSERT_EQ (MockTuiGetCell (24, 0).Attr, TUI_ATTR_KEYBAR);
  EdFree (E);
  /* 非 ASCII 占位属性（光标移开后） */
  E = NewEd (Fs, 262144);
  MockFsAddFile (Fs, U16 ("fs0:/zh.nsh"), (CONST UINT8 *)"\xE4\xB8\xAD\n", 4);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/zh.nsh")));
  PushScan (SCAN_END);
  EdRun (E);
  ASSERT_EQ (MockTuiGetCell (0, 7).Char, TUI_NONASCII_CHAR);
  ASSERT_EQ (MockTuiGetCell (0, 7).Attr, TUI_ATTR_NONASCII);
  EdFree (E);
  Fs->Destroy (Fs);
}

/* ---- T13-12 保存失败错误页不崩 ---- */

TEST (t13_12_save_failure) {
  FS_OPS *Fs = MockFsNew ();
  UINT8 Buf[64];
  UINTN Len;
  EDITOR_APP *E;
  /* MockFs 无权限语义：以非空目录为目标模拟保存失败
     （真实只读目录覆盖待 S09 POSIX 后端集成，见交接）。 */
  MockFsAddDir (Fs, U16 ("fs0:/ro"));
  MockFsAddFile (Fs, U16 ("fs0:/ro/child"), (CONST UINT8 *)"x", 1);
  MockFsAddFile (Fs, U16 ("fs0:/ok.nsh"), (CONST UINT8 *)"keep\n", 5);
  E = NewEd (Fs, 262144);
  ASSERT_OK (EdOpen (E, U16 ("fs0:/ok.nsh")));
  PushScan (SCAN_F5);
  PushBackspaces (12);      /* 清空 "fs0:/ok.nsh" */
  PushStr ("fs0:/ro");
  PushChar ((CHAR16)0x0D);   /* ENTER */
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_ERROR);
  EdRender (E);
  ASSERT_TRUE (RowHas (12, "directory"));
  PushChar ((CHAR16)'k');   /* 非致命 → 回编辑 */
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_EDIT);
  ASSERT_TRUE (RowHas (23, "*"));   /* SaveFailed → 仍视为已修改 */
  /* 重试到合法路径成功 */
  PushScan (SCAN_F5);
  PushBackspaces (12);
  PushStr ("fs0:/new.nsh");
  PushChar ((CHAR16)0x0D);   /* ENTER */
  EdRun (E);
  ASSERT_EQ (EdState (E), ED_EDIT);
  ASSERT_FALSE (RowHas (23, "*"));
  Len = sizeof (Buf);
  ReadFileBytes (Fs, U16 ("fs0:/new.nsh"), Buf, &Len);
  ASSERT_EQ (Len, 5);
  if (Len == 5) ASSERT_MEM_EQ (Buf, "keep\n", 5);
  /* 原文件与目录完好 */
  Len = sizeof (Buf);
  ReadFileBytes (Fs, U16 ("fs0:/ok.nsh"), Buf, &Len);
  ASSERT_EQ (Len, 5);
  EdFree (E);
  Fs->Destroy (Fs);
}

int main (void) {
  printf ("== S13 Editor host tests (MockTui + MockFs + real EditorModel/Utf8) ==\n");
  RUN_TEST (t13_1_utf8_roundtrip);
  RUN_TEST (t13_2_bom_crlf);
  RUN_TEST (t13_3_modified_lifecycle);
  RUN_TEST (t13_4_exit_three_keys);
  RUN_TEST (t13_5_find_navigation);
  RUN_TEST (t13_6_goto_line);
  RUN_TEST (t13_7_undo);
  RUN_TEST (t13_8_size_limit);
  RUN_TEST (t13_9_save_as);
  RUN_TEST (t13_10_status_codepoint);
  RUN_TEST (t13_11_render_snapshot);
  RUN_TEST (t13_12_save_failure);
  RUN_TESTS_END ();
}
