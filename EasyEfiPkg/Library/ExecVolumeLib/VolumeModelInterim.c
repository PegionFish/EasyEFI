/** @file
  VolumeModel 契约的 INTERIM 实现（S09 临时自供，纯逻辑）。

  背景：S05 VolumeModelLib 并行开发中，S00 stub 只有 Probe 占位函数，
  DpFindGpt/DpDiskPrefix/VolIsP3/VolSameDisk 无可链接符号。本文件按
  Include/Contract/VolumeModel.h 冻结语义实现这四个函数，供 S09 在
  S05 合入 main 之前构建/测试使用：
    - 宿主：host.mk 在 S05 真实源码缺席时自动编译本文件；
    - UEFI：module.dsc.inc 以 <LibraryClasses> 作用域覆盖把 VolumeModelLib
      解析到 VolumeModelInterim.inf（仅 ExecVolumeLib 与 S09 冒烟应用）。

  **S05 合入 main 后必须移除**（见 docs/handoffs/S09.md 集成注意事项）：
  删除 module.dsc.inc 的两处覆盖 + 本文件 + VolumeModelInterim.inf。

  语义要点（与契约一致）：
    - 设备路径节点：Type(1) SubType(1) Length(2 LE，含 4 字节头)；
      HD 节点 = MEDIA(0x04)/HARDDRIVE(0x01)，42 字节；
      SignatureType: 0=无 1=MBR 2=GPT。
    - GPT HD 节点的 Signature[16] 按 UEFI Spec 2.10 §10.3.1 是
      UniquePartitionGUID（原样拷贝进 DiskSignature 字段；契约注释称
      DiskGUID，但设备路径不含 DiskGUID——本实现不依赖该字段语义）。
      PartTypeGuid 无法从设备路径获得，置零（S09 不消费该字段）。
    - 盘身份 = 首个 HD 节点之前的前缀逐字节相等（VolSameDisk）。
    - VolIsP3 = 同盘前缀 + 卷 PartNumber==3。
    - MBR HD 节点 → EFI_UNSUPPORTED；畸形/截断 → EFI_INVALID_PARAMETER。
**/
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/VolumeModel.h"

#define DP_TYPE_MEDIA       0x04
#define DP_SUBTYPE_HD       0x01
#define DP_TYPE_END         0x7F
#define DP_SUBTYPE_END      0xFF
#define HD_NODE_LEN         42
#define HD_SIGTYPE_MBR      0x01
#define HD_SIGTYPE_GPT      0x02

STATIC VOID
ByteCopy (
  UINT8       *Dst,
  CONST UINT8 *Src,
  UINTN       Len
  )
{
  while (Len-- != 0) {
    *Dst++ = *Src++;
  }
}

STATIC INTN
ByteCompare (
  CONST UINT8 *A,
  CONST UINT8 *B,
  UINTN       Len
  )
{
  UINTN i;
  for (i = 0; i < Len; i++) {
    if (A[i] != B[i]) {
      return (INTN)A[i] - (INTN)B[i];
    }
  }
  return 0;
}

