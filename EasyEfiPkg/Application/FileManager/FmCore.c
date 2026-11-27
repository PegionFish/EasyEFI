/** @file
  FileManager core（S12）：纯状态机，宿主可测（S11 同构）。
  依赖面：TuiScreen/TuiInput/FsOps/ExecOps/TuiTheme/EfiMem（契约），
  不触 gBS/gST/Shell 协议/JsonLib；渲染自绘（业务语义，见 FmCore.h）。
  范围冻结 design §7.2：单文件级操作，无移动/递归目录复制删除/内容搜索/鼠标。
**/
#include "FmCore.h"

/* ---------------- 布局常量（80x25 基线，小屏钳制） ---------------- */
#define FM_LIST_W      44   /* 目录视图左栏宽（名称22 + 大小10 + 修改时间10） */
#define FM_NAME_W      21
#define FM_HEX_BPR     4    /* hex 预览每行字节数（适配右栏 ≥27 列） */
#define FM_COPY_CHUNK  4096
#define FM_MAX_DEPTH   32   /* 递归搜索深度上限 */

struct FM_CORE {
  CONST FM_INIT   *Init;               /* glue 持有并原地更新（Rescan），core 只读借用 */
  FM_STATE        State;
  BOOLEAN         Exit;
  UINTN           Rows, Cols, Visible;

  UINTN           VolIdx;
  CHAR16          (*VolCwd)[FM_MAX_PATH];  /* 每卷各自 cwd（复制目标=该卷当前目录） */
  UINTN           VolCwdCount;
  FS_ENTRY        *Entries;                /* 当前目录枚举（callee 分配，core 释放） */
  UINTN           Count;
  UINTN           *View;                   /* 过滤视图 → Entries 下标 */
  UINTN           ViewCount;
  UINTN           Sel, Top;            /* DIR 视图选择（卷选择器用独立 VSel/VTop，互不干扰） */
  UINTN           VSel, VTop;          /* VOLUMES 视图选择 */
  CHAR16          Filter[FM_MAX_FILTER];
  BOOLEAN         FilterActive;        /* '/' 显式进入或首字符启动；v/s/r 命令仅过滤未激活时生效 */

  UINT8           *PreviewBuf;
  UINTN           PreviewCap;          /* 生效上限（Init 值，0 → 16384 兜底） */
  UINTN           PreviewLen;
  BOOLEAN         PreviewBinary;
  BOOLEAN         PreviewHex;
  BOOLEAN         PreviewValid;

  CHAR16          SearchRoot[FM_MAX_PATH];
  FS_ENTRY        *Results;                /* Name=相对 SearchRoot 路径 */
  UINTN           ResultCount, ResultSel, ResultTop;

  FM_CONFIRM_KIND ConfirmKind;
  CHAR16          ConfirmTitle[64];
  CHAR16          ConfirmBody[768];

  UINTN           SrcVol, DstVol;
  CHAR16          SrcPath[FM_MAX_PATH];
  CHAR16          DstPath[FM_MAX_PATH];
  BOOLEAN         CopyMode;

  FM_INPUT_KIND   InputKind;
  CHAR16          InputBuf[FM_MAX_NAME];
  UINTN           InputLen;
  BOOLEAN         NewIsDir;

  CHAR16          Msg[256];
};

/* ---------------- 字符串/数值助手（双环境；禁 wcs*） ---------------- */

STATIC VOID str_copy (CHAR16 *Dst, UINTN Cap, CONST CHAR16 *Src) {
  UINTN i;
  for (i = 0; Src[i] && i + 1 < Cap; i++) Dst[i] = Src[i];
  Dst[i] = 0;
}

STATIC VOID str_cat (CHAR16 *Dst, UINTN Cap, CONST CHAR16 *Src) {
  UINTN n = EfiStrLen (Dst), i;
  for (i = 0; Src[i] && n + i + 1 < Cap; i++) Dst[n + i] = Src[i];
  Dst[n + i] = 0;
}

STATIC CHAR16 lower (CHAR16 C) { return (C >= L'A' && C <= L'Z') ? (CHAR16)(C + 32) : C; }

/* 大小写不敏感子串（ASCII 折叠；非 ASCII 原样比较） */
STATIC BOOLEAN str_contains (CONST CHAR16 *Hay, CONST CHAR16 *Needle) {
  UINTN i, j;
  if (!Needle[0]) return TRUE;
  for (i = 0; Hay[i]; i++) {
    for (j = 0; Needle[j]; j++) {
      if (!Hay[i + j] || lower (Hay[i + j]) != lower (Needle[j])) break;
    }
    if (!Needle[j]) return TRUE;
  }
  return FALSE;
}

STATIC BOOLEAN str_ieq (CONST CHAR16 *A, CONST CHAR16 *B) {
  UINTN i;
  for (i = 0; A[i] || B[i]; i++) if (lower (A[i]) != lower (B[i])) return FALSE;
  return TRUE;
}

/* 路径拼接：join("" , "x") = "x"；join("a","b") = "a/b"（卷相对，无首尾 '/'） */
STATIC VOID path_join (CHAR16 *Out, UINTN Cap, CONST CHAR16 *Dir, CONST CHAR16 *Name) {
  Out[0] = 0;
  if (Dir[0]) { str_copy (Out, Cap, Dir); str_cat (Out, Cap, U16("/")); }
  str_cat (Out, Cap, Name);
}

/* 父目录：dirname("a/b")="a"；dirname("x")="" */
STATIC VOID path_parent (CHAR16 *Out, UINTN Cap, CONST CHAR16 *Path) {
  UINTN i, last = 0;
  for (i = 0; Path[i]; i++) if (Path[i] == L'/') last = i;
  if (!last) { Out[0] = 0; return; }
  for (i = 0; i < last && i + 1 < Cap; i++) Out[i] = Path[i];
  Out[i] = 0;
}

STATIC CONST CHAR16 *path_base (CONST CHAR16 *Path) {
  CONST CHAR16 *p = Path, *b = Path;
  for (; *p; p++) if (*p == L'/') b = p + 1;
  return b;
}

/* 扩展名判定（大小写不敏感）：.efi / .nsh */
STATIC BOOLEAN ext_is (CONST CHAR16 *Name, CONST CHAR16 *Ext) {
  UINTN n = EfiStrLen (Name), e = EfiStrLen (Ext), i;
  if (n < e) return FALSE;
  for (i = 0; i < e; i++) if (lower (Name[n - e + i]) != lower (Ext[i])) return FALSE;
  return TRUE;
}

STATIC VOID u64_str (CHAR16 *Out, UINTN Cap, UINT64 V) {
  CHAR8 tmp[32];
  UINTN n = 0, i;
  do { tmp[n++] = (CHAR8)('0' + (V % 10)); V /= 10; } while (V && n < 30);
  for (i = 0; i < n && i + 1 < Cap; i++) Out[i] = (CHAR16)tmp[n - 1 - i];
  Out[i] = 0;
}

/* 容量人类可读（卷列表） */
STATIC VOID size_human (CHAR16 *Out, UINTN Cap, UINT64 V) {
  CONST CHAR16 *unit = U16("B");
  UINT64 whole = V;
  if (V >= 1024ULL * 1024 * 1024) { whole = V / (1024ULL * 1024 * 1024); unit = U16("GiB"); }
  else if (V >= 1024 * 1024)      { whole = V / (1024 * 1024);      unit = U16("MiB"); }
  else if (V >= 1024)             { whole = V / 1024;               unit = U16("KiB"); }
  u64_str (Out, Cap, whole);
  str_cat (Out, Cap, unit);
}

