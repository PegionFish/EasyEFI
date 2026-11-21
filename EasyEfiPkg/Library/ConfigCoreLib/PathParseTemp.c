/** @file
  ★ 临时 PathParse 实现（S03 测试脚手架，非 S04 交付）★
  S04 并行开发中，S00 stub 只有 probe 函数（无 PathParse 符号），而 T03 全绿与
  QEMU 冒烟链接现在就需要它。本文件按 Contract/ExecPath.h + 任务书 S04 规则忠实实现，
  仅供：宿主测试（host.mk）与冒烟 app（TestApp/ConfigSmoke.inf [Sources]）链接。
  **不进 ConfigCoreLib.inf**（避免与 S04 真实实现重复定义）。
  S18 合并 S04 后：host.mk 改编译 ExecPathLib 真实源码、ConfigSmoke.inf 移除本文件。
  规则：data:/auto:/fsN:/boot: 前缀；反斜杠→正斜杠；拒绝 .. 穿越/空路径/未知前缀/
  fs 索引非数字/boot 非 4 位十六进制（menu 除外）；.nsh → IsScript；boot:menu → BootMenu。
**/
#include "Contract/EfiCompat.h"
#include "Contract/ExecPath.h"

#define TEMP_PATH_MAX_BYTES 768   /* 255 CHAR16 × 最坏 3 字节/CP（BMP） */

STATIC BOOLEAN HexVal (CHAR8 C, UINTN *V) {
  if (C >= '0' && C <= '9') { *V = (UINTN)(C - '0'); return TRUE; }
  if (C >= 'a' && C <= 'f') { *V = (UINTN)(C - 'a' + 10); return TRUE; }
  if (C >= 'A' && C <= 'F') { *V = (UINTN)(C - 'A' + 10); return TRUE; }
  return FALSE;
}

/* 最小 UTF-8 解码：非法字节 → U+FFFD（逐字节降级，不丢位置）。 */
STATIC UINTN Utf8Next (CONST CHAR8 *S, UINTN Len, UINTN *Pos, UINT32 *Cp) {
  UINT8 B0 = (UINT8)S[*Pos];
  UINTN Need, I;
  UINT32 V;
  if (B0 < 0x80) { *Cp = B0; *Pos += 1; return 1; }
  if (B0 >= 0xC2 && B0 <= 0xDF) { Need = 1; V = B0 & 0x1F; }
  else if (B0 >= 0xE0 && B0 <= 0xEF) { Need = 2; V = B0 & 0x0F; }
  else if (B0 >= 0xF0 && B0 <= 0xF4) { Need = 3; V = B0 & 0x07; }
  else { *Cp = 0xFFFD; *Pos += 1; return 1; }
  if (*Pos + Need >= Len) { *Cp = 0xFFFD; *Pos += 1; return 1; }  /* 续字节不足 */
  for (I = 1; I <= Need; I++) {
    UINT8 B = (UINT8)S[*Pos + I];
    if ((B & 0xC0) != 0x80) { *Cp = 0xFFFD; *Pos += 1; return 1; }
    V = (V << 6) | (B & 0x3F);
  }
  /* overlong / 代理区 / 超界 */
  if ((Need == 1 && V < 0x80) || (Need == 2 && V < 0x800) ||
      (Need == 3 && V < 0x10000) || V > 0x10FFFF ||
      (V >= 0xD800 && V <= 0xDFFF)) {
    *Cp = 0xFFFD; *Pos += 1; return 1;
  }
  *Cp = V;
  *Pos += Need + 1;
  return Need + 1;
}

