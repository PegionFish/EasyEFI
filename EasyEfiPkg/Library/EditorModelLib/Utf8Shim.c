/** @file
  Utf8Shim — S02 临时依赖垫片（S01 Utf8Lib 尚未合并进 main）。

  背景：S00 的 Utf8LibStub.c 只有 Probe 占位函数，未实现 Utf8.h 契约函数，
  模型无法链接。本文件在 S02 独占目录内提供保守实现，满足契约核心不变量
  Encode(Decode(x)) == x（任意字节序列往返恒等）：
    - 解码：ASCII (<0x80) → CP token；任何 >=0x80 字节 → RAW token（逐字节不透明）。
    - 编码：RAW 原样输出；CP → 标准 UTF-8（非法码点 → U+FFFD，与解码保守策略配对）。
    - BOM 检测 / 宽度恒 1 按契约实现。

  与真实 S01 的差异：非 ASCII 永不解出多字节 CP token → 中文按字节占位渲染、
  CP needle 不命中非 ASCII 内容。字节往返/行尾/BOM/撤销等语义不受影响。

  合并 S01 后（leader 通知 → git merge main）：
    1. EditorModelLib.inf [Sources] 删除 Utf8Shim.c（保留 Utf8Lib 依赖）；
    2. host.mk 默认 REAL_UTF8=1（MODSRC 换 ../Utf8Lib/Utf8.c，-DS02_REAL_UTF8）；
    3. 删除本文件。详见 docs/handoffs/S02.md。
**/
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/Utf8.h"

/* 非法码点（代理区/超 U+10FFFF）归一为 U+FFFD，宽度 3 */
STATIC UINT32 NormCp (UINT32 Cp) {
  if ((Cp >= 0xD800 && Cp <= 0xDFFF) || Cp > 0x10FFFF) return 0xFFFD;
  return Cp;
}

STATIC UINTN CpWidth (UINT32 Cp) {
  Cp = NormCp (Cp);
  if (Cp < 0x80)    return 1;
  if (Cp < 0x800)   return 2;
  if (Cp < 0x10000) return 3;
  return 4;
}

STATIC UINTN EncodeCp (UINT32 Cp, UINT8 *Out) {
  Cp = NormCp (Cp);
  if (Cp < 0x80) {
    Out[0] = (UINT8)Cp;
    return 1;
  }
  if (Cp < 0x800) {
    Out[0] = (UINT8)(0xC0 | (Cp >> 6));
    Out[1] = (UINT8)(0x80 | (Cp & 0x3F));
    return 2;
  }
  if (Cp < 0x10000) {
    Out[0] = (UINT8)(0xE0 | (Cp >> 12));
    Out[1] = (UINT8)(0x80 | ((Cp >> 6) & 0x3F));
    Out[2] = (UINT8)(0x80 | (Cp & 0x3F));
    return 3;
  }
  Out[0] = (UINT8)(0xF0 | (Cp >> 18));
  Out[1] = (UINT8)(0x80 | ((Cp >> 12) & 0x3F));
  Out[2] = (UINT8)(0x80 | ((Cp >> 6) & 0x3F));
  Out[3] = (UINT8)(0x80 | (Cp & 0x3F));
  return 4;
}

EFI_STATUS Utf8Decode (CONST UINT8 *Bytes, UINTN Len, UTF8_TOKEN **Out, UINTN *Count) {
  UTF8_TOKEN *T;
  UINTN i;
  if (!Out || !Count || (!Bytes && Len)) return EFI_INVALID_PARAMETER;
  *Out = NULL;
  *Count = 0;
  if (Len == 0) return EFI_SUCCESS;
  T = MemAlloc (Len * sizeof (UTF8_TOKEN));
  if (!T) return EFI_OUT_OF_RESOURCES;
  for (i = 0; i < Len; i++) {
    if (Bytes[i] < 0x80) {
      T[i].Type = UTF8_TOKEN_CP;
      T[i].Cp = Bytes[i];
      T[i].Raw = 0;
    } else {
      T[i].Type = UTF8_TOKEN_RAW;
      T[i].Raw = Bytes[i];
      T[i].Cp = 0;
    }
  }
  *Out = T;
  *Count = Len;
  return EFI_SUCCESS;
}

EFI_STATUS Utf8Encode (CONST UTF8_TOKEN *Toks, UINTN Count, UINT8 **Out, UINTN *Len) {
  UINTN i, Total = 0, Pos = 0;
  UINT8 *Buf;
  if (!Out || !Len || (!Toks && Count)) return EFI_INVALID_PARAMETER;
  *Out = NULL;
  *Len = 0;
  for (i = 0; i < Count; i++) {
    Total += (Toks[i].Type == UTF8_TOKEN_RAW) ? 1 : CpWidth (Toks[i].Cp);
  }
  if (Total == 0) return EFI_SUCCESS;
  Buf = MemAlloc (Total);
  if (!Buf) return EFI_OUT_OF_RESOURCES;
  for (i = 0; i < Count; i++) {
    if (Toks[i].Type == UTF8_TOKEN_RAW) {
      Buf[Pos++] = Toks[i].Raw;
    } else {
      Pos += EncodeCp (Toks[i].Cp, Buf + Pos);
    }
  }
  *Out = Buf;
  *Len = Total;
  return EFI_SUCCESS;
}

BOOLEAN Utf8HasBom (CONST UINT8 *Bytes, UINTN Len) {
  return (BOOLEAN)(Bytes && Len >= 3 &&
                   Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF);
}

UINTN Utf8TokenWidth (CONST UTF8_TOKEN *Tok) {
  (VOID)Tok;
  return 1;   /* 契约：显示宽度恒 1（占位对齐） */
}

VOID Utf8FreeTokens (UTF8_TOKEN *Toks) {
  MemFree (Toks);
}