/* 追加定宽十进制（前导零） */
STATIC VOID cat_pad (CHAR16 *Out, UINTN Cap, UINT64 V, UINTN Width) {
  CHAR16 num[32], pad[32];
  UINTN n, i;
  u64_str (num, 32, V);
  n = EfiStrLen (num);
  if (n >= Width) { str_cat (Out, Cap, num); return; }
  for (i = 0; i + n < Width && i < 31; i++) pad[i] = L'0';
  pad[i] = 0;
  str_cat (Out, Cap, pad);
  str_cat (Out, Cap, num);
}

/* Unix 秒 → YYYY-MM-DD（civil-from-days，Howard Hinnant 算法；0 → "----------"） */
STATIC VOID mtime_str (CHAR16 *Out, UINTN Cap, UINT64 Sec) {
  INT64 z, era, doe, yoe, y, doy, mp, d, m;
  if (!Sec) { str_copy (Out, Cap, U16("----------")); return; }
  z = (INT64)(Sec / 86400) + 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = z - era * 146097;
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = yoe + era * 400;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  if (m <= 2) y++;
  Out[0] = 0;
  cat_pad (Out, Cap, (UINT64)y, 4);
  str_cat (Out, Cap, U16("-"));
  cat_pad (Out, Cap, (UINT64)m, 2);
  str_cat (Out, Cap, U16("-"));
  cat_pad (Out, Cap, (UINT64)d, 2);
}

/* ---------------- 内部状态助手 ---------------- */

STATIC FS_OPS *cur_fs (FM_CORE *C) {
  return (C->VolIdx < C->Init->VolCount) ? C->Init->Vols[C->VolIdx].Fs : NULL;
}

STATIC VOID full_path (FM_CORE *C, UINTN Vol, CONST CHAR16 *Rel, CHAR16 *Out, UINTN Cap) {
  Out[0] = 0;
  if (Vol < C->Init->VolCount && C->Init->FsNames) str_copy (Out, Cap, C->Init->FsNames[Vol]);
  str_cat (Out, Cap, U16("/"));
  str_cat (Out, Cap, Rel);
}

STATIC VOID set_message (FM_CORE *C, CONST CHAR16 *Text) {
  str_copy (C->Msg, sizeof (C->Msg) / sizeof (CHAR16), Text);
  C->State = FM_STATE_MESSAGE;
}

/* 过滤视图重建；Sel/Top 钳制 */
STATIC VOID rebuild_view (FM_CORE *C) {
  UINTN i;
  MemFree (C->View);
  C->View = MemAlloc ((C->Count ? C->Count : 1) * sizeof (UINTN));
  C->ViewCount = 0;
  for (i = 0; i < C->Count; i++) {
    if (str_contains (C->Entries[i].Name, C->Filter)) C->View[C->ViewCount++] = i;
  }
  if (C->Sel >= C->ViewCount) C->Sel = C->ViewCount ? C->ViewCount - 1 : 0;
  if (C->Top >= C->ViewCount) C->Top = 0;
  if (C->Sel < C->Top) C->Top = C->Sel;
  if (C->Visible && C->Sel >= C->Top + C->Visible) C->Top = C->Sel - C->Visible + 1;
}

/* bounded 预览：读 ≤PreviewMaxBytes；NUL 判二进制（design §7.2 固件识别） */
STATIC VOID refresh_preview (FM_CORE *C) {
  FS_OPS *Fs = cur_fs (C);
  FS_HANDLE *H = NULL;
  CHAR16 path[FM_MAX_PATH];
  UINTN total = 0;
  UINTN i;
  C->PreviewValid = FALSE;
  C->PreviewLen = 0;
  C->PreviewBinary = FALSE;
  if (!Fs || !C->ViewCount) return;
  {
    FS_ENTRY *e = &C->Entries[C->View[C->Sel]];
    if (e->IsDir) return;
    path_join (path, FM_MAX_PATH, C->VolCwd[C->VolIdx], e->Name);
  }
  if (EFI_ERROR (Fs->Open (Fs, path, FS_MODE_READ, &H))) return;
  while (total < C->PreviewCap) {
    UINTN n = C->PreviewCap - total;
    if (EFI_ERROR (Fs->Read (Fs, H, C->PreviewBuf + total, &n)) || n == 0) break;
    total += n;
  }
  Fs->Close (Fs, H);
  for (i = 0; i < total; i++) if (C->PreviewBuf[i] == 0) { C->PreviewBinary = TRUE; break; }
  C->PreviewLen = total;
  C->PreviewValid = TRUE;
}

STATIC VOID reload_dir (FM_CORE *C) {
  FS_OPS *Fs = cur_fs (C);
  MemFree (C->Entries);
  C->Entries = NULL;
  C->Count = 0;
  if (Fs) {
    if (EFI_ERROR (Fs->Enum (Fs, C->VolCwd[C->VolIdx], &C->Entries, &C->Count))) {
      C->Entries = NULL;
      C->Count = 0;
    }
  }
  rebuild_view (C);
  refresh_preview (C);
}

STATIC VOID goto_dir (FM_CORE *C, CONST CHAR16 *Rel) {
  str_copy (C->VolCwd[C->VolIdx], FM_MAX_PATH, Rel);
  C->Sel = 0;
  C->Top = 0;
  C->Filter[0] = 0;
  C->FilterActive = FALSE;
  reload_dir (C);
}

STATIC VOID ascend (FM_CORE *C) {
  CHAR16 parent[FM_MAX_PATH];
  C->Filter[0] = 0;
  C->FilterActive = FALSE;
  if (!C->VolCwd[C->VolIdx][0]) {
    C->State = FM_STATE_VOLUMES;
    C->VSel = C->VolIdx;
    C->VTop = 0;
    return;
  }
  path_parent (parent, FM_MAX_PATH, C->VolCwd[C->VolIdx]);
  goto_dir (C, parent);
}

STATIC VOID move_sel (FM_CORE *C, INTN Delta) {
  INTN n = (INTN)C->Sel + Delta;
  if (n < 0) n = 0;
  if (C->ViewCount && (UINTN)n >= C->ViewCount) n = (INTN)C->ViewCount - 1;
  if (!C->ViewCount) n = 0;
  C->Sel = (UINTN)n;
  if (C->Sel < C->Top) C->Top = C->Sel;
  if (C->Visible && C->Sel >= C->Top + C->Visible) C->Top = C->Sel - C->Visible + 1;
  refresh_preview (C);
}

/* ---------------- 递归文件名搜索（DFS，深度/命中数有界） ---------------- */

STATIC VOID search_walk (FM_CORE *C, FS_OPS *Fs, CONST CHAR16 *Rel, UINTN Depth) {
  FS_ENTRY *ents = NULL;
  UINTN n = 0, i;
  CHAR16 dir[FM_MAX_PATH];
  path_join (dir, FM_MAX_PATH, C->SearchRoot, Rel);
  if (EFI_ERROR (Fs->Enum (Fs, dir, &ents, &n))) return;
  for (i = 0; i < n && C->ResultCount < FM_MAX_RESULTS; i++) {
    CHAR16 rel[FM_MAX_PATH];
    path_join (rel, FM_MAX_PATH, Rel, ents[i].Name);
    if (ents[i].IsDir) {
      if (Depth < FM_MAX_DEPTH) search_walk (C, Fs, rel, Depth + 1);
    } else if (str_contains (ents[i].Name, C->InputBuf)) {
      FS_ENTRY *r = &C->Results[C->ResultCount++];
      str_copy (r->Name, 256, rel);
      r->Size = ents[i].Size;
      r->Mtime = ents[i].Mtime;
      r->IsDir = FALSE;
    }
  }
  MemFree (ents);
}

