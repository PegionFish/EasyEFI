/* S02 EditorModelLib 宿主测试（编号用例 T02-1..10，任务书 DoD 依据）。
 * 运行: make -f host.mk test（模块目录内；链接真实 S01 Utf8Lib）。
 * 历史：S01 合并前中文 CP 级子用例曾以 S02_REAL_UTF8 门控（垫片阶段），现已全部启用。
 */
#include "test.h"
#include "Contract/EditorModel.h"

/* ---- 助手 ---- */

static EDITOR_MODEL *NewModel (UINTN MaxBytes, UINTN Depth) {
  EDITOR_MODEL *M = ModelNew (MaxBytes, Depth);
  ASSERT_TRUE (M != NULL);
  return M;
}

static void LoadStr (EDITOR_MODEL *M, const char *S) {
  ASSERT_OK (ModelLoad (M, (CONST UINT8 *)S, strlen (S)));
}

static void LoadBytes (EDITOR_MODEL *M, const UINT8 *B, UINTN N) {
  ASSERT_OK (ModelLoad (M, B, N));
}

/* 保存并与期望字节逐一对比（callee 分配 → caller MemFree） */
static void ExpectSave (EDITOR_MODEL *M, const void *Exp, UINTN N) {
  UINT8 *Out = NULL;
  UINTN Len = 0;
  ASSERT_OK (ModelSave (M, &Out, &Len));
  ASSERT_EQ (Len, N);
  if (Len == N && N) ASSERT_MEM_EQ (Out, Exp, N);
  MemFree (Out);
}

/* 经渲染投影检查 ASCII 行内容（不触发保存点移动） */
static void ExpectLine (EDITOR_MODEL *M, UINTN Line, const char *Exp) {
  TUI_CELL Cells[256];
  UINTN i, N = strlen (Exp);
  ASSERT_TRUE (N + 1 <= 256);
  memset (Cells, 0, sizeof (Cells));
  ASSERT_OK (ModelRenderRow (M, Line, 0, N + 1, Cells));
  for (i = 0; i < N; i++) ASSERT_EQ (Cells[i].Char, (CHAR16)(UINT8)Exp[i]);
  ASSERT_EQ (Cells[N].Char, (CHAR16)' ');   /* 行尾之后 = 空格填充 */
}

static void InsertAscii (EDITOR_MODEL *M, const char *S) {
  UINT32 Cps[256];
  UINTN i, N = strlen (S);
  ASSERT_TRUE (N <= 256);
  for (i = 0; i < N; i++) Cps[i] = (UINT8)S[i];
  ASSERT_OK (ModelInsert (M, Cps, N));
}

static void SetCur (EDITOR_MODEL *M, UINTN L, UINTN C) {
  ASSERT_OK (ModelSetCursor (M, L, C));
}

/* ---- T02-1 加载/保存往返 ---- */

