/* S10 ExecImageLib 宿主测试（T10-1..4 + 边界）。
 * mock 记录器：tests/HostSeam.c（内部缝）+ tests/HostVol.c（S09 契约合成卷集）
 * + fixture/VolModelFixture.c（S05 契约 VolSameDisk）+ S00 MockFs。
 * 运行：make -f host.mk test（模块目录内）。 */
#include "test.h"
#include "MockFs.h"
#include "HostSeam.h"
#include "HostVol.h"
#include "Contract/ExecOps.h"

/* ---------------- 合成设备路径：USB(端口) + HD(GPT) + END ---------------- */

static CONST UINT8 SIG_A[16] = {
  0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
  0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00
};
static CONST UINT8 SIG_B[16] = {
  0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04,
  0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C
};

static VOID PutLe32 (UINT8 *P, UINT32 V) {
  P[0] = (UINT8)V; P[1] = (UINT8)(V >> 8); P[2] = (UINT8)(V >> 16); P[3] = (UINT8)(V >> 24);
}
static VOID PutLe64 (UINT8 *P, UINT64 V) {
  UINTN i;
  for (i = 0; i < 8; i++) P[i] = (UINT8)(V >> (8 * i));
}

/* UsbPort 不同 → 控制器前缀不同 → 异盘；同 UsbPort 不同分区 → 同盘 */
static UINTN MakeDp (UINT8 *Buf, UINT8 UsbPort, UINT32 PartNum, CONST UINT8 *Sig) {
  UINTN O = 0, i;
  Buf[O] = 0x03; Buf[O + 1] = 0x05; Buf[O + 2] = 6; Buf[O + 3] = 0;   /* USB 节点 */
  Buf[O + 4] = UsbPort; Buf[O + 5] = 0;
  O += 6;
  Buf[O] = 0x04; Buf[O + 1] = 0x01; Buf[O + 2] = 42; Buf[O + 3] = 0;  /* HD(GPT) 节点 */
  PutLe32 (Buf + O + 4, PartNum);
  PutLe64 (Buf + O + 8, 2048);
  PutLe64 (Buf + O + 16, 100000);
  for (i = 0; i < 16; i++) Buf[O + 24 + i] = Sig[i];
  Buf[O + 40] = 0x02;                  /* PartitionFormat = GPT */
  Buf[O + 41] = 0x02;                  /* SignatureType = GPT */
  O += 42;
  Buf[O] = 0x7F; Buf[O + 1] = 0xFF; Buf[O + 2] = 4; Buf[O + 3] = 0;   /* END */
  O += 4;
  return O;                            /* 52 */
}

/* ASCII → CHAR16 本地拷贝（U16() 旋转缓冲不可跨语句持有） */
static VOID SetStr (CHAR16 *Dst, UINTN Cap, CONST CHAR8 *Src) {
  UINTN i;
  for (i = 0; Src[i] != 0 && i + 1 < Cap; i++) Dst[i] = (CHAR16)(UINT8)Src[i];
  Dst[i] = 0;
}

/* 断言 LoadImage 记录的 Dp = VolDp 前缀（去 END）+ FilePath 节点（File 已规范化）+ END */
static VOID CheckFileDp (CONST SEAM_CALL *C, CONST UINT8 *VolDp, UINTN VolLen, CONST CHAR16 *File) {
  UINTN EndOff = VolLen - 4;
  UINTN FileChars = EfiStrLen (File);            /* File 已含前导 '\\' */
  UINTN NodeBytes = 4 + (FileChars + 1) * 2;
  UINTN Total = EndOff + NodeBytes + 4;
  UINTN i, Off;
  ASSERT_TRUE (C != NULL);
  if (C == NULL) return;
  ASSERT_EQ (C->DpLen, Total);
  if (C->DpLen != Total) return;
  ASSERT_MEM_EQ (C->Dp, VolDp, EndOff);
  ASSERT_EQ (C->Dp[EndOff], 0x04);
  ASSERT_EQ (C->Dp[EndOff + 1], 0x04);
  ASSERT_EQ ((UINTN)C->Dp[EndOff + 2] | ((UINTN)C->Dp[EndOff + 3] << 8), NodeBytes);
  Off = EndOff + 4;
  for (i = 0; i < FileChars; i++) {
    ASSERT_EQ (C->Dp[Off + i * 2], (UINT8)File[i]);
    ASSERT_EQ (C->Dp[Off + i * 2 + 1], 0);
  }
  ASSERT_EQ (C->Dp[Off + FileChars * 2], 0);
  ASSERT_EQ (C->Dp[Off + FileChars * 2 + 1], 0);
  Off += (FileChars + 1) * 2;
  ASSERT_EQ (C->Dp[Off], 0x7F);
  ASSERT_EQ (C->Dp[Off + 1], 0xFF);
  ASSERT_EQ (C->Dp[Off + 2], 4);
  ASSERT_EQ (C->Dp[Off + 3], 0);
}

