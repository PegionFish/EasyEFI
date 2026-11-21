/** @file
  ExecImageLib —— ExecOps 真实后端核心（双环境：UEFI 构建 + 宿主测试）。

  语义依据 design §5.6 / D9 / D10 与评审 F01 / F03 / F20（均已对 edk2-stable202608 源码核实）：
  - ExecEfi：LoadImage(卷设备路径+文件路径) + StartImage；StartImage 前保存 ConOut
    模式/关闭自身事件，返回后恢复；ImageReturn 捕获目标返回码。
  - ExecNsh：LoadImage(启动 ESP 的 Shell.efi) + LoadOptions = L"<脚本全限定路径> [参数]"。
    Shell 命令行 file-name 隐式 -nostartup（Shell.c ProcessCommandLine 原文），防 startup.nsh
    递归；不存在 -s 选项 [F01]。脚本路径含空格时必须加双引号（ShellParametersProtocol.c
    ParseCommandLineToArgs 按空格分词，StripQuotation=TRUE）。脚本结束后停在嵌套 Shell
    提示符（不加 -exit），exit 回调用方 [F20]。
  - ExecBootNext：BootNext(UINT16 LE, NV|BS|RT) + ResetSystem(EfiResetCold)。
  - ExecAutoCandidates：经 VolEnumerate（S09 契约）枚举卷，VolSameDisk（S05 契约）排除
    启动盘全部分区 [F03]，按相对路径找文件；0 命中 EFI_NOT_FOUND。

  平台服务经内部缝 ExecImageSeam.h（UEFI=ExecImageUefi.c，宿主=tests/HostSeam.c）。
  本文件只 include Contract 契约头 + 内部缝头，遵守纯逻辑纪律（宿主 wchar_t 32 位）。

  契约解释（契约未明说处，见 docs/handoffs/S10.md）：
  - ExecEfi 的 FullPath = 卷根相对文件路径（前导 '/' 或 '\\' 均可）；Fs 指定卷。
    卷设备路径经 VolEnumerate 输出中 Vols[i].Fs==Fs 指针匹配取得（契约内唯一 Fs↔Dp
    关联面）；匹配失败时兜底=全系统唯一含该文件的卷（多命中 fail-closed）。
  - ExecNsh 的 ShellFullPath = 启动 ESP 根相对路径（"本 ESP 的 Shell.efi"）；
    ScriptFullPath = 运行时全限定路径（须含 ':'，fsN: 形式，由调用方解析）。
  - EXEC_CANDIDATE.FullPath = 卷根相对全路径（前导 '\\'），与 Fs 配对唯一确定文件；
    fsN: 前缀属 Shell 概念，由调用方 glue 按需添加。
**/
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/ExecOps.h"
#include "Contract/FsOps.h"
#include "Contract/VolumeModel.h"
#include "ExecImageSeam.h"

/* ---------------- 字符串助手 ---------------- */

STATIC UINTN StrBytes (CONST CHAR16 *S) {
  return (EfiStrLen (S) + 1) * sizeof (CHAR16);
}

/* 规范化为根相对路径（去前导斜杠）：FsOps 契约允许 Dir 相对路径且后端自行规范化；
   统一无前导斜杠形式查询 Info，与 auto: 的 RelPath 形式一致 */
STATIC CONST CHAR16 *RootRel (CONST CHAR16 *P) {
  while (*P == L'/' || *P == L'\\') {
    P++;
  }
  return P;
}

/* RelPath 防御：拒绝 '..' 穿越分量（文法权威在 S04 PathParse，此处兜底） */
STATIC BOOLEAN HasDotDot (CONST CHAR16 *P) {
  UINTN i = 0;
  while (P[i] != 0) {
    UINTN Start = i;
    while (P[i] != 0 && P[i] != L'/' && P[i] != L'\\') {
      i++;
    }
    if (i - Start == 2 && P[Start] == L'.' && P[Start + 1] == L'.') {
      return TRUE;
    }
    if (P[i] != 0) {
      i++;
    }
  }
  return FALSE;
}

/* ---------------- 设备路径字节助手（纯逻辑，UEFI Spec 2.10 §10） ---------------- */

#define DP_TYPE_MEDIA   0x04
#define DP_TYPE_END     0x7F
#define DP_SUB_HD       0x01
#define DP_SUB_FILE     0x04
#define DP_SUB_END      0xFF