TEST (t02_1_roundtrip) {
  EDITOR_MODEL *M = NewModel (262144, 256);
  MODEL_INFO Info;
  /* 中文注释 .nsh（CRLF）：字节往返恒等 */
  const char *Zh = "# \xE4\xB8\xAD\xE6\x96\x87\xE6\xB3\xA8\xE9\x87\x8A\r\nfs0:\\fpt.efi\r\n";
  LoadStr (M, Zh);
  ExpectSave (M, Zh, strlen (Zh));
  ASSERT_OK (ModelInfo (M, &Info));
  ASSERT_EQ (Info.LineCount, 3);
  ASSERT_FALSE (Info.HasBom);
  ASSERT_EQ (Info.DominantEol, 1);
  ASSERT_FALSE (Info.Modified);
  ASSERT_EQ (Info.ByteSize, strlen (Zh));
  /* BOM 有则保留 */
  {
    const UINT8 Bom[] = { 0xEF, 0xBB, 0xBF, 'e', 'c', 'h', 'o', ' ', 'h', 'i', '\n' };
    LoadBytes (M, Bom, sizeof (Bom));
    ExpectSave (M, Bom, sizeof (Bom));
    ASSERT_OK (ModelInfo (M, &Info));
    ASSERT_TRUE (Info.HasBom);
  }
  /* 仅 BOM */
  {
    const UINT8 BomOnly[] = { 0xEF, 0xBB, 0xBF };
    LoadBytes (M, BomOnly, sizeof (BomOnly));
    ExpectSave (M, BomOnly, sizeof (BomOnly));
    ASSERT_OK (ModelInfo (M, &Info));
    ASSERT_TRUE (Info.HasBom);
    ASSERT_EQ (Info.LineCount, 1);
  }
  /* BOM 不添加 */
  LoadStr (M, "x");
  ExpectSave (M, "x", 1);
  ASSERT_OK (ModelInfo (M, &Info));
  ASSERT_FALSE (Info.HasBom);
  /* 混合行尾 + 无尾换行：逐行终止符保留 */
  {
    const char *Mix = "a\r\nb\nc\r\nd";
    LoadStr (M, Mix);
    ExpectSave (M, Mix, strlen (Mix));
    ASSERT_OK (ModelInfo (M, &Info));
    ASSERT_EQ (Info.LineCount, 4);
    ASSERT_EQ (Info.DominantEol, 1);   /* CRLF 2 > LF 1 */
  }
  {
    const char *MixLf = "a\nb\r\nc\n";
    LoadStr (M, MixLf);
    ExpectSave (M, MixLf, strlen (MixLf));
    ASSERT_OK (ModelInfo (M, &Info));
    ASSERT_EQ (Info.DominantEol, 0);   /* LF 2 > CRLF 1 */
  }
  /* 无尾换行 */
  LoadStr (M, "abc");
  ExpectSave (M, "abc", 3);
  ASSERT_OK (ModelInfo (M, &Info));
  ASSERT_EQ (Info.LineCount, 1);
  /* 空文件 */
  ASSERT_OK (ModelLoad (M, NULL, 0));
  ExpectSave (M, "", 0);
  ASSERT_OK (ModelInfo (M, &Info));
  ASSERT_EQ (Info.LineCount, 1);
  /* 孤立 CR 按内容保留（不作行终止符） */
  LoadStr (M, "a\rb\r");
  ExpectSave (M, "a\rb\r", 4);
  ASSERT_OK (ModelInfo (M, &Info));
  ASSERT_EQ (Info.LineCount, 1);
  ModelFree (M);
}

/* ---- T02-2 边界编辑 ---- */

TEST (t02_2_boundary_edit) {
  EDITOR_MODEL *M = NewModel (262144, 256);
  /* 空文件 */
  LoadStr (M, "");
  InsertAscii (M, "ab");
  ExpectSave (M, "ab", 2);
  ASSERT_OK (ModelBackspace (M));
  ExpectSave (M, "a", 1);
  ASSERT_OK (ModelBackspace (M));
  ExpectSave (M, "", 0);
  ASSERT_OK (ModelBackspace (M));   /* 文件头：空操作不崩 */
  ExpectSave (M, "", 0);
  ASSERT_OK (ModelDeleteFwd (M));   /* 文件尾：空操作不崩 */
  ExpectSave (M, "", 0);
  /* 单行无换行：行首/行尾插入删除 */
  LoadStr (M, "abc");
  SetCur (M, 0, 0);
  ASSERT_OK (ModelDeleteFwd (M));
  ExpectSave (M, "bc", 2);
  SetCur (M, 0, 2);
  ASSERT_OK (ModelBackspace (M));
  ExpectSave (M, "b", 1);
  SetCur (M, 0, 0);
  InsertAscii (M, "X");
  ExpectSave (M, "Xb", 2);
  SetCur (M, 0, 2);
  InsertAscii (M, "Y");
  ExpectSave (M, "XbY", 3);
  SetCur (M, 0, 1);
  InsertAscii (M, "Z");
  ExpectSave (M, "XZbY", 4);
  /* 行首退格 → 并上一行（去上一行终止符，保留本行终止符） */
  LoadStr (M, "a\r\nb\n");
  SetCur (M, 1, 0);
  ASSERT_OK (ModelBackspace (M));
  ExpectSave (M, "ab\n", 3);
  /* 行尾前删 → 并下一行（去本行终止符，保留下一行终止符） */
  LoadStr (M, "a\nb\r\n");
  SetCur (M, 0, 1);
  ASSERT_OK (ModelDeleteFwd (M));
  ExpectSave (M, "ab\r\n", 4);
  /* 文件尾前删空操作（最后一行行尾） */
  LoadStr (M, "ab");
  SetCur (M, 0, 2);
  ASSERT_OK (ModelDeleteFwd (M));
  ExpectSave (M, "ab", 2);
  /* 空行合并 */
  LoadStr (M, "\n\n");
  SetCur (M, 1, 0);
  ASSERT_OK (ModelBackspace (M));
  ExpectSave (M, "\n", 1);
  ModelFree (M);
}

