/** @file
  EditorModelLib — 行数组编辑模型（S02 实现；契约 Include/Contract/EditorModel.h）。

  纯逻辑：只收发字节缓冲，不做文件 I/O；只 include Contract 契约头；内存走 MemAlloc/MemFree。
  每行 = UTF8_TOKEN 序列 + 原始终止符（none/LF/CRLF）；保存按 token 重编码 → 字节往返保留。
  BOM 有则保留、不添加；新行终止符随文件主约定（CRLF 数 > LF 数 → CRLF，否则 LF）。
  孤立 CR（无后续 LF）按内容 token 保留，不作行终止符（契约只定义 LF/CRLF 两种）。

  撤销：有界栈（UndoDepth，满则丢弃最旧）；每步只快照受影响的 1–2 行。
  Modified = (Seq != SavedSeq)：加载/保存重置保存点，撤销回保存点 → Modified=FALSE。
  连续插入合并为一步（仅当中间无任何其它操作且光标紧接上次插入末尾）。

  插入经 Utf8Encode→Utf8Decode 落地为 token：编码策略完全委托 Utf8Lib，
  模型内所有 token 均为解码器产物，字节宽度精确、Save→Load→Save 幂等。
  非法码点（代理区/超 U+10FFFF）插入由 Utf8Encode 拒绝 → EFI_INVALID_PARAMETER，
  模型不改动。

  光标：行/列均 0 基（列 = token 索引，可等于行长 = 行尾插入位）；越界一律钳制。
  ModelGotoLine 为 1 基。MODEL_INFO.ByteSize = 加载时字节数（契约语义）。
  MaxBytes 同时约束加载与编辑增长（超限 EFI_BAD_BUFFER_SIZE）；0 = 不限。
  无操作边界（文件头退格/文件尾前删等）返回 EFI_SUCCESS 空操作；空栈撤销 EFI_NOT_FOUND。
  查找：token 级 CP 子串（RAW 不透明字节不参与匹配）；大小写开关仅折叠 ASCII；
  空 needle → 0 命中；允许重叠命中。渲染：1 token = 1 格（宽度恒 1，宽字符不越界）。

  注意：增长统一走 MemAlloc+copy+MemFree，不使用 MemRealloc（S00 原 UEFI 后端
  ReallocatePool 参数序有误，S01 上报后 leader 已在 main 修复为尺寸跟踪头；
  本库保持只依赖 MemAlloc/MemFree 两个原语）。
**/
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/EditorModel.h"

#define EOL_NONE  0
#define EOL_LF    1
#define EOL_CRLF  2

typedef struct {
  UTF8_TOKEN *Toks;
  UINTN       Count;
  UINTN       Cap;
  UINT8       Eol;      /* EOL_NONE / EOL_LF / EOL_CRLF（原始终止符） */
} MODEL_LINE;

typedef struct {
  UINT64     PreSeq;    /* 操作前序号（撤销恢复） */
  UINTN      Index;     /* 行拼接位置 */
  UINTN      OldCount;  /* 快照行数（1 或 2） */
  UINTN      NewCount;  /* 操作后该位置行数（撤销时移除） */
  MODEL_LINE Saved[2];  /* 操作前深快照（token 所有权归本条目） */
  UINTN      PreLine;   /* 操作前光标 */
  UINTN      PreCol;
  UINTN      PostLine;  /* 插入合并判定：插入末尾位置 */
  UINTN      PostCol;
  BOOLEAN    IsInsert;
} UNDO_ENTRY;

struct EDITOR_MODEL {
  MODEL_LINE *Lines;
  UINTN       LineCount;
  UINTN       LineCap;
  UINTN       CursorLine;
  UINTN       CursorCol;
  BOOLEAN     HasBom;
  UINT8       DominantEol;   /* 契约编码：0=LF 1=CRLF */
  UINTN       MaxBytes;      /* 0 = 不限 */
  UINTN       UndoDepth;
  UNDO_ENTRY **Undo;
  UINTN       UndoCount;
  UINT64      Seq;
  UINT64      SavedSeq;
  UINT64      LoadedBytes;
  UINTN       CurrentBytes;  /* 当前序列化字节数（限额用） */
  BOOLEAN     ChainOpen;     /* 连续插入合并链是否开启 */
};

/* ---- 基础助手（不依赖 libc：宿主/UEFI 单一路径） ---- */

