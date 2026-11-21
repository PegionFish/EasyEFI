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
  由 SimpleFileSystem 句柄构造 FsOps（内部 OpenVolume 取根）。
  失败返回 NULL。Destroy 时 Close 根并释放。
**/
FS_OPS *FsUefiBackend (EFI_HANDLE SfsHandle);

/**
  由已打开的卷根构造 FsOps（接管 Root 所有权；Destroy 时 Close）。
  VolEnumerate 复用已打开的根，避免二次 OpenVolume。
**/
FS_OPS *FsUefiFromRoot (EFI_HANDLE SfsHandle, EFI_FILE_PROTOCOL *Root);

#endif /* UEFI_BUILD */

#endif /* EXEC_VOLUME_INTERNAL_H */