/* ---- T02-3 换行约定 ---- */

TEST (t02_3_newline_convention) {
  EDITOR_MODEL *M = NewModel (262144, 256);
  /* CRLF 文件中新行用 CRLF */
  LoadStr (M, "a\r\nb\r\n");
  SetCur (M, 0, 1);
  ASSERT_OK (ModelNewline (M));
  ExpectSave (M, "a\r\n\r\nb\r\n", 8);
  /* LF 文件用 LF */
  LoadStr (M, "a\nb\n");
  SetCur (M, 0, 1);
  ASSERT_OK (ModelNewline (M));
  ExpectSave (M, "a\n\nb\n", 5);
  /* 无换行文件 → 默认 LF */
  LoadStr (M, "ab");
  SetCur (M, 0, 1);
  ASSERT_OK (ModelNewline (M));
  ExpectSave (M, "a\nb", 3);
  /* 混合行尾 CRLF 占主 → 新行 CRLF（原行终止符不变） */
  LoadStr (M, "a\r\nb\r\nc\n");
  SetCur (M, 2, 1);
  ASSERT_OK (ModelNewline (M));
  ExpectSave (M, "a\r\nb\r\nc\r\n\n", 10);
  /* 行首分裂 */
  LoadStr (M, "ab\n");
  SetCur (M, 0, 0);
  ASSERT_OK (ModelNewline (M));
  ExpectSave (M, "\nab\n", 4);
  ModelFree (M);
}

/* ---- T02-4 撤销分组 ---- */

