/** @file
  编辑器模型（契约，S00 冻结；S02 实现）。纯逻辑：只收发字节缓冲，不做文件 I/O。
  行数组模型：每行 = UTF8_TOKEN 序列 + 原始终止符；保存字节往返保留（design §7.3）。
**/
#ifndef EDITOR_MODEL_H
#define EDITOR_MODEL_H

#include "EfiCompat.h"
#include "Utf8.h"
#include "TuiScreen.h"   /* TUI_CELL */

typedef struct EDITOR_MODEL EDITOR_MODEL;

typedef struct {
  UINTN Line;
  UINTN Col;      /* token 索引 */
} MODEL_HIT;

typedef struct {
  UINTN   LineCount;
  UINTN   CursorLine;
  UINTN   CursorCol;
  BOOLEAN HasBom;
  UINT8   DominantEol;   /* 0=LF 1=CRLF（新行随此约定） */
  BOOLEAN Modified;
  UINT64  ByteSize;      /* 加载时字节数 */
} MODEL_INFO;

EDITOR_MODEL *ModelNew (UINTN MaxBytes, UINTN UndoDepth);
VOID          ModelFree (EDITOR_MODEL *M);
EFI_STATUS    ModelLoad (EDITOR_MODEL *M, CONST UINT8 *Bytes, UINTN Len);  /* 超限 EFI_BAD_BUFFER_SIZE */
EFI_STATUS    ModelSave (EDITOR_MODEL *M, UINT8 **Out, UINTN *Len);
EFI_STATUS    ModelInsert (EDITOR_MODEL *M, CONST UINT32 *Cps, UINTN N);   /* 连续插入合并为一个撤销步 */
EFI_STATUS    ModelBackspace (EDITOR_MODEL *M);
EFI_STATUS    ModelDeleteFwd (EDITOR_MODEL *M);
EFI_STATUS    ModelNewline (EDITOR_MODEL *M);
EFI_STATUS    ModelMove (EDITOR_MODEL *M, INTN Dx, INTN Dy);   /* 光标移动，越界钳制 */
EFI_STATUS    ModelSetCursor (EDITOR_MODEL *M, UINTN Line, UINTN Col);
EFI_STATUS    ModelGotoLine (EDITOR_MODEL *M, UINTN Line);     /* 1-based */
EFI_STATUS    ModelFind (EDITOR_MODEL *M, CONST UINT32 *Needle, UINTN N, BOOLEAN CaseSens,
                         MODEL_HIT **Hits, UINTN *Count);
EFI_STATUS    ModelUndo (EDITOR_MODEL *M);
BOOLEAN       ModelModified (EDITOR_MODEL *M);
EFI_STATUS    ModelInfo (EDITOR_MODEL *M, MODEL_INFO *Out);
/* 渲染投影：ASCII 直显；非 ASCII → TUI_NONASCII_CHAR + TUI_ATTR_NONASCII */
EFI_STATUS    ModelRenderRow (EDITOR_MODEL *M, UINTN Line, UINTN ColStart, UINTN Width,
                              TUI_CELL *Out);
VOID          ModelFreeHits (MODEL_HIT *Hits);
/* 指定位置 token（S13 状态栏 U+XXXX 显示用；S02 提案批准）。越界 EFI_NOT_FOUND */
EFI_STATUS    ModelTokenAt (EDITOR_MODEL *M, UINTN Line, UINTN Col, UTF8_TOKEN *Out);

#endif
