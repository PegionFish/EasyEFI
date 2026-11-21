/* S05 VolumeModelLib host tests — T05-1..8 (docs/parallel-tasks.md §S05).
 * Synthetic device-path byte buffers; pure logic, no firmware/mocks needed.
 * Run: make -f host.mk test   (clang/gcc + ASan/UBSan).
 */
#include "test.h"
#include "Contract/VolumeModel.h"

/* ---- little-endian writers ---- */
static void PutU16 (UINT8 *P, UINT16 V) { P[0] = (UINT8)V; P[1] = (UINT8)(V >> 8); }
static void PutU32 (UINT8 *P, UINT32 V) { int i; for (i = 0; i < 4; i++) P[i] = (UINT8)(V >> (8 * i)); }
static void PutU64 (UINT8 *P, UINT64 V) { int i; for (i = 0; i < 8; i++) P[i] = (UINT8)(V >> (8 * i)); }

/* ---- device-path builder (Spec 2.10 §10 node layout) ---- */
typedef struct { UINT8 B[1024]; UINTN N; } DP;
static void DpInit (DP *D) { D->N = 0; }
static void DpNode (DP *D, UINT8 Type, UINT8 Sub, CONST UINT8 *Payload, UINTN PLen) {
  UINT16 L = (UINT16)(4 + PLen);
  D->B[D->N++] = Type; D->B[D->N++] = Sub;
  PutU16 (D->B + D->N, L); D->N += 2;
  if (PLen) memcpy (D->B + D->N, Payload, PLen);
  D->N += PLen;
}
static void DpEnd (DP *D) { DpNode (D, 0x7F, 0xFF, NULL, 0); }
static void DpAcpi (DP *D, UINT32 Hid, UINT32 Uid) {
  UINT8 P[8]; PutU32 (P, Hid); PutU32 (P + 4, Uid); DpNode (D, 0x02, 0x01, P, 8);
}
static void DpPci (DP *D, UINT8 Dev, UINT8 Func) {
  UINT8 P[2]; P[0] = Func; P[1] = Dev; DpNode (D, 0x01, 0x01, P, 2);  /* PCI: Function, Device */
}
static void DpUsb (DP *D, UINT8 Port, UINT8 Iface) {
  UINT8 P[2]; P[0] = Port; P[1] = Iface; DpNode (D, 0x03, 0x05, P, 2);
}
static void DpFilePath (DP *D) {
  UINT8 P[4] = { 'f', 0, 'o', 0 }; DpNode (D, 0x04, 0x04, P, 4);  /* opaque to walker */
}

/* GPT DiskGUIDs (mixed-endian byte patterns, kept verbatim by the lib). */
static CONST UINT8 GUID_A[16] = {0x10,0x32,0x54,0x76,0x98,0xBA,0xDC,0xFE,0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF};
static CONST UINT8 GUID_B[16] = {0xEF,0xCD,0xAB,0x89,0x67,0x45,0x23,0x01,0xFE,0xDC,0xBA,0x98,0x76,0x54,0x32,0x10};

static void DpHd (DP *D, UINT32 PartNum, UINT64 Start, UINT64 Size, CONST UINT8 *Sig, UINT8 Fmt, UINT8 SigType) {
  UINT8 P[38];
  PutU32 (P, PartNum); PutU64 (P + 4, Start); PutU64 (P + 12, Size);
  memcpy (P + 20, Sig, 16); P[36] = Fmt; P[37] = SigType;
  DpNode (D, 0x04, 0x01, P, 38);   /* MEDIA_DEVICE_PATH / MEDIA_HARDDRIVE_DP */
}
static void DpHdGpt (DP *D, UINT32 PartNum, CONST UINT8 *Sig) {
  DpHd (D, PartNum, 2048, 102400, Sig, 0x02, 0x02);
}

/* Standard boot path: ACPI(12) + PCI(6) + USB(6) + HD(42) + END(4) = 70 bytes.
   Controller prefix = 24 bytes; HD node at offset 24. */