TEST (t02_4_undo_grouping) {
  EDITOR_MODEL *M = NewModel (262144, 256);
  /* 连续输入 = 1 步；撤销回保存点 Modified 消失 */
  LoadStr (M, "x\n");
  InsertAscii (M, "a");
  InsertAscii (M, "b");
  InsertAscii (M, "c");
  ASSERT_TRUE (ModelModified (M));
  ASSERT_OK (ModelUndo (M));        /* 一次撤销移除全部连续插入 */
  ExpectLine (M, 0, "x");
  ASSERT_FALSE (ModelModified (M));
  ASSERT_ERR (ModelUndo (M));       /* 栈空 */
  /* 换行/删除独立成步（导航不计步）。加载后光标在 (0,0)，先移到行尾。 */
  LoadStr (M, "ab");
  SetCur (M, 0, 2);
  InsertAscii (M, "c");             /* 步1 "abc" */
  ASSERT_OK (ModelNewline (M));     /* 步2 "abc\n"+"" 光标(1,0) */
  InsertAscii (M, "d");             /* 步3 "abc\nd" */
  SetCur (M, 0, 3);
  ASSERT_OK (ModelDeleteFwd (M));   /* 步4 并行 → "abcd" */
  ASSERT_TRUE (ModelModified (M));
  ASSERT_OK (ModelUndo (M));
  ExpectLine (M, 0, "abc");
  ExpectLine (M, 1, "d");
  ASSERT_OK (ModelUndo (M));
  ExpectLine (M, 0, "abc");
  ExpectLine (M, 1, "");
  ASSERT_OK (ModelUndo (M));
  ExpectLine (M, 0, "abc");
  ASSERT_OK (ModelUndo (M));
  ExpectLine (M, 0, "ab");
  ASSERT_FALSE (ModelModified (M));
  ASSERT_ERR (ModelUndo (M));
  /* 栈满丢弃最旧（深度 3，5 个独立插入步） */
  {
    EDITOR_MODEL *B = NewModel (262144, 3);
    LoadStr (B, "");
    InsertAscii (B, "1"); SetCur (B, 0, 0);
    InsertAscii (B, "2"); SetCur (B, 0, 0);
    InsertAscii (B, "3"); SetCur (B, 0, 0);
    InsertAscii (B, "4"); SetCur (B, 0, 0);
    InsertAscii (B, "5");
    ExpectLine (B, 0, "54321");
    ASSERT_OK (ModelUndo (B));
    ExpectLine (B, 0, "4321");
    ASSERT_OK (ModelUndo (B));
    ExpectLine (B, 0, "321");
    ASSERT_OK (ModelUndo (B));
    ExpectLine (B, 0, "21");
    ASSERT_ERR (ModelUndo (B));     /* 最旧 2 步已丢弃 */
    ASSERT_TRUE (ModelModified (B));
    ModelFree (B);
  }
  /* 光标移动打断连续合并 */
  LoadStr (M, "");
  InsertAscii (M, "a");             /* 步1 */
  ASSERT_OK (ModelMove (M, -1, 0));
  InsertAscii (M, "b");             /* 步2（不合并） */
  ASSERT_OK (ModelUndo (M));
  ExpectLine (M, 0, "a");
  ASSERT_OK (ModelUndo (M));
  ExpectLine (M, 0, "");
  ASSERT_FALSE (ModelModified (M));
  /* 保存点随保存移动：撤销到保存点 FALSE，再撤销（保存前步骤）TRUE */
  LoadStr (M, "a");
  SetCur (M, 0, 1);
  InsertAscii (M, "b");             /* 步1 "ab" */
  ASSERT_OK (ModelNewline (M));     /* 步2 "ab\n"+"" */
  ExpectSave (M, "ab\n", 3);        /* 保存点 */
  ASSERT_FALSE (ModelModified (M));
  SetCur (M, 1, 0);
  InsertAscii (M, "c");             /* 步3 "ab\nc" */
  ASSERT_TRUE (ModelModified (M));
  ASSERT_OK (ModelUndo (M));        /* 回保存点 */
  ASSERT_FALSE (ModelModified (M));
  ASSERT_OK (ModelUndo (M));        /* 撤销换行（保存前步骤） */
  ExpectLine (M, 0, "ab");
  ASSERT_TRUE (ModelModified (M));
  ASSERT_OK (ModelUndo (M));
  ExpectLine (M, 0, "a");
  ASSERT_TRUE (ModelModified (M));
  /* UndoDepth=0：撤销禁用但 Modified 跟踪仍有效 */
  {
    EDITOR_MODEL *Z = NewModel (262144, 0);
    LoadStr (Z, "a");
    SetCur (Z, 0, 1);
    InsertAscii (Z, "b");
    ASSERT_TRUE (ModelModified (Z));
    ASSERT_ERR (ModelUndo (Z));
    ExpectSave (Z, "ab", 2);
    ASSERT_FALSE (ModelModified (Z));
    ModelFree (Z);
  }
  ModelFree (M);
}

/* ---- T02-5 查找 ---- */