STATIC VOID run_search (FM_CORE *C) {
  FS_OPS *Fs = cur_fs (C);
  MemFree (C->Results);
  C->Results = MemAlloc (FM_MAX_RESULTS * sizeof (FS_ENTRY));
  C->ResultCount = 0;
  C->ResultSel = 0;
  C->ResultTop = 0;
  str_copy (C->SearchRoot, FM_MAX_PATH, C->VolCwd[C->VolIdx]);
  if (Fs) search_walk (C, Fs, U16(""), 0);
  if (!C->ResultCount) {
    set_message (C, U16("No matches."));
    return;
  }
  C->State = FM_STATE_RESULTS;
}

/* ---------------- 复制（单文件，跨卷；覆盖=先删后建保证截断） ---------------- */

STATIC VOID do_copy (FM_CORE *C, BOOLEAN Overwrite) {
  FS_OPS *SrcFs, *DstFs;
  FS_HANDLE *Sh = NULL, *Dh = NULL;
  UINT8 buf[FM_COPY_CHUNK];
  CHAR16 dstfull[FM_MAX_PATH + 16];
  CHAR16 msg[300];
  EFI_STATUS Status;

  C->CopyMode = FALSE;
  SrcFs = C->Init->Vols[C->SrcVol].Fs;
  DstFs = C->Init->Vols[C->DstVol].Fs;
  full_path (C, C->DstVol, C->DstPath, dstfull, sizeof (dstfull) / sizeof (CHAR16));
  if (C->SrcVol == C->DstVol && str_ieq (C->SrcPath, C->DstPath)) {
    set_message (C, U16("Source and destination are the same file."));
    return;
  }
  if (Overwrite) {
    /* UEFI SimpleFileSystem 与 mock 均不截断已存在文件：先 Delete 再 CREATE */
    if (EFI_ERROR (DstFs->Delete (DstFs, C->DstPath))) {
      set_message (C, U16("Copy failed: cannot replace existing target."));
      return;
    }
  }
  Status = SrcFs->Open (SrcFs, C->SrcPath, FS_MODE_READ, &Sh);
  if (EFI_ERROR (Status)) { set_message (C, U16("Copy failed: cannot open source.")); return; }
  Status = DstFs->Open (DstFs, C->DstPath, FS_MODE_WRITE | FS_MODE_CREATE, &Dh);
  if (EFI_ERROR (Status)) { SrcFs->Close (SrcFs, Sh); set_message (C, U16("Copy failed: cannot create target.")); return; }
  for (;;) {
    UINTN n = sizeof (buf);
    Status = SrcFs->Read (SrcFs, Sh, buf, &n);
    if (EFI_ERROR (Status)) break;
    if (n == 0) { Status = EFI_SUCCESS; break; }
    {
      UINTN w = n;
      Status = DstFs->Write (DstFs, Dh, buf, &w);
      if (EFI_ERROR (Status) || w != n) { Status = EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR; break; }
    }
  }
  if (!EFI_ERROR (Status)) Status = DstFs->Flush (DstFs, Dh);
  SrcFs->Close (SrcFs, Sh);
  DstFs->Close (DstFs, Dh);
  if (EFI_ERROR (Status)) {
    set_message (C, U16("Copy failed: I/O error."));
    return;
  }
  str_copy (msg, 300, U16("Copied to "));
  str_cat (msg, 300, dstfull);
  set_message (C, msg);
}

/* ---------------- 确认执行 ---------------- */

STATIC VOID confirm_exec (FM_CORE *C) {
  FS_OPS *Fs = cur_fs (C);
  CHAR16 rel[FM_MAX_PATH], full[FM_MAX_PATH + 16];
  EFI_STATUS Status, ImageReturn = EFI_SUCCESS;
  FS_ENTRY *e = &C->Entries[C->View[C->Sel]];
  path_join (rel, FM_MAX_PATH, C->VolCwd[C->VolIdx], e->Name);
  full_path (C, C->VolIdx, rel, full, sizeof (full) / sizeof (CHAR16));
  if (ext_is (e->Name, U16(".nsh"))) {
    Status = ExecNsh (C->Init->ShellPath, full, NULL);
  } else {
    Status = ExecEfi (Fs, full, NULL, &ImageReturn);
  }
  TuiInvalidate ();   /* 子程序可能已改写屏幕：作废 shadow，下次刷新全量重绘 */
  C->State = FM_STATE_DIR;
  if (EFI_ERROR (Status)) {
    set_message (C, U16("Execution failed (load error)."));
    return;
  }
  reload_dir (C);
}

STATIC VOID confirm_delete (FM_CORE *C) {
  FS_OPS *Fs = cur_fs (C);
  CHAR16 rel[FM_MAX_PATH];
  FS_ENTRY *e = &C->Entries[C->View[C->Sel]];
  path_join (rel, FM_MAX_PATH, C->VolCwd[C->VolIdx], e->Name);
  C->State = FM_STATE_DIR;
  if (EFI_ERROR (Fs->Delete (Fs, rel))) {
    set_message (C, U16("Delete failed (directory must be empty)."));
    return;
  }
  reload_dir (C);
}

STATIC VOID dispatch_confirm (FM_CORE *C) {
  switch (C->ConfirmKind) {
    case FM_CONFIRM_EXEC:      confirm_exec (C);   break;
    case FM_CONFIRM_DELETE:    confirm_delete (C); break;
    case FM_CONFIRM_OVERWRITE: do_copy (C, TRUE);    break;
  }
}

/* ---------------- 输入提交 ---------------- */

STATIC VOID input_commit (FM_CORE *C) {
  FS_OPS *Fs = cur_fs (C);
  CHAR16 path[FM_MAX_PATH];
  if (!C->InputLen) { set_message (C, U16("Empty name.")); return; }
  switch (C->InputKind) {
    case FM_INPUT_SEARCH:
      run_search (C);
      return;
    case FM_INPUT_RENAME: {
      CHAR16 old[FM_MAX_PATH];
      UINTN i;
      path_join (old, FM_MAX_PATH, C->VolCwd[C->VolIdx], C->Entries[C->View[C->Sel]].Name);
      path_join (path, FM_MAX_PATH, C->VolCwd[C->VolIdx], C->InputBuf);
      C->State = FM_STATE_DIR;
      if (EFI_ERROR (Fs->Rename (Fs, old, path))) {
        set_message (C, U16("Rename failed (target exists?)."));
        return;
      }
      reload_dir (C);
      for (i = 0; i < C->ViewCount; i++) {
        if (str_ieq (C->Entries[C->View[i]].Name, C->InputBuf)) { C->Sel = i; break; }
      }
      if (C->Sel < C->Top) C->Top = C->Sel;
      if (C->Visible && C->Sel >= C->Top + C->Visible) C->Top = C->Sel - C->Visible + 1;
      refresh_preview (C);
      return;
    }
    case FM_INPUT_NEW:
      path_join (path, FM_MAX_PATH, C->VolCwd[C->VolIdx], C->InputBuf);
      C->State = FM_STATE_DIR;
      if (C->NewIsDir) {
        if (EFI_ERROR (Fs->MkDir (Fs, path))) { set_message (C, U16("mkdir failed (exists?).")); return; }
      } else {
        FS_HANDLE *H = NULL;
        if (EFI_ERROR (Fs->Open (Fs, path, FS_MODE_WRITE | FS_MODE_CREATE, &H))) {
          set_message (C, U16("Create file failed."));
          return;
        }
        Fs->Close (Fs, H);
      }
      reload_dir (C);
      return;
  }
}