static void BuildBoot (DP *D, UINT32 PartNum, CONST UINT8 *Sig) {
  DpInit (D);
  DpAcpi (D, 0x0A034100, 0);
  DpPci (D, 0x14, 0x00);
  DpUsb (D, 0x03, 0x00);
  DpHdGpt (D, PartNum, Sig);
  DpEnd (D);
}

/* ===================== T05-1 synthetic GPT parse ===================== */
TEST (t05_1_synthetic_gpt_parse) {
  DP D; GPT_NODE_INFO Info; UINT8 *Pref; UINTN PLen; int i;

  BuildBoot (&D, 1, GUID_A);
  ASSERT_EQ (D.N, 70);
  ASSERT_OK (DpFindGpt (D.B, D.N, &Info));
  ASSERT_EQ (Info.PartNumber, 1);
  ASSERT_EQ (Info.PartStartLba, 2048);
  ASSERT_EQ (Info.PartEndLba, 2048 + 102400 - 1);
  ASSERT_MEM_EQ (Info.DiskSignature, GUID_A, 16);
  for (i = 0; i < 16; i++) ASSERT_EQ (Info.PartTypeGuid[i], 0);  /* not in DP node */

  ASSERT_OK (DpDiskPrefix (D.B, D.N, &Pref, &PLen));
  ASSERT_EQ (PLen, 24);
  ASSERT_MEM_EQ (Pref, D.B, 24);
  MemFree (Pref);

  /* HD-only path (no controller): empty prefix, still parses. */
  DpInit (&D); DpHdGpt (&D, 3, GUID_B); DpEnd (&D);
  ASSERT_OK (DpFindGpt (D.B, D.N, &Info));
  ASSERT_EQ (Info.PartNumber, 3);
  ASSERT_MEM_EQ (Info.DiskSignature, GUID_B, 16);
  ASSERT_OK (DpDiskPrefix (D.B, D.N, &Pref, &PLen));
  ASSERT_EQ (PLen, 0);
  ASSERT_TRUE (Pref != NULL);   /* callee always allocates; caller MemFree */
  MemFree (Pref);
}

/* ===================== T05-2 nested partitions: first HD ===================== */
TEST (t05_2_nested_first_hd) {
  DP D; GPT_NODE_INFO Info; UINT8 *Pref; UINTN PLen;

  /* Two stacked HD nodes: the first (outer partition) wins. */
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0); DpPci (&D, 0x14, 0);
  DpHdGpt (&D, 1, GUID_A); DpHdGpt (&D, 3, GUID_A); DpEnd (&D);
  ASSERT_OK (DpFindGpt (D.B, D.N, &Info));
  ASSERT_EQ (Info.PartNumber, 1);
  ASSERT_OK (DpDiskPrefix (D.B, D.N, &Pref, &PLen));
  ASSERT_EQ (PLen, 18);   /* ACPI + PCI only */
  ASSERT_MEM_EQ (Pref, D.B, 18);
  MemFree (Pref);

  /* HD followed by a file-path node: still the first HD, prefix stops at HD. */
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0); DpPci (&D, 0x14, 0);
  DpHdGpt (&D, 2, GUID_B); DpFilePath (&D); DpEnd (&D);
  ASSERT_OK (DpFindGpt (D.B, D.N, &Info));
  ASSERT_EQ (Info.PartNumber, 2);
  ASSERT_OK (DpDiskPrefix (D.B, D.N, &Pref, &PLen));
  ASSERT_EQ (PLen, 18);
  MemFree (Pref);
}

