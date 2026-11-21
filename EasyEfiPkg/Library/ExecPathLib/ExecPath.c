/** @file
  ExecPathLib — 路径文法唯一解释器（S04 实现；契约 Include/Contract/ExecPath.h）。

  文法（design §6）：
    data:/<rel>       P3 根相对（运行时解析为实际 fsN）
    auto:/<rel>       外部介质枚举（排除启动盘）
    fs<N>:/<rel>      显式卷号（N 为十进制数字，不推荐）
    boot:<XXXX|menu>  NVRAM 启动项（XXXX = 4 位十六进制；menu = 动态枚举）

  规范化与拒绝规则：
    - 反斜杠 → 正斜杠 [F15]；输出 Rel 无前导 '/'、无空分量、无 '.'/'..' 分量
      （'..' 穿越防御；'.' 与空分量一并拒绝，保持文法严格、规范化无歧义）。
    - 拒绝：空路径 / 未知前缀 / fs 索引非数字或溢出 / boot 非 4 位十六进制
      （menu 除外）/ Rel 超过 255 个 CHAR16（EFI_BAD_BUFFER_SIZE）/
      非法 UTF-8、超 UCS-2 码点（含星面 4 字节序列）、C0 控制字符与 DEL。
    - '.nsh' 后缀 → IsScript；boot:menu → BootMenu。

  解释约定（详见 docs/handoffs/S04.md）：
    - 前缀关键字大小写敏感（小写 data:/auto:/fs/boot:，严格按文法）；
    - 值层面大小写不敏感：十六进制数字、menu 关键字、.nsh 后缀（FAT 惯例）；
    - data:/auto:/fsN: 后必须紧跟 '/'（或规范化前的 '\'），严格文法。

  纯逻辑纪律：只 include Contract 契约头；无动态分配（无需 MemAlloc/MemFree）；
  无 UEFI 协议头、无 libc、无 wcs*；CHAR16 字面量如需一律 U16()。
**/
#include "Contract/EfiCompat.h"
#include "Contract/ExecPath.h"

/* CHAR8 字面前缀匹配（手写，避免 libc；S 短于 Prefix 时遇 NUL 即失配，不越界）。 */
STATIC BOOLEAN AsciiHasPrefix (CONST CHAR8 *S, CONST CHAR8 *Prefix) {
  UINTN i;
  for (i = 0; Prefix[i] != '\0'; i++) {
    if (S[i] != Prefix[i]) return FALSE;
  }
  return TRUE;
}

STATIC BOOLEAN AsciiIsDigit (CHAR8 C) {
  return (BOOLEAN)(C >= '0' && C <= '9');
}

/* 十六进制数字（大小写不敏感）→ 数值。 */
STATIC BOOLEAN AsciiHexVal (CHAR8 C, UINTN *V) {
  if (C >= '0' && C <= '9') { *V = (UINTN)(C - '0');      return TRUE; }
  if (C >= 'a' && C <= 'f') { *V = (UINTN)(C - 'a' + 10); return TRUE; }
  if (C >= 'A' && C <= 'F') { *V = (UINTN)(C - 'A' + 10); return TRUE; }
  return FALSE;
}

/* CHAR8 整串大小写不敏感相等（menu 关键字用）。 */
STATIC BOOLEAN AsciiEqualNoCase (CONST CHAR8 *A, CONST CHAR8 *B) {
  UINTN i;
  for (i = 0; A[i] != '\0' && B[i] != '\0'; i++) {
    CHAR8 Ca = A[i], Cb = B[i];
    if (Ca >= 'A' && Ca <= 'Z') Ca = (CHAR8)(Ca + ('a' - 'A'));
    if (Cb >= 'A' && Cb <= 'Z') Cb = (CHAR8)(Cb + ('a' - 'A'));
    if (Ca != Cb) return FALSE;
  }
  return (BOOLEAN)(A[i] == '\0' && B[i] == '\0');
}

STATIC CHAR16 Lower16 (CHAR16 C) {
  return (C >= 'A' && C <= 'Z') ? (CHAR16)(C + ('a' - 'A')) : C;
}