/* ---------------- 确认框构造 ---------------- */

STATIC VOID ask_confirm (FM_CORE *C, FM_CONFIRM_KIND Kind, CONST CHAR16 *Title, CONST CHAR16 *Body) {
  C->ConfirmKind = Kind;
  str_copy (C->ConfirmTitle, 64, Title);
  str_copy (C->ConfirmBody, 768, Body);
  C->State = FM_STATE_CONFIRM;
}

STATIC VOID ask_exec (FM_CORE *C) {
  CHAR16 rel[FM_MAX_PATH], full[FM_MAX_PATH + 16], body[768], num[32];
  FS_ENTRY *e = &C->Entries[C->View[C->Sel]];
  path_join (rel, FM_MAX_PATH, C->VolCwd[C->VolIdx], e->Name);
  full_path (C, C->VolIdx, rel, full, sizeof (full) / sizeof (CHAR16));
  str_copy (body, 768, U16("Execute this file?\n"));
  str_cat (body, 768, full);
  str_cat (body, 768, U16("\nSize: "));
  u64_str (num, 32, e->Size);
  str_cat (body, 768, num);
  str_cat (body, 768, U16(" bytes"));
  ask_confirm (C, FM_CONFIRM_EXEC, U16("EXECUTE"), body);
}

STATIC VOID ask_delete (FM_CORE *C) {
  CHAR16 rel[FM_MAX_PATH], full[FM_MAX_PATH + 16], body[768], num[32];
  FS_ENTRY *e = &C->Entries[C->View[C->Sel]];
  path_join (rel, FM_MAX_PATH, C->VolCwd[C->VolIdx], e->Name);
  full_path (C, C->VolIdx, rel, full, sizeof (full) / sizeof (CHAR16));
  str_copy (body, 768, U16("Delete:\n"));
  str_cat (body, 768, full);
  if (e->IsDir) {
    str_cat (body, 768, U16("\n(directory - must be empty)"));
  } else {
    str_cat (body, 768, U16("\nSize: "));
    u64_str (num, 32, e->Size);
    str_cat (body, 768, num);
    str_cat (body, 768, U16(" bytes"));
  }
  ask_confirm (C, FM_CONFIRM_DELETE, U16("DELETE"), body);
}

/* ---------------- 生命周期 ---------------- */

FM_CORE *FmCoreNew (CONST FM_INIT *Init) {
  FM_CORE *C = MemAlloc (sizeof (FM_CORE));
  UINTN i;
  if (!C) return NULL;
  C->Init = Init;
  C->PreviewCap = Init->PreviewMaxBytes ? Init->PreviewMaxBytes : 16384;
  C->PreviewBuf = MemAlloc (C->PreviewCap);
  C->VolCwdCount = Init->VolCount;
  C->VolCwd = MemAlloc ((Init->VolCount ? Init->VolCount : 1) * sizeof (*C->VolCwd));
  for (i = 0; i < Init->VolCount; i++) C->VolCwd[i][0] = 0;
  TuiGetSize (&C->Rows, &C->Cols);
  C->Visible = C->Rows > 4 ? C->Rows - 3 : 1;
  if (!Init->VolCount) {
    set_message (C, U16("No volumes found. Press R to rescan, F10 to quit."));
    return C;
  }
  /* 默认进入 P3（若有），否则卷 0 */
  C->VolIdx = 0;
  for (i = 0; i < Init->VolCount; i++) if (Init->Vols[i].IsP3) { C->VolIdx = i; break; }
  C->State = FM_STATE_DIR;
  reload_dir (C);
  return C;
}

VOID FmCoreFree (FM_CORE *C) {
  if (!C) return;
  MemFree (C->Entries);
  MemFree (C->View);
  MemFree (C->Results);
  MemFree (C->PreviewBuf);
  MemFree (C->VolCwd);
  MemFree (C);
}

/* ---------------- 按键处理 ---------------- */

/* Rescan 后卷数可能变化：同步每卷 cwd 数组（保留旧值，新卷置根） */
STATIC VOID sync_volcwd (FM_CORE *C) {
  UINTN n = C->Init->VolCount, i;
  if (n == C->VolCwdCount) return;
  {
    CHAR16 (*nc)[FM_MAX_PATH] = MemAlloc ((n ? n : 1) * sizeof (*nc));
    for (i = 0; i < n; i++) {
      if (i < C->VolCwdCount) str_copy (nc[i], FM_MAX_PATH, C->VolCwd[i]);
      else nc[i][0] = 0;
    }
    MemFree (C->VolCwd);
    C->VolCwd = nc;
    C->VolCwdCount = n;
  }
}

STATIC VOID handle_volumes (FM_CORE *C, CONST TUI_KEY *Key, TUI_COMMAND Cmd) {
  UINTN n = C->Init->VolCount;
  switch (Cmd) {
    case CMD_UP:    if (C->VSel) C->VSel--; break;
    case CMD_DOWN:  if (C->VSel + 1 < n) C->VSel++; break;
    case CMD_PGUP:  C->VSel = C->VSel > C->Visible ? C->VSel - C->Visible : 0; break;
    case CMD_PGDN:  C->VSel += C->Visible; if (C->VSel >= n) C->VSel = n ? n - 1 : 0; break;
    case CMD_HOME:  C->VSel = 0; break;
    case CMD_END:   C->VSel = n ? n - 1 : 0; break;
    case CMD_ENTER:
      if (!n) break;
      if (C->CopyMode) {
        CHAR16 dst[FM_MAX_PATH];
        FS_ENTRY info;
        C->DstVol = C->VSel;
        path_join (dst, FM_MAX_PATH, C->VolCwd[C->VSel], path_base (C->SrcPath));
        str_copy (C->DstPath, FM_MAX_PATH, dst);
        if (!EFI_ERROR (C->Init->Vols[C->DstVol].Fs->Info (C->Init->Vols[C->DstVol].Fs, dst, &info))) {
          CHAR16 full[FM_MAX_PATH + 16], body[768], num[32];
          full_path (C, C->DstVol, dst, full, sizeof (full) / sizeof (CHAR16));
          str_copy (body, 768, U16("Target exists:\n"));
          str_cat (body, 768, full);
          str_cat (body, 768, U16("\nSize: "));
          u64_str (num, 32, info.Size);
          str_cat (body, 768, num);
          str_cat (body, 768, U16(" bytes\nOverwrite?"));
          ask_confirm (C, FM_CONFIRM_OVERWRITE, U16("OVERWRITE"), body);
        } else {
          do_copy (C, FALSE);
        }
      } else {
        C->VolIdx = C->VSel;
        C->State = FM_STATE_DIR;
        C->Sel = 0;
        C->Top = 0;
        C->Filter[0] = 0;
        C->FilterActive = FALSE;
        reload_dir (C);
      }
      break;
    case CMD_ESC:
      if (C->CopyMode) { C->CopyMode = FALSE; C->State = FM_STATE_DIR; }
      else C->Exit = TRUE;
      break;
    case CMD_F10: C->Exit = TRUE; break;
    case CMD_CHAR:
      if (Key->UnicodeChar == L'r' || Key->UnicodeChar == L'R') {
        if (C->Init->Ops && C->Init->Ops->Rescan) {
          if (EFI_ERROR (C->Init->Ops->Rescan (C->Init->Ops->Ctx))) {
            set_message (C, U16("Volume rescan failed."));
            break;
          }
          sync_volcwd (C);
        }
        if (C->VolIdx >= C->Init->VolCount) C->VolIdx = C->Init->VolCount ? C->Init->VolCount - 1 : 0;
        if (C->VSel >= C->Init->VolCount) C->VSel = C->Init->VolCount ? C->Init->VolCount - 1 : 0;
      }
      break;
    default: break;
  }
  if (C->State == FM_STATE_VOLUMES && C->VSel < C->VTop) C->VTop = C->VSel;
  if (C->State == FM_STATE_VOLUMES && C->Visible && C->VSel >= C->VTop + C->Visible) C->VTop = C->VSel - C->Visible + 1;
}

