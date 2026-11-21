/** @file
  VolumeModelLib — GPT pure logic on device-path byte buffers (S05).

  Operates on UEFI Spec 2.10 §10 device-path byte buffers. No gBS/gST, no
  MdePkg/protocol headers — host-testable. GPT-only (EasyEFI image layout,
  design.md §3.1/§5.2); MBR HD nodes are explicitly rejected.

  HARDDRIVE_DEVICE_PATH node (Spec 2.10 §10.3.1), 42 bytes total:
    [0]  UINT8  Type = 0x04 (MEDIA_DEVICE_PATH)
    [1]  UINT8  SubType = 0x01 (MEDIA_HARDDRIVE_DP)
    [2]  UINT16 Length = 42 (little-endian, includes 4-byte header)
    [4]  UINT32 PartitionNumber (1-based; 0 = whole device, per spec)
    [8]  UINT64 PartitionStart (LBA)
    [16] UINT64 PartitionSize (logical-block count)
    [24] UINT8  Signature[16] (GPT DiskGUID, mixed-endian, kept verbatim)
    [40] UINT8  MBRType (0x01 = legacy MBR, 0x02 = GPT)
    [41] UINT8  SignatureType (0x00 = none, 0x01 = MBR, 0x02 = GUID)

  Disk identity = controller path = the bytes before the first HD node
  (design.md §5.2). VolSameDisk compares those prefixes byte-for-byte, so a
  clone disk (same DiskGUID, different controller path) is NOT the same disk.

  Semantics notes (see docs/handoffs/S05.md):
    - The first HD node decides DpFindGpt: GPT -> success, MBR ->
      EFI_UNSUPPORTED, malformed -> EFI_INVALID_PARAMETER. No HD -> NOT_FOUND.
    - DpDiskPrefix stops at the first HD node or the first END node
      (END_ENTIRE / END_INSTANCE); the END node itself is excluded. Only the
      first device-path instance is considered. A well-formed buffer with no
      HD node yields the whole walked span as prefix (whole-device identity).
    - GPT_NODE_INFO.PartTypeGuid is zero-filled: the partition type GUID is
      NOT carried by HARDDRIVE_DEVICE_PATH (callers needing it must read the
      GPT partition table itself).
    - All multi-byte fields are read byte-wise (little-endian): no alignment
      assumption on the input buffer.
**/
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/VolumeModel.h"

/* UEFI Spec 2.10 §10 device-path constants. Local copies: pure-logic libs
   may only include Contract headers, not MdePkg/Protocol/DevicePath.h.
   Verified against edk2 MdePkg/Include/Protocol/DevicePath.h. */
#define DP_TYPE_MEDIA        0x04
#define DP_SUB_MEDIA_HD      0x01
#define DP_TYPE_END          0x7F
#define DP_SUB_END_INSTANCE  0x01
#define DP_SUB_END_ENTIRE    0xFF

#define HD_NODE_LEN          42   /* sizeof (HARDDRIVE_DEVICE_PATH) */
#define HD_OFF_PART_NUMBER   4
#define HD_OFF_PART_START    8
#define HD_OFF_PART_SIZE     16
#define HD_OFF_SIGNATURE     24
#define HD_OFF_MBR_TYPE      40
#define HD_OFF_SIG_TYPE      41

#define MBR_TYPE_PCAT        0x01
#define MBR_TYPE_GPT         0x02
#define SIG_TYPE_GUID        0x02

/* P3 = data partition number per the frozen partition layout (design.md
   §3.1). Contract-mandated constant (VolIsP3), not a tunable threshold. */
#define P3_PARTITION_NUMBER  3

/* ---- little-endian byte-wise readers (no alignment assumption) ---- */

STATIC UINT16 RdU16 (CONST UINT8 *P) {
  return (UINT16)((UINT16)P[0] | ((UINT16)P[1] << 8));
}