static VOID CheckSeq (CONST UINT32 *Kinds, UINTN N) {
  UINTN i;
  ASSERT_EQ (SeamCount (), N);
  for (i = 0; i < N && i < SeamCount (); i++) {
    ASSERT_EQ (SeamCall (i)->Kind, Kinds[i]);
  }
}

/* ---------------- T10-1 LoadOptions 构造黄金对比 ---------------- */

TEST (t10_1_load_options) {
  UINT8 BootDp[64];
  UINTN BootLen = MakeDp (BootDp, 7, 1, SIG_A);
  CHAR16 ShellBuf[64];
  CONST CHAR16 *Shell = ShellBuf;
  CONST SEAM_CALL *C;
  UINT32 Seq[] = {
    SEAM_BOOT_VOLUME_DP, SEAM_LOAD_IMAGE, SEAM_CONSOLE_SAVE,
    SEAM_EVENTS_CLOSE, SEAM_START_IMAGE, SEAM_CONSOLE_RESTORE
  };
  SetStr (ShellBuf, 64, "\\EFI\\BOOT\\BOOTX64.EFI");

  /* LoadOptions 黄金 = "Shell.efi " argv[0] 令牌 + 脚本路径[ + 参数]。
     argv[0] 不可省：ProcessCommandLine 跳过 Argv[0]（镜像名），file-name 须在 Argv[1+]。 */

  /* a. 无参数 */
  SeamReset (); HostVolReset ();
  SeamSetBootVolDp (BootDp, BootLen);
  ASSERT_OK (ExecNsh (Shell, U16 ("fs0:\\scripts\\flash.nsh"), NULL));
  CheckSeq (Seq, 6);
  C = SeamCall (1);
  ASSERT_TRUE (EfiStrCmp (C->Options, U16 ("Shell.efi fs0:\\scripts\\flash.nsh")) == 0);
  ASSERT_EQ (C->OptionsSize, (EfiStrLen (U16 ("Shell.efi fs0:\\scripts\\flash.nsh")) + 1) * 2);
  CheckFileDp (C, BootDp, BootLen, Shell);

  /* b. 带参数 */
  SeamReset ();
  SeamSetBootVolDp (BootDp, BootLen);
  ASSERT_OK (ExecNsh (Shell, U16 ("fs0:\\scripts\\flash.nsh"), U16 ("-a -b")));
  C = SeamCall (1);
  ASSERT_TRUE (EfiStrCmp (C->Options, U16 ("Shell.efi fs0:\\scripts\\flash.nsh -a -b")) == 0);

  /* c. 空参数 = 无参数（无尾随空格） */
  SeamReset ();
  SeamSetBootVolDp (BootDp, BootLen);
  ASSERT_OK (ExecNsh (Shell, U16 ("fs0:\\scripts\\flash.nsh"), U16 ("")));
  C = SeamCall (1);
  ASSERT_TRUE (EfiStrCmp (C->Options, U16 ("Shell.efi fs0:\\scripts\\flash.nsh")) == 0);

  /* d. 空格路径 → 双引号包裹（Shell 按空格分词，必须引用） */
  SeamReset ();
  SeamSetBootVolDp (BootDp, BootLen);
  ASSERT_OK (ExecNsh (Shell, U16 ("fs0:\\my scripts\\flash.nsh"), NULL));
  C = SeamCall (1);
  ASSERT_TRUE (EfiStrCmp (C->Options, U16 ("Shell.efi \"fs0:\\my scripts\\flash.nsh\"")) == 0);

  /* e. 空格路径 + 参数 */
  SeamReset ();
  SeamSetBootVolDp (BootDp, BootLen);
  ASSERT_OK (ExecNsh (Shell, U16 ("fs0:\\my scripts\\flash.nsh"), U16 ("-x")));
  C = SeamCall (1);
  ASSERT_TRUE (EfiStrCmp (C->Options, U16 ("Shell.efi \"fs0:\\my scripts\\flash.nsh\" -x")) == 0);

  /* f. 嵌套 Shell 退出码透传（StartImage 返回值） */
  SeamReset ();
  SeamSetBootVolDp (BootDp, BootLen);
  SeamSetChildStatus (EFI_ABORTED);
  ASSERT_EQ (ExecNsh (Shell, U16 ("fs0:\\s.nsh"), NULL), EFI_ABORTED);
}