STATIC VOID CopyBytes (VOID *Dst, CONST VOID *Src, UINTN N) {
  UINT8 *D = (UINT8 *)Dst;
  CONST UINT8 *S = (CONST UINT8 *)Src;
  UINTN i;
  for (i = 0; i < N; i++) D[i] = S[i];
}

STATIC VOID MoveBytes (VOID *Dst, CONST VOID *Src, UINTN N) {
  UINT8 *D = (UINT8 *)Dst;
  CONST UINT8 *S = (CONST UINT8 *)Src;
  UINTN i;
  if (D == S || N == 0) return;
  if (D < S) {
    for (i = 0; i < N; i++) D[i] = S[i];
  } else {
    for (i = N; i > 0; i--) D[i - 1] = S[i - 1];
  }
}

/* token 序列化字节宽度。模型内 CP token 均为解码器产物（合法码点），宽度精确。 */
STATIC UINTN TokenBytes (CONST UTF8_TOKEN *T) {
  if (T->Type == UTF8_TOKEN_RAW) return 1;
  if (T->Cp < 0x80)    return 1;
  if (T->Cp < 0x800)   return 2;
  if (T->Cp < 0x10000) return 3;
  return 4;
}

STATIC UINTN EolBytes (UINT8 Eol) {
  return Eol == EOL_CRLF ? 2 : (Eol == EOL_LF ? 1 : 0);
}

STATIC VOID LineDestroy (MODEL_LINE *L) {
  MemFree (L->Toks);
  L->Toks = NULL;
  L->Count = 0;
  L->Cap = 0;
}

