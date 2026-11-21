/** @file
  ExecVolumeLib 内部接口（非契约；契约见 Include/Contract/FsOps.h、VolumeModel.h）。
  双环境助手供宿主测试直接驱动；UEFI 构造器仅 UEFI_BUILD 可见。
**/
#ifndef EXEC_VOLUME_INTERNAL_H
#define EXEC_VOLUME_INTERNAL_H

#include "Contract/FsOps.h"
#include "Contract/VolumeModel.h"

/* ---- 双环境助手（FsCommon.c） ---- */

/**
  路径规范化（callee 分配，caller MemFree）：
  '\\'→'/'；折叠重复 '/'；去掉首尾 '/'（根 → 空串）；去掉 "." 分量；
  拒绝 ".." 分量与空输入 → EFI_INVALID_PARAMETER（防越出卷根）。
**/
EFI_STATUS FsNormalizePath (CONST CHAR16 *In, CHAR16 **Out);

/**
  卷根标记验证（design §5.2 步骤 3）：.easyefi 或 launcher.json 存在即 TRUE。
**/
BOOLEAN FsHasMarker (FS_OPS *Fs);

/**
  卷集排序 + IsP3 标记（VolEnumerate 的可宿主测试核心）：
  IsP3 = VolIsP3(BootDp, VolDp) && FsHasMarker(Vol->Fs)；
  排序：IsP3 优先，其余按设备路径字节序升序（稳定）。
  BootDp==NULL/BootLen==0 → 全部 IsP3=FALSE，仅按字节序排序。
**/
EFI_STATUS VolSortMark (VOLUME_INFO *Vols, UINTN Count, CONST UINT8 *BootDp, UINTN BootLen);

/**
  P3 发现算法核心（design §5.2 步骤 2–4，在给定卷集上）：
  1) 同盘+分区3 候选 → 标记验证；
  2) 兜底：全卷集扫标记，优先启动盘（VolSameDisk），再按卷集顺序取首个；
  3) 无命中 → EFI_NOT_FOUND。
  成功时 *Index = 命中卷下标（调用方负责 Fs 生命周期交接）。
  多命中时的"列候选让用户选"属 UI 职责（契约返回单个 Fs），此处确定性取一。
**/
EFI_STATUS VolFindP3InSet (VOLUME_INFO *Vols, UINTN Count, CONST UINT8 *BootDp,
                           UINTN BootLen, UINTN *Index);

/* ---- UEFI 后端构造器（FsUefiBackend.c / VolUefi.c，仅 UEFI_BUILD） ---- */
#ifdef UEFI_BUILD

#include <Protocol/SimpleFileSystem.h>

/**
  由 SimpleFileSystem 句柄取 FsOps（卷缓存：同句柄跨调用返回同一实例，
  契约裁决 main 332553a）。失败返回 NULL。返回实例为借用，调用方不得 Destroy。
**/
FS_OPS *FsUefiBackend (EFI_HANDLE SfsHandle);

/**
  由已打开的卷根构造 FsOps（接管 Root 所有权；Destroy 时 Close）。
  仅供卷缓存内部使用（VolUefi.c）；构造后须入缓存再出借。
**/
FS_OPS *FsUefiFromRoot (EFI_HANDLE SfsHandle, EFI_FILE_PROTOCOL *Root);

/**
  健壮 GetInfo（callee 分配，caller MemFree）：非零起始缓冲 + 有界重试。
  兼容标准两阶段语义（FAT）与 EfiFs/ntfs-3g 非标准语义（S14 交接已知限制 2：
  NULL/0 问尺寸不回写；缓冲偏大返回 TOO_SMALL+精确尺寸；恰好相等才成功）。
**/
EFI_STATUS UefiGetInfoRobust (EFI_FILE_PROTOCOL *File, EFI_GUID *Guid, VOID **Buf, UINTN *Size);

/**
  自身启动设备路径全字节（callee MemAlloc，含 END 节点）。
  注意：LoadedImage.FilePath 仅是文件部分（相对 DeviceHandle）；
  完整路径 = DeviceHandle 的设备路径（含 HD(GPT) 分区节点）+ FilePath。
**/
EFI_STATUS VolOwnBootDp (UINT8 **Dp, UINTN *Len);

#endif /* UEFI_BUILD */

#endif /* EXEC_VOLUME_INTERNAL_H */
