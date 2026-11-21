/** @file
  FsOps POSIX 后端（仅宿主编译，不进 .inf；host.mk 引入）。
  真实临时目录文件操作，供应用 core（S11/S12/S13）宿主测试。
  语义对齐 tests/host/mocks/MockFs.c：
    - Open 不截断已有文件（与 UEFI SimpleFileSystem/FAT 一致）；
    - Open 目录 → EFI_INVALID_PARAMETER；
    - Write 需 FS_MODE_WRITE，否则 EFI_ACCESS_DENIED；
    - Delete 非空目录拒绝（EFI_ACCESS_DENIED）；
    - Rename 目标已存在拒绝（EFI_ACCESS_DENIED）；MkDir 已存在拒绝；
    - Enum 目录优先按名（CHAR16 码元序）排序；Info.Name = 规范化输入路径。
  与 mock 的有意差异（真实文件系统语义，见交接注记）：
    - Enum 不存在的目录 → EFI_NOT_FOUND（mock 平面表返回空集成功）。
  安全：路径一律相对卷根；拒绝 ".." 分量（FsNormalizePath）。
  CHAR16 按 UTF-16 码元处理（代理对 ↔ 4 字节 UTF-8），与 FAT/NTFS 文件名一致。
**/
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/FsOps.h"
#include "ExecVolumeInternal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

typedef struct {
  FS_OPS Ops;
  CHAR8  *Root;                     /* UTF-8，无尾 '/' */
  UINTN  RootLen;
} POSIX_FS;

typedef struct {
  POSIX_FS *Fs;
  int      Fd;
  UINT32   Mode;
} POSIX_HANDLE;

/* ---- 编码转换 ---- */