STATIC EFI_STATUS LineReserve (MODEL_LINE *L, UINTN Need) {
  UINTN NewCap;
  UTF8_TOKEN *NewToks;
  if (Need <= L->Cap) return EFI_SUCCESS;
  NewCap = L->Cap ? L->Cap : 16;
  while (NewCap < Need) NewCap *= 2;
  NewToks = MemAlloc (NewCap * sizeof (UTF8_TOKEN));
  if (!NewToks) return EFI_OUT_OF_RESOURCES;
  if (L->Count) CopyBytes (NewToks, L->Toks, L->Count * sizeof (UTF8_TOKEN));
  MemFree (L->Toks);
  L->Toks = NewToks;
  L->Cap = NewCap;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS LineClone (MODEL_LINE *Dst, CONST MODEL_LINE *Src) {
  Dst->Toks = NULL;
  Dst->Count = 0;
  Dst->Cap = 0;
  Dst->Eol = Src->Eol;
  if (Src->Count) {
    Dst->Toks = MemAlloc (Src->Count * sizeof (UTF8_TOKEN));
    if (!Dst->Toks) return EFI_OUT_OF_RESOURCES;
    CopyBytes (Dst->Toks, Src->Toks, Src->Count * sizeof (UTF8_TOKEN));
    Dst->Count = Src->Count;
    Dst->Cap = Src->Count;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS FillLine (MODEL_LINE *L, CONST UTF8_TOKEN *Toks, UINTN N, UINT8 Eol) {
  L->Toks = NULL;
  L->Count = 0;
  L->Cap = 0;
  L->Eol = Eol;
  if (N) {
    L->Toks = MemAlloc (N * sizeof (UTF8_TOKEN));
    if (!L->Toks) return EFI_OUT_OF_RESOURCES;
    CopyBytes (L->Toks, Toks, N * sizeof (UTF8_TOKEN));
    L->Count = N;
    L->Cap = N;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS LinesReserve (EDITOR_MODEL *M, UINTN Need) {
  UINTN NewCap;
  MODEL_LINE *NewArr;
  if (Need <= M->LineCap) return EFI_SUCCESS;
  NewCap = M->LineCap ? M->LineCap : 16;
  while (NewCap < Need) NewCap *= 2;
  NewArr = MemAlloc (NewCap * sizeof (MODEL_LINE));
  if (!NewArr) return EFI_OUT_OF_RESOURCES;
  if (M->LineCount) CopyBytes (NewArr, M->Lines, M->LineCount * sizeof (MODEL_LINE));
  MemFree (M->Lines);
  M->Lines = NewArr;
  M->LineCap = NewCap;
  return EFI_SUCCESS;
}

/* 移除 Index 处 RemoveCount 行（释放其 token），插入 NewCount 行（结构体浅拷贝，
   token 所有权自 NewLines 转移进模型）。容量不足时先增长（失败则不改动）。 */
STATIC EFI_STATUS LinesSplice (EDITOR_MODEL *M, UINTN Index, UINTN RemoveCount,
                               CONST MODEL_LINE *NewLines, UINTN NewCount) {
  UINTN Need = M->LineCount - RemoveCount + NewCount;
  UINTN i, Tail;
  EFI_STATUS Status = LinesReserve (M, Need);
  if (EFI_ERROR (Status)) return Status;
  for (i = Index; i < Index + RemoveCount; i++) LineDestroy (&M->Lines[i]);
  Tail = M->LineCount - Index - RemoveCount;
  if (Tail && NewCount != RemoveCount) {
    MoveBytes (&M->Lines[Index + NewCount], &M->Lines[Index + RemoveCount],
               Tail * sizeof (MODEL_LINE));
  }
  for (i = 0; i < NewCount; i++) {
    CopyBytes (&M->Lines[Index + i], &NewLines[i], sizeof (MODEL_LINE));
  }
  M->LineCount = Need;
  return EFI_SUCCESS;
}

STATIC UINTN ComputeBytes (EDITOR_MODEL *M) {
  UINTN Total = M->HasBom ? 3 : 0;
  UINTN i, j;
  for (i = 0; i < M->LineCount; i++) {
    MODEL_LINE *L = &M->Lines[i];
    for (j = 0; j < L->Count; j++) Total += TokenBytes (&L->Toks[j]);
    Total += EolBytes (L->Eol);
  }
  return Total;
}

/* ---- 撤销栈 ---- */

STATIC VOID UndoEntryDestroy (UNDO_ENTRY *E) {
  if (!E) return;
  LineDestroy (&E->Saved[0]);
  LineDestroy (&E->Saved[1]);
  MemFree (E);
}

STATIC VOID UndoClear (EDITOR_MODEL *M) {
  UINTN i;
  for (i = 0; i < M->UndoCount; i++) UndoEntryDestroy (M->Undo[i]);
  M->UndoCount = 0;
}

/* 接管 E 所有权。栈满丢弃最旧；UndoDepth==0 → 不存储（Modified 跟踪仍靠 Seq）。 */
STATIC VOID UndoPush (EDITOR_MODEL *M, UNDO_ENTRY *E) {
  if (M->UndoDepth == 0) {
    UndoEntryDestroy (E);
    return;
  }
  if (M->UndoCount == M->UndoDepth) {
    UndoEntryDestroy (M->Undo[0]);
    MoveBytes (&M->Undo[0], &M->Undo[1], (M->UndoCount - 1) * sizeof (UNDO_ENTRY *));
    M->UndoCount--;
  }
  M->Undo[M->UndoCount++] = E;
}

/* 新建撤销条目：快照 1–2 行。失败返回 NULL（调用方未改动模型）。 */
STATIC UNDO_ENTRY *UndoNew (EDITOR_MODEL *M, UINTN Index, UINTN OldCount, UINTN NewCount) {
  UNDO_ENTRY *E = MemAlloc (sizeof (UNDO_ENTRY));
  UINTN i;
  if (!E) return NULL;
  E->PreSeq = M->Seq;
  E->Index = Index;
  E->OldCount = OldCount;
  E->NewCount = NewCount;
  E->PreLine = M->CursorLine;
  E->PreCol = M->CursorCol;
  E->IsInsert = FALSE;
  for (i = 0; i < OldCount; i++) {
    if (EFI_ERROR (LineClone (&E->Saved[i], &M->Lines[Index + i]))) {
      UndoEntryDestroy (E);
      return NULL;
    }
  }
  return E;
}

/* ---- 生命周期 ---- */

EDITOR_MODEL *ModelNew (UINTN MaxBytes, UINTN UndoDepth) {
  EDITOR_MODEL *M = MemAlloc (sizeof (EDITOR_MODEL));
  if (!M) return NULL;
  M->MaxBytes = MaxBytes;
  M->UndoDepth = UndoDepth;
  if (UndoDepth) {
    M->Undo = MemAlloc (UndoDepth * sizeof (UNDO_ENTRY *));
    if (!M->Undo) {
      MemFree (M);
      return NULL;
    }
  }
  M->Lines = MemAlloc (16 * sizeof (MODEL_LINE));
  if (!M->Lines) {
    MemFree (M->Undo);
    MemFree (M);
    return NULL;
  }
  M->LineCap = 16;
  M->LineCount = 1;      /* 空文件 = 1 个空行（无终止符） */
  M->DominantEol = 0;    /* LF */
  return M;
}

VOID ModelFree (EDITOR_MODEL *M) {
  UINTN i;
  if (!M) return;
  for (i = 0; i < M->LineCount; i++) LineDestroy (&M->Lines[i]);
  MemFree (M->Lines);
  UndoClear (M);
  MemFree (M->Undo);
  MemFree (M);
}

/* ---- 加载 / 保存 ---- */

EFI_STATUS ModelLoad (EDITOR_MODEL *M, CONST UINT8 *Bytes, UINTN Len) {
  EFI_STATUS Status;
  UTF8_TOKEN *Toks = NULL;
  UINTN Count = 0, Start, i, Li, Ts, NewLineCount;
  UINTN NlCount = 0, CrlfCount = 0;
  MODEL_LINE *NewLines = NULL;
  BOOLEAN Bom;

  if (!M || (!Bytes && Len)) return EFI_INVALID_PARAMETER;
  if (M->MaxBytes && Len > M->MaxBytes) return EFI_BAD_BUFFER_SIZE;

  Bom = Utf8HasBom (Bytes, Len);
  Start = Bom ? 3 : 0;
  Status = Utf8Decode (Len > Start ? Bytes + Start : NULL, Len - Start, &Toks, &Count);
  if (EFI_ERROR (Status)) return Status;

  /* 第一遍：数行与终止符（LF 分行；紧邻其前的 CR 归入 CRLF 终止符） */
  NewLineCount = 1;
  for (i = 0; i < Count; i++) {
    if (Toks[i].Type == UTF8_TOKEN_CP && Toks[i].Cp == 0x0A) {
      NewLineCount++;
      if (i > 0 && Toks[i - 1].Type == UTF8_TOKEN_CP && Toks[i - 1].Cp == 0x0D) CrlfCount++;
      else NlCount++;
    }
  }

  NewLines = MemAlloc (NewLineCount * sizeof (MODEL_LINE));
  if (!NewLines) {
    Utf8FreeTokens (Toks);
    return EFI_OUT_OF_RESOURCES;
  }

  /* 第二遍：填行（失败则整体放弃，模型保持原状） */
  Li = 0;
  Ts = 0;
  for (i = 0; i < Count; i++) {
    if (Toks[i].Type == UTF8_TOKEN_CP && Toks[i].Cp == 0x0A) {
      UINTN End = i;
      UINT8 Eol = EOL_LF;
      if (i > Ts && Toks[i - 1].Type == UTF8_TOKEN_CP && Toks[i - 1].Cp == 0x0D) {
        End = i - 1;
        Eol = EOL_CRLF;
      }
      Status = FillLine (&NewLines[Li], Toks ? Toks + Ts : NULL, End - Ts, Eol);
      if (EFI_ERROR (Status)) goto Fail;
      Li++;
      Ts = i + 1;
    }
  }
  Status = FillLine (&NewLines[Li], Toks ? Toks + Ts : NULL, Count - Ts, EOL_NONE);
  if (EFI_ERROR (Status)) goto Fail;

  /* 原子替换 */
  for (i = 0; i < M->LineCount; i++) LineDestroy (&M->Lines[i]);
  MemFree (M->Lines);
  UndoClear (M);
  M->Lines = NewLines;
  M->LineCount = NewLineCount;
  M->LineCap = NewLineCount;
  M->HasBom = Bom;
  M->DominantEol = (UINT8)(CrlfCount > NlCount ? 1 : 0);
  M->CursorLine = 0;
  M->CursorCol = 0;
  M->Seq = 0;
  M->SavedSeq = 0;
  M->LoadedBytes = Len;
  M->ChainOpen = FALSE;
  M->CurrentBytes = ComputeBytes (M);
  Utf8FreeTokens (Toks);
  return EFI_SUCCESS;

Fail:
  for (i = 0; i < Li; i++) LineDestroy (&NewLines[i]);
  MemFree (NewLines);
  Utf8FreeTokens (Toks);
  return Status;
}

EFI_STATUS ModelSave (EDITOR_MODEL *M, UINT8 **Out, UINTN *Len) {
  UINTN Total, i, Pos = 0;
  UINT8 *Buf;
  EFI_STATUS Status;

  if (!M || !Out || !Len) return EFI_INVALID_PARAMETER;
  Total = ComputeBytes (M);
  Buf = MemAlloc (Total ? Total : 1);
  if (!Buf) return EFI_OUT_OF_RESOURCES;

  if (M->HasBom) {
    Buf[0] = 0xEF;
    Buf[1] = 0xBB;
    Buf[2] = 0xBF;
    Pos = 3;
  }
  for (i = 0; i < M->LineCount; i++) {
    MODEL_LINE *L = &M->Lines[i];
    if (L->Count) {
      UINT8 *Enc = NULL;
      UINTN EncLen = 0;
      Status = Utf8Encode (L->Toks, L->Count, &Enc, &EncLen);
      if (EFI_ERROR (Status)) {
        MemFree (Buf);
        return Status;
      }
      CopyBytes (Buf + Pos, Enc, EncLen);
      Pos += EncLen;
      MemFree (Enc);
    }
    if (L->Eol == EOL_CRLF) {
      Buf[Pos++] = 0x0D;
      Buf[Pos++] = 0x0A;
    } else if (L->Eol == EOL_LF) {
      Buf[Pos++] = 0x0A;
    }
  }
  *Out = Buf;
  *Len = Total;
  M->SavedSeq = M->Seq;   /* 保存点前移 → Modified=FALSE */
  M->ChainOpen = FALSE;
  return EFI_SUCCESS;
}

/* ---- 编辑 ---- */

EFI_STATUS ModelInsert (EDITOR_MODEL *M, CONST UINT32 *Cps, UINTN N) {
  EFI_STATUS Status;
  UTF8_TOKEN *CpToks = NULL;
  UTF8_TOKEN *NewToks = NULL;
  UINT8 *Bytes = NULL;
  UINTN ByteLen = 0, NewCount = 0, i;
  MODEL_LINE *L;
  UNDO_ENTRY *E = NULL;
  UNDO_ENTRY *Top;
  BOOLEAN Merge;

  if (!M || (!Cps && N)) return EFI_INVALID_PARAMETER;
  if (N == 0) return EFI_SUCCESS;

  /* CP → 字节 → token：编码策略委托 Utf8Lib，字节宽度精确 */
  CpToks = MemAlloc (N * sizeof (UTF8_TOKEN));
  if (!CpToks) return EFI_OUT_OF_RESOURCES;
  for (i = 0; i < N; i++) {
    CpToks[i].Cp = Cps[i];
    CpToks[i].Raw = 0;
    CpToks[i].Type = UTF8_TOKEN_CP;
  }
  Status = Utf8Encode (CpToks, N, &Bytes, &ByteLen);
  MemFree (CpToks);
  if (EFI_ERROR (Status)) return Status;
  Status = Utf8Decode (Bytes, ByteLen, &NewToks, &NewCount);
  MemFree (Bytes);
  if (EFI_ERROR (Status)) return Status;

  if (M->MaxBytes && M->CurrentBytes + ByteLen > M->MaxBytes) {
    Utf8FreeTokens (NewToks);
    return EFI_BAD_BUFFER_SIZE;
  }

  L = &M->Lines[M->CursorLine];
  Status = LineReserve (L, L->Count + NewCount);
  if (EFI_ERROR (Status)) {
    Utf8FreeTokens (NewToks);
    return Status;
  }

  Merge = FALSE;
  if (M->ChainOpen && M->UndoCount > 0) {
    Top = M->Undo[M->UndoCount - 1];
    Merge = (BOOLEAN)(Top->IsInsert &&
                      Top->PostLine == M->CursorLine &&
                      Top->PostCol == M->CursorCol);
  }
  if (!Merge) {
    E = UndoNew (M, M->CursorLine, 1, 1);
    if (!E) {
      Utf8FreeTokens (NewToks);
      return EFI_OUT_OF_RESOURCES;
    }
    E->IsInsert = TRUE;
    E->PostLine = M->CursorLine;
    E->PostCol = M->CursorCol + NewCount;   /* 须在 UndoPush 前设置（Depth=0 时 E 即被释放） */
    UndoPush (M, E);
    M->Seq++;
  }

  /* 容量已预留，以下不可失败 */
  MoveBytes (&L->Toks[M->CursorCol + NewCount], &L->Toks[M->CursorCol],
             (L->Count - M->CursorCol) * sizeof (UTF8_TOKEN));
  if (NewCount) {
    CopyBytes (&L->Toks[M->CursorCol], NewToks, NewCount * sizeof (UTF8_TOKEN));
  }
  L->Count += NewCount;
  M->CursorCol += NewCount;
  M->CurrentBytes += ByteLen;
  M->ChainOpen = TRUE;
  if (Merge) {
    M->Undo[M->UndoCount - 1]->PostCol = M->CursorCol;
  }
  Utf8FreeTokens (NewToks);
  return EFI_SUCCESS;
}

EFI_STATUS ModelBackspace (EDITOR_MODEL *M) {
  MODEL_LINE *L, *Prev;
  UNDO_ENTRY *E;
  UINTN JoinCol;
  EFI_STATUS Status;

  if (!M) return EFI_INVALID_PARAMETER;
  L = &M->Lines[M->CursorLine];

  if (M->CursorCol > 0) {
    /* 行内删前一 token */
    E = UndoNew (M, M->CursorLine, 1, 1);
    if (!E) return EFI_OUT_OF_RESOURCES;
    UndoPush (M, E);
    M->Seq++;
    M->CurrentBytes -= TokenBytes (&L->Toks[M->CursorCol - 1]);
    MoveBytes (&L->Toks[M->CursorCol - 1], &L->Toks[M->CursorCol],
               (L->Count - M->CursorCol) * sizeof (UTF8_TOKEN));
    L->Count--;
    M->CursorCol--;
    M->ChainOpen = FALSE;
    return EFI_SUCCESS;
  }

  if (M->CursorLine == 0) return EFI_SUCCESS;   /* 文件头：空操作 */

  /* 行首：与上一行合并（去掉上一行终止符，保留本行终止符） */
  Prev = &M->Lines[M->CursorLine - 1];
  JoinCol = Prev->Count;
  Status = LineReserve (Prev, Prev->Count + L->Count);
  if (EFI_ERROR (Status)) return Status;
  E = UndoNew (M, M->CursorLine - 1, 2, 1);
  if (!E) return EFI_OUT_OF_RESOURCES;
  UndoPush (M, E);
  M->Seq++;
  M->CurrentBytes -= EolBytes (Prev->Eol);
  if (L->Count) {
    CopyBytes (Prev->Toks + JoinCol, L->Toks, L->Count * sizeof (UTF8_TOKEN));
  }
  Prev->Count += L->Count;
  Prev->Eol = L->Eol;
  LinesSplice (M, M->CursorLine, 1, NULL, 0);   /* 行数减少，不会失败 */
  M->CursorLine--;
  M->CursorCol = JoinCol;
  M->ChainOpen = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS ModelDeleteFwd (EDITOR_MODEL *M) {
  MODEL_LINE *L, *Next;
  UNDO_ENTRY *E;
  EFI_STATUS Status;

  if (!M) return EFI_INVALID_PARAMETER;
  L = &M->Lines[M->CursorLine];

  if (M->CursorCol < L->Count) {
    /* 行内删光标处 token */
    E = UndoNew (M, M->CursorLine, 1, 1);
    if (!E) return EFI_OUT_OF_RESOURCES;
    UndoPush (M, E);
    M->Seq++;
    M->CurrentBytes -= TokenBytes (&L->Toks[M->CursorCol]);
    MoveBytes (&L->Toks[M->CursorCol], &L->Toks[M->CursorCol + 1],
               (L->Count - M->CursorCol - 1) * sizeof (UTF8_TOKEN));
    L->Count--;
    M->ChainOpen = FALSE;
    return EFI_SUCCESS;
  }

  if (M->CursorLine + 1 >= M->LineCount) return EFI_SUCCESS;   /* 文件尾：空操作 */

  /* 行尾：与下一行合并（去掉本行终止符，保留下一行终止符） */
  Next = &M->Lines[M->CursorLine + 1];
  Status = LineReserve (L, L->Count + Next->Count);
  if (EFI_ERROR (Status)) return Status;
  E = UndoNew (M, M->CursorLine, 2, 1);
  if (!E) return EFI_OUT_OF_RESOURCES;
  UndoPush (M, E);
  M->Seq++;
  M->CurrentBytes -= EolBytes (L->Eol);
  if (Next->Count) {
    CopyBytes (L->Toks + L->Count, Next->Toks, Next->Count * sizeof (UTF8_TOKEN));
  }
  L->Count += Next->Count;
  L->Eol = Next->Eol;
  LinesSplice (M, M->CursorLine + 1, 1, NULL, 0);
  M->ChainOpen = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS ModelNewline (EDITOR_MODEL *M) {
  MODEL_LINE *L;
  MODEL_LINE Right;
  UNDO_ENTRY *E;
  UINT8 NewEol;
  UINTN Col;
  EFI_STATUS Status;

  if (!M) return EFI_INVALID_PARAMETER;
  NewEol = M->DominantEol ? EOL_CRLF : EOL_LF;
  if (M->MaxBytes && M->CurrentBytes + EolBytes (NewEol) > M->MaxBytes) {
    return EFI_BAD_BUFFER_SIZE;
  }
  L = &M->Lines[M->CursorLine];
  Col = M->CursorCol;

  /* 右半行（继承原终止符） */
  Right.Toks = NULL;
  Right.Count = 0;
  Right.Cap = 0;
  Right.Eol = L->Eol;
  if (L->Count > Col) {
    Right.Count = L->Count - Col;
    Right.Toks = MemAlloc (Right.Count * sizeof (UTF8_TOKEN));
    if (!Right.Toks) return EFI_OUT_OF_RESOURCES;
    CopyBytes (Right.Toks, L->Toks + Col, Right.Count * sizeof (UTF8_TOKEN));
    Right.Cap = Right.Count;
  }

  Status = LinesReserve (M, M->LineCount + 1);
  if (EFI_ERROR (Status)) {
    LineDestroy (&Right);
    return Status;
  }
  L = &M->Lines[M->CursorLine];   /* LinesReserve 可能重分配行数组，重新取指针 */
  E = UndoNew (M, M->CursorLine, 1, 2);   /* 快照原整行（须在截断前） */
  if (!E) {
    LineDestroy (&Right);
    return EFI_OUT_OF_RESOURCES;
  }
  UndoPush (M, E);
  M->Seq++;

  /* 容量已预留，以下不可失败 */
  L->Count = Col;
  L->Eol = NewEol;              /* 左半行随主约定 */
  LinesSplice (M, M->CursorLine + 1, 0, &Right, 1);
  M->CurrentBytes += EolBytes (NewEol);
  M->CursorLine++;
  M->CursorCol = 0;
  M->ChainOpen = FALSE;
  return EFI_SUCCESS;
}

/* ---- 光标 ---- */

EFI_STATUS ModelMove (EDITOR_MODEL *M, INTN Dx, INTN Dy) {
  INTN NewLine, NewCol, Len;
  if (!M) return EFI_INVALID_PARAMETER;
  NewLine = (INTN)M->CursorLine + Dy;
  if (NewLine < 0) NewLine = 0;
  if (NewLine >= (INTN)M->LineCount) NewLine = (INTN)M->LineCount - 1;
  M->CursorLine = (UINTN)NewLine;
  Len = (INTN)M->Lines[NewLine].Count;
  NewCol = (INTN)M->CursorCol;
  if (NewCol > Len) NewCol = Len;   /* 垂直移动：列钳制到新行长度 */
  NewCol += Dx;
  if (NewCol < 0) NewCol = 0;
  if (NewCol > Len) NewCol = Len;
  M->CursorCol = (UINTN)NewCol;
  M->ChainOpen = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS ModelSetCursor (EDITOR_MODEL *M, UINTN Line, UINTN Col) {
  if (!M) return EFI_INVALID_PARAMETER;
  if (Line >= M->LineCount) Line = M->LineCount - 1;
  if (Col > M->Lines[Line].Count) Col = M->Lines[Line].Count;
  M->CursorLine = Line;
  M->CursorCol = Col;
  M->ChainOpen = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS ModelGotoLine (EDITOR_MODEL *M, UINTN Line) {
  if (!M) return EFI_INVALID_PARAMETER;
  if (Line == 0) Line = 1;                 /* 1 基，越界钳制 */
  if (Line > M->LineCount) Line = M->LineCount;
  return ModelSetCursor (M, Line - 1, 0);
}

/* ---- 查找 ---- */

STATIC BOOLEAN MatchAt (CONST MODEL_LINE *L, UINTN Pos, CONST UINT32 *Needle,
                        UINTN N, BOOLEAN CaseSens) {
  UINTN j;
  for (j = 0; j < N; j++) {
    CONST UTF8_TOKEN *T = &L->Toks[Pos + j];
    UINT32 A, B;
    if (T->Type != UTF8_TOKEN_CP) return FALSE;   /* RAW 不透明，不参与匹配 */
    A = T->Cp;
    B = Needle[j];
    if (!CaseSens) {
      if (A >= 'A' && A <= 'Z') A += 32;   /* 仅折叠 ASCII */
      if (B >= 'A' && B <= 'Z') B += 32;
    }
    if (A != B) return FALSE;
  }
  return TRUE;
}

EFI_STATUS ModelFind (EDITOR_MODEL *M, CONST UINT32 *Needle, UINTN N, BOOLEAN CaseSens,
                      MODEL_HIT **Hits, UINTN *Count) {
  UINTN i, j, k, Total = 0;
  MODEL_HIT *Arr;

  if (!M || !Hits || !Count || (!Needle && N)) return EFI_INVALID_PARAMETER;
  *Hits = NULL;
  *Count = 0;
  if (N == 0) return EFI_SUCCESS;   /* 空 needle → 0 命中 */

  for (i = 0; i < M->LineCount; i++) {
    MODEL_LINE *L = &M->Lines[i];
    if (L->Count < N) continue;
    for (j = 0; j + N <= L->Count; j++) {
      if (MatchAt (L, j, Needle, N, CaseSens)) Total++;
    }
  }
  if (Total == 0) return EFI_SUCCESS;

  Arr = MemAlloc (Total * sizeof (MODEL_HIT));
  if (!Arr) return EFI_OUT_OF_RESOURCES;
  k = 0;
  for (i = 0; i < M->LineCount; i++) {
    MODEL_LINE *L = &M->Lines[i];
    if (L->Count < N) continue;
    for (j = 0; j + N <= L->Count; j++) {
      if (MatchAt (L, j, Needle, N, CaseSens)) {
        Arr[k].Line = i;
        Arr[k].Col = j;
        k++;
      }
    }
  }
  *Hits = Arr;
  *Count = Total;
  return EFI_SUCCESS;
}

VOID ModelFreeHits (MODEL_HIT *Hits) {
  MemFree (Hits);
}

/* ---- 撤销 / 状态 ---- */

EFI_STATUS ModelUndo (EDITOR_MODEL *M) {
  UNDO_ENTRY *E;
  EFI_STATUS Status;
  if (!M) return EFI_INVALID_PARAMETER;
  if (M->UndoCount == 0) return EFI_NOT_FOUND;
  Status = LinesReserve (M, M->LineCount + 2);   /* 最坏 1→2 行；失败则不弹栈 */
  if (EFI_ERROR (Status)) return Status;
  E = M->Undo[--M->UndoCount];
  LinesSplice (M, E->Index, E->NewCount, E->Saved, E->OldCount);  /* token 所有权转移 */
  M->CursorLine = E->PreLine;
  M->CursorCol = E->PreCol;
  M->Seq = E->PreSeq;
  M->CurrentBytes = ComputeBytes (M);
  M->ChainOpen = FALSE;
  MemFree (E);
  return EFI_SUCCESS;
}

BOOLEAN ModelModified (EDITOR_MODEL *M) {
  return (BOOLEAN)(M && M->Seq != M->SavedSeq);
}

EFI_STATUS ModelInfo (EDITOR_MODEL *M, MODEL_INFO *Out) {
  if (!M || !Out) return EFI_INVALID_PARAMETER;
  Out->LineCount = M->LineCount;
  Out->CursorLine = M->CursorLine;
  Out->CursorCol = M->CursorCol;
  Out->HasBom = M->HasBom;
  Out->DominantEol = M->DominantEol;
  Out->Modified = ModelModified (M);
  Out->ByteSize = M->LoadedBytes;
  return EFI_SUCCESS;
}

/* ---- 渲染投影 ---- */

EFI_STATUS ModelRenderRow (EDITOR_MODEL *M, UINTN Line, UINTN ColStart, UINTN Width,
                           TUI_CELL *Out) {
  MODEL_LINE *L;
  UINTN c;
  if (!M || !Out) return EFI_INVALID_PARAMETER;
  if (Line >= M->LineCount) return EFI_INVALID_PARAMETER;
  if (Width == 0) return EFI_SUCCESS;
  if (ColStart + Width < ColStart) return EFI_INVALID_PARAMETER;   /* 溢出防护 */
  L = &M->Lines[Line];
  for (c = 0; c < Width; c++) {
    UINTN Idx = ColStart + c;
    if (Idx >= L->Count) {
      Out[c].Char = (CHAR16)' ';
      Out[c].Attr = TUI_ATTR_NORMAL;
    } else {
      CONST UTF8_TOKEN *T = &L->Toks[Idx];
      if (T->Type == UTF8_TOKEN_CP && T->Cp < 0x80) {
        Out[c].Char = (CHAR16)T->Cp;
        Out[c].Attr = TUI_ATTR_NORMAL;
      } else {
        /* 非 ASCII CP 与 RAW 字节：单格占位（宽度恒 1，宽字符不越界） */
        Out[c].Char = TUI_NONASCII_CHAR;
        Out[c].Attr = TUI_ATTR_NONASCII;
      }
    }
  }
  return EFI_SUCCESS;
}
