/** @file
  文件操作 Ops（契约，S00 冻结；S09 实现 UEFI SimpleFileSystem 后端 + POSIX 后端）。
  双后端是并行开发的关键缝：应用 core 用 POSIX 后端在宿主临时目录真实读写。
  路径一律 UCS-2 全限定或 Dir 相对；'/' 与 '\\' 均接受（实现规范化）。
**/
#ifndef FS_OPS_H
#define FS_OPS_H

#include "EfiCompat.h"

#define FS_MODE_READ    0x01
#define FS_MODE_WRITE   0x02
#define FS_MODE_CREATE  0x04   /* 不存在则创建（与 WRITE 合用） */

typedef struct FS_HANDLE FS_HANDLE;

typedef struct {
  CHAR16  Name[256];
  UINT64  Size;
  UINT64  Mtime;      /* Unix 秒；不可得为 0 */
  BOOLEAN IsDir;
} FS_ENTRY;

typedef struct FS_OPS FS_OPS;
struct FS_OPS {
  EFI_STATUS (*Open)   (FS_OPS *Fs, CONST CHAR16 *Path, UINT32 Mode, FS_HANDLE **H);
  EFI_STATUS (*Read)   (FS_OPS *Fs, FS_HANDLE *H, VOID *Buf, UINTN *Len);   /* *Len 入=容量 出=实读；0=EOF */
  EFI_STATUS (*Write)  (FS_OPS *Fs, FS_HANDLE *H, CONST VOID *Buf, UINTN *Len);
  EFI_STATUS (*Close)  (FS_OPS *Fs, FS_HANDLE *H);
  EFI_STATUS (*Flush)  (FS_OPS *Fs, FS_HANDLE *H);
  EFI_STATUS (*Delete) (FS_OPS *Fs, CONST CHAR16 *Path);          /* 文件或空目录；非空目录拒绝 */
  EFI_STATUS (*Rename) (FS_OPS *Fs, CONST CHAR16 *Old, CONST CHAR16 *New);
  EFI_STATUS (*MkDir)  (FS_OPS *Fs, CONST CHAR16 *Path);
  EFI_STATUS (*Enum)   (FS_OPS *Fs, CONST CHAR16 *Dir, FS_ENTRY **Entries, UINTN *Count); /* 目录优先按名排序；callee 分配 */
  EFI_STATUS (*Info)   (FS_OPS *Fs, CONST CHAR16 *Path, FS_ENTRY *Out);
  VOID       (*Destroy)(FS_OPS *Fs);
};

/* 卷信息（VolEnumerate 输出） */
typedef struct {
  CHAR16  Label[64];
  CHAR16  FsName[16];      /* "FAT32"/"NTFS" 等 */
  UINT64  Capacity;
  UINT64  Free;
  BOOLEAN IsP3;            /* 本镜像数据区标记 */
  UINT8   *Dp;             /* 卷设备路径（ callee 分配，随 VOLUME_INFO 释放） */
  UINTN   DpLen;
  FS_OPS  *Fs;
} VOLUME_INFO;

EFI_STATUS VolEnumerate (VOLUME_INFO **Vols, UINTN *Count);
/* P3 发现（design §5.2）：同盘+分区3+.easyefi 标记；兜底全盘扫标记；失败 EFI_NOT_FOUND */
EFI_STATUS VolFindP3 (CONST UINT8 *BootDp, UINTN BootLen, FS_OPS **P3Fs);
VOID       VolFree (VOLUME_INFO *Vols, UINTN Count);

/* 宿主 POSIX 后端（S09 交付；root 为临时目录 UTF-8 路径） */
FS_OPS *FsPosixBackend (CONST CHAR8 *RootDir);

#endif