/**
  解码一个 UTF-8 码点（NUL 终止输入）。只接受可落入 UCS-2 的合法序列：
  1/2/3 字节；拒绝 overlong、代理区、孤立续字节、截断，以及 >=0xF0 前导
  （4 字节序列码点必然 >U+FFFF，UCS-2 无法表示）。
  逐字节读取：续字节为 NUL 时立即拒绝，绝不读越过终止符。
**/
STATIC EFI_STATUS Utf8DecodeCp (CONST CHAR8 *S, UINTN *Pos, UINT32 *Cp) {
  UINT8 B0 = (UINT8)S[*Pos];
  UINT8 B1, B2;

  if (B0 < 0x80) {
    *Cp = B0;
    *Pos += 1;
    return EFI_SUCCESS;
  }
  if (B0 >= 0xC2 && B0 <= 0xDF) {          /* 2 字节；0xC0/0xC1 为 overlong */
    B1 = (UINT8)S[*Pos + 1];
    if (B1 < 0x80 || B1 > 0xBF) return EFI_INVALID_PARAMETER;
    *Cp = ((UINT32)(B0 & 0x1F) << 6) | (UINT32)(B1 & 0x3F);
    *Pos += 2;
    return EFI_SUCCESS;
  }
  if (B0 >= 0xE0 && B0 <= 0xEF) {          /* 3 字节 */
    B1 = (UINT8)S[*Pos + 1];
    if (B1 < 0x80 || B1 > 0xBF) return EFI_INVALID_PARAMETER;
    if (B0 == 0xE0 && B1 < 0xA0) return EFI_INVALID_PARAMETER;  /* overlong */
    if (B0 == 0xED && B1 > 0x9F) return EFI_INVALID_PARAMETER;  /* UTF-16 代理区 */
    B2 = (UINT8)S[*Pos + 2];
    if (B2 < 0x80 || B2 > 0xBF) return EFI_INVALID_PARAMETER;
    *Cp = ((UINT32)(B0 & 0x0F) << 12) | ((UINT32)(B1 & 0x3F) << 6) | (UINT32)(B2 & 0x3F);
    *Pos += 3;
    return EFI_SUCCESS;
  }
  /* 0x80-0xBF 孤立续字节；0xC0/0xC1 overlong；>=0xF0 星面/非法。 */
  return EFI_INVALID_PARAMETER;
}

/**
  校验规范化后的 Rel：分量非空（拒绝前导/尾随/连续斜杠），拒绝 '.' 与 '..'。
  含点文件名（foo..bar / ..hidden / ...）是合法分量，不受影响。
**/
STATIC EFI_STATUS RelValidate (CONST CHAR16 *Rel) {
  UINTN Start = 0;
  UINTN K;
  for (K = 0; ; K++) {
    if (Rel[K] == '/' || Rel[K] == 0) {
      UINTN Len = K - Start;
      if (Len == 0) return EFI_INVALID_PARAMETER;
      if (Len == 1 && Rel[Start] == '.') return EFI_INVALID_PARAMETER;
      if (Len == 2 && Rel[Start] == '.' && Rel[Start + 1] == '.') {
        return EFI_INVALID_PARAMETER;      /* '..' 穿越 */
      }
      if (Rel[K] == 0) break;
      Start = K + 1;
    }
  }
  return EFI_SUCCESS;
}

/* '.nsh' 后缀判定（大小写不敏感，FAT 惯例）。 */
STATIC BOOLEAN HasNshSuffix (CONST CHAR16 *Rel, UINTN Len) {
  if (Len < 4) return FALSE;
  if (Rel[Len - 4] != '.') return FALSE;
  return (BOOLEAN)(Lower16 (Rel[Len - 3]) == 'n' &&
                   Lower16 (Rel[Len - 2]) == 's' &&
                   Lower16 (Rel[Len - 1]) == 'h');
}