/* ===================== T05-3 same / different disk ===================== */
TEST (t05_3_same_disk) {
  DP A, B;

  /* Same controller, different partition AND different signature -> same disk. */
  BuildBoot (&A, 1, GUID_A);
  BuildBoot (&B, 3, GUID_B);
  ASSERT_TRUE (VolSameDisk (A.B, A.N, B.B, B.N));

  /* Clone disk: SAME signature, different controller (PCI device) -> NOT same. */
  DpInit (&B); DpAcpi (&B, 0x0A034100, 0); DpPci (&B, 0x15, 0); DpUsb (&B, 3, 0);
  DpHdGpt (&B, 1, GUID_A); DpEnd (&B);
  ASSERT_FALSE (VolSameDisk (A.B, A.N, B.B, B.N));

  /* Different USB port -> NOT same. */
  DpInit (&B); DpAcpi (&B, 0x0A034100, 0); DpPci (&B, 0x14, 0); DpUsb (&B, 4, 0);
  DpHdGpt (&B, 1, GUID_A); DpEnd (&B);
  ASSERT_FALSE (VolSameDisk (A.B, A.N, B.B, B.N));

  /* Different ACPI UID -> NOT same. */
  DpInit (&B); DpAcpi (&B, 0x0A034100, 1); DpPci (&B, 0x14, 0); DpUsb (&B, 3, 0);
  DpHdGpt (&B, 1, GUID_A); DpEnd (&B);
  ASSERT_FALSE (VolSameDisk (A.B, A.N, B.B, B.N));

  /* Identical full path -> same. */
  BuildBoot (&B, 1, GUID_A);
  ASSERT_TRUE (VolSameDisk (A.B, A.N, B.B, B.N));

  /* Whole-device view (no HD) vs partitioned view, same controller -> same. */
  DpInit (&B); DpAcpi (&B, 0x0A034100, 0); DpPci (&B, 0x14, 0); DpUsb (&B, 3, 0); DpEnd (&B);
  ASSERT_TRUE (VolSameDisk (A.B, A.N, B.B, B.N));

  /* Malformed / NULL / empty inputs -> FALSE (never crash, never TRUE). */
  ASSERT_FALSE (VolSameDisk (A.B, A.N, B.B, 3));
  ASSERT_FALSE (VolSameDisk (NULL, 0, A.B, A.N));
  ASSERT_FALSE (VolSameDisk (A.B, 0, A.B, A.N));
  ASSERT_FALSE (VolSameDisk (A.B, A.N, NULL, 0));
}

/* ===================== T05-4 P3 determination ===================== */
TEST (t05_4_is_p3) {
  DP Boot, Vol; UINT32 n;

  BuildBoot (&Boot, 1, GUID_A);
  BuildBoot (&Vol, 3, GUID_A);
  ASSERT_TRUE (VolIsP3 (Boot.B, Boot.N, Vol.B, Vol.N));

  /* Boot from P2 (AA64 ESP): boot partition number is irrelevant. */
  BuildBoot (&Boot, 2, GUID_A);
  ASSERT_TRUE (VolIsP3 (Boot.B, Boot.N, Vol.B, Vol.N));

  /* Non-3 partition numbers on the same disk -> FALSE. */
  { UINT32 nums[] = {0, 1, 2, 4, 5, 0xFFFFFFFF};
    for (n = 0; n < sizeof (nums) / sizeof (nums[0]); n++) {
      BuildBoot (&Vol, nums[n], GUID_A);
      ASSERT_FALSE (VolIsP3 (Boot.B, Boot.N, Vol.B, Vol.N));
    } }

  /* Partition 3 on a DIFFERENT disk (clone, same signature) -> FALSE. */
  DpInit (&Vol); DpAcpi (&Vol, 0x0A034100, 0); DpPci (&Vol, 0x15, 0); DpUsb (&Vol, 3, 0);
  DpHdGpt (&Vol, 3, GUID_A); DpEnd (&Vol);
  ASSERT_FALSE (VolIsP3 (Boot.B, Boot.N, Vol.B, Vol.N));

  /* MBR boot path -> FALSE (GPT-only). */
  { UINT8 Sig[16]; memset (Sig, 0, 16); PutU32 (Sig, 0xDEADBEEF);
    DpInit (&Boot); DpAcpi (&Boot, 0x0A034100, 0); DpPci (&Boot, 0x14, 0); DpUsb (&Boot, 3, 0);
    DpHd (&Boot, 1, 63, 204800, Sig, 0x01, 0x01); DpEnd (&Boot);
    BuildBoot (&Vol, 3, GUID_A);
    ASSERT_FALSE (VolIsP3 (Boot.B, Boot.N, Vol.B, Vol.N));
    /* MBR volume -> FALSE. */
    BuildBoot (&Boot, 1, GUID_A);
    DpInit (&Vol); DpAcpi (&Vol, 0x0A034100, 0); DpPci (&Vol, 0x14, 0); DpUsb (&Vol, 3, 0);
    DpHd (&Vol, 3, 63, 204800, Sig, 0x01, 0x01); DpEnd (&Vol);
    ASSERT_FALSE (VolIsP3 (Boot.B, Boot.N, Vol.B, Vol.N)); }

  /* Malformed / NULL / empty -> FALSE. */
  BuildBoot (&Vol, 3, GUID_A);
  ASSERT_FALSE (VolIsP3 (Boot.B, Boot.N, Vol.B, 5));
  ASSERT_FALSE (VolIsP3 (NULL, 0, Vol.B, Vol.N));
  ASSERT_FALSE (VolIsP3 (Boot.B, 0, Vol.B, Vol.N));
  ASSERT_FALSE (VolIsP3 (Boot.B, Boot.N, NULL, 0));
}