TEST (t02_5_find) {
  EDITOR_MODEL *M = NewModel (262144, 256);
  MODEL_HIT *Hits;
  UINTN Count;
  UINT32 Abc[] = { 'a', 'b', 'c' };
  UINT32 Aa[] = { 'a', 'a' };
  LoadStr (M, "abc ABC\nabcx\n");
  /* 大小写敏感 */
  ASSERT_OK (ModelFind (M, Abc, 3, TRUE, &Hits, &Count));
  ASSERT_EQ (Count, 2);
  if (Count == 2) {
    ASSERT_EQ (Hits[0].Line, 0); ASSERT_EQ (Hits[0].Col, 0);
    ASSERT_EQ (Hits[1].Line, 1); ASSERT_EQ (Hits[1].Col, 0);
  }
  ModelFreeHits (Hits);
  /* 大小写不敏感（ASCII 折叠） */
  ASSERT_OK (ModelFind (M, Abc, 3, FALSE, &Hits, &Count));
  ASSERT_EQ (Count, 3);
  if (Count == 3) {
    ASSERT_EQ (Hits[1].Line, 0); ASSERT_EQ (Hits[1].Col, 4);
  }
  ModelFreeHits (Hits);
  /* 空 needle → 0 命中 */
  ASSERT_OK (ModelFind (M, NULL, 0, TRUE, &Hits, &Count));
  ASSERT_EQ (Count, 0);
  ASSERT_TRUE (Hits == NULL);
  /* 无命中 */
  {
    UINT32 Zz[] = { 'z', 'z' };
    ASSERT_OK (ModelFind (M, Zz, 2, TRUE, &Hits, &Count));
    ASSERT_EQ (Count, 0);
    ASSERT_TRUE (Hits == NULL);
  }
  /* 重叠命中 */
  LoadStr (M, "aaaa");
  ASSERT_OK (ModelFind (M, Aa, 2, TRUE, &Hits, &Count));
  ASSERT_EQ (Count, 3);
  ModelFreeHits (Hits);
  /* RAW 字节不参与 CP 匹配（非法 UTF-8 不透明） */
  {
    UINT8 Bad[] = { 'a', 0xFF, 'b' };
    UINT32 Needle[] = { 0xFF };
    LoadBytes (M, Bad, sizeof (Bad));
    ASSERT_OK (ModelFind (M, Needle, 1, TRUE, &Hits, &Count));
    ASSERT_EQ (Count, 0);
  }
  /* 中文 needle（非 ASCII 解出 CP token） */
  {
    UINT32 Zh[] = { 0x4E2D, 0x6587 };
    LoadStr (M, "# \xE4\xB8\xAD\xE6\x96\x87\n\xE4\xB8\xAD\xE6\x96\x87 ok\n");
    ASSERT_OK (ModelFind (M, Zh, 2, TRUE, &Hits, &Count));
    ASSERT_EQ (Count, 2);
    if (Count == 2) {
      ASSERT_EQ (Hits[0].Line, 0); ASSERT_EQ (Hits[0].Col, 2);
      ASSERT_EQ (Hits[1].Line, 1); ASSERT_EQ (Hits[1].Col, 0);
    }
    ModelFreeHits (Hits);
  }
  ModelFreeHits (NULL);   /* NULL 安全 */
  ModelFree (M);
}

/* ---- T02-6 跳行与光标钳制 ---- */

TEST (t02_6_goto_clamp) {
  EDITOR_MODEL *M = NewModel (262144, 256);
  MODEL_INFO Info;
  LoadStr (M, "short\na bit longer\nx");   /* 无尾换行 → 恰 3 行 */
  ASSERT_OK (ModelGotoLine (M, 2));
  ASSERT_OK (ModelInfo (M, &Info));
  ASSERT_EQ (Info.CursorLine, 1);
  ASSERT_EQ (Info.CursorCol, 0);
  ASSERT_OK (ModelGotoLine (M, 0));    /* 钳制到第 1 行 */
  ASSERT_OK (ModelInfo (M, &Info));
  ASSERT_EQ (Info.CursorLine, 0);
  ASSERT_OK (ModelGotoLine (M, 999));  /* 钳制到末行 */
  ASSERT_OK (ModelInfo (M, &Info));
  ASSERT_EQ (Info.CursorLine, 2);
  ASSERT_OK (ModelSetCursor (M, 999, 999));
  ASSERT_OK (ModelInfo (M, &Info));
  ASSERT_EQ (Info.CursorLine, 2);
  ASSERT_EQ (Info.CursorCol, 1);
  ASSERT_OK (ModelMove (M, 1000, 1000));
  ASSERT_OK (ModelInfo (M, &Info));
  ASSERT_EQ (Info.CursorLine, 2);
  ASSERT_EQ (Info.CursorCol, 1);
  ASSERT_OK (ModelMove (M, -1000, -1000));
  ASSERT_OK (ModelInfo (M, &Info));
  ASSERT_EQ (Info.CursorLine, 0);
  ASSERT_EQ (Info.CursorCol, 0);
  /* SetCursor 列钳制到行长 */
  ASSERT_OK (ModelSetCursor (M, 1, 999));
  ASSERT_OK (ModelInfo (M, &Info));
  ASSERT_EQ (Info.CursorCol, 12);
  /* 垂直移动钳制列到目标行长度（无目标列记忆，回移不恢复原列） */
  ASSERT_OK (ModelSetCursor (M, 1, 10));
  ASSERT_OK (ModelMove (M, 0, -1));
  ASSERT_OK (ModelInfo (M, &Info));
  ASSERT_EQ (Info.CursorLine, 0);
  ASSERT_EQ (Info.CursorCol, 5);
  ASSERT_OK (ModelMove (M, 0, 1));
  ASSERT_OK (ModelInfo (M, &Info));
  ASSERT_EQ (Info.CursorLine, 1);
  ASSERT_EQ (Info.CursorCol, 5);
  /* 中文列 = 码点数（token 索引） */
  LoadStr (M, "\xE4\xB8\xAD\xE6\x96\x87x");
  ASSERT_OK (ModelSetCursor (M, 0, 100));
  ASSERT_OK (ModelInfo (M, &Info));
  ASSERT_EQ (Info.CursorCol, 3);
  /* ModelTokenAt（契约 01efe65 批准新增；S13 状态栏 U+XXXX 用） */
  {
    UTF8_TOKEN Tok;
    ASSERT_OK (ModelTokenAt (M, 0, 0, &Tok));
    ASSERT_EQ (Tok.Type, UTF8_TOKEN_CP);
    ASSERT_EQ (Tok.Cp, 0x4E2D);
    ASSERT_OK (ModelTokenAt (M, 0, 1, &Tok));
    ASSERT_EQ (Tok.Cp, 0x6587);
    ASSERT_OK (ModelTokenAt (M, 0, 2, &Tok));
    ASSERT_EQ (Tok.Cp, 'x');
    ASSERT_EQ (ModelTokenAt (M, 0, 3, &Tok), EFI_NOT_FOUND);   /* 行尾插入位无 token */
    ASSERT_EQ (ModelTokenAt (M, 99, 0, &Tok), EFI_NOT_FOUND);
    ASSERT_EQ (ModelTokenAt (M, 0, 99, &Tok), EFI_NOT_FOUND);
    ASSERT_EQ (ModelTokenAt (NULL, 0, 0, &Tok), EFI_INVALID_PARAMETER);
    ASSERT_EQ (ModelTokenAt (M, 0, 0, NULL), EFI_INVALID_PARAMETER);
    /* RAW 字节原样可查（非法 UTF-8 不透明） */
    {
      UINT8 Bad[] = { 'a', 0xFF };
      LoadBytes (M, Bad, sizeof (Bad));
      ASSERT_OK (ModelTokenAt (M, 0, 1, &Tok));
      ASSERT_EQ (Tok.Type, UTF8_TOKEN_RAW);
      ASSERT_EQ (Tok.Raw, 0xFF);
    }
  }
  ModelFree (M);
}