/* ---------------- T10-2 BootNext 变量值编码 ---------------- */

TEST (t10_2_boot_next) {
  CONST SEAM_CALL *C;
  CONST UINT8 ExpectGuid[16] = {
    0x61, 0xDF, 0xE4, 0x8B, 0xCA, 0x93, 0xD2, 0x11,
    0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C
  };

  SeamReset ();
  ASSERT_OK (ExecBootNext (0x1234));
  ASSERT_EQ (SeamCount (), 2);
  C = SeamCall (0);
  ASSERT_EQ (C->Kind, SEAM_SET_VARIABLE);
  ASSERT_TRUE (EfiStrCmp (C->VarName, U16 ("BootNext")) == 0);
  ASSERT_MEM_EQ (C->Guid, ExpectGuid, 16);
  ASSERT_EQ (C->Attr, 0x7);              /* NV | BS | RT（UEFI Spec §3.3） */
  ASSERT_EQ (C->DataSize, 2);
  ASSERT_EQ (C->Data[0], 0x34);          /* UINT16 LE */
  ASSERT_EQ (C->Data[1], 0x12);
  ASSERT_EQ (SeamCall (1)->Kind, SEAM_RESET_SYSTEM);
  ASSERT_EQ (SeamCall (1)->ResetType, 0); /* EfiResetCold */

  /* 边界编号 */
  SeamReset ();
  ASSERT_OK (ExecBootNext (0x0000));
  ASSERT_EQ (SeamCall (0)->Data[0], 0x00);
  ASSERT_EQ (SeamCall (0)->Data[1], 0x00);
  SeamReset ();
  ASSERT_OK (ExecBootNext (0xFFFF));
  ASSERT_EQ (SeamCall (0)->Data[0], 0xFF);
  ASSERT_EQ (SeamCall (0)->Data[1], 0xFF);

  /* 写失败 → 不重启，错误透传 */
  SeamReset ();
  SeamSetVarStatus (EFI_WRITE_PROTECTED);
  ASSERT_EQ (ExecBootNext (1), EFI_WRITE_PROTECTED);
  ASSERT_EQ (SeamCount (), 1);
  ASSERT_EQ (SeamCall (0)->Kind, SEAM_SET_VARIABLE);

  /* ExecReset = cold reset */
  SeamReset ();
  ASSERT_OK (ExecReset ());
  ASSERT_EQ (SeamCount (), 1);
  ASSERT_EQ (SeamCall (0)->Kind, SEAM_RESET_SYSTEM);
  ASSERT_EQ (SeamCall (0)->ResetType, 0);
}

/* ---------------- T10-3 auto: 排除逻辑（合成卷集） ---------------- */