STATIC VOID handle_dir (FM_CORE *C, CONST TUI_KEY *Key, TUI_COMMAND Cmd) {
  FS_ENTRY *e;
  switch (Cmd) {
    case CMD_UP:    move_sel (C, -1); return;
    case CMD_DOWN:  move_sel (C, 1); return;
    case CMD_PGUP:  move_sel (C, -(INTN)C->Visible); return;
    case CMD_PGDN:  move_sel (C, (INTN)C->Visible); return;
    case CMD_HOME:  move_sel (C, -(INTN)C->ViewCount); return;
    case CMD_END:   move_sel (C, (INTN)C->ViewCount); return;
    case CMD_ENTER:
      if (!C->ViewCount) return;
      e = &C->Entries[C->View[C->Sel]];
      if (e->IsDir) {
        CHAR16 next[FM_MAX_PATH];
        path_join (next, FM_MAX_PATH, C->VolCwd[C->VolIdx], e->Name);
        goto_dir (C, next);
      } else if (ext_is (e->Name, U16(".efi")) || ext_is (e->Name, U16(".nsh"))) {
        ask_exec (C);
      } else {
        set_message (C, U16("Not executable (.efi / .nsh only)."));
      }
      return;
    case CMD_BACKSPACE:
      if (C->FilterActive) {
        if (C->Filter[0]) C->Filter[EfiStrLen (C->Filter) - 1] = 0;
        C->Sel = 0; C->Top = 0;
        rebuild_view (C);
        refresh_preview (C);
      } else {
        ascend (C);
      }
      return;
    case CMD_ESC:
      if (C->FilterActive) {
        C->Filter[0] = 0;
        C->FilterActive = FALSE;
        C->Sel = 0; C->Top = 0;
        rebuild_view (C);
        refresh_preview (C);
      } else {
        ascend (C);
      }
      return;
    case CMD_CHAR: {
      CHAR16 ch = Key->UnicodeChar;
      if (ch < 0x20) return;
      /* 命令键 v/s/r 仅在过滤未激活时生效；'/' 显式进入过滤；
         过滤激活后所有可打印字符（含 v/s/r）追加以支持任意子串 */
      if (!C->FilterActive) {
        if (ch == L'v' || ch == L'V') {
          C->State = FM_STATE_VOLUMES;
          C->VSel = C->VolIdx;
          C->VTop = 0;
          return;
        }
        if (ch == L's' || ch == L'S') {
          C->InputKind = FM_INPUT_SEARCH;
          C->InputBuf[0] = 0;
          C->InputLen = 0;
          C->State = FM_STATE_INPUT;
          return;
        }
        if (ch == L'r' || ch == L'R') {
          if (C->Init->Ops && C->Init->Ops->Rescan) {
            if (EFI_ERROR (C->Init->Ops->Rescan (C->Init->Ops->Ctx))) {
              set_message (C, U16("Volume rescan failed."));
              return;
            }
            sync_volcwd (C);
            if (C->VolIdx >= C->Init->VolCount) C->VolIdx = C->Init->VolCount ? C->Init->VolCount - 1 : 0;
          }
          reload_dir (C);
          return;
        }
        if (ch == L'/') {
          C->FilterActive = TRUE;
          C->Filter[0] = 0;
          C->Sel = 0; C->Top = 0;
          rebuild_view (C);
          refresh_preview (C);
          return;
        }
        C->FilterActive = TRUE;   /* 其它可打印字符：键入即过滤 */
      }
      if (EfiStrLen (C->Filter) + 1 < FM_MAX_FILTER) {
        UINTN n = EfiStrLen (C->Filter);
        C->Filter[n] = ch;
        C->Filter[n + 1] = 0;
        C->Sel = 0; C->Top = 0;
        rebuild_view (C);
        refresh_preview (C);
      }
      return;
    }
    case CMD_F3:
      C->PreviewHex = !C->PreviewHex;
      return;
    case CMD_F4:
      if (!C->ViewCount) return;
      e = &C->Entries[C->View[C->Sel]];
      if (e->IsDir) { set_message (C, U16("F4: not a file.")); return; }
      if (C->PreviewBinary) {
        set_message (C, U16("Binary file - press F3 for hex preview."));
        return;
      }
      {
        CHAR16 rel[FM_MAX_PATH], full[FM_MAX_PATH + 16];
        EFI_STATUS Status, Ret = EFI_SUCCESS;
        FS_OPS *EdFs = (C->Init->BootFs != NULL) ? C->Init->BootFs : cur_fs (C);
        path_join (rel, FM_MAX_PATH, C->VolCwd[C->VolIdx], e->Name);
        full_path (C, C->VolIdx, rel, full, sizeof (full) / sizeof (CHAR16));
        Status = ExecEfi (EdFs, C->Init->EditorPath, full, &Ret);
        TuiInvalidate ();   /* 编辑器可能已改写屏幕：作废 shadow */
        if (EFI_ERROR (Status)) { set_message (C, U16("Editor launch failed.")); return; }
        reload_dir (C);
      }
      return;
    case CMD_F5:
      if (!C->ViewCount) return;
      e = &C->Entries[C->View[C->Sel]];
      if (e->IsDir) { set_message (C, U16("Copy is single-file only (no recursive dir copy).")); return; }
      C->SrcVol = C->VolIdx;
      path_join (C->SrcPath, FM_MAX_PATH, C->VolCwd[C->VolIdx], e->Name);
      C->CopyMode = TRUE;
      C->State = FM_STATE_VOLUMES;
      C->VSel = C->VolIdx;
      C->VTop = 0;
      return;
    case CMD_F6:
      if (!C->ViewCount) return;
      C->InputKind = FM_INPUT_RENAME;
      str_copy (C->InputBuf, FM_MAX_NAME, C->Entries[C->View[C->Sel]].Name);
      C->InputLen = EfiStrLen (C->InputBuf);
      C->State = FM_STATE_INPUT;
      return;
    case CMD_F7:
      C->InputKind = FM_INPUT_NEW;
      C->InputBuf[0] = 0;
      C->InputLen = 0;
      C->NewIsDir = FALSE;
      C->State = FM_STATE_INPUT;
      return;
    case CMD_F8:
      if (!C->ViewCount) return;
      ask_delete (C);
      return;
    case CMD_F10:
      C->Exit = TRUE;
      return;
    default:
      return;
  }
}

