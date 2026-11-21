/** @file
  设备路径纯逻辑（契约，S00 冻结；S05 实现）。
  在 UEFI Spec 2.10 §10 格式的设备路径字节缓冲上操作，不碰 gBS——宿主可测。
  GPT-only（本镜像布局）；MBR HD 节点明确拒绝。
**/
#ifndef VOLUME_MODEL_H
#define VOLUME_MODEL_H

#include "EfiCompat.h"

typedef struct {
  UINT8  DiskSignature[16];   /* GPT DiskGUID（混合端序，按 spec 原样） */
  UINT32 PartNumber;
  UINT8  PartTypeGuid[16];
  UINT64 PartStartLba;
  UINT64 PartEndLba;
} GPT_NODE_INFO;

/* 首个 MEDIA_HARDDRIVE_DP + GPT 格式节点；MBR/畸形 → EFI_UNSUPPORTED/EFI_INVALID_PARAMETER */
EFI_STATUS DpFindGpt (CONST UINT8 *Dp, UINTN Len, GPT_NODE_INFO *Out);

/* HD 节点之前的前缀（盘身份=控制器路径；callee 分配） */
EFI_STATUS DpDiskPrefix (CONST UINT8 *Dp, UINTN Len, UINT8 **Prefix, UINTN *PrefixLen);

/* P3 判定：同盘前缀 + PartNumber==3 */
BOOLEAN VolIsP3 (CONST UINT8 *BootDp, UINTN BootLen, CONST UINT8 *VolDp, UINTN VolLen);

/* 同盘判定：前缀逐字节相等（克隆盘同签名不同控制器路径 → FALSE） */
BOOLEAN VolSameDisk (CONST UINT8 *A, UINTN ALen, CONST UINT8 *B, UINTN BLen);

#endif