STATIC UINT32
ReadLe32 (
  CONST UINT8 *P
  )
{
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

STATIC UINT64
ReadLe64 (
  CONST UINT8 *P
  )
{
  return (UINT64)ReadLe32 (P) | ((UINT64)ReadLe32 (P + 4) << 32);
}

/**
  遍历设备路径节点。*Off 入=当前节点偏移，出=下一节点偏移。
  遇到 END 节点返回 EFI_NOT_FOUND（正常终止）；畸形返回 EFI_INVALID_PARAMETER。
**/
STATIC
EFI_STATUS
DpNextNode (
  CONST UINT8 *Dp,
  UINTN       Len,
  UINTN       *Off,
  UINT8       *Type,
  UINT8       *SubType,
  UINTN       *NodeLen
  )
{
  UINTN o = *Off;
  UINTN nl;

  if (o + 4 > Len) {
    return EFI_INVALID_PARAMETER;
  }
  nl = (UINTN)Dp[o + 2] | ((UINTN)Dp[o + 3] << 8);
  if (nl < 4 || o + nl > Len) {
    return EFI_INVALID_PARAMETER;
  }
  *Type    = Dp[o];
  *SubType = Dp[o + 1];
  *NodeLen = nl;
  *Off     = o + nl;
  if (*Type == DP_TYPE_END && *SubType == DP_SUBTYPE_END) {
    return EFI_NOT_FOUND;
  }
  return EFI_SUCCESS;
}

/**
  定位首个 HD 节点（任意 SignatureType）。
  返回 EFI_SUCCESS 时 HdOff 与 HdLen 指向该节点；无 HD 节点返回 EFI_NOT_FOUND。
**/
STATIC
EFI_STATUS
DpFindHd (
  CONST UINT8 *Dp,
  UINTN       Len,
  UINTN       *HdOff,
  UINTN       *HdLen
  )
{
  UINTN      off = 0;
  UINT8      type, sub;
  UINTN      nl;
  EFI_STATUS st;

  if (Dp == NULL || Len < 4) {
    return EFI_INVALID_PARAMETER;
  }
  for (;;) {
    UINTN cur = off;
    st = DpNextNode (Dp, Len, &off, &type, &sub, &nl);
    if (st == EFI_NOT_FOUND) {
      return EFI_NOT_FOUND;           /* END：全程无 HD */
    }
    if (EFI_ERROR (st)) {
      return st;
    }
    if (type == DP_TYPE_MEDIA && sub == DP_SUBTYPE_HD) {
      if (nl < HD_NODE_LEN) {
        return EFI_INVALID_PARAMETER; /* 截断的 HD 节点 */
      }
      *HdOff = cur;
      *HdLen = nl;
      return EFI_SUCCESS;
    }
  }
}

EFI_STATUS
DpFindGpt (
  CONST UINT8   *Dp,
  UINTN         Len,
  GPT_NODE_INFO *Out
  )
{
  UINTN       hdOff, hdLen;
  CONST UINT8 *N;
  UINT8       sigType;
  EFI_STATUS  st;
  UINT64      start, size;

  if (Out == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  st = DpFindHd (Dp, Len, &hdOff, &hdLen);
  if (EFI_ERROR (st)) {
    return st;
  }
  N       = Dp + hdOff;
  sigType = N[41];
  if (sigType == HD_SIGTYPE_MBR) {
    return EFI_UNSUPPORTED;           /* 本镜像 GPT-only */
  }
  if (sigType != HD_SIGTYPE_GPT) {
    return EFI_UNSUPPORTED;
  }
  /* HD 节点布局：Hdr[4] PartNumber[4] Start[8] Size[8] Signature[16] MBRType[1] SigType[1] */
  ByteCopy (Out->DiskSignature, N + 24, 16);
  Out->PartNumber = ReadLe32 (N + 4);
  /* PartTypeGuid 不在设备路径中（HD 节点 Signature=UniquePartitionGUID），置零 */
  {
    UINTN i;
    for (i = 0; i < 16; i++) {
      Out->PartTypeGuid[i] = 0;
    }
  }
  start            = ReadLe64 (N + 8);
  size             = ReadLe64 (N + 16);
  Out->PartStartLba = start;
  Out->PartEndLba   = (size == 0) ? start : start + size - 1;
  return EFI_SUCCESS;
}

EFI_STATUS
DpDiskPrefix (
  CONST UINT8 *Dp,
  UINTN       Len,
  UINT8       **Prefix,
  UINTN       *PrefixLen
  )
{
  UINTN      hdOff, hdLen;
  UINTN      plen;
  UINT8      *buf;
  EFI_STATUS st;

  if (Prefix == NULL || PrefixLen == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  st = DpFindHd (Dp, Len, &hdOff, &hdLen);
  if (st == EFI_NOT_FOUND) {
    plen = Len;                       /* 无 HD 节点：整个缓冲即设备身份 */
  } else if (EFI_ERROR (st)) {
    return st;
  } else {
    plen = hdOff;
  }
  buf = (UINT8 *)MemAlloc (plen ? plen : 1);
  if (buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  if (plen != 0) {
    ByteCopy (buf, Dp, plen);
  }
  *Prefix    = buf;
  *PrefixLen = plen;
  return EFI_SUCCESS;
}

BOOLEAN
VolSameDisk (
  CONST UINT8 *A,
  UINTN       ALen,
  CONST UINT8 *B,
  UINTN       BLen
  )
{
  UINT8      *pa = NULL, *pb = NULL;
  UINTN      la = 0, lb = 0;
  BOOLEAN    same = FALSE;
  EFI_STATUS sa, sb;

  sa = DpDiskPrefix (A, ALen, &pa, &la);
  sb = DpDiskPrefix (B, BLen, &pb, &lb);
  if (!EFI_ERROR (sa) && !EFI_ERROR (sb) && la == lb) {
    same = (BOOLEAN)(ByteCompare (pa, pb, la) == 0);
  }
  MemFree (pa);
  MemFree (pb);
  return same;
}

BOOLEAN
VolIsP3 (
  CONST UINT8 *BootDp,
  UINTN       BootLen,
  CONST UINT8 *VolDp,
  UINTN       VolLen
  )
{
  GPT_NODE_INFO Gi;

  if (!VolSameDisk (BootDp, BootLen, VolDp, VolLen)) {
    return FALSE;
  }
  if (EFI_ERROR (DpFindGpt (VolDp, VolLen, &Gi))) {
    return FALSE;
  }
  return (BOOLEAN)(Gi.PartNumber == 3);
}