TEST (t10_3_auto_exclude) {
  UINT8 Dp0[64], Dp1[64], Dp2[64], Dp3[64], Dp4[64], Dp5[64];
  UINTN L0, L1, L2, L3, L4, L5;
  FS_OPS *Fs0, *Fs1, *Fs2, *Fs3, *Fs4, *Fs5;
  EXEC_CANDIDATE *Cands = NULL;
  UINTN N = 0;
  CONST UINT8 Data[4] = { 'M', 'Z', 0, 0 };

  L0 = MakeDp (Dp0, 1, 1, SIG_A);      /* 启动盘 P1 */
  L1 = MakeDp (Dp1, 1, 2, SIG_A);      /* 启动盘 P2（同盘异分区） */
  L2 = MakeDp (Dp2, 2, 1, SIG_B);      /* 外部盘 A */
  L3 = MakeDp (Dp3, 3, 1, SIG_B);      /* 外部盘 B（无文件） */
  L4 = MakeDp (Dp4, 4, 1, SIG_B);      /* 外部盘 C（仅目录同名） */
  L5 = MakeDp (Dp5, 5, 1, SIG_A);      /* 克隆盘：同签名异控制器 → 不排除 */

  Fs0 = MockFsNew (); Fs1 = MockFsNew (); Fs2 = MockFsNew ();
  Fs3 = MockFsNew (); Fs4 = MockFsNew (); Fs5 = MockFsNew ();
  MockFsAddFile (Fs0, U16 ("EFI/BOOT/BOOTX64.EFI"), Data, 4);
  MockFsAddFile (Fs1, U16 ("EFI/BOOT/BOOTX64.EFI"), Data, 4);
  MockFsAddFile (Fs2, U16 ("EFI/BOOT/BOOTX64.EFI"), Data, 4);
  MockFsAddFile (Fs5, U16 ("EFI/BOOT/BOOTX64.EFI"), Data, 4);
  MockFsAddDir (Fs4, U16 ("EFI/BOOT/BOOTX64.EFI"));

  HostVolReset ();
  HostVolAdd (U16 ("BOOT1"), Dp0, L0, Fs0);
  HostVolAdd (U16 ("BOOT2"), Dp1, L1, Fs1);
  HostVolAdd (U16 ("EXTA"), Dp2, L2, Fs2);
  HostVolAdd (U16 ("EMPTY"), Dp3, L3, Fs3);
  HostVolAdd (U16 ("DIRONLY"), Dp4, L4, Fs4);
  HostVolAdd (U16 ("CLONE"), Dp5, L5, Fs5);

  ASSERT_OK (ExecAutoCandidates (U16 ("EFI/BOOT/BOOTX64.EFI"), Dp0, L0, &Cands, &N));
  ASSERT_EQ (N, 2);                    /* 启动盘全部分区排除；目录/无文件排除 */
  if (N == 2) {
    ASSERT_TRUE (Cands[0].Fs == Fs2);
    ASSERT_TRUE (Cands[1].Fs == Fs5);
    ASSERT_TRUE (EfiStrCmp (Cands[0].FullPath, U16 ("\\EFI\\BOOT\\BOOTX64.EFI")) == 0);
    ASSERT_TRUE (EfiStrCmp (Cands[0].Display, U16 ("EXTA: \\EFI\\BOOT\\BOOTX64.EFI")) == 0);
    ASSERT_TRUE (EfiStrCmp (Cands[1].Display, U16 ("CLONE: \\EFI\\BOOT\\BOOTX64.EFI")) == 0);
  }
  ExecFreeCandidates (Cands, N);

  /* 0 命中 → EFI_NOT_FOUND */
  ASSERT_EQ (ExecAutoCandidates (U16 ("NO/SUCH.EFI"), Dp0, L0, &Cands, &N), EFI_NOT_FOUND);
  ASSERT_TRUE (Cands == NULL);
  ASSERT_EQ (N, 0);

  /* 参数防御 */
  ASSERT_ERR (ExecAutoCandidates (NULL, Dp0, L0, &Cands, &N));
  ASSERT_ERR (ExecAutoCandidates (U16 ("x"), NULL, 0, &Cands, &N));
  ASSERT_ERR (ExecAutoCandidates (U16 ("x"), Dp0, L0, NULL, &N));
  ASSERT_ERR (ExecAutoCandidates (U16 ("/EFI/x"), Dp0, L0, &Cands, &N));
  ASSERT_ERR (ExecAutoCandidates (U16 ("EFI/../x"), Dp0, L0, &Cands, &N));
  ASSERT_ERR (ExecAutoCandidates (U16 ("a/../../x"), Dp0, L0, &Cands, &N));
  ASSERT_ERR (ExecAutoCandidates (U16 (""), Dp0, L0, &Cands, &N));

  Fs0->Destroy (Fs0); Fs1->Destroy (Fs1); Fs2->Destroy (Fs2);
  Fs3->Destroy (Fs3); Fs4->Destroy (Fs4); Fs5->Destroy (Fs5);
  HostVolReset ();
}