/* ===================== T05-5 MBR rejection ===================== */
TEST (t05_5_mbr_rejected) {
  DP D; GPT_NODE_INFO Info; UINT8 *Pref; UINTN PLen;
  UINT8 MbrSig[16]; memset (MbrSig, 0, 16); PutU32 (MbrSig, 0xDEADBEEF);

  DpInit (&D); DpAcpi (&D, 0x0A034100, 0); DpPci (&D, 0x14, 0);
  DpHd (&D, 1, 63, 204800, MbrSig, 0x01, 0x01); DpEnd (&D);
  ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_UNSUPPORTED);

  /* MBR with no disk signature -> still UNSUPPORTED. */
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0); DpPci (&D, 0x14, 0);
  DpHd (&D, 1, 63, 204800, MbrSig, 0x01, 0x00); DpEnd (&D);
  ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_UNSUPPORTED);

  /* Prefix extraction is format-independent: MBR path still yields identity. */
  ASSERT_OK (DpDiskPrefix (D.B, D.N, &Pref, &PLen));
  ASSERT_EQ (PLen, 18);
  MemFree (Pref);

  /* GPT format with wrong signature type -> malformed. */
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0);
  DpHd (&D, 1, 2048, 102400, GUID_A, 0x02, 0x01); DpEnd (&D);
  ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_INVALID_PARAMETER);
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0);
  DpHd (&D, 1, 2048, 102400, GUID_A, 0x02, 0x00); DpEnd (&D);
  ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_INVALID_PARAMETER);

  /* Unknown / reserved partition format values -> malformed. */
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0);
  DpHd (&D, 1, 2048, 102400, GUID_A, 0x00, 0x00); DpEnd (&D);
  ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_INVALID_PARAMETER);
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0);
  DpHd (&D, 1, 2048, 102400, GUID_A, 0x03, 0x02); DpEnd (&D);
  ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_INVALID_PARAMETER);
}