/**
  解析路径文法（契约 ExecPath.h）。callee 不分配内存；Out 由调用方提供，
  入口先清零（失败时 Out 为干净零值）。

  @retval EFI_SUCCESS           解析成功。
  @retval EFI_INVALID_PARAMETER 文法非法（空/未知前缀/穿越/坏索引/坏 boot/
                                非法 UTF-8/控制字符/NULL 参数）。
  @retval EFI_BAD_BUFFER_SIZE   Rel 超过 255 个 CHAR16。
**/
EFI_STATUS PathParse (CONST CHAR8 *Utf8Path, RESOLVED_PATH *Out) {
  PATH_KIND    Kind;
  CONST CHAR8  *Rest;
  UINTN        FsIndex = 0;
  UINTN        RelCap, Count, Pos, I;
  EFI_STATUS   Status;

  if (Utf8Path == NULL || Out == NULL) return EFI_INVALID_PARAMETER;

  RelCap = sizeof (Out->Rel) / sizeof (Out->Rel[0]);
  Out->Kind       = PATH_DATA;
  Out->FsIndex    = 0;
  Out->BootNumber = 0;
  Out->BootMenu   = FALSE;
  Out->IsScript   = FALSE;
  for (I = 0; I < RelCap; I++) Out->Rel[I] = 0;

  /* ---- 前缀识别（关键字小写敏感） ---- */
  if (AsciiHasPrefix (Utf8Path, "data:")) {
    Kind = PATH_DATA; Rest = Utf8Path + 5;
  } else if (AsciiHasPrefix (Utf8Path, "auto:")) {
    Kind = PATH_AUTO; Rest = Utf8Path + 5;
  } else if (AsciiHasPrefix (Utf8Path, "boot:")) {
    Kind = PATH_BOOT; Rest = Utf8Path + 5;
  } else if (AsciiHasPrefix (Utf8Path, "fs")) {
    UINTN Val = 0, Digits = 0, D;
    CONST UINTN Max = ~(UINTN)0;
    Kind = PATH_FS;
    I = 2;
    while (AsciiIsDigit (Utf8Path[I])) {
      D = (UINTN)(Utf8Path[I] - '0');
      if (Val > (Max - D) / 10) return EFI_INVALID_PARAMETER;   /* UINTN 溢出 */
      Val = Val * 10 + D;
      I++; Digits++;
    }
    if (Digits == 0 || Utf8Path[I] != ':') return EFI_INVALID_PARAMETER;
    FsIndex = Val;
    Rest = Utf8Path + I + 1;
  } else {
    return EFI_INVALID_PARAMETER;          /* 空路径 / 未知前缀 */
  }

  /* ---- boot:XXXX | boot:menu ---- */
  if (Kind == PATH_BOOT) {
    if (AsciiEqualNoCase (Rest, "menu")) {
      Out->BootMenu = TRUE;
    } else {
      UINTN V, Number = 0;
      for (I = 0; I < 4; I++) {
        if (!AsciiHexVal (Rest[I], &V)) return EFI_INVALID_PARAMETER;
        Number = (Number << 4) | V;
      }
      if (Rest[4] != '\0') return EFI_INVALID_PARAMETER;  /* 必须恰好 4 位 */
      Out->BootNumber = (UINT16)Number;
    }
    Out->Kind = PATH_BOOT;
    return EFI_SUCCESS;
  }

  /* ---- data:/auto:/fsN: 后必须紧跟分隔符（'\' 规范化接受） ---- */
  if (Rest[0] != '/' && Rest[0] != '\\') return EFI_INVALID_PARAMETER;

  /* ---- UTF-8 → UCS-2，反斜杠规范化，长度/控制字符检查 ---- */
  Count = 0;
  Pos   = 1;
  while (Rest[Pos] != '\0') {
    UINT32 Cp;
    Status = Utf8DecodeCp (Rest, &Pos, &Cp);
    if (EFI_ERROR (Status)) return Status;
    if (Cp < 0x20 || Cp == 0x7F) return EFI_INVALID_PARAMETER;  /* 控制字符 */
    if (Count >= RelCap - 1) return EFI_BAD_BUFFER_SIZE;        /* 超长 */
    Out->Rel[Count] = (Cp == '\\') ? (CHAR16)'/' : (CHAR16)Cp;
    Count++;
  }
  Out->Rel[Count] = 0;

  Status = RelValidate (Out->Rel);
  if (EFI_ERROR (Status)) return Status;

  Out->Kind     = Kind;
  Out->FsIndex  = FsIndex;
  Out->IsScript = HasNshSuffix (Out->Rel, Count);
  return EFI_SUCCESS;
}

/**
  架构掩码过滤（契约 ExecPath.h）：bit0=x64（ARCH_X64_BIT），
  bit1=aarch64（ARCH_AARCH64_BIT）。条目掩码与当前架构位有交集即匹配；
  掩码 0（未指定架构）不匹配任何架构。
**/
BOOLEAN PathArchMatch (UINT8 ItemArchMask, UINT8 CurrentArch) {
  return (BOOLEAN)((ItemArchMask & CurrentArch) != 0);
}
