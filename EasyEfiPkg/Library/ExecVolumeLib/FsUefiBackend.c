/** @file
  FsOps UEFI 后端（仅 UEFI 编译，进 .inf）：EFI_SIMPLE_FILE_SYSTEM_PROTOCOL +
  EFI_FILE_PROTOCOL 真实实现。NTFS 卷依赖已加载的 ntfs-3g 驱动（S14/S15 职责，
  本库不加载驱动）。语义对齐 MockFs / POSIX 后端：
    - Open 不截断（UEFI FAT 原生语义）；Open 目录 → EFI_INVALID_PARAMETER；
    - Write 需 FS_MODE_WRITE；Delete 非空目录拒绝；Rename 目标存在拒绝；
    - Enum 目录优先按名排序；Info.Name = 规范化输入路径。
  UEFI 协议适配要点（edk2-stable202608 实证）：
    - FatOpenEx 仅接受 READ / READ|WRITE / READ|WRITE|CREATE → 协议模式恒含 READ；
    - EFI_FILE_PROTOCOL 无 Rename 方法 → 经 SetInfo(EFI_FILE_INFO) 改 FileName，
      新名相对父目录 → 跨目录改名返回 EFI_UNSUPPORTED（FM F6 为同目录改名）；
    - Enum 用迭代 Read（FileInfo GUID，rev1 兼容；FAT/ntfs-3g 均支持）。
**/
#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/FsOps.h"
#include "ExecVolumeInternal.h"

typedef struct {
  FS_OPS                          Ops;
  EFI_HANDLE                      Handle;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs;
  EFI_FILE_PROTOCOL               *Root;   /* 所有权归本后端 */
} UEFI_FS;

typedef struct {
  UEFI_FS           *Fs;
  EFI_FILE_PROTOCOL *File;
  UINT32            Mode;
} UEFI_HANDLE;

/* ---- 助手 ---- */

/** 规范化路径 → UEFI 反斜杠路径（callee 分配）。空串（根）输出空串。 */
STATIC
EFI_STATUS
PathToUefi (
  CONST CHAR16 *In,
  CHAR16       **Out
  )
{
  CHAR16     *norm = NULL;
  CHAR16     *buf;
  UINTN      i, len;
  EFI_STATUS st;

  st = FsNormalizePath (In, &norm);
  if (EFI_ERROR (st)) {
    return st;
  }
  len = EfiStrLen (norm);
  buf = (CHAR16 *)MemAlloc ((len + 1) * sizeof (CHAR16));
  if (buf == NULL) {
    MemFree (norm);
    return EFI_OUT_OF_RESOURCES;
  }
  for (i = 0; i < len; i++) {
    buf[i] = (norm[i] == (CHAR16)'/') ? (CHAR16)'\\' : norm[i];
  }
  buf[len] = 0;
  MemFree (norm);
  *Out = buf;
  return EFI_SUCCESS;
}

