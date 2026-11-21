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

/**
  健壮 GetInfo（见 ExecVolumeInternal.h 注释）：非零起始缓冲 + 有界重试。
  收敛路径：
    - 标准驱动（FAT）：缓冲不足 → TOO_SMALL + 所需尺寸 → 扩到所需 → 成功；
    - EfiFs：缓冲偏大 → TOO_SMALL + 精确尺寸 → 缩到精确 → 成功；
    - 尺寸不回写（sz 不变/0）→ 倍增重试，上限 5 轮报错（不死循环）。
**/
EFI_STATUS
UefiGetInfoRobust (
  EFI_FILE_PROTOCOL *File,
  EFI_GUID          *Guid,
  VOID              **Buf,
  UINTN             *Size
  )
{
  UINTN      alloc = SIZE_OF_EFI_FILE_INFO + 512;
  UINTN      sz, need, tries;
  VOID       *b, *nb;
  EFI_STATUS st;

  b = MemAlloc (alloc);
  if (b == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  for (tries = 0; tries < 5; tries++) {
    sz = alloc;
    st = File->GetInfo (File, Guid, &sz, b);
    if (!EFI_ERROR (st)) {
      *Buf  = b;
      *Size = sz;
      return EFI_SUCCESS;
    }
    if (st != EFI_BUFFER_TOO_SMALL) {
      MemFree (b);
      return st;
    }
    need = (sz != 0 && sz != alloc) ? sz : alloc * 2;
    nb = MemRealloc (b, need);
    if (nb == NULL) {
      MemFree (b);
      return EFI_OUT_OF_RESOURCES;
    }
    b     = nb;
    alloc = need;
  }
  MemFree (b);
  return EFI_DEVICE_ERROR;
}

/**
  迭代 Read 取下一目录条目（健壮尺寸协商，兼容 FAT/EfiFs）。
  *Buf 持久缓冲（入/出，callee 首次分配，调用方收尾 MemFree）；*Alloc 为其容量。
  成功且 *Info!=NULL：一条目；成功且 *Info==NULL：遍历结束；错误经返回值。
**/
STATIC
EFI_STATUS
ReadDirEntry (
  EFI_FILE_PROTOCOL *Dir,
  EFI_FILE_INFO     **Info,
  VOID              **Buf,
  UINTN             *Alloc
  )
{
  UINTN      sz, need, tries;
  VOID       *nb;
  EFI_STATUS st;

  if (*Buf == NULL) {
    *Alloc = SIZE_OF_EFI_FILE_INFO + 512;
    *Buf   = MemAlloc (*Alloc);
    if (*Buf == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
  }
  for (tries = 0; tries < 5; tries++) {
    sz = *Alloc;
    st = Dir->Read (Dir, &sz, *Buf);
    if (!EFI_ERROR (st)) {
      *Info = (sz == 0) ? NULL : (EFI_FILE_INFO *)*Buf;
      return EFI_SUCCESS;
    }
    if (st != EFI_BUFFER_TOO_SMALL) {
      return st;
    }
    need = (sz != 0 && sz != *Alloc) ? sz : *Alloc * 2;
    nb = MemRealloc (*Buf, need);
    if (nb == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    *Buf   = nb;
    *Alloc = need;
  }
  return EFI_DEVICE_ERROR;
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
  EFI_FILE_INFO *info;
  VOID          *buf = NULL;
  UINTN         alloc = 0;
  BOOLEAN       has = FALSE;

  for (;;) {
    st = ReadDirEntry (Dir, &info, &buf, &alloc);
    if (EFI_ERROR (st)) {
      MemFree (buf);
      return st;
    }
    if (info == NULL) {
      break;                        /* 目录遍历结束 */
    }
    if (!IsDotName (info->FileName)) {
      has = TRUE;
      break;
    }
  }
  MemFree (buf);
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
  st = UefiGetInfoRobust (h->File, &gEfiFileInfoGuid, (VOID **)&info, &sz);
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
  /* 必须 READ|WRITE：FatDelete 拒绝 ReadOnly 句柄（IFile->ReadOnly →
   * EFI_WRITE_PROTECTED → EFI_WARN_DELETE_FAILURE，文件不会被删）。
   * 目录亦可以 WRITE 模式打开（FAT 只读属性检查排除目录）。 */
  st = F->Root->Open (F->Root, &file, upath, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  MemFree (upath);
  if (EFI_ERROR (st)) {
    return st;
  }
  st = UefiGetInfoRobust (file, &gEfiFileInfoGuid, (VOID **)&info, &sz);
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
  /* 协议语义：Delete 无论成败都关闭句柄（"Close and delete the file handle"），
   * 不可再 Close。edk2-stable202608 起警告码不带 MAX_BIT（ENCODE_WARNING=x），
   * EFI_ERROR(EFI_WARN_DELETE_FAILURE)==FALSE —— 必须显式转错误，
   * 否则调用方误判删除成功。 */
  st = file->Delete (file);
  if (st == EFI_WARN_DELETE_FAILURE) {
    st = EFI_DEVICE_ERROR;
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

/** 以给定 FileName 构造 EFI_FILE_INFO 并 SetInfo（保留尺寸/时间/属性）。 */
STATIC
EFI_STATUS
RenameApply (
  EFI_FILE_PROTOCOL *File,
  EFI_FILE_INFO     *Info,
  CONST CHAR16      *Name
  )
{
  EFI_FILE_INFO *set;
  UINTN         nameLen = EfiStrLen (Name);
  UINTN         setSz   = SIZE_OF_EFI_FILE_INFO + (nameLen + 1) * sizeof (CHAR16);
  EFI_STATUS    st;

  set = (EFI_FILE_INFO *)MemAlloc (setSz);
  if (set == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  set->Size         = setSz;
  set->FileSize     = Info->FileSize;
  set->PhysicalSize = Info->PhysicalSize;
  CopyMem (&set->CreateTime, &Info->CreateTime, sizeof (EFI_TIME));
  CopyMem (&set->LastAccessTime, &Info->LastAccessTime, sizeof (EFI_TIME));
  CopyMem (&set->ModificationTime, &Info->ModificationTime, sizeof (EFI_TIME));
  set->Attribute = Info->Attribute;
  CopyMem (set->FileName, Name, (nameLen + 1) * sizeof (CHAR16));
  st = File->SetInfo (File, &gEfiFileInfoGuid, setSz, set);
  MemFree (set);
  return st;
}

/** 探测路径是否存在：存在 → EFI_SUCCESS（即开即关）；不存在 → EFI_NOT_FOUND。 */
STATIC
EFI_STATUS
ProbePath (
  UEFI_FS  *F,
  CHAR16   *UPath
  )
{
  EFI_FILE_PROTOCOL *p = NULL;
  EFI_STATUS        st;

  st = F->Root->Open (F->Root, &p, UPath, EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (st)) {
    p->Close (p);
    return EFI_SUCCESS;
  }
  return st;
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
  CHAR16            *absName = NULL;
  CONST CHAR16      *baseO, *baseN;
  EFI_FILE_PROTOCOL *file = NULL;
  EFI_FILE_INFO     *info = NULL;
  UINTN             sz, plen, blen, k;
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
    st = EFI_UNSUPPORTED;           /* 跨目录改名：FAT SetInfo 不支持（MVP 冻结） */
    goto Done;
  }
  /* 目标已存在拒绝（对齐 mock） */
  st = PathToUefi (New, &upathN);
  if (EFI_ERROR (st)) {
    goto Done;
  }
  st = ProbePath (F, upathN);
  if (st != EFI_NOT_FOUND) {
    st = (st == EFI_SUCCESS) ? EFI_ACCESS_DENIED : st;
    goto Done;
  }
  st = PathToUefi (Old, &upathO);
  if (EFI_ERROR (st)) {
    goto Done;
  }
  st = F->Root->Open (F->Root, &file, upathO, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (EFI_ERROR (st)) {
    goto Done;
  }
  st = UefiGetInfoRobust (file, &gEfiFileInfoGuid, (VOID **)&info, &sz);
  if (EFI_ERROR (st)) {
    goto Done;
  }
  /* SetInfo 改名两种驱动语义（实证）：
   *   FAT（FatSetFileInfo）：FileName = 相对父目录的新基名；
   *   EfiFs/ntfs-3g（NtfsSetFileInfo）：FileName 须为卷根绝对路径才改名，
   *     基名被静默忽略且返回 SUCCESS（S14 零补丁上游行为）。
   * 策略：先基名 → 验证新名存在；未生效则绝对路径重试 → 再验证；
   * 仍未生效 → EFI_DEVICE_ERROR（不信任未生效的 SUCCESS）。 */
  st = RenameApply (file, info, baseN);
  if (EFI_ERROR (st)) {
    goto Done;
  }
  st = ProbePath (F, upathN);
  if (st != EFI_NOT_FOUND) {
    goto Done;                      /* SUCCESS=已生效；其它错误原样上报 */
  }
  plen    = EfiStrLen (parentN);
  blen    = EfiStrLen (baseN);
  absName = (CHAR16 *)MemAlloc ((plen + blen + 3) * sizeof (CHAR16));
  if (absName == NULL) {
    st = EFI_OUT_OF_RESOURCES;
    goto Done;
  }
  absName[0] = (CHAR16)'\\';
  if (plen != 0) {
    CopyMem (absName + 1, parentN, plen * sizeof (CHAR16));
    absName[plen + 1] = (CHAR16)'\\';
    CopyMem (absName + plen + 2, baseN, (blen + 1) * sizeof (CHAR16));
  } else {
    CopyMem (absName + 1, baseN, (blen + 1) * sizeof (CHAR16));
  }
  for (k = 0; absName[k] != 0; k++) {
    if (absName[k] == (CHAR16)'/') {
      absName[k] = (CHAR16)'\\';
    }
  }
  st = RenameApply (file, info, absName);
  if (EFI_ERROR (st)) {
    goto Done;
  }
  st = ProbePath (F, upathN);
  if (st == EFI_NOT_FOUND) {
    st = EFI_DEVICE_ERROR;          /* 两种形式都未生效 */
  }

Done:
  if (file != NULL) {
    file->Close (file);
  }
  MemFree (absName);
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
  VOID              *rbuf = NULL;
  UINTN             ralloc = 0;
  FS_ENTRY          *out = NULL;
  UINTN             n = 0, cap = 0, i, j;
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
    st = ReadDirEntry (dir, &info, &rbuf, &ralloc);
    if (EFI_ERROR (st)) {
      break;
    }
    if (info == NULL) {
      break;                        /* 遍历结束 */
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
          st = EFI_OUT_OF_RESOURCES;
          break;
        }
        out = nb;
        cap = ncap;
      }
      out[n++] = e;
    }
  }
  MemFree (rbuf);
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
  st = UefiGetInfoRobust (file, &gEfiFileInfoGuid, (VOID **)&info, &sz);
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