/* ---------------- T10-4 事件清理/控制台恢复调用序列 ---------------- */

TEST (t10_4_exec_sequence) {
  UINT8 Dp[64];
  UINTN Len = MakeDp (Dp, 9, 1, SIG_B);
  FS_OPS *Fs = MockFsNew ();
  EFI_STATUS Ret = 0;
  CONST SEAM_CALL *C;
  CONST UINT8 Data[4] = { 'M', 'Z', 0, 0 };
  UINT32 Seq[] = {
    SEAM_LOAD_IMAGE, SEAM_CONSOLE_SAVE, SEAM_EVENTS_CLOSE,
    SEAM_START_IMAGE, SEAM_CONSOLE_RESTORE
  };

  MockFsAddFile (Fs, U16 ("EFI/Apps/T.efi"), Data, 4);
  HostVolReset ();
  HostVolAdd (U16 ("VOL"), Dp, Len, Fs);

  /* 正常路径：序列 + ImageReturn 捕获 + Dp/Options 黄金 */
  SeamReset ();
  SeamSetChildStatus (EFI_ABORTED);
  ASSERT_EQ (ExecEfi (Fs, U16 ("\\EFI\\Apps\\T.efi"), U16 ("opt str"), &Ret), EFI_ABORTED);
  ASSERT_EQ (Ret, EFI_ABORTED);
  CheckSeq (Seq, 5);
  C = SeamCall (0);
  CheckFileDp (C, Dp, Len, U16 ("\\EFI\\Apps\\T.efi"));
  ASSERT_TRUE (EfiStrCmp (C->Options, U16 ("opt str")) == 0);
  ASSERT_EQ (C->OptionsSize, (EfiStrLen (U16 ("opt str")) + 1) * 2);
  ASSERT_EQ (SeamCall (3)->Kind, SEAM_START_IMAGE);
  ASSERT_EQ (SeamCall (3)->ChildWritten, EFI_ABORTED);

  /* NULL LoadOptions */
  SeamReset ();
  ASSERT_OK (ExecEfi (Fs, U16 ("\\EFI\\Apps\\T.efi"), NULL, &Ret));
  ASSERT_EQ (SeamCall (0)->OptionsSize, 0);
  ASSERT_EQ (Ret, EFI_SUCCESS);

  /* 正斜杠规范化 */
  SeamReset ();
  ASSERT_OK (ExecEfi (Fs, U16 ("/EFI/Apps/T.efi"), NULL, &Ret));
  CheckFileDp (SeamCall (0), Dp, Len, U16 ("\\EFI\\Apps\\T.efi"));

  /* LoadImage 失败：不触碰控制台/不 StartImage；ImageReturn=加载错误 */
  SeamReset ();
  SeamSetLoadStatus (EFI_LOAD_ERROR);
  Ret = 0;
  ASSERT_EQ (ExecEfi (Fs, U16 ("\\EFI\\Apps\\T.efi"), NULL, &Ret), EFI_LOAD_ERROR);
  ASSERT_EQ (Ret, EFI_LOAD_ERROR);
  ASSERT_EQ (SeamCount (), 1);
  ASSERT_EQ (SeamCall (0)->Kind, SEAM_LOAD_IMAGE);

  /* 文件不存在：零平台调用 */
  SeamReset ();
  ASSERT_EQ (ExecEfi (Fs, U16 ("\\NO.efi"), NULL, &Ret), EFI_NOT_FOUND);
  ASSERT_EQ (SeamCount (), 0);

  /* 目录拒绝 */
  MockFsAddDir (Fs, U16 ("ADIR"));
  SeamReset ();
  ASSERT_ERR (ExecEfi (Fs, U16 ("\\ADIR"), NULL, &Ret));
  ASSERT_EQ (SeamCount (), 0);

  /* 参数防御 */
  ASSERT_ERR (ExecEfi (NULL, U16 ("x"), NULL, &Ret));
  ASSERT_ERR (ExecEfi (Fs, NULL, NULL, &Ret));
  ASSERT_ERR (ExecEfi (Fs, U16 (""), NULL, &Ret));

  /* ImageReturn 可选 */
  SeamReset ();
  ASSERT_OK (ExecEfi (Fs, U16 ("\\EFI\\Apps\\T.efi"), NULL, NULL));

  Fs->Destroy (Fs);
  HostVolReset ();
}

