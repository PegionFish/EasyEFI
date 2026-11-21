/* MockFs — 平面内存文件系统 mock（S00，leader 独占）。
 * 路径→字节表，目录隐式（前缀）。供应用 core 在 S09 POSIX 后端就绪前开发。
 * 测试经 MockFsAddFile/MockFsAddDir 播种；FS_OPS 全操作可用。 */
#include "Contract/FsOps.h"
#include "Contract/EfiMem.h"
#include <string.h>
#include <stdio.h>

#define MOCKFS_MAX 256
#define MOCKFS_PATH 512
#define MOCKFS_CAP 65536

typedef struct {
  CHAR16 Path[MOCKFS_PATH];
  UINT8  *Data;
  UINTN  Len;
  BOOLEAN IsDir;
  BOOLEAN Used;
} MOCKFS_NODE;

typedef struct {
  FS_OPS Ops;
  MOCKFS_NODE Nodes[MOCKFS_MAX];
} MOCKFS;

typedef struct {
  MOCKFS *Fs;
  UINTN Idx;
  UINTN Pos;
  UINT32 Mode;
  BOOLEAN Used;
} MOCKFS_HANDLE;

#define MOCKFS_MAXH 32
static MOCKFS_HANDLE g_handles[MOCKFS_MAXH];

static void norm (CONST CHAR16 *In, CHAR16 *Out) {
  UINTN i, j = 0;
  for (i = 0; In[i]; i++) {
    CHAR16 c = In[i] == L'\\' ? L'/' : In[i];
    if (c == L'/' && j > 0 && Out[j-1] == L'/') continue;
    Out[j++] = c;
  }
  if (j > 1 && Out[j-1] == L'/') j--;
  Out[j] = 0;
}
static MOCKFS_NODE *find (MOCKFS *F, CONST CHAR16 *Path) {
  CHAR16 P[MOCKFS_PATH];
  UINTN i;
  norm (Path, P);
  for (i = 0; i < MOCKFS_MAX; i++)
    if (F->Nodes[i].Used && EfiStrCmp (F->Nodes[i].Path, P) == 0)
      return &F->Nodes[i];
  return NULL;
}
static MOCKFS_NODE *alloc_node (MOCKFS *F) {
  UINTN i;
  for (i = 0; i < MOCKFS_MAX; i++) if (!F->Nodes[i].Used) { F->Nodes[i].Used = TRUE; return &F->Nodes[i]; }
  return NULL;
}

void MockFsAddFile (FS_OPS *Fs, CONST CHAR16 *Path, CONST UINT8 *Data, UINTN Len) {
  MOCKFS *F = (MOCKFS *)Fs;
  MOCKFS_NODE *N = find (F, Path);
  if (!N) { N = alloc_node (F); if (!N) return; norm (Path, N->Path); }
  MemFree (N->Data);
  N->Data = MemAlloc (Len ? Len : 1);
  memcpy (N->Data, Data, Len);
  N->Len = Len; N->IsDir = FALSE;
}
void MockFsAddDir (FS_OPS *Fs, CONST CHAR16 *Path) {
  MOCKFS *F = (MOCKFS *)Fs;
  MOCKFS_NODE *N = find (F, Path);
  if (!N) { N = alloc_node (F); if (!N) return; norm (Path, N->Path); }
  N->IsDir = TRUE;
}