/* ---- T02-7 超限拒绝 ---- */

TEST (t02_7_size_limit) {
  EDITOR_MODEL *M = NewModel (16, 8);
  UINT8 Buf[32];
  UINT8 Exp[16];
  UINT8 BomBuf[17];
  UINT32 C = 'b';
  UINTN i;
  memset (Buf, 'a', sizeof (Buf));
  for (i = 0; i < 15; i++) Exp[i] = 'a';
  Exp[15] = 'b';
  /* 加载限额：MaxBytes OK，MaxBytes+1 拒绝 */
  ASSERT_OK (ModelLoad (M, Buf, 16));
  ASSERT_EQ (ModelLoad (M, Buf, 17), EFI_BAD_BUFFER_SIZE);
  ExpectSave (M, Buf, 16);   /* 拒绝后状态不变 */
  /* 编辑增长同样受限 */
  ASSERT_OK (ModelLoad (M, Buf, 15));
  SetCur (M, 0, 15);
  ASSERT_OK (ModelInsert (M, &C, 1));   /* 16 = 上限 */
  ExpectSave (M, Exp, 16);
  ASSERT_EQ (ModelInsert (M, &C, 1), EFI_BAD_BUFFER_SIZE);
  ASSERT_EQ (ModelNewline (M), EFI_BAD_BUFFER_SIZE);   /* LF +1 也超限 */
  ExpectSave (M, Exp, 16);   /* 拒绝后内容不变 */
  /* BOM 计入限额 */
  BomBuf[0] = 0xEF; BomBuf[1] = 0xBB; BomBuf[2] = 0xBF;
  memset (BomBuf + 3, 'a', 14);
  ASSERT_OK (ModelLoad (M, BomBuf, 16));
  ASSERT_EQ (ModelLoad (M, BomBuf, 17), EFI_BAD_BUFFER_SIZE);
  /* CRLF 新行 +2 受限 */
  {
    EDITOR_MODEL *C2 = NewModel (8, 4);
    LoadStr (C2, "a\r\nb\r\n");   /* 6 字节，主约定 CRLF */
    SetCur (C2, 0, 1);
    ASSERT_OK (ModelNewline (C2));                    /* +2 = 8 */
    ASSERT_EQ (ModelNewline (C2), EFI_BAD_BUFFER_SIZE);
    ExpectSave (C2, "a\r\n\r\nb\r\n", 8);
    ModelFree (C2);
  }
  ModelFree (M);
}