STATIC VOID handle_results (FM_CORE *C, TUI_COMMAND Cmd) {
  UINTN n = C->ResultCount;
  switch (Cmd) {
    case CMD_UP:    if (C->ResultSel) C->ResultSel--; break;
    case CMD_DOWN:  if (C->ResultSel + 1 < n) C->ResultSel++; break;
    case CMD_PGUP:  C->ResultSel = C->ResultSel > C->Visible ? C->ResultSel - C->Visible : 0; break;
    case CMD_PGDN:  C->ResultSel += C->Visible; if (C->ResultSel >= n) C->ResultSel = n ? n - 1 : 0; break;
    case CMD_HOME:  C->ResultSel = 0; break;
    case CMD_END:   C->ResultSel = n ? n - 1 : 0; break;
    case CMD_ENTER:
      if (n) {
        CHAR16 parent[FM_MAX_PATH], full[FM_MAX_PATH], base[256];
        UINTN i;
        str_copy (base, 256, path_base (C->Results[C->ResultSel].Name));
        path_parent (parent, FM_MAX_PATH, C->Results[C->ResultSel].Name);
        path_join (full, FM_MAX_PATH, C->SearchRoot, parent);
        C->State = FM_STATE_DIR;
        goto_dir (C, full);
        for (i = 0; i < C->ViewCount; i++) {
          if (str_ieq (C->Entries[C->View[i]].Name, base)) { C->Sel = i; break; }
        }
        if (C->Sel < C->Top) C->Top = C->Sel;
        if (C->Visible && C->Sel >= C->Top + C->Visible) C->Top = C->Sel - C->Visible + 1;
        refresh_preview (C);
      }
      break;
    case CMD_ESC:
      C->State = FM_STATE_DIR;
      break;
    case CMD_F10:
      C->Exit = TRUE;
      break;
    default: break;
  }
  if (C->ResultSel < C->ResultTop) C->ResultTop = C->ResultSel;
  if (C->Visible && C->ResultSel >= C->ResultTop + C->Visible) C->ResultTop = C->ResultSel - C->Visible + 1;
}

STATIC VOID handle_input (FM_CORE *C, CONST TUI_KEY *Key, TUI_COMMAND Cmd) {
  switch (Cmd) {
    case CMD_CHAR:
      if (Key->UnicodeChar >= 0x20 && Key->UnicodeChar != L'/' && Key->UnicodeChar != L'\\' &&
          C->InputLen + 1 < FM_MAX_NAME) {
        C->InputBuf[C->InputLen++] = Key->UnicodeChar;
        C->InputBuf[C->InputLen] = 0;
      }
      return;
    case CMD_BACKSPACE:
      if (C->InputLen) C->InputBuf[--C->InputLen] = 0;
      return;
    case CMD_TAB:
      if (C->InputKind == FM_INPUT_NEW) C->NewIsDir = !C->NewIsDir;
      return;
    case CMD_ENTER:
      input_commit (C);
      return;
    case CMD_ESC:
      C->State = FM_STATE_DIR;
      return;
    case CMD_F10:
      C->Exit = TRUE;
      return;
    default:
      return;
  }
}

EFI_STATUS FmCoreHandleKey (FM_CORE *C, CONST TUI_KEY *Key) {
  TUI_COMMAND Cmd;
  if (!C || !Key) return EFI_INVALID_PARAMETER;
  if (C->Exit) return EFI_SUCCESS;
  Cmd = TuiKeyToCommand (Key);
  switch (C->State) {
    case FM_STATE_VOLUMES: handle_volumes (C, Key, Cmd); break;
    case FM_STATE_DIR:     handle_dir (C, Key, Cmd); break;
    case FM_STATE_RESULTS: handle_results (C, Cmd); break;
    case FM_STATE_INPUT:   handle_input (C, Key, Cmd); break;
    case FM_STATE_CONFIRM:
      if (Cmd == CMD_CHAR && (Key->UnicodeChar == L'y' || Key->UnicodeChar == L'Y')) {
        dispatch_confirm (C);
      } else if (Cmd == CMD_CHAR && (Key->UnicodeChar == L'n' || Key->UnicodeChar == L'N')) {
        C->CopyMode = FALSE;
        C->State = FM_STATE_DIR;
      } else if (Cmd == CMD_ESC || Cmd == CMD_ENTER) {
        /* ENTER 不默认执行（默认 N）：仅 ESC/N 取消；ENTER 视为取消 */
        C->CopyMode = FALSE;
        C->State = FM_STATE_DIR;
      } else if (Cmd == CMD_F10) {
        C->Exit = TRUE;
      }
      break;
    case FM_STATE_MESSAGE:
      C->State = C->Init->VolCount ? FM_STATE_DIR : FM_STATE_VOLUMES;
      if (C->State == FM_STATE_DIR) reload_dir (C);
      if (Cmd == CMD_F10) C->Exit = TRUE;
      break;
  }
  return EFI_SUCCESS;
}

/* ---------------- 渲染 ---------------- */

STATIC VOID draw_text (UINTN Row, UINTN Col, UINTN Width, CONST CHAR16 *Text, UINT8 Attr) {
  CHAR16 buf[512];
  UINTN i;
  if (!Width) return;
  for (i = 0; Text[i] && i < Width && i < 511; i++) buf[i] = Text[i];
  buf[i] = 0;
  TuiPutText (Row, Col, buf, Attr);
}

STATIC VOID draw_row (UINTN Row, UINTN Col, UINTN Width, CONST CHAR16 *Text, UINT8 Attr) {
  TuiFill (Row, Col, Width, 1, L' ', Attr);
  draw_text (Row, Col, Width, Text, Attr);
}

STATIC VOID draw_box (UINTN Row, UINTN Col, UINTN W, UINTN H, CONST CHAR16 *Title) {
  UINTN i;
  TuiFill (Row, Col, W, H, L' ', TUI_ATTR_NORMAL);
  for (i = 0; i < W; i++) {
    TuiPutCell (Row, Col + i, (i == 0 || i == W - 1) ? L'+' : L'-', TUI_ATTR_NORMAL);
    TuiPutCell (Row + H - 1, Col + i, (i == 0 || i == W - 1) ? L'+' : L'-', TUI_ATTR_NORMAL);
  }
  for (i = 1; i + 1 < H; i++) {
    TuiPutCell (Row + i, Col, L'|', TUI_ATTR_NORMAL);
    TuiPutCell (Row + i, Col + W - 1, L'|', TUI_ATTR_NORMAL);
  }
  if (Title && Title[0]) draw_text (Row, Col + 2, W > 4 ? W - 4 : 0, Title, TUI_ATTR_TITLE);
}

/* 居中对话框：body 按 '\n' 分段 + 宽度折行 */
STATIC VOID draw_dialog (FM_CORE *C, CONST CHAR16 *Title, CONST CHAR16 *Body, CONST CHAR16 *Footer) {
  UINTN W = C->Cols > 8 ? (C->Cols < 76 ? C->Cols - 4 : 76) : 4;
  UINTN inner = W - 4;
  UINTN lines, H, row, col, ln, seglen, cur;
  CONST CHAR16 *p;
  CHAR16 line[512];

  /* 折行后行数估算 */
  lines = 0;
  cur = 0;
  for (p = Body; ; p++) {
    if (*p == L'\n' || !*p) {
      lines += cur / inner + 1;
      cur = 0;
      if (!*p) break;
    } else cur++;
  }
  H = lines + 4;
  if (H > C->Rows - 2) H = C->Rows - 2;
  row = C->Rows > H ? (C->Rows - H) / 2 : 0;
  col = C->Cols > W ? (C->Cols - W) / 2 : 0;
  draw_box (row, col, W, H, Title);
  ln = row + 1;
  seglen = 0;
  for (p = Body; ; p++) {
    if (*p == L'\n' || !*p) {
      UINTN off = 0;
      do {
        UINTN take = seglen - off < inner ? seglen - off : inner;
        UINTN j;
        if (ln + 1 >= row + H - 1) break;
        for (j = 0; j < take; j++) line[j] = *(p - seglen + off + j);
        line[take] = 0;
        draw_text (ln, col + 2, inner, line, TUI_ATTR_NORMAL);
        ln++;
        off += take;
      } while (off < seglen);
      seglen = 0;
      if (!*p) break;
    } else seglen++;
  }
  if (Footer && ln + 1 < row + H) draw_text (row + H - 2, col + 2, inner, Footer, TUI_ATTR_KEYBAR);
}

