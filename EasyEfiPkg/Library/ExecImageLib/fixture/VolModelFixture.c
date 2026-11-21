/** @file
  TEST-ONLY fixture —— 按契约语义实现 S05 VolumeModel 的 VolSameDisk，供 S10
  宿主测试与 QEMU 冒烟使用（worktree 内 VolumeModelLib 是 S00 stub，未定义契约符号）。
  集成时由真实 VolumeModelLib 取代（smoke/Smoke.dsc 的 fixture 解析随之移除）。

  契约语义（VolumeModel.h）：同盘判定 = HD 节点之前的前缀逐字节相等
  （前缀含控制器路径；克隆盘同签名不同控制器路径 → FALSE）。
  本 fixture 只实现 S10 引用的 VolSameDisk；其余契约函数不在 S10 链接面内。
**/
#include "Contract/EfiCompat.h"
#include "Contract/VolumeModel.h"

/* 前缀长度 = 首个 MEDIA_HARDDRIVE_DP 节点之前的字节数；无 HD 节点则取 END 之前。
   畸形/截断 → 错误。 */
STATIC EFI_STATUS PrefixLen (CONST UINT8 *Dp, UINTN Len, UINTN *Out) {
  UINTN Off = 0;
  if (Dp == NULL || Len == 0) {
    return EFI_INVALID_PARAMETER;
  }
  while (Off + 4 <= Len) {
    UINTN L = (UINTN)Dp[Off + 2] | ((UINTN)Dp[Off + 3] << 8);
    if (L < 4 || Off + L > Len) {
      return EFI_INVALID_PARAMETER;
    }
    if ((Dp[Off] == 0x04 && Dp[Off + 1] == 0x01) ||   /* HD 节点 */
        (Dp[Off] == 0x7F && Dp[Off + 1] == 0xFF)) {   /* END */
      *Out = Off;
      return EFI_SUCCESS;
    }
    Off += L;
  }
  return EFI_INVALID_PARAMETER;
}

BOOLEAN VolSameDisk (CONST UINT8 *A, UINTN ALen, CONST UINT8 *B, UINTN BLen) {
  UINTN PA, PB, i;
  if (A == NULL || B == NULL) {
    return FALSE;
  }
  if (EFI_ERROR (PrefixLen (A, ALen, &PA)) || EFI_ERROR (PrefixLen (B, BLen, &PB))) {
    return FALSE;
  }
  if (PA == 0 || PA != PB) {
    return FALSE;                        /* 空前缀=无控制器身份，不可判同盘 */
  }
  for (i = 0; i < PA; i++) {
    if (A[i] != B[i]) {
      return FALSE;
    }
  }
  return TRUE;
}