/* 节点长度（LE，偏移 2..3）；截断/非法返回 0 */
STATIC UINTN NodeLen (CONST UINT8 *Dp, UINTN Len, UINTN Off) {
  UINTN L;
  if (Off + 4 > Len) {
    return 0;
  }
  L = (UINTN)Dp[Off + 2] | ((UINTN)Dp[Off + 3] << 8);
  if (L < 4 || Off + L > Len) {
    return 0;
  }
  return L;
}

/* 定位 END 节点偏移；畸形/无 END → EFI_INVALID_PARAMETER */
STATIC EFI_STATUS DpEndOffset (CONST UINT8 *Dp, UINTN Len, UINTN *EndOff) {
  UINTN Off = 0;
  while (Off < Len) {
    UINTN L = NodeLen (Dp, Len, Off);
    if (L == 0) {
      return EFI_INVALID_PARAMETER;
    }
    if (Dp[Off] == DP_TYPE_END && Dp[Off + 1] == DP_SUB_END) {
      *EndOff = Off;
      return EFI_SUCCESS;
    }
    Off += L;
  }
  return EFI_INVALID_PARAMETER;
}

/**
  卷设备路径（含 END）+ MEDIA_FILEPATH_DP 节点（File 规范化为 '\\' 分隔、前导 '\\'）+ END。
  callee 以 MemAlloc 分配 *Out。File 为卷根相对路径；拒绝空/纯根/含 ':'（fsN: 前缀属调用方）。
**/
STATIC EFI_STATUS BuildFileDp (CONST UINT8 *VolDp, UINTN VolLen, CONST CHAR16 *File,
                               UINT8 **Out, UINTN *OutLen) {
  EFI_STATUS Status;
  UINTN      EndOff, FileChars, NodeBytes, Total, Off, i;
  UINT8      *Buf;
  CONST CHAR16 *P;

  if (VolDp == NULL || VolLen == 0 || File == NULL || Out == NULL || OutLen == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  P = File;
  while (*P == L'/' || *P == L'\\') {
    P++;
  }
  if (*P == 0 || EfiStrChr (P, L':') != NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Status = DpEndOffset (VolDp, VolLen, &EndOff);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  FileChars = 1 + EfiStrLen (P);              /* 前导 '\\' + 其余 */
  NodeBytes = 4 + (FileChars + 1) * 2;        /* 节点头 + UCS-2 名 + NUL */
  if (NodeBytes > 0xFFFF) {
    return EFI_INVALID_PARAMETER;
  }
  Total = EndOff + NodeBytes + 4;
  Buf = (UINT8 *)MemAlloc (Total);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  for (i = 0; i < EndOff; i++) {
    Buf[i] = VolDp[i];
  }
  Off = EndOff;
  Buf[Off]     = DP_TYPE_MEDIA;
  Buf[Off + 1] = DP_SUB_FILE;
  Buf[Off + 2] = (UINT8)(NodeBytes & 0xFF);
  Buf[Off + 3] = (UINT8)(NodeBytes >> 8);
  Off += 4;
  Buf[Off] = (UINT8)L'\\'; Buf[Off + 1] = 0;
  Off += 2;
  for (i = 0; P[i] != 0; i++) {
    CHAR16 C = (P[i] == L'/') ? L'\\' : P[i];
    Buf[Off]     = (UINT8)(C & 0xFF);         /* UCS-2 LE（UEFI 双架构均小端） */
    Buf[Off + 1] = (UINT8)(C >> 8);
    Off += 2;
  }
  Buf[Off] = 0; Buf[Off + 1] = 0;
  Off += 2;
  Buf[Off] = DP_TYPE_END; Buf[Off + 1] = DP_SUB_END; Buf[Off + 2] = 4; Buf[Off + 3] = 0;

  *Out = Buf;
  *OutLen = Total;
  return EFI_SUCCESS;
}

/* ---------------- ExecEfi ---------------- */

EFI_STATUS
ExecEfi (
  FS_OPS       *Fs,
  CONST CHAR16 *FullPath,
  CONST CHAR16 *LoadOptions,
  EFI_STATUS   *ImageReturn
  )
{
  EFI_STATUS    Status, Child;
  FS_ENTRY      Entry;
  VOLUME_INFO   *Vols = NULL;
  UINTN         N = 0, i, Hit, HitIdx;
  UINT8         *VolDp = NULL, *FullDp = NULL;
  UINTN         VolLen = 0, FullLen = 0;
  VOID          *Image = NULL;
  UINTN         OptSize;

  if (ImageReturn != NULL) {
    *ImageReturn = EFI_SUCCESS;
  }
  if (Fs == NULL || Fs->Info == NULL || FullPath == NULL || FullPath[0] == 0) {
    return EFI_INVALID_PARAMETER;
  }

  /* 1. 指定卷上存在性检查（目录拒绝）；Info 查询统一根相对形式 */
  Status = Fs->Info (Fs, RootRel (FullPath), &Entry);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Entry.IsDir) {
    return EFI_INVALID_PARAMETER;
  }

  /* 2. 卷设备路径解析：VOLUME_INFO 是契约内唯一 Fs↔Dp 关联面 */
  Status = VolEnumerate (&Vols, &N);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  HitIdx = N;
  for (i = 0; i < N; i++) {
    if (Vols[i].Fs == Fs) {
      HitIdx = i;
      break;
    }
  }
  if (HitIdx == N) {
    /* 兜底：Fs 实例跨调用不稳定时，取全系统唯一含该文件的卷；多命中 fail-closed */
    Hit = 0;
    for (i = 0; i < N; i++) {
      if (Vols[i].Fs != NULL && Vols[i].Fs->Info != NULL &&
          !EFI_ERROR (Vols[i].Fs->Info (Vols[i].Fs, RootRel (FullPath), &Entry)) && !Entry.IsDir) {
        Hit++;
        HitIdx = i;
      }
    }
    if (Hit != 1) {
      VolFree (Vols, N);
      return EFI_NOT_FOUND;
    }
  }
  if (Vols[HitIdx].Dp == NULL || Vols[HitIdx].DpLen == 0) {
    VolFree (Vols, N);
    return EFI_NOT_FOUND;
  }
  VolDp = (UINT8 *)MemAlloc (Vols[HitIdx].DpLen);
  if (VolDp == NULL) {
    VolFree (Vols, N);
    return EFI_OUT_OF_RESOURCES;
  }
  for (i = 0; i < Vols[HitIdx].DpLen; i++) {
    VolDp[i] = Vols[HitIdx].Dp[i];
  }
  VolLen = Vols[HitIdx].DpLen;
  VolFree (Vols, N);

  /* 3. 卷设备路径 + 文件节点 */
  Status = BuildFileDp (VolDp, VolLen, FullPath, &FullDp, &FullLen);
  MemFree (VolDp);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* 4. LoadImage（LoadOptions 经 LoadedImage 协议注入，见 glue） */
  OptSize = (LoadOptions != NULL) ? StrBytes (LoadOptions) : 0;
  Status = gExecSeam->LoadImage (FullDp, FullLen, LoadOptions, OptSize, &Image);
  MemFree (FullDp);
  if (EFI_ERROR (Status)) {
    if (ImageReturn != NULL) {
      *ImageReturn = Status;
    }
    return Status;
  }

  /* 5. 清理自身事件/保存控制台 → StartImage → 恢复控制台（契约注释顺序） */
  (VOID)gExecSeam->ConsoleSave ();
  (VOID)gExecSeam->EventsClose ();
  Child = EFI_SUCCESS;
  Status = gExecSeam->StartImage (Image, &Child);
  (VOID)gExecSeam->ConsoleRestore ();

  if (ImageReturn != NULL) {
    *ImageReturn = Child;
  }
  return Status;
}

/* ---------------- ExecNsh ---------------- */

EFI_STATUS
ExecNsh (
  CONST CHAR16 *ShellFullPath,
  CONST CHAR16 *ScriptFullPath,
  CONST CHAR16 *Args
  )
{
  EFI_STATUS   Status, Child;
  UINT8        *VolDp = NULL, *ShellDp = NULL;
  UINTN        VolLen = 0, ShellLen = 0;
  CHAR16       *Opts = NULL;
  UINTN        ScriptLen, ArgsLen, Total, Pos, i;
  BOOLEAN      Quote;
  VOID         *Image = NULL;

  if (ShellFullPath == NULL || ShellFullPath[0] == 0 ||
      ScriptFullPath == NULL || ScriptFullPath[0] == 0) {
    return EFI_INVALID_PARAMETER;
  }
  /* 脚本路径必须是运行时全限定路径（fsN: 形式）；含双引号无法安全引用 */
  if (EfiStrChr (ScriptFullPath, L':') == NULL ||
      EfiStrChr (ScriptFullPath, L'"') != NULL) {
    return EFI_INVALID_PARAMETER;
  }

  /* LoadOptions = ["]<script>["][ <args>]（UCS-2，含 NUL）。
     含空格必须加引号：Shell 按空格分词后再对含空格 file-name 自动补引号。 */
  Quote     = (EfiStrChr (ScriptFullPath, L' ') != NULL);
  ScriptLen = EfiStrLen (ScriptFullPath);
  ArgsLen   = (Args != NULL && Args[0] != 0) ? EfiStrLen (Args) : 0;
  Total     = ScriptLen + (Quote ? 2 : 0) + (ArgsLen != 0 ? 1 + ArgsLen : 0) + 1;
  Opts = (CHAR16 *)MemAlloc (Total * sizeof (CHAR16));
  if (Opts == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Pos = 0;
  if (Quote) {
    Opts[Pos++] = L'"';
  }
  for (i = 0; i < ScriptLen; i++) {
    Opts[Pos++] = ScriptFullPath[i];
  }
  if (Quote) {
    Opts[Pos++] = L'"';
  }
  if (ArgsLen != 0) {
    Opts[Pos++] = L' ';
    for (i = 0; i < ArgsLen; i++) {
      Opts[Pos++] = Args[i];
    }
  }
  Opts[Pos] = 0;

  /* 启动卷（本镜像来源卷）设备路径 + Shell 文件节点 */
  Status = gExecSeam->BootVolumeDp (&VolDp, &VolLen);
  if (EFI_ERROR (Status)) {
    MemFree (Opts);
    return Status;
  }
  Status = BuildFileDp (VolDp, VolLen, ShellFullPath, &ShellDp, &ShellLen);
  MemFree (VolDp);
  if (EFI_ERROR (Status)) {
    MemFree (Opts);
    return Status;
  }

  Status = gExecSeam->LoadImage (ShellDp, ShellLen, Opts, Total * sizeof (CHAR16), &Image);
  MemFree (ShellDp);
  if (EFI_ERROR (Status)) {
    MemFree (Opts);
    return Status;
  }

  (VOID)gExecSeam->ConsoleSave ();
  (VOID)gExecSeam->EventsClose ();
  Child = EFI_SUCCESS;
  Status = gExecSeam->StartImage (Image, &Child);
  (VOID)gExecSeam->ConsoleRestore ();

  /* 子镜像同步执行完毕，LoadOptions 已被消费（Shell 启动时自行拷贝） */
  MemFree (Opts);
  return Status;
}

/* ---------------- BootNext / Reset ---------------- */

/* EFI_GLOBAL_VARIABLE_GUID 的 16 字节内存布局（小端） */
STATIC CONST UINT8 mEfiGlobalVariableGuid[16] = {
  0x61, 0xDF, 0xE4, 0x8B, 0xCA, 0x93, 0xD2, 0x11,
  0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C
};

EFI_STATUS
ExecBootNext (
  UINT16 Number
  )
{
  EFI_STATUS Status;
  UINT8      Data[2];

  Data[0] = (UINT8)(Number & 0xFF);        /* UINT16 LE（UEFI Spec §3.3） */
  Data[1] = (UINT8)(Number >> 8);
  Status = gExecSeam->SetVariable (
             U16 ("BootNext"),
             mEfiGlobalVariableGuid,
             EXEC_VAR_NV | EXEC_VAR_BS | EXEC_VAR_RT,
             Data,
             sizeof (Data)
             );
  if (EFI_ERROR (Status)) {
    return Status;                         /* 写失败不重启 */
  }
  gExecSeam->ResetSystem (EXEC_RESET_COLD);
  return EFI_SUCCESS;                      /* UEFI 上不可达 */
}

EFI_STATUS
ExecReset (
  VOID
  )
{
  gExecSeam->ResetSystem (EXEC_RESET_COLD);
  return EFI_SUCCESS;                      /* UEFI 上不可达 */
}

/* ---------------- auto: 候选 ---------------- */

/* 候选判定：排除启动盘（同盘前缀，含其全部分区 [F03]）+ 相对路径命中非目录文件 */
STATIC BOOLEAN IsAutoCandidate (CONST VOLUME_INFO *Vol, CONST UINT8 *BootDp,
                                UINTN BootLen, CONST CHAR16 *RelPath) {
  FS_ENTRY Entry;
  if (Vol->Dp == NULL || Vol->DpLen == 0 || Vol->Fs == NULL || Vol->Fs->Info == NULL) {
    return FALSE;
  }
  if (VolSameDisk (BootDp, BootLen, Vol->Dp, Vol->DpLen)) {
    return FALSE;
  }
  if (EFI_ERROR (Vol->Fs->Info (Vol->Fs, RelPath, &Entry))) {
    return FALSE;
  }
  return (BOOLEAN)!Entry.IsDir;
}

/* Display = 卷标签 + ": " + 相对路径（'\\' 形式），有界 Cap（含 NUL） */
STATIC VOID BuildDisplay (CHAR16 *Dst, UINTN Cap, CONST CHAR16 *Label, CONST CHAR16 *RelPath) {
  UINTN Pos = 0, i;
  if (Cap == 0) {
    return;
  }
  for (i = 0; Pos + 1 < Cap && Label[i] != 0; i++) {
    Dst[Pos++] = Label[i];
  }
  if (Pos + 2 < Cap) {
    Dst[Pos++] = L':';
    Dst[Pos++] = L' ';
  }
  if (Pos + 1 < Cap) {
    Dst[Pos++] = L'\\';
  }
  for (i = 0; Pos + 1 < Cap && RelPath[i] != 0; i++) {
    Dst[Pos++] = (RelPath[i] == L'/') ? L'\\' : RelPath[i];
  }
  Dst[Pos] = 0;
}

/* FullPath = '\\' + 相对路径（'\\' 形式），有界 Cap（含 NUL） */
STATIC VOID BuildFullPath (CHAR16 *Dst, UINTN Cap, CONST CHAR16 *RelPath) {
  UINTN Pos = 0, i;
  if (Cap < 2) {
    return;
  }
  Dst[Pos++] = L'\\';
  for (i = 0; Pos + 1 < Cap && RelPath[i] != 0; i++) {
    Dst[Pos++] = (RelPath[i] == L'/') ? L'\\' : RelPath[i];
  }
  Dst[Pos] = 0;
}

EFI_STATUS
ExecAutoCandidates (
  CONST CHAR16     *RelPath,
  CONST UINT8      *BootDp,
  UINTN            BootLen,
  EXEC_CANDIDATE   **Out,
  UINTN            *Count
  )
{
  EFI_STATUS     Status;
  VOLUME_INFO    *Vols = NULL;
  UINTN          N = 0, i, Hits = 0, K = 0;
  EXEC_CANDIDATE *List;

  if (Out != NULL) {
    *Out = NULL;
  }
  if (Count != NULL) {
    *Count = 0;
  }
  if (RelPath == NULL || RelPath[0] == 0 || BootDp == NULL || BootLen == 0 ||
      Out == NULL || Count == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (RelPath[0] == L'/' || RelPath[0] == L'\\' || HasDotDot (RelPath)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = VolEnumerate (&Vols, &N);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  for (i = 0; i < N; i++) {
    if (IsAutoCandidate (&Vols[i], BootDp, BootLen, RelPath)) {
      Hits++;
    }
  }
  if (Hits == 0) {
    VolFree (Vols, N);
    return EFI_NOT_FOUND;
  }
  List = (EXEC_CANDIDATE *)MemAlloc (Hits * sizeof (EXEC_CANDIDATE));
  if (List == NULL) {
    VolFree (Vols, N);
    return EFI_OUT_OF_RESOURCES;
  }
  for (i = 0; i < N; i++) {
    if (!IsAutoCandidate (&Vols[i], BootDp, BootLen, RelPath)) {
      continue;
    }
    BuildDisplay (List[K].Display, sizeof (List[K].Display) / sizeof (CHAR16),
                  Vols[i].Label, RelPath);
    BuildFullPath (List[K].FullPath, sizeof (List[K].FullPath) / sizeof (CHAR16), RelPath);
    List[K].Fs = Vols[i].Fs;               /* 借用，不拥有（VolFree 不得销毁 Fs 实例） */
    K++;
  }
  VolFree (Vols, N);
  *Out = List;
  *Count = Hits;
  return EFI_SUCCESS;
}

VOID
ExecFreeCandidates (
  EXEC_CANDIDATE *List,
  UINTN          Count
  )
{
  (VOID)Count;                             /* 单块分配；Fs 为借用，不释放 */
  if (List != NULL) {
    MemFree (List);
  }
}