/* Fs 实例跨调用不稳定时的兜底：唯一命中解析；多命中 fail-closed */
TEST (t10_4_fs_fallback) {
  UINT8 DpA[64], DpB[64];
  UINTN LenA = MakeDp (DpA, 1, 1, SIG_A);
  UINTN LenB = MakeDp (DpB, 2, 1, SIG_B);
  FS_OPS *FsArg = MockFsNew ();        /* 传入 ExecEfi，但不注册进卷集 */
  FS_OPS *FsA = MockFsNew ();
  FS_OPS *FsB = MockFsNew ();
  EFI_STATUS Ret = 0;
  CONST UINT8 Data[2] = { 'X', 0 };

  MockFsAddFile (FsArg, U16 ("X.efi"), Data, 2);
  MockFsAddFile (FsA, U16 ("X.efi"), Data, 2);
  HostVolReset ();
  HostVolAdd (U16 ("A"), DpA, LenA, FsA);
  HostVolAdd (U16 ("B"), DpB, LenB, FsB);

  SeamReset ();
  ASSERT_OK (ExecEfi (FsArg, U16 ("\\X.efi"), NULL, &Ret));
  CheckFileDp (SeamCall (0), DpA, LenA, U16 ("\\X.efi"));

  /* 两卷同路径且无指针匹配 → 拒绝（不猜卷） */
  MockFsAddFile (FsB, U16 ("X.efi"), Data, 2);
  SeamReset ();
  ASSERT_EQ (ExecEfi (FsArg, U16 ("\\X.efi"), NULL, &Ret), EFI_NOT_FOUND);
  ASSERT_EQ (SeamCount (), 0);

  FsArg->Destroy (FsArg); FsA->Destroy (FsA); FsB->Destroy (FsB);
  HostVolReset ();
}

/* ---------------- 边界补充 ---------------- */

TEST (t10_nsh_invalid) {
  SeamReset (); HostVolReset ();
  ASSERT_ERR (ExecNsh (NULL, U16 ("fs0:\\x.nsh"), NULL));
  ASSERT_ERR (ExecNsh (U16 ("\\Shell.efi"), NULL, NULL));
  ASSERT_ERR (ExecNsh (U16 ("\\Shell.efi"), U16 (""), NULL));
  ASSERT_ERR (ExecNsh (U16 (""), U16 ("fs0:\\x.nsh"), NULL));
  ASSERT_ERR (ExecNsh (U16 ("\\Shell.efi"), U16 ("relative.nsh"), NULL));   /* 缺 fsN: */
  ASSERT_ERR (ExecNsh (U16 ("\\Shell.efi"), U16 ("fs0:\\a\"b.nsh"), NULL)); /* 含双引号 */
  ASSERT_EQ (SeamCount (), 0);
  /* 启动卷设备路径不可得 */
  ASSERT_ERR (ExecNsh (U16 ("\\Shell.efi"), U16 ("fs0:\\x.nsh"), NULL));
}

TEST (t10_free_candidates_null) {
  ExecFreeCandidates (NULL, 0);
  ExecFreeCandidates (NULL, 5);
}

int main (void) {
  RUN_TEST (t10_1_load_options);
  RUN_TEST (t10_2_boot_next);
  RUN_TEST (t10_3_auto_exclude);
  RUN_TEST (t10_4_exec_sequence);
  RUN_TEST (t10_4_fs_fallback);
  RUN_TEST (t10_nsh_invalid);
  RUN_TEST (t10_free_candidates_null);
  RUN_TESTS_END ();
}