/* ---- T02-8 渲染投影 ---- */

TEST (t02_8_render) {
  EDITOR_MODEL *M = NewModel (262144, 256);
  TUI_CELL Cells[64];
  /* ASCII 直显；非法字节 → 占位 + 属性（两阶段均成立） */
  {
    UINT8 Raw[] = { 'a', 0xFF, 'b' };
    LoadBytes (M, Raw, sizeof (Raw));
    ASSERT_OK (ModelRenderRow (M, 0, 0, 3, Cells));
    ASSERT_EQ (Cells[0].Char, (CHAR16)'a');
    ASSERT_EQ (Cells[0].Attr, TUI_ATTR_NORMAL);
    ASSERT_EQ (Cells[1].Char, TUI_NONASCII_CHAR);
    ASSERT_EQ (Cells[1].Attr, TUI_ATTR_NONASCII);
    ASSERT_EQ (Cells[2].Char, (CHAR16)'b');
    ASSERT_EQ (Cells[2].Attr, TUI_ATTR_NORMAL);
  }
  /* 列窗口切片 */
  LoadStr (M, "abcdef");
  ASSERT_OK (ModelRenderRow (M, 0, 2, 3, Cells));
  ASSERT_EQ (Cells[0].Char, (CHAR16)'c');
  ASSERT_EQ (Cells[1].Char, (CHAR16)'d');
  ASSERT_EQ (Cells[2].Char, (CHAR16)'e');
  /* 越过行尾 → 空格填充 */
  ASSERT_OK (ModelRenderRow (M, 0, 4, 4, Cells));
  ASSERT_EQ (Cells[0].Char, (CHAR16)'e');
  ASSERT_EQ (Cells[1].Char, (CHAR16)'f');
  ASSERT_EQ (Cells[2].Char, (CHAR16)' ');
  ASSERT_EQ (Cells[2].Attr, TUI_ATTR_NORMAL);
  ASSERT_EQ (Cells[3].Char, (CHAR16)' ');
  ASSERT_OK (ModelRenderRow (M, 0, 100, 2, Cells));
  ASSERT_EQ (Cells[0].Char, (CHAR16)' ');
  ASSERT_EQ (Cells[1].Char, (CHAR16)' ');
  /* Width 0 合法；越界行 / NULL Out 拒绝 */
  ASSERT_OK (ModelRenderRow (M, 0, 0, 0, Cells));
  ASSERT_ERR (ModelRenderRow (M, 99, 0, 4, Cells));
  ASSERT_ERR (ModelRenderRow (M, 0, 0, 4, NULL));
  /* 中文每码点单格占位；Width 1 恰好 1 格（宽字符不越界） */
  LoadStr (M, "\xE4\xB8\xAD\xE6\x96\x87x");   /* 中文x */
  ASSERT_OK (ModelRenderRow (M, 0, 0, 3, Cells));
  ASSERT_EQ (Cells[0].Char, TUI_NONASCII_CHAR);
  ASSERT_EQ (Cells[0].Attr, TUI_ATTR_NONASCII);
  ASSERT_EQ (Cells[1].Char, TUI_NONASCII_CHAR);
  ASSERT_EQ (Cells[1].Attr, TUI_ATTR_NONASCII);
  ASSERT_EQ (Cells[2].Char, (CHAR16)'x');
  ASSERT_OK (ModelRenderRow (M, 0, 1, 1, Cells));
  ASSERT_EQ (Cells[0].Char, TUI_NONASCII_CHAR);
  ModelFree (M);
}

/* ---- T02-9 fuzz：随机编辑后 Save→Load→Save 幂等 ---- */