/* ===================== T05-6 fuzz (>=5000 rounds) ===================== */
static UINT32 g_rng = 0x12345678u;
static UINT32 Rng (void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

TEST (t05_6_fuzz) {
  UINT8 Buf[320]; UINTN round, len, i, off;
  GPT_NODE_INFO Info; UINT8 *Pref; UINTN PLen; EFI_STATUS S;
  DP D;

  /* 20000 rounds of deterministic random bytes, varied lengths (incl. 0). */
  for (round = 0; round < 20000; round++) {
    len = Rng () % (sizeof (Buf) + 1);
    for (i = 0; i < len; i++) Buf[i] = (UINT8)Rng ();
    S = DpFindGpt (Buf, len, &Info);
    ASSERT_TRUE (S == EFI_SUCCESS || EFI_ERROR (S));
    S = DpDiskPrefix (Buf, len, &Pref, &PLen);
    if (!EFI_ERROR (S)) { ASSERT_TRUE (Pref != NULL); ASSERT_TRUE (PLen <= len); MemFree (Pref); }
    (void) VolIsP3 (Buf, len, Buf, len);
    (void) VolSameDisk (Buf, len, Buf, len);
  }

  /* 5000 rounds: valid prefix + randomly mutated HD payload / length fields. */
  BuildBoot (&D, 3, GUID_A);
  for (round = 0; round < 5000; round++) {
    memcpy (Buf, D.B, D.N);
    for (i = 0; i < 4; i++) Buf[24 + (Rng () % 42)] = (UINT8)Rng ();  /* hit HD node */
    S = DpFindGpt (Buf, D.N, &Info);
    ASSERT_TRUE (S == EFI_SUCCESS || EFI_ERROR (S));
    S = DpDiskPrefix (Buf, D.N, &Pref, &PLen);
    if (!EFI_ERROR (S)) { ASSERT_TRUE (PLen <= D.N); MemFree (Pref); }
    (void) VolIsP3 (D.B, D.N, Buf, D.N);
    (void) VolSameDisk (D.B, D.N, Buf, D.N);
  }

  /* Truncation sweep: every cut point of a valid path must be safe. */
  for (len = 0; len <= D.N; len++) {
    S = DpFindGpt (D.B, len, &Info);
    ASSERT_TRUE (S == EFI_SUCCESS || EFI_ERROR (S));
    S = DpDiskPrefix (D.B, len, &Pref, &PLen);
    if (!EFI_ERROR (S)) { ASSERT_TRUE (PLen <= len); MemFree (Pref); }
    (void) VolIsP3 (D.B, D.N, D.B, len);
    (void) VolSameDisk (D.B, D.N, D.B, len);
  }

  /* Unaligned base offsets 1..7 (byte-wise reads must not assume alignment). */
  for (off = 1; off <= 7; off++) {
    memcpy (Buf + off, D.B, D.N);
    ASSERT_OK (DpFindGpt (Buf + off, D.N, &Info));
    ASSERT_EQ (Info.PartNumber, 3);
    ASSERT_OK (DpDiskPrefix (Buf + off, D.N, &Pref, &PLen));
    ASSERT_EQ (PLen, 24);
    MemFree (Pref);
  }
}

/* ===================== T05-7 byte order / alignment ===================== */
TEST (t05_7_endian_align) {
  DP D; GPT_NODE_INFO Info; UINT8 *Pref; UINTN PLen; UINTN off;
  UINT64 Start = 0xFEDCBA9876543210ULL, Size = 0x1000;
  UINT32 PartNum = 0x11223344;
  UINT8 *Raw;

  DpInit (&D); DpAcpi (&D, 0x0A034100, 0);
  DpHd (&D, PartNum, Start, Size, GUID_A, 0x02, 0x02); DpEnd (&D);
  ASSERT_OK (DpFindGpt (D.B, D.N, &Info));
  ASSERT_EQ (Info.PartNumber, 0x11223344);
  ASSERT_EQ (Info.PartStartLba, Start);
  ASSERT_EQ (Info.PartEndLba, Start + Size - 1);
  ASSERT_MEM_EQ (Info.DiskSignature, GUID_A, 16);   /* verbatim, no endian swap */

  /* On-wire little-endian Length field: HD at offset 12, Length=42 -> 2A 00. */
  ASSERT_EQ (D.B[14], 42);
  ASSERT_EQ (D.B[15], 0);
  /* PartitionNumber LE bytes at HD+4 = offset 16. */
  ASSERT_EQ (D.B[16], 0x44); ASSERT_EQ (D.B[17], 0x33);
  ASSERT_EQ (D.B[18], 0x22); ASSERT_EQ (D.B[19], 0x11);

  /* Heap buffers at every misalignment 1..7: identical results, UBSan-clean. */
  Raw = MemAlloc (D.N + 8);
  ASSERT_TRUE (Raw != NULL);
  for (off = 1; off <= 7; off++) {
    memcpy (Raw + off, D.B, D.N);
    ASSERT_OK (DpFindGpt (Raw + off, D.N, &Info));
    ASSERT_EQ (Info.PartNumber, 0x11223344);
    ASSERT_EQ (Info.PartStartLba, Start);
    ASSERT_OK (DpDiskPrefix (Raw + off, D.N, &Pref, &PLen));
    ASSERT_EQ (PLen, 12);
    ASSERT_MEM_EQ (Pref, D.B, 12);
    MemFree (Pref);
  }
  MemFree (Raw);
}

/* ===================== T05-8 alloc/free pairing (ASan) ===================== */
TEST (t05_8_mem_pairing) {
  DP D; UINT8 *Pref; UINTN PLen; UINTN i;

  BuildBoot (&D, 3, GUID_A);
  /* Repeated alloc/free cycles: ASan catches double-free / use-after-free. */
  for (i = 0; i < 2000; i++) {
    ASSERT_OK (DpDiskPrefix (D.B, D.N, &Pref, &PLen));
    ASSERT_EQ (PLen, 24);
    MemFree (Pref);
  }
  /* Error path: outputs reset to NULL/0, no allocation leaked. */
  Pref = (UINT8 *)1; PLen = 99;
  ASSERT_ERR (DpDiskPrefix (D.B, 3, &Pref, &PLen));
  ASSERT_TRUE (Pref == NULL);
  ASSERT_EQ (PLen, 0);

  /* Boolean APIs allocate+free internally; hammer them. */
  for (i = 0; i < 2000; i++) {
    ASSERT_TRUE (VolSameDisk (D.B, D.N, D.B, D.N));
    ASSERT_TRUE (VolIsP3 (D.B, D.N, D.B, D.N));
  }
}

/* ===================== extra edge cases ===================== */
TEST (t05_extra_edges) {
  DP D; GPT_NODE_INFO Info; UINT8 *Pref; UINTN PLen; UINTN extra;

  /* NULL / zero parameters. */
  ASSERT_EQ (DpFindGpt (NULL, 10, &Info), EFI_INVALID_PARAMETER);
  ASSERT_EQ (DpDiskPrefix (NULL, 10, &Pref, &PLen), EFI_INVALID_PARAMETER);
  BuildBoot (&D, 3, GUID_A);
  ASSERT_EQ (DpFindGpt (D.B, 0, &Info), EFI_INVALID_PARAMETER);
  ASSERT_EQ (DpFindGpt (D.B, D.N, NULL), EFI_INVALID_PARAMETER);
  ASSERT_EQ (DpDiskPrefix (D.B, 0, &Pref, &PLen), EFI_INVALID_PARAMETER);
  ASSERT_EQ (DpDiskPrefix (D.B, D.N, NULL, &PLen), EFI_INVALID_PARAMETER);
  ASSERT_EQ (DpDiskPrefix (D.B, D.N, &Pref, NULL), EFI_INVALID_PARAMETER);

  /* END-only path: no HD -> NOT_FOUND; empty prefix. */
  DpInit (&D); DpEnd (&D);
  ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_NOT_FOUND);
  ASSERT_OK (DpDiskPrefix (D.B, D.N, &Pref, &PLen));
  ASSERT_EQ (PLen, 0); ASSERT_TRUE (Pref != NULL); MemFree (Pref);

  /* Controller-only fragment, no END (clean buffer end). */
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0); DpPci (&D, 0x14, 0);
  ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_NOT_FOUND);
  ASSERT_OK (DpDiskPrefix (D.B, D.N, &Pref, &PLen));
  ASSERT_EQ (PLen, 18); MemFree (Pref);

  /* Node Length < 4. */
  DpInit (&D); D.B[0] = 0x02; D.B[1] = 0x01; D.B[2] = 0x03; D.B[3] = 0x00; D.N = 4;
  ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_INVALID_PARAMETER);
  ASSERT_ERR (DpDiskPrefix (D.B, D.N, &Pref, &PLen));

  /* Node Length > remaining bytes. */
  DpInit (&D); D.B[0] = 0x02; D.B[1] = 0x01; D.B[2] = 0xFF; D.B[3] = 0xFF; D.N = 4;
  ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_INVALID_PARAMETER);
  ASSERT_ERR (DpDiskPrefix (D.B, D.N, &Pref, &PLen));

  /* Truncated header: controller node + 1..3 stray bytes (no END). */
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0);
  for (extra = 1; extra <= 3; extra++) {
    D.B[D.N++] = 0xAA;
    ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_INVALID_PARAMETER);
    ASSERT_ERR (DpDiskPrefix (D.B, D.N, &Pref, &PLen));
  }

  /* HD node with wrong Length (41 / 43): FindGpt rejects; prefix still OK. */
  { UINT8 P[38]; PutU32 (P, 3); PutU64 (P + 4, 2048); PutU64 (P + 12, 102400);
    memcpy (P + 20, GUID_A, 16); P[36] = 0x02; P[37] = 0x02;
    DpInit (&D); DpAcpi (&D, 0x0A034100, 0); DpNode (&D, 0x04, 0x01, P, 37); DpEnd (&D);
    ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_INVALID_PARAMETER);
    ASSERT_OK (DpDiskPrefix (D.B, D.N, &Pref, &PLen)); ASSERT_EQ (PLen, 12); MemFree (Pref);
    DpInit (&D); DpAcpi (&D, 0x0A034100, 0); DpNode (&D, 0x04, 0x01, P, 38);
    D.B[14] = 43; D.B[15] = 0;   /* Length field at HD offset(12)+2 -> claim 43 */
    D.B[D.N++] = 0;              /* one extra byte so the 43-byte node fits */
    DpEnd (&D);
    ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_INVALID_PARAMETER); }

  /* Unknown END subtype -> malformed. */
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0); DpNode (&D, 0x7F, 0x02, NULL, 0);
  ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_INVALID_PARAMETER);
  ASSERT_ERR (DpDiskPrefix (D.B, D.N, &Pref, &PLen));

  /* END_INSTANCE stops the walk: HD in the second instance is ignored. */
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0); DpNode (&D, 0x7F, 0x01, NULL, 0);
  DpHdGpt (&D, 3, GUID_A); DpEnd (&D);
  ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_NOT_FOUND);
  ASSERT_OK (DpDiskPrefix (D.B, D.N, &Pref, &PLen)); ASSERT_EQ (PLen, 12); MemFree (Pref);

  /* Geometry: zero size / overflow rejected; exact boundary accepted. */
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0); DpHd (&D, 3, 2048, 0, GUID_A, 0x02, 0x02); DpEnd (&D);
  ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_INVALID_PARAMETER);
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0);
  DpHd (&D, 3, 0xFFFFFFFFFFFFFFFFULL, 16, GUID_A, 0x02, 0x02); DpEnd (&D);
  ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_INVALID_PARAMETER);
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0);
  DpHd (&D, 3, 0xFFFFFFFFFFFFFFF0ULL, 16, GUID_A, 0x02, 0x02); DpEnd (&D);
  ASSERT_EQ (DpFindGpt (D.B, D.N, &Info), EFI_INVALID_PARAMETER);
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0);
  DpHd (&D, 3, 0xFFFFFFFFFFFFFFF0ULL, 15, GUID_A, 0x02, 0x02); DpEnd (&D);
  ASSERT_OK (DpFindGpt (D.B, D.N, &Info));
  ASSERT_EQ (Info.PartEndLba, 0xFFFFFFFFFFFFFFFEULL);

  /* PartitionNumber 0 = whole device (spec): parsed, but never P3. */
  DpInit (&D); DpAcpi (&D, 0x0A034100, 0); DpHdGpt (&D, 0, GUID_A); DpEnd (&D);
  ASSERT_OK (DpFindGpt (D.B, D.N, &Info));
  ASSERT_EQ (Info.PartNumber, 0);
  { DP Boot; BuildBoot (&Boot, 1, GUID_A);
    ASSERT_FALSE (VolIsP3 (Boot.B, Boot.N, D.B, D.N)); }

  /* Two HD-at-offset-0 paths: empty prefixes compare equal. */
  { DP E; DpInit (&D); DpHdGpt (&D, 1, GUID_A); DpEnd (&D);
    DpInit (&E); DpHdGpt (&E, 3, GUID_B); DpEnd (&E);
    ASSERT_TRUE (VolSameDisk (D.B, D.N, E.B, E.N)); }
}

int main (void) {
  RUN_TEST (t05_1_synthetic_gpt_parse);
  RUN_TEST (t05_2_nested_first_hd);
  RUN_TEST (t05_3_same_disk);
  RUN_TEST (t05_4_is_p3);
  RUN_TEST (t05_5_mbr_rejected);
  RUN_TEST (t05_6_fuzz);
  RUN_TEST (t05_7_endian_align);
  RUN_TEST (t05_8_mem_pairing);
  RUN_TEST (t05_extra_edges);
  RUN_TESTS_END ();
}