STATIC VOID draw_title (FM_CORE *C) {
  CHAR16 line[512];
  str_copy (line, 512, U16("EasyEFI FileManager"));
  if (C->Init->VolCount && C->VolIdx < C->Init->VolCount) {
    str_cat (line, 512, U16("  ["));
    str_cat (line, 512, C->Init->Vols[C->VolIdx].Label);
    str_cat (line, 512, U16("]"));
    if (C->Init->Vols[C->VolIdx].IsP3) str_cat (line, 512, U16(" P3"));
  }
  if (C->State == FM_STATE_DIR && C->Filter[0]) {
    str_cat (line, 512, U16("  filter:"));
    str_cat (line, 512, C->Filter);
  }
  if (C->CopyMode) str_cat (line, 512, U16("  COPY: pick target volume"));
  draw_row (0, 0, C->Cols, line, TUI_ATTR_TITLE);
}

STATIC VOID draw_status_keybar (FM_CORE *C) {
  CHAR16 line[512];
  CONST CHAR16 *keys;
  if (C->State == FM_STATE_VOLUMES) {
    str_copy (line, 512, C->CopyMode ? U16("Select copy target volume") : U16("Volume list"));
  } else {
    full_path (C, C->VolIdx, C->VolCwd[C->VolIdx], line, 512);
  }
  draw_row (C->Rows - 2, 0, C->Cols, line, TUI_ATTR_STATUSBAR);
  switch (C->State) {
    case FM_STATE_VOLUMES: keys = U16("Enter:select ESC:back R:rescan F10:quit"); break;
    case FM_STATE_RESULTS: keys = U16("Enter:jump ESC:back F10:quit"); break;
    case FM_STATE_INPUT:   keys = U16("Enter:ok ESC:cancel Tab:type(new) F10:quit"); break;
    case FM_STATE_CONFIRM: keys = U16("Y:yes N/ESC:no (default N)"); break;
    case FM_STATE_MESSAGE: keys = U16("any key:continue F10:quit"); break;
    default:               keys = U16("F3:hex F4:edit F5:copy F6:ren F7:new F8:del V:vol S:search R:refresh F10:quit"); break;
  }
  draw_row (C->Rows - 1, 0, C->Cols, keys, TUI_ATTR_KEYBAR);
}

STATIC VOID draw_volumes (FM_CORE *C) {
  UINTN i, row;
  for (i = 0; i < C->Visible; i++) {
    UINTN idx = C->VTop + i;
    CHAR16 line[512], cap[32];
    row = 1 + i;
    if (idx >= C->Init->VolCount) {
      draw_row (row, 0, C->Cols, U16(""), TUI_ATTR_NORMAL);
      continue;
    }
    str_copy (line, 512, C->Init->Vols[idx].Label);
    str_cat (line, 512, U16("  ["));
    str_cat (line, 512, C->Init->Vols[idx].FsName);
    str_cat (line, 512, U16("]  "));
    size_human (cap, 32, C->Init->Vols[idx].Capacity);
    str_cat (line, 512, cap);
    if (C->Init->Vols[idx].IsP3) str_cat (line, 512, U16("  P3"));
    draw_row (row, 0, C->Cols, line, idx == C->VSel ? TUI_ATTR_SELECTED : TUI_ATTR_NORMAL);
  }
}

STATIC VOID draw_dir_list (FM_CORE *C) {
  UINTN i;
  UINTN listw = C->Cols > 50 ? FM_LIST_W : C->Cols;
  for (i = 0; i < C->Visible; i++) {
    UINTN idx = C->Top + i;
    UINTN row = 1 + i;
    CHAR16 line[512], name[FM_NAME_W + 2], num[32], mt[16];
    UINT8 attr;
    if (idx >= C->ViewCount) {
      draw_row (row, 0, listw, U16(""), TUI_ATTR_NORMAL);
      continue;
    }
    {
      FS_ENTRY *e = &C->Entries[C->View[idx]];
      UINTN j;
      for (j = 0; e->Name[j] && j < FM_NAME_W - 1; j++) name[j] = e->Name[j];
      name[j] = 0;
      if (e->IsDir) str_cat (name, FM_NAME_W + 2, U16("/"));
      str_copy (line, 512, name);
      if (e->IsDir) {
        str_copy (num, 32, U16("<DIR>"));
      } else {
        u64_str (num, 32, e->Size);
      }
      mtime_str (mt, 16, e->Mtime);
      if (listw >= FM_LIST_W) {
        /* 名称列 0..21，大小右对齐 22..31，修改时间 33..42 */
        UINTN k;
        while (EfiStrLen (line) < 22 && EfiStrLen (line) + 1 < 511) str_cat (line, 512, U16(" "));
        for (k = EfiStrLen (num); k < 10; k++) str_cat (line, 512, U16(" "));
        str_cat (line, 512, num);
        str_cat (line, 512, U16(" "));
        str_cat (line, 512, mt);
      } else {
        UINTN pad = listw > EfiStrLen (line) + EfiStrLen (num) ? listw - EfiStrLen (line) - EfiStrLen (num) : 1;
        UINTN k;
        for (k = 0; k < pad && EfiStrLen (line) + 1 < 511; k++) str_cat (line, 512, U16(" "));
        str_cat (line, 512, num);
      }
      attr = idx == C->Sel ? TUI_ATTR_SELECTED : TUI_ATTR_NORMAL;
      draw_row (row, 0, listw, line, attr);
    }
  }
}