static UINT32 g_rng = 0x12345678u;
static UINT32 Rng (void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

TEST (t02_9_fuzz_idempotent) {
  UINTN Round, Op;
  for (Round = 0; Round < 300; Round++) {
    EDITOR_MODEL *M = NewModel (4096, 16);
    UINT8 Base[256];
    UINTN BaseLen = Rng () % 200;
    UINT8 *S1 = NULL, *S2 = NULL;
    UINTN L1 = 0, L2 = 0;
    for (Op = 0; Op < BaseLen; Op++) Base[Op] = (UINT8)(Rng () & 0xFF);
    if (BaseLen >= 3 && (Rng () & 7) == 0) {
      Base[0] = 0xEF; Base[1] = 0xBB; Base[2] = 0xBF;   /* 偶发 BOM */
    }
    ASSERT_OK (ModelLoad (M, Base, BaseLen));
    for (Op = 0; Op < 60; Op++) {
      UINT32 R = Rng () % 10;
      UINT32 Cp;
      switch (R) {
        case 0: case 1: case 2:
          Cp = 0x20 + (Rng () % 0x5F);        /* 可打印 ASCII */
          if ((Rng () & 7) == 0) Cp = 0x4E2D; /* 偶发中文码点 */
          ModelInsert (M, &Cp, 1);
          break;
        case 3: ModelBackspace (M); break;
        case 4: ModelDeleteFwd (M); break;
        case 5: ModelNewline (M); break;
        case 6: ModelMove (M, (INTN)(Rng () % 9) - 4, (INTN)(Rng () % 9) - 4); break;
        case 7: ModelGotoLine (M, Rng () % 20); break;
        case 8: ModelUndo (M); break;
        case 9: {
          UINT32 Needle[2] = { 'a', 'b' };
          MODEL_HIT *H;
          UINTN C;
          ModelFind (M, Needle, 2, (BOOLEAN)(Rng () & 1), &H, &C);
          ModelFreeHits (H);
          break;
        }
      }
    }
    ASSERT_OK (ModelSave (M, &S1, &L1));
    ASSERT_OK (ModelLoad (M, S1, L1));
    ASSERT_OK (ModelSave (M, &S2, &L2));
    ASSERT_EQ (L1, L2);
    if (L1 == L2 && L1) ASSERT_MEM_EQ (S1, S2, L1);
    MemFree (S1);
    MemFree (S2);
    ModelFree (M);
  }
}

/* ---- T02-10 内存配对 / ASan 干净 ---- */

TEST (t02_10_alloc_pairing) {
  UINTN i;
  /* 整套用例在 ASan/UBSan 下运行；本用例高频走分配/释放/栈溢出丢弃路径。
   * macOS clang 无 LSan，泄漏配对以 ASan 报告 + 显式 Free 路径覆盖为准。 */
  for (i = 0; i < 100; i++) {
    EDITOR_MODEL *M = NewModel (1024, 4);   /* 小栈 → 频繁丢弃最旧条目 */
    MODEL_HIT *H;
    UINTN C;
    UINT8 *Out;
    UINTN Len;
    UINT32 Cp = 'x';
    LoadStr (M, "line one\nline two\n");
    ASSERT_OK (ModelInsert (M, &Cp, 1));
    ASSERT_OK (ModelNewline (M));
    ASSERT_OK (ModelBackspace (M));
    ASSERT_OK (ModelUndo (M));
    ASSERT_OK (ModelInsert (M, &Cp, 1));
    ASSERT_OK (ModelInsert (M, &Cp, 1));
    ASSERT_OK (ModelUndo (M));
    ASSERT_OK (ModelFind (M, &Cp, 1, TRUE, &H, &C));
    ModelFreeHits (H);
    ASSERT_OK (ModelSave (M, &Out, &Len));
    ASSERT_OK (ModelLoad (M, Out, Len));
    MemFree (Out);
    ModelFree (M);
  }
  ModelFree (NULL);       /* NULL 安全 */
  ModelFreeHits (NULL);
}

int main (void) {
  printf ("== S02 EditorModelLib host tests (real Utf8Lib) ==\n");
  RUN_TEST (t02_1_roundtrip);
  RUN_TEST (t02_2_boundary_edit);
  RUN_TEST (t02_3_newline_convention);
  RUN_TEST (t02_4_undo_grouping);
  RUN_TEST (t02_5_find);
  RUN_TEST (t02_6_goto_clamp);
  RUN_TEST (t02_7_size_limit);
  RUN_TEST (t02_8_render);
  RUN_TEST (t02_9_fuzz_idempotent);
  RUN_TEST (t02_10_alloc_pairing);
  RUN_TESTS_END ();
}