static EFI_STATUS mOpen (FS_OPS *Fs, CONST CHAR16 *Path, UINT32 Mode, FS_HANDLE **H) {
  MOCKFS *F = (MOCKFS *)Fs;
  MOCKFS_NODE *N = find (F, Path);
  UINTN i;
  if (!N) {
    if (!(Mode & FS_MODE_CREATE)) return EFI_NOT_FOUND;
    N = alloc_node (F); if (!N) return EFI_OUT_OF_RESOURCES;
    norm (Path, N->Path); N->Data = MemAlloc (1); N->Len = 0; N->IsDir = FALSE;
  }
  if (N->IsDir) return EFI_INVALID_PARAMETER;
  for (i = 0; i < MOCKFS_MAXH; i++) if (!g_handles[i].Used) {
    g_handles[i].Used = TRUE; g_handles[i].Fs = F; g_handles[i].Idx = (UINTN)(N - F->Nodes);
    g_handles[i].Pos = 0; g_handles[i].Mode = Mode;
    *H = (FS_HANDLE *)&g_handles[i];
    return EFI_SUCCESS;
  }
  return EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS mRead (FS_OPS *Fs, FS_HANDLE *H, VOID *Buf, UINTN *Len) {
  MOCKFS_HANDLE *h = (MOCKFS_HANDLE *)H;
  MOCKFS_NODE *N = &h->Fs->Nodes[h->Idx];
  UINTN avail = N->Len > h->Pos ? N->Len - h->Pos : 0;
  UINTN n = *Len < avail ? *Len : avail;
  memcpy (Buf, N->Data + h->Pos, n);
  h->Pos += n; *Len = n;
  return EFI_SUCCESS;
}
static EFI_STATUS mWrite (FS_OPS *Fs, FS_HANDLE *H, CONST VOID *Buf, UINTN *Len) {
  MOCKFS_HANDLE *h = (MOCKFS_HANDLE *)H;
  MOCKFS_NODE *N = &h->Fs->Nodes[h->Idx];
  UINTN need = h->Pos + *Len;
  if (!(h->Mode & FS_MODE_WRITE)) return EFI_ACCESS_DENIED;
  if (need > MOCKFS_CAP) return EFI_BAD_BUFFER_SIZE;
  if (need > N->Len) {
    UINT8 *nd = MemAlloc (need);
    memcpy (nd, N->Data, N->Len);
    MemFree (N->Data); N->Data = nd; N->Len = need;
  }
  memcpy (N->Data + h->Pos, Buf, *Len);
  h->Pos += *Len;
  return EFI_SUCCESS;
}
static EFI_STATUS mClose (FS_OPS *Fs, FS_HANDLE *H) { ((MOCKFS_HANDLE *)H)->Used = FALSE; return EFI_SUCCESS; }
static EFI_STATUS mFlush (FS_OPS *Fs, FS_HANDLE *H) { return EFI_SUCCESS; }
static EFI_STATUS mDelete (FS_OPS *Fs, CONST CHAR16 *Path) {
  MOCKFS *F = (MOCKFS *)Fs;
  MOCKFS_NODE *N = find (F, Path);
  CHAR16 P[MOCKFS_PATH];
  UINTN i, plen;
  if (!N) return EFI_NOT_FOUND;
  norm (Path, P); plen = EfiStrLen (P);
  if (N->IsDir) {
    for (i = 0; i < MOCKFS_MAX; i++) {
      if (!F->Nodes[i].Used || &F->Nodes[i] == N) continue;
      if (EfiStrnCmp (F->Nodes[i].Path, P, plen) == 0 &&
          F->Nodes[i].Path[plen] == L'/') return EFI_ACCESS_DENIED;  /* 非空目录 */
    }
  }
  MemFree (N->Data); N->Data = NULL; N->Used = FALSE;
  return EFI_SUCCESS;
}
static EFI_STATUS mRename (FS_OPS *Fs, CONST CHAR16 *Old, CONST CHAR16 *New) {
  MOCKFS *F = (MOCKFS *)Fs;
  MOCKFS_NODE *N = find (F, Old);
  if (!N) return EFI_NOT_FOUND;
  if (find (F, New)) return EFI_ACCESS_DENIED;
  norm (New, N->Path);
  return EFI_SUCCESS;
}
static EFI_STATUS mMkDir (FS_OPS *Fs, CONST CHAR16 *Path) {
  MOCKFS *F = (MOCKFS *)Fs;
  MOCKFS_NODE *N;
  if (find (F, Path)) return EFI_ACCESS_DENIED;
  N = alloc_node (F); if (!N) return EFI_OUT_OF_RESOURCES;
  norm (Path, N->Path); N->IsDir = TRUE;
  return EFI_SUCCESS;
}
static EFI_STATUS mEnum (FS_OPS *Fs, CONST CHAR16 *Dir, FS_ENTRY **Entries, UINTN *Count) {
  MOCKFS *F = (MOCKFS *)Fs;
  CHAR16 P[MOCKFS_PATH];
  UINTN i, n = 0, plen;
  FS_ENTRY *out;
  norm (Dir, P); plen = EfiStrLen (P);
  for (i = 0; i < MOCKFS_MAX; i++) {
    MOCKFS_NODE *N = &F->Nodes[i];
    if (!N->Used) continue;
    if (plen == 0 || (EfiStrnCmp (N->Path, P, plen) == 0 && N->Path[plen] == L'/')) {
      CONST CHAR16 *rest = N->Path + (plen ? plen + 1 : 0);
      if (!EfiStrChr (rest, L'/')) n++;
    }
  }
  out = MemAlloc ((n ? n : 1) * sizeof (FS_ENTRY));
  n = 0;
  for (i = 0; i < MOCKFS_MAX; i++) {
    MOCKFS_NODE *N = &F->Nodes[i];
    if (!N->Used) continue;
    if (plen == 0 || (EfiStrnCmp (N->Path, P, plen) == 0 && N->Path[plen] == L'/')) {
      CONST CHAR16 *rest = N->Path + (plen ? plen + 1 : 0);
      if (!EfiStrChr (rest, L'/')) {
        UINTN j;
        for (j = 0; rest[j] && j < 255; j++) out[n].Name[j] = rest[j];
        out[n].Name[j] = 0;
        out[n].Size = N->Len; out[n].Mtime = 0; out[n].IsDir = N->IsDir;
        n++;
      }
    }
  }
  /* 目录优先按名排序（简单插入排序） */
  for (i = 1; i < n; i++) {
    FS_ENTRY tmp = out[i];
    UINTN j = i;
    while (j > 0 && (out[j-1].IsDir < tmp.IsDir ||
           (out[j-1].IsDir == tmp.IsDir && EfiStrCmp (out[j-1].Name, tmp.Name) > 0))) {
      out[j] = out[j-1]; j--;
    }
    out[j] = tmp;
  }
  *Entries = out; *Count = n;
  return EFI_SUCCESS;
}
static EFI_STATUS mInfo (FS_OPS *Fs, CONST CHAR16 *Path, FS_ENTRY *Out) {
  MOCKFS_NODE *N = find ((MOCKFS *)Fs, Path);
  UINTN j;
  if (!N) return EFI_NOT_FOUND;
  for (j = 0; N->Path[j] && j < 255; j++) Out->Name[j] = N->Path[j];
  Out->Name[j] = 0;
  Out->Size = N->Len; Out->Mtime = 0; Out->IsDir = N->IsDir;
  return EFI_SUCCESS;
}
static VOID mDestroy (FS_OPS *Fs) {
  MOCKFS *F = (MOCKFS *)Fs;
  UINTN i;
  for (i = 0; i < MOCKFS_MAX; i++) if (F->Nodes[i].Used) MemFree (F->Nodes[i].Data);
  MemFree (F);
}

FS_OPS *MockFsNew (VOID) {
  MOCKFS *F = MemAlloc (sizeof (MOCKFS));
  F->Ops.Open = mOpen; F->Ops.Read = mRead; F->Ops.Write = mWrite;
  F->Ops.Close = mClose; F->Ops.Flush = mFlush; F->Ops.Delete = mDelete;
  F->Ops.Rename = mRename; F->Ops.MkDir = mMkDir; F->Ops.Enum = mEnum;
  F->Ops.Info = mInfo; F->Ops.Destroy = mDestroy;
  return &F->Ops;
}