STATIC UINT32 RdU32 (CONST UINT8 *P) {
  return (UINT32)P[0] | ((UINT32)P[1] << 8) |
         ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

STATIC UINT64 RdU64 (CONST UINT8 *P) {
  return (UINT64)RdU32 (P) | ((UINT64)RdU32 (P + 4) << 32);
}

/* ---- byte helpers (no MdePkg BaseMemoryLib dependency) ---- */

STATIC VOID CopyBytes (UINT8 *Dst, CONST UINT8 *Src, UINTN N) {
  UINTN i;
  for (i = 0; i < N; i++) Dst[i] = Src[i];
}

STATIC VOID ZeroBytes (UINT8 *Dst, UINTN N) {
  UINTN i;
  for (i = 0; i < N; i++) Dst[i] = 0;
}

/* ---- node walker ---- */

typedef struct {
  BOOLEAN FoundHd;   /* TRUE if walk stopped at a MEDIA_HARDDRIVE_DP node */
  UINTN   Stop;      /* offset of first HD node, first END node, or clean end */
} DP_WALK;

/* Walk device-path nodes from the start. Stops at the first HD node (any
   format — payload validation is DpFindGpt's job), the first END node, or a
   clean buffer end. Every header is bounds-checked; malformed/truncated
   buffers return EFI_INVALID_PARAMETER without reading out of bounds. */
STATIC EFI_STATUS DpWalk (CONST UINT8 *Dp, UINTN Len, DP_WALK *Walk) {
  UINTN Off = 0;

  Walk->FoundHd = FALSE;
  Walk->Stop    = Len;

  while (Len - Off >= 4) {
    UINT8  Type    = Dp[Off];
    UINT8  Sub     = Dp[Off + 1];
    UINT16 NodeLen = RdU16 (Dp + Off + 2);

    if (NodeLen < 4 || NodeLen > Len - Off) {
      return EFI_INVALID_PARAMETER;   /* bad length or truncated node */
    }
    if (Type == DP_TYPE_MEDIA && Sub == DP_SUB_MEDIA_HD) {
      Walk->FoundHd = TRUE;
      Walk->Stop    = Off;
      return EFI_SUCCESS;
    }
    if (Type == DP_TYPE_END) {
      if (Sub != DP_SUB_END_INSTANCE && Sub != DP_SUB_END_ENTIRE) {
        return EFI_INVALID_PARAMETER;   /* unknown END subtype */
      }
      Walk->Stop = Off;   /* prefix excludes END; first instance only */
      return EFI_SUCCESS;
    }
    Off += NodeLen;
  }

  if (Off != Len) {
    return EFI_INVALID_PARAMETER;   /* 1..3 stray bytes: truncated header */
  }
  return EFI_SUCCESS;   /* clean end, no HD/END (controller-only fragment) */
}

/* ---- public API (Contract/VolumeModel.h) ---- */

EFI_STATUS DpFindGpt (CONST UINT8 *Dp, UINTN Len, GPT_NODE_INFO *Out) {
  DP_WALK     Walk;
  EFI_STATUS  Status;
  CONST UINT8 *Hd;
  UINT8       MbrType, SigType;
  UINT64      Start, Size;

  if (Dp == NULL || Out == NULL || Len == 0) {
    return EFI_INVALID_PARAMETER;
  }
  Status = DpWalk (Dp, Len, &Walk);
  if (EFI_ERROR (Status)) return Status;
  if (!Walk.FoundHd) return EFI_NOT_FOUND;

  Hd = Dp + Walk.Stop;   /* node header already bounds-checked by DpWalk */
  if (RdU16 (Hd + 2) != HD_NODE_LEN) {
    return EFI_INVALID_PARAMETER;   /* HD node must be exactly 42 bytes */
  }
  MbrType = Hd[HD_OFF_MBR_TYPE];
  SigType = Hd[HD_OFF_SIG_TYPE];
  if (MbrType == MBR_TYPE_PCAT) {
    return EFI_UNSUPPORTED;   /* GPT-only image: explicit MBR rejection */
  }
  if (MbrType != MBR_TYPE_GPT || SigType != SIG_TYPE_GUID) {
    return EFI_INVALID_PARAMETER;   /* malformed GPT node */
  }

  Start = RdU64 (Hd + HD_OFF_PART_START);
  Size  = RdU64 (Hd + HD_OFF_PART_SIZE);
  if (Size == 0 || Start > ~(UINT64)0 - Size) {
    return EFI_INVALID_PARAMETER;   /* zero-size / overflowing geometry */
  }

  Out->PartNumber = RdU32 (Hd + HD_OFF_PART_NUMBER);
  CopyBytes (Out->DiskSignature, Hd + HD_OFF_SIGNATURE, 16);
  ZeroBytes (Out->PartTypeGuid, 16);   /* not derivable from the DP node */
  Out->PartStartLba = Start;
  Out->PartEndLba   = Start + Size - 1;
  return EFI_SUCCESS;
}

EFI_STATUS DpDiskPrefix (CONST UINT8 *Dp, UINTN Len, UINT8 **Prefix, UINTN *PrefixLen) {
  DP_WALK    Walk;
  EFI_STATUS Status;
  UINT8      *Buf;
  UINTN      N;

  if (Dp == NULL || Prefix == NULL || PrefixLen == NULL || Len == 0) {
    return EFI_INVALID_PARAMETER;
  }
  *Prefix    = NULL;
  *PrefixLen = 0;

  Status = DpWalk (Dp, Len, &Walk);
  if (EFI_ERROR (Status)) return Status;

  N   = Walk.Stop;
  Buf = MemAlloc (N != 0 ? N : 1);   /* always non-NULL so caller can MemFree */
  if (Buf == NULL) return EFI_OUT_OF_RESOURCES;
  CopyBytes (Buf, Dp, N);
  *Prefix    = Buf;
  *PrefixLen = N;
  return EFI_SUCCESS;
}

BOOLEAN VolSameDisk (CONST UINT8 *A, UINTN ALen, CONST UINT8 *B, UINTN BLen) {
  UINT8   *Pa = NULL, *Pb = NULL;
  UINTN   Na = 0, Nb = 0, i;
  BOOLEAN Same = FALSE;

  if (DpDiskPrefix (A, ALen, &Pa, &Na) == EFI_SUCCESS &&
      DpDiskPrefix (B, BLen, &Pb, &Nb) == EFI_SUCCESS &&
      Na == Nb) {
    Same = TRUE;
    for (i = 0; i < Na; i++) {
      if (Pa[i] != Pb[i]) { Same = FALSE; break; }
    }
  }
  if (Pa != NULL) MemFree (Pa);
  if (Pb != NULL) MemFree (Pb);
  return Same;
}

BOOLEAN VolIsP3 (CONST UINT8 *BootDp, UINTN BootLen, CONST UINT8 *VolDp, UINTN VolLen) {
  GPT_NODE_INFO BootInfo, VolInfo;

  /* Boot identity requires a GPT HD node (design.md §5.2 step 1); an MBR or
     malformed boot path can never anchor P3 discovery. */
  if (EFI_ERROR (DpFindGpt (BootDp, BootLen, &BootInfo))) return FALSE;
  if (EFI_ERROR (DpFindGpt (VolDp, VolLen, &VolInfo)))   return FALSE;
  if (VolInfo.PartNumber != P3_PARTITION_NUMBER)         return FALSE;
  return VolSameDisk (BootDp, BootLen, VolDp, VolLen);
}