/** 两段式 GetInfo：先问尺寸再取内容（callee 分配）。 */
STATIC
EFI_STATUS
GetInfoAlloc (
  EFI_FILE_PROTOCOL *File,
  EFI_GUID          *Guid,
  VOID              **Buf,
  UINTN             *Size
  )
{
  UINTN      sz = 0;
  VOID       *b;
  EFI_STATUS st;

  st = File->GetInfo (File, Guid, &sz, NULL);
  if (st != EFI_BUFFER_TOO_SMALL) {
    return EFI_ERROR (st) ? st : EFI_DEVICE_ERROR;
  }
  b = MemAlloc (sz);
  if (b == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  st = File->GetInfo (File, Guid, &sz, b);
  if (EFI_ERROR (st)) {
    MemFree (b);
    return st;
  }
  *Buf  = b;
  *Size = sz;
  return EFI_SUCCESS;
}

/** EFI_TIME → Unix 秒（days-from-civil；无效/1970 前 → 0）。 */
STATIC
UINT64
EfiTimeToUnix (
  CONST EFI_TIME *T
  )
{
  INT64  y, era, days;
  UINT64 m, d, yoe, doy, doe;

  if (T->Year < 1970 || T->Month < 1 || T->Month > 12 || T->Day < 1 || T->Day > 31) {
    return 0;
  }
  y = T->Year;
  m = T->Month;
  d = T->Day;
  if (m <= 2) {
    y--;
  }
  era  = (y >= 0 ? y : y - 399) / 400;
  yoe  = (UINT64)(y - era * 400);
  doy  = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe  = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  days = era * 146097 + (INT64)doe - 719468;
  if (days < 0) {
    return 0;
  }
  return (UINT64)days * 86400 + (UINT64)T->Hour * 3600 + (UINT64)T->Minute * 60 + T->Second;
}

STATIC
BOOLEAN
IsDotName (
  CONST CHAR16 *Name
  )
{
  return (BOOLEAN)(EfiStrCmp (Name, U16 (".")) == 0 || EfiStrCmp (Name, U16 ("..")) == 0);
}

/** 目录是否含子条目（跳过 "." / ".."）。迭代 Read，rev1 兼容。 */
STATIC
EFI_STATUS
DirHasChildren (
  EFI_FILE_PROTOCOL *Dir,
  BOOLEAN           *Has
  )
{
  EFI_STATUS    st;
  UINTN         sz;
  EFI_FILE_INFO *info;
  BOOLEAN       has = FALSE;

  for (;;) {
    sz = 0;
    st = Dir->Read (Dir, &sz, NULL);
    if (st == EFI_BUFFER_TOO_SMALL) {
      info = (EFI_FILE_INFO *)MemAlloc (sz);
      if (info == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }
      st = Dir->Read (Dir, &sz, info);
      if (EFI_ERROR (st)) {
        MemFree (info);
        return st;
      }
      if (sz == 0) {
        MemFree (info);
        break;                      /* 目录遍历结束 */
      }
      if (!IsDotName (info->FileName)) {
        has = TRUE;
        MemFree (info);
        break;
      }
      MemFree (info);
    } else if (!EFI_ERROR (st) && sz == 0) {
      break;
    } else {
      return EFI_ERROR (st) ? st : EFI_DEVICE_ERROR;
    }
  }
  *Has = has;
  return EFI_SUCCESS;
}

/** 打开卷根的新句柄（Enum 根目录用；不扰动后端持有的 Root 游标）。 */
STATIC
EFI_STATUS
OpenFreshRoot (
  UEFI_FS           *F,
  EFI_FILE_PROTOCOL **Root
  )
{
  return F->Sfs->OpenVolume (F->Sfs, Root);
}

/* ---- FsOps 实现 ---- */

STATIC
EFI_STATUS
uOpen (
  FS_OPS       *Fs,
  CONST CHAR16 *Path,
  UINT32       Mode,
  FS_HANDLE    **H
  )
{
  UEFI_FS       *F = (UEFI_FS *)Fs;
  UEFI_HANDLE   *h;
  CHAR16        *upath = NULL;
  EFI_FILE_INFO *info = NULL;
  UINTN         sz;
  UINT64        pmode;
  EFI_STATUS    st;

  if (H == NULL || Mode == 0 || (Mode & ~(FS_MODE_READ | FS_MODE_WRITE | FS_MODE_CREATE)) != 0) {
    return EFI_INVALID_PARAMETER;
  }
  st = PathToUefi (Path, &upath);
  if (EFI_ERROR (st)) {
    return st;
  }
  if (upath[0] == 0) {
    MemFree (upath);
    return EFI_INVALID_PARAMETER;   /* 根目录不可 Open */
  }
  /* FatOpenEx 只接受 READ / READ|WRITE / READ|WRITE|CREATE */
  pmode = EFI_FILE_MODE_READ;
  if (Mode & (FS_MODE_WRITE | FS_MODE_CREATE)) {
    pmode |= EFI_FILE_MODE_WRITE;
  }
  if (Mode & FS_MODE_CREATE) {
    pmode |= EFI_FILE_MODE_CREATE;
  }
  h = (UEFI_HANDLE *)MemAlloc (sizeof (UEFI_HANDLE));
  if (h == NULL) {
    MemFree (upath);
    return EFI_OUT_OF_RESOURCES;
  }
  st = F->Root->Open (F->Root, &h->File, upath, pmode, 0);
  MemFree (upath);
  if (EFI_ERROR (st)) {
    MemFree (h);
    return st;
  }
  /* 目录拒绝（对齐 mock）：取 FileInfo 判属性 */
  st = GetInfoAlloc (h->File, &gEfiFileInfoGuid, (VOID **)&info, &sz);
  if (EFI_ERROR (st)) {
    h->File->Close (h->File);
    MemFree (h);
    return st;
  }
  if ((info->Attribute & EFI_FILE_DIRECTORY) != 0) {
    MemFree (info);
    h->File->Close (h->File);
    MemFree (h);
    return EFI_INVALID_PARAMETER;
  }
  MemFree (info);
  h->Fs   = F;
  h->Mode = Mode;
  *H = (FS_HANDLE *)h;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
uRead (
  FS_OPS    *Fs,
  FS_HANDLE *H,
  VOID      *Buf,
  UINTN     *Len
  )
{
  UEFI_HANDLE *h = (UEFI_HANDLE *)H;

  (VOID)Fs;
  if (h == NULL || Buf == NULL || Len == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  return h->File->Read (h->File, Len, Buf);
}

STATIC
EFI_STATUS
uWrite (
  FS_OPS     *Fs,
  FS_HANDLE  *H,
  CONST VOID *Buf,
  UINTN      *Len
  )
{
  UEFI_HANDLE *h = (UEFI_HANDLE *)H;

  (VOID)Fs;
  if (h == NULL || Buf == NULL || Len == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (!(h->Mode & FS_MODE_WRITE)) {
    return EFI_ACCESS_DENIED;
  }
  return h->File->Write (h->File, Len, (VOID *)Buf);
}

STATIC
EFI_STATUS
uClose (
  FS_OPS    *Fs,
  FS_HANDLE *H
  )
{
  UEFI_HANDLE *h = (UEFI_HANDLE *)H;
  EFI_STATUS  st;

  (VOID)Fs;
  if (h == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  st = h->File->Close (h->File);
  MemFree (h);
  return st;
}

STATIC
EFI_STATUS
uFlush (
  FS_OPS    *Fs,
  FS_HANDLE *H
  )
{
  UEFI_HANDLE *h = (UEFI_HANDLE *)H;

  (VOID)Fs;
  if (h == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  return h->File->Flush (h->File);
}

STATIC
EFI_STATUS
uDelete (
  FS_OPS       *Fs,
  CONST CHAR16 *Path
  )
{
  UEFI_FS           *F = (UEFI_FS *)Fs;
  CHAR16            *upath = NULL;
  EFI_FILE_PROTOCOL *file = NULL;
  EFI_FILE_INFO     *info = NULL;
  UINTN             sz;
  EFI_STATUS        st;

  st = PathToUefi (Path, &upath);
  if (EFI_ERROR (st)) {
    return st;
  }
  if (upath[0] == 0) {
    MemFree (upath);
    return EFI_INVALID_PARAMETER;   /* 卷根不可删 */
  }
  st = F->Root->Open (F->Root, &file, upath, EFI_FILE_MODE_READ, 0);
  MemFree (upath);
  if (EFI_ERROR (st)) {
    return st;
  }
  st = GetInfoAlloc (file, &gEfiFileInfoGuid, (VOID **)&info, &sz);
  if (EFI_ERROR (st)) {
    file->Close (file);
    return st;
  }
  if ((info->Attribute & EFI_FILE_DIRECTORY) != 0) {
    BOOLEAN has = FALSE;
    MemFree (info);
    st = DirHasChildren (file, &has);
    if (EFI_ERROR (st)) {
      file->Close (file);
      return st;
    }
    if (has) {
      file->Close (file);
      return EFI_ACCESS_DENIED;     /* 非空目录拒绝 */
    }
  } else {
    MemFree (info);
  }
  st = file->Delete (file);         /* 成功即关闭句柄 */
  if (EFI_ERROR (st)) {
    file->Close (file);
  }
  return st;
}

/** 取路径的父目录分量与基名（规范化后；callee 分配 Parent，Base 指向 Parent 内或独立）。 */
STATIC
VOID
SplitPath (
  CONST CHAR16 *Norm,
  CHAR16       **Parent,
  CONST CHAR16 **Base
  )
{
  UINTN  len = EfiStrLen (Norm);
  UINTN  i;
  CHAR16 *p;

  *Base = Norm;
  for (i = len; i > 0; i--) {
    if (Norm[i - 1] == (CHAR16)'/') {
      break;
    }
  }
  /* i = 最后一个 '/' 之后的位置（基名起点）；无 '/' 则 i=0 */
  *Base = Norm + i;
  p = (CHAR16 *)MemAlloc ((i + 1) * sizeof (CHAR16));
  if (p == NULL) {
    *Parent = NULL;
    return;
  }
  if (i > 0) {
    CopyMem (p, Norm, (i - 1) * sizeof (CHAR16));
  }
  p[i > 0 ? i - 1 : 0] = 0;
  *Parent = p;
}

STATIC
EFI_STATUS
uRename (
  FS_OPS       *Fs,
  CONST CHAR16 *Old,
  CONST CHAR16 *New
  )
{
  UEFI_FS           *F = (UEFI_FS *)Fs;
  CHAR16            *normO = NULL, *normN = NULL;
  CHAR16            *upathO = NULL, *upathN = NULL;
  CHAR16            *parentO = NULL, *parentN = NULL;
  CONST CHAR16      *baseO, *baseN;
  EFI_FILE_PROTOCOL *file = NULL, *probe = NULL;
  EFI_FILE_INFO     *info = NULL, *set = NULL;
  UINTN             sz, baseLen, setSz;
  EFI_STATUS        st;

  st = FsNormalizePath (Old, &normO);
  if (EFI_ERROR (st)) {
    return st;
  }
  st = FsNormalizePath (New, &normN);
  if (EFI_ERROR (st)) {
    MemFree (normO);
    return st;
  }
  if (normO[0] == 0 || normN[0] == 0) {
    st = EFI_INVALID_PARAMETER;
    goto Done;
  }
  SplitPath (normO, &parentO, &baseO);
  SplitPath (normN, &parentN, &baseN);
  (VOID)baseO;
  if (parentO == NULL || parentN == NULL) {
    st = EFI_OUT_OF_RESOURCES;
    goto Done;
  }
  if (EfiStrCmp (parentO, parentN) != 0) {
    st = EFI_UNSUPPORTED;           /* SetInfo 改名相对父目录，不支持跨目录 */
    goto Done;
  }
  /* 目标已存在拒绝（对齐 mock） */
  st = PathToUefi (New, &upathN);
  if (EFI_ERROR (st)) {
    goto Done;
  }
  st = F->Root->Open (F->Root, &probe, upathN, EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (st)) {
    probe->Close (probe);
    st = EFI_ACCESS_DENIED;
    goto Done;
  }
  if (st != EFI_NOT_FOUND) {
    goto Done;                      /* 其它错误（如父目录缺失）原样上报 */
  }
  st = PathToUefi (Old, &upathO);
  if (EFI_ERROR (st)) {
    goto Done;
  }
  st = F->Root->Open (F->Root, &file, upathO, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (EFI_ERROR (st)) {
    goto Done;
  }
  st = GetInfoAlloc (file, &gEfiFileInfoGuid, (VOID **)&info, &sz);
  if (EFI_ERROR (st)) {
    goto Done;
  }
  baseLen = EfiStrLen (baseN);
  setSz   = SIZE_OF_EFI_FILE_INFO + (baseLen + 1) * sizeof (CHAR16);
  set = (EFI_FILE_INFO *)MemAlloc (setSz);
  if (set == NULL) {
    st = EFI_OUT_OF_RESOURCES;
    goto Done;
  }
  set->Size         = setSz;
  set->FileSize     = info->FileSize;
  set->PhysicalSize = info->PhysicalSize;
  CopyMem (&set->CreateTime, &info->CreateTime, sizeof (EFI_TIME));
  CopyMem (&set->LastAccessTime, &info->LastAccessTime, sizeof (EFI_TIME));
  CopyMem (&set->ModificationTime, &info->ModificationTime, sizeof (EFI_TIME));
  set->Attribute = info->Attribute;
  CopyMem (set->FileName, baseN, (baseLen + 1) * sizeof (CHAR16));
  st = file->SetInfo (file, &gEfiFileInfoGuid, setSz, set);

Done:
  if (file != NULL) {
    file->Close (file);
  }
  MemFree (set);
  MemFree (info);
  MemFree (upathO);
  MemFree (upathN);
  MemFree (parentO);
  MemFree (parentN);
  MemFree (normO);
  MemFree (normN);
  return st;
}

STATIC
EFI_STATUS
uMkDir (
  FS_OPS       *Fs,
  CONST CHAR16 *Path
  )
{
  UEFI_FS           *F = (UEFI_FS *)Fs;
  CHAR16            *upath = NULL;
  EFI_FILE_PROTOCOL *file = NULL;
  EFI_STATUS        st;

  st = PathToUefi (Path, &upath);
  if (EFI_ERROR (st)) {
    return st;
  }
  if (upath[0] == 0) {
    MemFree (upath);
    return EFI_INVALID_PARAMETER;
  }
  /* 已存在（文件或目录）拒绝（对齐 mock） */
  st = F->Root->Open (F->Root, &file, upath, EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (st)) {
    file->Close (file);
    MemFree (upath);
    return EFI_ACCESS_DENIED;
  }
  if (st != EFI_NOT_FOUND) {
    MemFree (upath);
    return st;
  }
  st = F->Root->Open (F->Root, &file, upath,
                      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                      EFI_FILE_DIRECTORY);
  MemFree (upath);
  if (EFI_ERROR (st)) {
    return st;
  }
  return file->Close (file);
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
uEnum (
  FS_OPS       *Fs,
  CONST CHAR16 *Dir,
  FS_ENTRY     **Entries,
  UINTN        *Count
  )
{
  UEFI_FS           *F = (UEFI_FS *)Fs;
  CHAR16            *upath = NULL;
  EFI_FILE_PROTOCOL *dir = NULL;
  EFI_FILE_INFO     *info;
  FS_ENTRY          *out = NULL;
  UINTN             n = 0, cap = 0, sz, i, j;
  EFI_STATUS        st;

  if (Entries == NULL || Count == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (Dir == NULL || Dir[0] == 0) {
    st = OpenFreshRoot (F, &dir);
  } else {
    st = PathToUefi (Dir, &upath);
    if (EFI_ERROR (st)) {
      return st;
    }
    if (upath[0] == 0) {
      st = OpenFreshRoot (F, &dir);
    } else {
      st = F->Root->Open (F->Root, &dir, upath, EFI_FILE_MODE_READ, 0);
    }
    MemFree (upath);
  }
  if (EFI_ERROR (st)) {
    return st;
  }
  for (;;) {
    sz = 0;
    st = dir->Read (dir, &sz, NULL);
    if (st == EFI_BUFFER_TOO_SMALL) {
      info = (EFI_FILE_INFO *)MemAlloc (sz);
      if (info == NULL) {
        st = EFI_OUT_OF_RESOURCES;
        break;
      }
      st = dir->Read (dir, &sz, info);
      if (EFI_ERROR (st)) {
        MemFree (info);
        break;
      }
      if (sz == 0) {
        MemFree (info);
        break;                      /* 遍历结束 */
      }
      if (!IsDotName (info->FileName)) {
        FS_ENTRY e;
        UINTN    k;
        ZeroMem (&e, sizeof (e));
        for (k = 0; info->FileName[k] != 0 && k < 255; k++) {
          e.Name[k] = info->FileName[k];
        }
        e.Name[k] = 0;
        e.IsDir = (BOOLEAN)((info->Attribute & EFI_FILE_DIRECTORY) != 0);
        e.Size  = e.IsDir ? 0 : info->FileSize;
        e.Mtime = EfiTimeToUnix (&info->ModificationTime);
        if (n == cap) {
          UINTN    ncap = cap ? cap * 2 : 16;
          FS_ENTRY *nb = (FS_ENTRY *)MemRealloc (out, ncap * sizeof (FS_ENTRY));
          if (nb == NULL) {
            MemFree (info);
            st = EFI_OUT_OF_RESOURCES;
            break;
          }
          out = nb;
          cap = ncap;
        }
        out[n++] = e;
      }
      MemFree (info);
    } else if (!EFI_ERROR (st) && sz == 0) {
      break;
    } else {
      st = EFI_ERROR (st) ? st : EFI_DEVICE_ERROR;
      break;
    }
  }
  dir->Close (dir);
  if (EFI_ERROR (st)) {
    MemFree (out);
    return st;
  }
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
uInfo (
  FS_OPS       *Fs,
  CONST CHAR16 *Path,
  FS_ENTRY     *Out
  )
{
  UEFI_FS           *F = (UEFI_FS *)Fs;
  CHAR16            *norm = NULL;
  CHAR16            *upath = NULL;
  EFI_FILE_PROTOCOL *file = NULL;
  EFI_FILE_INFO     *info = NULL;
  UINTN             sz, i;
  EFI_STATUS        st;

  if (Out == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  st = FsNormalizePath (Path, &norm);
  if (EFI_ERROR (st)) {
    return st;
  }
  if (norm[0] == 0) {
    MemFree (norm);
    return EFI_INVALID_PARAMETER;   /* 卷根 Info 不支持（对齐 Open 语义） */
  }
  st = PathToUefi (Path, &upath);
  if (EFI_ERROR (st)) {
    MemFree (norm);
    return st;
  }
  st = F->Root->Open (F->Root, &file, upath, EFI_FILE_MODE_READ, 0);
  MemFree (upath);
  if (EFI_ERROR (st)) {
    MemFree (norm);
    return st;
  }
  st = GetInfoAlloc (file, &gEfiFileInfoGuid, (VOID **)&info, &sz);
  file->Close (file);
  if (EFI_ERROR (st)) {
    MemFree (norm);
    return st;
  }
  for (i = 0; norm[i] != 0 && i < 255; i++) {
    Out->Name[i] = norm[i];
  }
  Out->Name[i] = 0;
  Out->IsDir = (BOOLEAN)((info->Attribute & EFI_FILE_DIRECTORY) != 0);
  Out->Size  = Out->IsDir ? 0 : info->FileSize;
  Out->Mtime = EfiTimeToUnix (&info->ModificationTime);
  MemFree (info);
  MemFree (norm);
  return EFI_SUCCESS;
}

STATIC
VOID
uDestroy (
  FS_OPS *Fs
  )
{
  UEFI_FS *F = (UEFI_FS *)Fs;

  if (F == NULL) {
    return;
  }
  if (F->Root != NULL) {
    F->Root->Close (F->Root);
  }
  MemFree (F);
}

FS_OPS *
FsUefiFromRoot (
  EFI_HANDLE        SfsHandle,
  EFI_FILE_PROTOCOL *Root
  )
{
  UEFI_FS   *F;
  EFI_STATUS st;

  if (Root == NULL) {
    return NULL;
  }
  F = (UEFI_FS *)MemAlloc (sizeof (UEFI_FS));
  if (F == NULL) {
    Root->Close (Root);
    return NULL;
  }
  st = gBS->HandleProtocol (SfsHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&F->Sfs);
  if (EFI_ERROR (st)) {
    MemFree (F);
    Root->Close (Root);
    return NULL;
  }
  F->Handle      = SfsHandle;
  F->Root        = Root;
  F->Ops.Open    = uOpen;
  F->Ops.Read    = uRead;
  F->Ops.Write   = uWrite;
  F->Ops.Close   = uClose;
  F->Ops.Flush   = uFlush;
  F->Ops.Delete  = uDelete;
  F->Ops.Rename  = uRename;
  F->Ops.MkDir   = uMkDir;
  F->Ops.Enum    = uEnum;
  F->Ops.Info    = uInfo;
  F->Ops.Destroy = uDestroy;
  return &F->Ops;
}

FS_OPS *
FsUefiBackend (
  EFI_HANDLE SfsHandle
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
  EFI_FILE_PROTOCOL               *root;
  EFI_STATUS                      st;

  st = gBS->HandleProtocol (SfsHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&sfs);
  if (EFI_ERROR (st)) {
    return NULL;
  }
  st = sfs->OpenVolume (sfs, &root);
  if (EFI_ERROR (st)) {
    return NULL;
  }
  return FsUefiFromRoot (SfsHandle, root);
}