STATIC VOID draw_preview (FM_CORE *C) {
  UINTN pcol = FM_LIST_W + 1;
  UINTN pw = C->Cols > pcol + 1 ? C->Cols - pcol : 0;
  UINTN i;
  CHAR16 line[512];
  if (!pw) return;
  for (i = 0; i < C->Visible; i++) TuiFill (1 + i, pcol - 1, 1, 1, L'|', TUI_ATTR_NORMAL);
  if (!C->ViewCount) {
    draw_text (1, pcol, pw, U16("(empty directory)"), TUI_ATTR_DISABLED);
    return;
  }
  {
    FS_ENTRY *e = &C->Entries[C->View[C->Sel]];
    CHAR16 num[32];
    str_copy (line, 512, e->Name);
    str_cat (line, 512, U16("  "));
    u64_str (num, 32, e->Size);
    str_cat (line, 512, num);
    str_cat (line, 512, U16("B"));
    draw_text (1, pcol, pw, line, TUI_ATTR_TITLE);
    if (e->IsDir) {
      draw_text (2, pcol, pw, U16("<directory>"), TUI_ATTR_DISABLED);
      return;
    }
    if (!C->PreviewValid) {
      draw_text (2, pcol, pw, U16("(preview unavailable)"), TUI_ATTR_DISABLED);
      return;
    }
    if (C->PreviewHex || C->PreviewBinary) {
      /* hex：offset + FM_HEX_BPR 字节 + ascii */
      UINTN r;
      draw_text (2, pcol, pw, C->PreviewBinary ? U16("[binary] F3:text") : U16("[hex] F3:text"), TUI_ATTR_KEYBAR);
      for (r = 0; r + 3 < C->Visible + 1; r++) {
        UINTN base = r * FM_HEX_BPR;
        UINTN j, pos = 0;
        CHAR16 hx[128];
        if (base >= C->PreviewLen) break;
        /* offset */
        {
          UINT64 v = base;
          CHAR8 tmp[16];
          UINTN n = 0, k;
          do { UINTN d = (UINTN)(v & 0xF); tmp[n++] = (CHAR8)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; } while (v && n < 8);
          while (n < 8) tmp[n++] = '0';
          for (k = 0; k < n; k++) hx[pos++] = (CHAR16)tmp[n - 1 - k];
        }
        hx[pos++] = L' ';
        for (j = 0; j < FM_HEX_BPR; j++) {
          if (base + j < C->PreviewLen) {
            UINT8 b = C->PreviewBuf[base + j];
            UINTN hi = b >> 4, lo = b & 0xF;
            hx[pos++] = (CHAR16)(hi < 10 ? '0' + hi : 'a' + hi - 10);
            hx[pos++] = (CHAR16)(lo < 10 ? '0' + lo : 'a' + lo - 10);
          } else {
            hx[pos++] = L' '; hx[pos++] = L' ';
          }
          hx[pos++] = L' ';
        }
        hx[pos++] = L'|';
        for (j = 0; j < FM_HEX_BPR && base + j < C->PreviewLen; j++) {
          UINT8 b = C->PreviewBuf[base + j];
          hx[pos++] = (b >= 0x20 && b < 0x7F) ? (CHAR16)b : L'.';
        }
        hx[pos++] = L'|';
        hx[pos] = 0;
        draw_text (3 + r, pcol, pw, hx, TUI_ATTR_NORMAL);
      }
    } else {
      /* 文本：非 ASCII/控制符 → '?' 占位（ConOut 无 CJK，design F04） */
      UINTN j, r = 0, col = 0;
      CHAR16 txt[512];
      draw_text (2, pcol, pw, U16("[text] F3:hex"), TUI_ATTR_KEYBAR);
      for (j = 0; j < C->PreviewLen && r + 3 < C->Visible + 1; j++) {
        UINT8 b = C->PreviewBuf[j];
        if (b == L'\n' || col >= pw) {
          txt[col] = 0;
          draw_text (3 + r, pcol, pw, txt, TUI_ATTR_NORMAL);
          r++;
          col = 0;
          if (b == L'\n') continue;
        }
        if (b == L'\r') continue;
        txt[col++] = (b >= 0x20 && b < 0x7F) ? (CHAR16)b : TUI_NONASCII_CHAR;
      }
      if (col && r + 3 < C->Visible + 1) {
        txt[col] = 0;
        draw_text (3 + r, pcol, pw, txt, TUI_ATTR_NORMAL);
      }
    }
  }
}

STATIC VOID draw_results (FM_CORE *C) {
  UINTN i;
  CHAR16 head[128];
  str_copy (head, 128, U16("Search results for: "));
  str_cat (head, 128, C->InputBuf);
  draw_row (1, 0, C->Cols, head, TUI_ATTR_TITLE);
  for (i = 0; i + 1 < C->Visible; i++) {
    UINTN idx = C->ResultTop + i;
    CHAR16 line[512], num[32];
    UINTN row = 2 + i;
    if (idx >= C->ResultCount) {
      draw_row (row, 0, C->Cols, U16(""), TUI_ATTR_NORMAL);
      continue;
    }
    str_copy (line, 512, C->Results[idx].Name);
    str_cat (line, 512, U16("  ("));
    u64_str (num, 32, C->Results[idx].Size);
    str_cat (line, 512, num);
    str_cat (line, 512, U16("B)"));
    draw_row (row, 0, C->Cols, line, idx == C->ResultSel ? TUI_ATTR_SELECTED : TUI_ATTR_NORMAL);
  }
}

EFI_STATUS FmCoreDraw (FM_CORE *C) {
  if (!C) return EFI_INVALID_PARAMETER;
  TuiClear ();
  draw_title (C);
  switch (C->State) {
    case FM_STATE_VOLUMES:
      draw_volumes (C);
      break;
    case FM_STATE_DIR:
      draw_dir_list (C);
      draw_preview (C);
      break;
    case FM_STATE_RESULTS:
      draw_results (C);
      break;
    case FM_STATE_CONFIRM:
      draw_dir_list (C);
      draw_dialog (C, C->ConfirmTitle, C->ConfirmBody, U16("[Y] Yes   [N] No / ESC cancel   (default: N)"));
      break;
    case FM_STATE_INPUT: {
      CHAR16 body[300];
      CONST CHAR16 *title = C->InputKind == FM_INPUT_RENAME ? U16("RENAME") :
                            C->InputKind == FM_INPUT_NEW ? U16("NEW") : U16("SEARCH");
      draw_dir_list (C);
      str_copy (body, 300, C->InputKind == FM_INPUT_SEARCH ? U16("File name contains:\n") :
                 C->InputKind == FM_INPUT_NEW ? (C->NewIsDir ? U16("New directory name:\n") : U16("New file name:\n")) :
                 U16("New name:\n"));
      str_cat (body, 300, C->InputBuf);
      str_cat (body, 300, U16("_"));
      if (C->InputKind == FM_INPUT_NEW) {
        str_cat (body, 300, U16("\n[Tab] type: "));
        str_cat (body, 300, C->NewIsDir ? U16("DIR") : U16("FILE"));
      }
      draw_dialog (C, title, body, U16("Enter:ok  ESC:cancel"));
      break;
    }
    case FM_STATE_MESSAGE:
      if (C->Init->VolCount) draw_dir_list (C);
      draw_dialog (C, U16("FILE MANAGER"), C->Msg, U16("press any key"));
      break;
  }
  draw_status_keybar (C);
  return TuiFlush ();
}

/* ---------------- 访问器 ---------------- */

FM_STATE        FmCoreState (CONST FM_CORE *C)        { return C->State; }
BOOLEAN         FmCoreExit (CONST FM_CORE *C)         { return C->Exit; }
UINTN           FmCoreVolIndex (CONST FM_CORE *C)     { return C->VolIdx; }
CONST CHAR16   *FmCoreCwd (CONST FM_CORE *C)          { return C->VolCwd[C->VolIdx]; }
UINTN           FmCoreViewCount (CONST FM_CORE *C)    { return C->ViewCount; }
UINTN           FmCoreSelected (CONST FM_CORE *C)     { return C->State == FM_STATE_VOLUMES ? C->VSel : C->Sel; }
CONST FS_ENTRY *FmCoreEntry (CONST FM_CORE *C, UINTN ViewIdx) {
  return ViewIdx < C->ViewCount ? &C->Entries[C->View[ViewIdx]] : NULL;
}
CONST CHAR16   *FmCoreFilter (CONST FM_CORE *C)       { return C->Filter; }
UINTN           FmCoreResultCount (CONST FM_CORE *C)  { return C->ResultCount; }
FM_CONFIRM_KIND FmCoreConfirmKind (CONST FM_CORE *C)  { return C->ConfirmKind; }
FM_INPUT_KIND   FmCoreInputKind (CONST FM_CORE *C)    { return C->InputKind; }
BOOLEAN         FmCorePreviewBinary (CONST FM_CORE *C){ return C->PreviewBinary; }
BOOLEAN         FmCorePreviewHex (CONST FM_CORE *C)   { return C->PreviewHex; }
UINTN           FmCorePreviewLen (CONST FM_CORE *C)   { return C->PreviewLen; }