EFI_STATUS PathParse (CONST CHAR8 *Utf8Path, RESOLVED_PATH *Out) {
  CHAR8  Norm[TEMP_PATH_MAX_BYTES];
  UINTN  Len, I, RelStart = 0;
  UINT8 *O;
  PATH_KIND Kind;
  UINTN  FsIndex = 0;
  UINT16 BootNumber = 0;
  BOOLEAN BootMenu = FALSE;
  CONST CHAR8 *Rel;
  UINTN  RelLen, Pos, Units;

  if (Utf8Path == NULL || Out == NULL) return EFI_INVALID_PARAMETER;
  O = (UINT8 *)Out;
  for (I = 0; I < sizeof (RESOLVED_PATH); I++) O[I] = 0;

  Len = 0;
  while (Utf8Path[Len] != '\0') Len++;
  if (Len == 0) return EFI_INVALID_PARAMETER;
  if (Len >= TEMP_PATH_MAX_BYTES) return EFI_BAD_BUFFER_SIZE;

  /* 反斜杠规范化 */
  for (I = 0; I < Len; I++) {
    Norm[I] = (Utf8Path[I] == '\\') ? '/' : Utf8Path[I];
  }
  Norm[Len] = '\0';

  if (Norm[0]=='d' && Norm[1]=='a' && Norm[2]=='t' && Norm[3]=='a' && Norm[4]==':' && Norm[5]=='/') {
    Kind = PATH_DATA; RelStart = 6;
  } else if (Norm[0]=='a' && Norm[1]=='u' && Norm[2]=='t' && Norm[3]=='o' && Norm[4]==':' && Norm[5]=='/') {
    Kind = PATH_AUTO; RelStart = 6;
  } else if (Norm[0]=='f' && Norm[1]=='s') {
    UINTN J = 2, Digits = 0;
    Kind = PATH_FS;
    while (Norm[J] >= '0' && Norm[J] <= '9') {
      FsIndex = FsIndex * 10 + (UINTN)(Norm[J] - '0');
      if (FsIndex > 0xFFFFFFFFu) return EFI_INVALID_PARAMETER;
      J++; Digits++;
    }
    if (Digits == 0 || Norm[J] != ':' || Norm[J+1] != '/') return EFI_INVALID_PARAMETER;
    RelStart = J + 2;
  } else if (Norm[0]=='b' && Norm[1]=='o' && Norm[2]=='o' && Norm[3]=='t' && Norm[4]==':') {
    Kind = PATH_BOOT;
    Rel = Norm + 5;
    if (Rel[0]=='m' && Rel[1]=='e' && Rel[2]=='n' && Rel[3]=='u' && Rel[4]=='\0') {
      BootMenu = TRUE;
    } else {
      UINTN J, V;
      for (J = 0; J < 4; J++) {
        if (!HexVal (Rel[J], &V)) return EFI_INVALID_PARAMETER;
        BootNumber = (UINT16)((BootNumber << 4) | V);
      }
      if (Rel[4] != '\0') return EFI_INVALID_PARAMETER;
    }
    Out->Kind = Kind;
    Out->BootNumber = BootNumber;
    Out->BootMenu = BootMenu;
    return EFI_SUCCESS;
  } else {
    return EFI_INVALID_PARAMETER;   /* 未知前缀 */
  }

  Rel = Norm + RelStart;
  RelLen = Len - RelStart;
  if (RelLen == 0 || Rel[0] == '/') return EFI_INVALID_PARAMETER;  /* 空路径 */

  /* 段检查：拒绝空段与 ".." 穿越 */
  {
    UINTN SegStart = 0, J;
    for (J = 0; J <= RelLen; J++) {
      if (J == RelLen || Rel[J] == '/') {
        UINTN SegLen = J - SegStart;
        if (SegLen == 0) return EFI_INVALID_PARAMETER;
        if (SegLen == 2 && Rel[SegStart] == '.' && Rel[SegStart+1] == '.') {
          return EFI_INVALID_PARAMETER;
        }
        SegStart = J + 1;
      }
    }
  }

  /* UTF-8 → CHAR16 Rel（容量 256 含 NUL） */
  Pos = 0; Units = 0;
  while (Pos < RelLen) {
    UINT32 Cp;
    Utf8Next (Rel, RelLen, &Pos, &Cp);
    if (Units + 1 >= 256) return EFI_BAD_BUFFER_SIZE;
    if (Cp >= 0x10000) {
      /* 非 BMP → 代理对（UCS-2 契约容不下；临时实现以 U+FFFD 占位，S04 裁决） */
      Out->Rel[Units++] = 0xFFFD;
    } else {
      Out->Rel[Units++] = (CHAR16)Cp;
    }
  }
  Out->Rel[Units] = 0;

  Out->Kind = Kind;
  Out->FsIndex = FsIndex;
  /* .nsh 后缀（大小写敏感，与 S04 规格字面一致） */
  if (RelLen >= 4 &&
      Rel[RelLen-4] == '.' && Rel[RelLen-3] == 'n' &&
      Rel[RelLen-2] == 's' && Rel[RelLen-1] == 'h') {
    Out->IsScript = TRUE;
  }
  return EFI_SUCCESS;
}

BOOLEAN PathArchMatch (UINT8 ItemArchMask, UINT8 CurrentArch) {
  return ((ItemArchMask & CurrentArch) != 0) ? TRUE : FALSE;
}