STATIC
EFI_STATUS
U16ToUtf8 (
  CONST CHAR16 *In,
  CHAR8        **Out,
  UINTN        *OutLen
  )
{
  UINTN  i, n = 0;
  CHAR8  *buf;
  UINT32 cp;

  /* 第一遍：计算长度并校验代理对 */
  for (i = 0; In[i] != 0; i++) {
    UINT32 u = In[i];
    if (u >= 0xD800 && u <= 0xDBFF) {
      UINT32 lo = In[i + 1];
      if (lo < 0xDC00 || lo > 0xDFFF) {
        return EFI_INVALID_PARAMETER; /* 孤立高代理 */
      }
      i++;
      n += 4;
    } else if (u >= 0xDC00 && u <= 0xDFFF) {
      return EFI_INVALID_PARAMETER;   /* 孤立低代理 */
    } else if (u < 0x80) {
      n += 1;
    } else if (u < 0x800) {
      n += 2;
    } else {
      n += 3;
    }
  }
  buf = (CHAR8 *)MemAlloc (n + 1);
  if (buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  n = 0;
  for (i = 0; In[i] != 0; i++) {
    UINT32 u = In[i];
    if (u >= 0xD800 && u <= 0xDBFF) {
      cp = 0x10000 + ((u - 0xD800) << 10) + ((UINT32)In[i + 1] - 0xDC00);
      i++;
      buf[n++] = (CHAR8)(0xF0 | (cp >> 18));
      buf[n++] = (CHAR8)(0x80 | ((cp >> 12) & 0x3F));
      buf[n++] = (CHAR8)(0x80 | ((cp >> 6) & 0x3F));
      buf[n++] = (CHAR8)(0x80 | (cp & 0x3F));
    } else if (u < 0x80) {
      buf[n++] = (CHAR8)u;
    } else if (u < 0x800) {
      buf[n++] = (CHAR8)(0xC0 | (u >> 6));
      buf[n++] = (CHAR8)(0x80 | (u & 0x3F));
    } else {
      buf[n++] = (CHAR8)(0xE0 | (u >> 12));
      buf[n++] = (CHAR8)(0x80 | ((u >> 6) & 0x3F));
      buf[n++] = (CHAR8)(0x80 | (u & 0x3F));
    }
  }
  buf[n]   = 0;
  *Out     = buf;
  *OutLen  = n;
  return EFI_SUCCESS;
}

/**
  UTF-8 → CHAR16（写入 Out，容量 Cap 码元含 NUL）。非法字节 → U+FFFD。
  返回写入码元数（不含 NUL）；超长截断。
**/
STATIC
UINTN
Utf8ToU16 (
  CONST CHAR8 *In,
  CHAR16      *Out,
  UINTN       Cap
  )
{
  UINTN        i = 0, n = 0;
  CONST UINT8  *s = (CONST UINT8 *)In;

  while (s[i] != 0 && n + 1 < Cap) {
    UINT32 cp;
    UINTN  adv;
    UINT8  b = s[i];
    if (b < 0x80) {
      cp = b; adv = 1;
    } else if ((b & 0xE0) == 0xC0 && (s[i + 1] & 0xC0) == 0x80) {
      cp = ((UINT32)(b & 0x1F) << 6) | (s[i + 1] & 0x3F);
      adv = 2;
      if (cp < 0x80) { cp = 0xFFFD; }
    } else if ((b & 0xF0) == 0xE0 && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80) {
      cp = ((UINT32)(b & 0x0F) << 12) | ((UINT32)(s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
      adv = 3;
      if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) { cp = 0xFFFD; }
    } else if ((b & 0xF8) == 0xF0 && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80 && (s[i + 3] & 0xC0) == 0x80) {
      cp = ((UINT32)(b & 0x07) << 18) | ((UINT32)(s[i + 1] & 0x3F) << 12) |
           ((UINT32)(s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
      adv = 4;
      if (cp < 0x10000 || cp > 0x10FFFF) { cp = 0xFFFD; }
    } else {
      cp = 0xFFFD; adv = 1;
    }
    if (cp >= 0x10000) {
      if (n + 2 >= Cap) {
        break;
      }
      cp -= 0x10000;
      Out[n++] = (CHAR16)(0xD800 + (cp >> 10));
      Out[n++] = (CHAR16)(0xDC00 + (cp & 0x3FF));
    } else {
      Out[n++] = (CHAR16)cp;
    }
    i += adv;
  }
  Out[n] = 0;
  return n;
}

/* ---- 路径与错误映射 ---- */

STATIC
EFI_STATUS
ErrnoToStatus (
  int Err
  )
{
  switch (Err) {
    case 0:        return EFI_SUCCESS;
    case ENOENT:   return EFI_NOT_FOUND;
    case ENOTDIR:  return EFI_NOT_FOUND;
    case EEXIST:   return EFI_ACCESS_DENIED;
    case ENOTEMPTY:return EFI_ACCESS_DENIED;
    case EACCES:   return EFI_ACCESS_DENIED;
    case EPERM:    return EFI_ACCESS_DENIED;
    case EISDIR:   return EFI_INVALID_PARAMETER;
    case EINVAL:   return EFI_INVALID_PARAMETER;
    case ENOMEM:   return EFI_OUT_OF_RESOURCES;
    default:       return EFI_DEVICE_ERROR;
  }
}

/**
  规范化 CHAR16 路径 → 卷根下 UTF-8 全路径（callee 分配）。
  Norm 为 FsNormalizePath 输出（根 = 空串）。
**/
STATIC
EFI_STATUS
BuildPath (
  POSIX_FS     *F,
  CONST CHAR16 *Norm,
  CHAR8        **Out
  )
{
  CHAR8      *rel = NULL;
  UINTN      relLen = 0;
  CHAR8      *full;
  EFI_STATUS st;

  st = U16ToUtf8 (Norm, &rel, &relLen);
  if (EFI_ERROR (st)) {
    return st;
  }
  full = (CHAR8 *)MemAlloc (F->RootLen + 1 + relLen + 1);
  if (full == NULL) {
    MemFree (rel);
    return EFI_OUT_OF_RESOURCES;
  }
  memcpy (full, F->Root, F->RootLen);
  if (relLen != 0) {
    full[F->RootLen] = '/';
    memcpy (full + F->RootLen + 1, rel, relLen + 1);
  } else {
    full[F->RootLen] = 0;
  }
  MemFree (rel);
  *Out = full;
  return EFI_SUCCESS;
}

/** 规范化 + 建路径一步到位；*Norm 与 *Full 均由 callee 分配。 */
STATIC
EFI_STATUS
PrepPath (
  POSIX_FS     *F,
  CONST CHAR16 *Path,
  CHAR16       **Norm,
  CHAR8        **Full
  )
{
  EFI_STATUS st = FsNormalizePath (Path, Norm);
  if (EFI_ERROR (st)) {
    return st;
  }
  st = BuildPath (F, *Norm, Full);
  if (EFI_ERROR (st)) {
    MemFree (*Norm);
    *Norm = NULL;
  }
  return st;
}

/* ---- FsOps 实现 ---- */

STATIC
EFI_STATUS
pOpen (
  FS_OPS      *Fs,
  CONST CHAR16 *Path,
  UINT32      Mode,
  FS_HANDLE   **H
  )
{
  POSIX_FS     *F = (POSIX_FS *)Fs;
  POSIX_HANDLE *h;
  CHAR16       *norm = NULL;
  CHAR8        *full = NULL;
  struct stat  sb;
  int          flags;
  EFI_STATUS   st;

  if (H == NULL || Mode == 0 || (Mode & ~(FS_MODE_READ | FS_MODE_WRITE | FS_MODE_CREATE)) != 0) {
    return EFI_INVALID_PARAMETER;
  }
  st = PrepPath (F, Path, &norm, &full);
  if (EFI_ERROR (st)) {
    return st;
  }
  if (norm[0] == 0) {
    MemFree (norm);
    MemFree (full);
    return EFI_INVALID_PARAMETER;   /* 卷根不可 Open */
  }
  if (stat (full, &sb) == 0 && S_ISDIR (sb.st_mode)) {
    MemFree (norm);
    MemFree (full);
    return EFI_INVALID_PARAMETER;   /* 目录不可 Open（对齐 mock；枚举走 Enum） */
  }
  flags = (Mode & (FS_MODE_WRITE | FS_MODE_CREATE)) ? O_RDWR : O_RDONLY;
  if (Mode & FS_MODE_CREATE) {
    flags |= O_CREAT;
  }
  h = (POSIX_HANDLE *)MemAlloc (sizeof (POSIX_HANDLE));
  if (h == NULL) {
    MemFree (norm);
    MemFree (full);
    return EFI_OUT_OF_RESOURCES;
  }
  h->Fd = open (full, flags, 0666);
  if (h->Fd < 0) {
    st = ErrnoToStatus (errno);
    MemFree (h);
    MemFree (norm);
    MemFree (full);
    return st;
  }
  h->Fs   = F;
  h->Mode = Mode;
  MemFree (norm);
  MemFree (full);
  *H = (FS_HANDLE *)h;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
pRead (
  FS_OPS    *Fs,
  FS_HANDLE *H,
  VOID      *Buf,
  UINTN     *Len
  )
{
  POSIX_HANDLE *h = (POSIX_HANDLE *)H;
  UINTN        done = 0;
  UINTN        cap;

  (VOID)Fs;
  if (h == NULL || Buf == NULL || Len == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  cap = *Len;
  while (done < cap) {
    ssize_t r = read (h->Fd, (UINT8 *)Buf + done, cap - done);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      *Len = done;
      return ErrnoToStatus (errno);
    }
    if (r == 0) {
      break;                        /* EOF */
    }
    done += (UINTN)r;
  }
  *Len = done;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
pWrite (
  FS_OPS     *Fs,
  FS_HANDLE  *H,
  CONST VOID *Buf,
  UINTN      *Len
  )
{
  POSIX_HANDLE *h = (POSIX_HANDLE *)H;
  UINTN        done = 0;
  UINTN        total;

  (VOID)Fs;
  if (h == NULL || Buf == NULL || Len == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (!(h->Mode & FS_MODE_WRITE)) {
    return EFI_ACCESS_DENIED;
  }
  total = *Len;
  while (done < total) {
    ssize_t r = write (h->Fd, (CONST UINT8 *)Buf + done, total - done);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      *Len = done;
      return ErrnoToStatus (errno);
    }
    done += (UINTN)r;
  }
  *Len = done;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
pClose (
  FS_OPS    *Fs,
  FS_HANDLE *H
  )
{
  POSIX_HANDLE *h = (POSIX_HANDLE *)H;

  (VOID)Fs;
  if (h == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (close (h->Fd) < 0) {
    EFI_STATUS st = ErrnoToStatus (errno);
    MemFree (h);
    return st;
  }
  MemFree (h);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
pFlush (
  FS_OPS    *Fs,
  FS_HANDLE *H
  )
{
  POSIX_HANDLE *h = (POSIX_HANDLE *)H;

  (VOID)Fs;
  if (h == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (fsync (h->Fd) < 0) {
    return ErrnoToStatus (errno);
  }
  return EFI_SUCCESS;
}

/** 目录是否为空（跳过 "." / ".."）。*Empty 输出；错误经返回值。 */
STATIC
EFI_STATUS
DirIsEmpty (
  CONST CHAR8 *Path,
  BOOLEAN     *Empty
  )
{
  DIR           *d = opendir (Path);
  struct dirent *de;
  BOOLEAN       empty = TRUE;

  if (d == NULL) {
    return ErrnoToStatus (errno);
  }
  while ((de = readdir (d)) != NULL) {
    if (strcmp (de->d_name, ".") == 0 || strcmp (de->d_name, "..") == 0) {
      continue;
    }
    empty = FALSE;
    break;
  }
  closedir (d);
  *Empty = empty;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
pDelete (
  FS_OPS      *Fs,
  CONST CHAR16 *Path
  )
{
  POSIX_FS    *F = (POSIX_FS *)Fs;
  CHAR16      *norm = NULL;
  CHAR8       *full = NULL;
  struct stat sb;
  EFI_STATUS  st;
  int         r;

  st = PrepPath (F, Path, &norm, &full);
  if (EFI_ERROR (st)) {
    return st;
  }
  if (norm[0] == 0) {
    MemFree (norm);
    MemFree (full);
    return EFI_INVALID_PARAMETER;   /* 卷根不可删 */
  }
  if (stat (full, &sb) != 0) {
    st = ErrnoToStatus (errno);
  } else if (S_ISDIR (sb.st_mode)) {
    BOOLEAN empty = FALSE;
    st = DirIsEmpty (full, &empty);
    if (!EFI_ERROR (st) && !empty) {
      st = EFI_ACCESS_DENIED;       /* 非空目录拒绝 */
    }
    if (!EFI_ERROR (st)) {
      r = rmdir (full);
      st = (r == 0) ? EFI_SUCCESS : ErrnoToStatus (errno);
    }
  } else {
    r = unlink (full);
    st = (r == 0) ? EFI_SUCCESS : ErrnoToStatus (errno);
  }
  MemFree (norm);
  MemFree (full);
  return st;
}

STATIC
EFI_STATUS
pRename (
  FS_OPS      *Fs,
  CONST CHAR16 *Old,
  CONST CHAR16 *New
  )
{
  POSIX_FS    *F = (POSIX_FS *)Fs;
  CHAR16      *normO = NULL, *normN = NULL;
  CHAR8       *fullO = NULL, *fullN = NULL;
  struct stat sb;
  EFI_STATUS  st;

  st = PrepPath (F, Old, &normO, &fullO);
  if (EFI_ERROR (st)) {
    return st;
  }
  st = PrepPath (F, New, &normN, &fullN);
  if (EFI_ERROR (st)) {
    MemFree (normO);
    MemFree (fullO);
    return st;
  }
  if (normO[0] == 0 || normN[0] == 0) {
    MemFree (normO);
    MemFree (normN);
    MemFree (fullO);
    MemFree (fullN);
    return EFI_INVALID_PARAMETER;   /* 卷根不可改名 */
  }
  if (stat (fullO, &sb) != 0) {
    st = ErrnoToStatus (errno);
  } else if (stat (fullN, &sb) == 0) {
    st = EFI_ACCESS_DENIED;         /* 目标已存在拒绝（对齐 mock；不覆盖） */
  } else if (errno != ENOENT && errno != ENOTDIR) {
    st = ErrnoToStatus (errno);
  } else {
    st = (rename (fullO, fullN) == 0) ? EFI_SUCCESS : ErrnoToStatus (errno);
  }
  MemFree (normO);
  MemFree (normN);
  MemFree (fullO);
  MemFree (fullN);
  return st;
}

STATIC
EFI_STATUS
pMkDir (
  FS_OPS      *Fs,
  CONST CHAR16 *Path
  )
{
  POSIX_FS   *F = (POSIX_FS *)Fs;
  CHAR16     *norm = NULL;
  CHAR8      *full = NULL;
  EFI_STATUS st;

  st = PrepPath (F, Path, &norm, &full);
  if (EFI_ERROR (st)) {
    return st;
  }
  if (norm[0] == 0) {
    MemFree (norm);
    MemFree (full);
    return EFI_INVALID_PARAMETER;   /* 卷根已存在 */
  }
  st = (mkdir (full, 0777) == 0) ? EFI_SUCCESS : ErrnoToStatus (errno);
  MemFree (norm);
  MemFree (full);
  return st;
}

STATIC
INTN
EntryCompare (
  CONST FS_ENTRY *A,
  CONST FS_ENTRY *B
  )
{
  if (A->IsDir != B->IsDir) {
    return A->IsDir ? -1 : 1;
  }
  return EfiStrCmp (A->Name, B->Name);
}

STATIC
EFI_STATUS
pEnum (
  FS_OPS      *Fs,
  CONST CHAR16 *Dir,
  FS_ENTRY    **Entries,
  UINTN       *Count
  )
{
  POSIX_FS      *F = (POSIX_FS *)Fs;
  CHAR16        *norm = NULL;
  CHAR8         *full = NULL;
  DIR           *d;
  struct dirent *de;
  FS_ENTRY      *out = NULL;
  UINTN         n = 0, cap = 0;
  EFI_STATUS    st;
  UINTN         i, j;

  if (Entries == NULL || Count == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  /* 空路径 / "/" = 卷根（FsNormalizePath 对 "/" 输出空串） */
  if (Dir == NULL || Dir[0] == 0) {
    norm = (CHAR16 *)MemAlloc (sizeof (CHAR16));
    if (norm == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    norm[0] = 0;
    st = BuildPath (F, norm, &full);
  } else {
    st = PrepPath (F, Dir, &norm, &full);
  }
  if (EFI_ERROR (st)) {
    MemFree (norm);
    return st;
  }
  d = opendir (full);
  if (d == NULL) {
    st = ErrnoToStatus (errno);
    MemFree (norm);
    MemFree (full);
    return st;
  }
  st = EFI_SUCCESS;
  while ((de = readdir (d)) != NULL) {
    CHAR8       *child;
    UINTN       flen = strlen (full);
    UINTN       nlen = strlen (de->d_name);
    struct stat sb;
    FS_ENTRY    e;

    if (strcmp (de->d_name, ".") == 0 || strcmp (de->d_name, "..") == 0) {
      continue;
    }
    child = (CHAR8 *)MemAlloc (flen + 1 + nlen + 1);
    if (child == NULL) {
      st = EFI_OUT_OF_RESOURCES;
      break;
    }
    memcpy (child, full, flen);
    child[flen] = '/';
    memcpy (child + flen + 1, de->d_name, nlen + 1);
    if (stat (child, &sb) != 0) {
      MemFree (child);
      continue;                     /* 竞态消失的条目跳过 */
    }
    MemFree (child);
    memset (&e, 0, sizeof (e));
    Utf8ToU16 (de->d_name, e.Name, 256);
    e.Size  = S_ISDIR (sb.st_mode) ? 0 : (UINT64)sb.st_size;
    e.Mtime = (UINT64)sb.st_mtime;
    e.IsDir = (BOOLEAN)(S_ISDIR (sb.st_mode) ? TRUE : FALSE);
    if (n == cap) {
      UINTN    ncap = cap ? cap * 2 : 16;
      FS_ENTRY *nb  = (FS_ENTRY *)MemRealloc (out, ncap * sizeof (FS_ENTRY));
      if (nb == NULL) {
        st = EFI_OUT_OF_RESOURCES;
        break;
      }
      out = nb;
      cap = ncap;
    }
    out[n++] = e;
  }
  closedir (d);
  MemFree (norm);
  MemFree (full);
  if (EFI_ERROR (st)) {
    MemFree (out);
    return st;
  }
  /* 目录优先按名排序（稳定插入排序，对齐 mock） */
  for (i = 1; i < n; i++) {
    FS_ENTRY tmp = out[i];
    j = i;
    while (j > 0 && EntryCompare (&out[j - 1], &tmp) > 0) {
      out[j] = out[j - 1];
      j--;
    }
    out[j] = tmp;
  }
  if (out == NULL) {
    out = (FS_ENTRY *)MemAlloc (sizeof (FS_ENTRY));
    if (out == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
  }
  *Entries = out;
  *Count   = n;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
pInfo (
  FS_OPS      *Fs,
  CONST CHAR16 *Path,
  FS_ENTRY    *Out
  )
{
  POSIX_FS    *F = (POSIX_FS *)Fs;
  CHAR16      *norm = NULL;
  CHAR8       *full = NULL;
  struct stat sb;
  EFI_STATUS  st;
  UINTN       i;

  if (Out == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  st = PrepPath (F, Path, &norm, &full);
  if (EFI_ERROR (st)) {
    return st;
  }
  if (norm[0] == 0) {
    MemFree (norm);
    MemFree (full);
    return EFI_INVALID_PARAMETER;   /* 卷根 Info 不支持（对齐 UEFI 后端） */
  }
  if (stat (full, &sb) != 0) {
    st = ErrnoToStatus (errno);
  } else {
    for (i = 0; norm[i] != 0 && i < 255; i++) {
      Out->Name[i] = norm[i];
    }
    Out->Name[i] = 0;
    Out->Size  = S_ISDIR (sb.st_mode) ? 0 : (UINT64)sb.st_size;
    Out->Mtime = (UINT64)sb.st_mtime;
    Out->IsDir = (BOOLEAN)(S_ISDIR (sb.st_mode) ? TRUE : FALSE);
  }
  MemFree (norm);
  MemFree (full);
  return st;
}

STATIC
VOID
pDestroy (
  FS_OPS *Fs
  )
{
  POSIX_FS *F = (POSIX_FS *)Fs;

  if (F == NULL) {
    return;
  }
  MemFree (F->Root);
  MemFree (F);
}

FS_OPS *
FsPosixBackend (
  CONST CHAR8 *RootDir
  )
{
  POSIX_FS    *F;
  struct stat sb;
  UINTN       len;

  if (RootDir == NULL || RootDir[0] == 0) {
    return NULL;
  }
  if (stat (RootDir, &sb) != 0 || !S_ISDIR (sb.st_mode)) {
    return NULL;
  }
  len = strlen (RootDir);
  while (len > 1 && RootDir[len - 1] == '/') {
    len--;
  }
  F = (POSIX_FS *)MemAlloc (sizeof (POSIX_FS));
  if (F == NULL) {
    return NULL;
  }
  F->Root = (CHAR8 *)MemAlloc (len + 1);
  if (F->Root == NULL) {
    MemFree (F);
    return NULL;
  }
  memcpy (F->Root, RootDir, len);
  F->Root[len] = 0;
  F->RootLen   = len;
  F->Ops.Open    = pOpen;
  F->Ops.Read    = pRead;
  F->Ops.Write   = pWrite;
  F->Ops.Close   = pClose;
  F->Ops.Flush   = pFlush;
  F->Ops.Delete  = pDelete;
  F->Ops.Rename  = pRename;
  F->Ops.MkDir   = pMkDir;
  F->Ops.Enum    = pEnum;
  F->Ops.Info    = pInfo;
  F->Ops.Destroy = pDestroy;
  return &F->Ops;
}
