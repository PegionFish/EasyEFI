/** @file
  Utf8Lib — UTF-8 ↔ token 编解码（S01 实现；契约见 Include/Contract/Utf8.h）。

  核心不变量：对任意字节序列 x，Utf8Encode(Utf8Decode(x)) == x（往返恒等）。
  - 合法序列（RFC 3629：禁 overlong、禁代理区 D800..DFFF、≤ U+10FFFF）→ CP token；
  - 非法序列：首字节降级 RAW token，从下一字节重新扫描（逐字节降级，不丢任何字节信息）；
  - 每个输入字节恰好归属一个 token，CP 重编码必还原原始字节 → 往返恒等由构造保证；
  - 不做任何规范化（NFC 等禁止）；BOM 只是普通 CP（U+FEFF），检测用 Utf8HasBom，
    保留/剥离由上层（S02 模型）处置。

  内存约定：callee 分配（MemAlloc），caller 经 Utf8FreeTokens/MemFree 释放。
  空输入 → *Out=NULL、*Count=0、*Len=0、EFI_SUCCESS；Utf8FreeTokens(NULL) 安全无操作
  （UEFI FreePool(NULL) 会 ASSERT，故包装函数必须判空）。
  不使用 MemRealloc：Decode 两遍扫描（先计数后填充）精确分配，Encode 先算总长再分配。
**/
#include "Contract/Utf8.h"
#include "Contract/EfiMem.h"

#define IS_CONT(B)  (((B) & 0xC0) == 0x80)   /* 续字节 10xxxxxx */

/**
  扫描 Bytes[I] 起始的序列，填充一个 token，返回消耗字节数（1..4）。
  合法 → CP；否则首字节降级 RAW、消耗 1 字节。
  Decode 的计数/填充两遍共用本函数，保证两遍判定完全一致。
**/
STATIC UINTN NextToken (CONST UINT8 *Bytes, UINTN Len, UINTN I, UTF8_TOKEN *Tok) {
  UINT8  B0, B1, B2, B3;
  UINT32 Cp;

  B0       = Bytes[I];
  Tok->Cp  = 0;
  Tok->Raw = 0;

  if (B0 < 0x80) {                              /* ASCII 单字节 */
    Tok->Type = UTF8_TOKEN_CP;
    Tok->Cp   = B0;
    return 1;
  }

  if (B0 >= 0xC2 && B0 <= 0xDF) {               /* 2 字节：C0/C1 必 overlong，直接排除 */
    if (I + 1 < Len && IS_CONT (Bytes[I + 1])) {
      Cp        = ((UINT32)(B0 & 0x1F) << 6) | (UINT32)(Bytes[I + 1] & 0x3F);
      Tok->Type = UTF8_TOKEN_CP;
      Tok->Cp   = Cp;
      return 2;
    }
  } else if (B0 >= 0xE0 && B0 <= 0xEF) {        /* 3 字节 */
    if (I + 2 < Len) {
      B1 = Bytes[I + 1];
      B2 = Bytes[I + 2];
      if (IS_CONT (B1) && IS_CONT (B2) &&
          (B0 != 0xE0 || B1 >= 0xA0) &&         /* 拒 overlong（E0 80..9F） */
          (B0 != 0xED || B1 <= 0x9F)) {         /* 拒代理区 D800..DFFF（ED A0..BF） */
        Cp        = ((UINT32)(B0 & 0x0F) << 12) | ((UINT32)(B1 & 0x3F) << 6) |
                    (UINT32)(B2 & 0x3F);
        Tok->Type = UTF8_TOKEN_CP;
        Tok->Cp   = Cp;
        return 3;
      }
    }
  } else if (B0 >= 0xF0 && B0 <= 0xF4) {        /* 4 字节 */
    if (I + 3 < Len) {
      B1 = Bytes[I + 1];
      B2 = Bytes[I + 2];
      B3 = Bytes[I + 3];
      if (IS_CONT (B1) && IS_CONT (B2) && IS_CONT (B3) &&
          (B0 != 0xF0 || B1 >= 0x90) &&         /* 拒 overlong（F0 80..8F） */
          (B0 != 0xF4 || B1 <= 0x8F)) {         /* 拒 > U+10FFFF（F4 90..BF） */
        Cp        = ((UINT32)(B0 & 0x07) << 18) | ((UINT32)(B1 & 0x3F) << 12) |
                    ((UINT32)(B2 & 0x3F) << 6) | (UINT32)(B3 & 0x3F);
        Tok->Type = UTF8_TOKEN_CP;
        Tok->Cp   = Cp;
        return 4;
      }
    }
  }

  /* 非法首字节（80..BF 孤立续字节 / C0 C1 / F5..FF）或续字节缺失/非法：
     首字节降级 RAW，从下一字节重扫 —— 逐字节降级，往返恒等不受影响。 */
  Tok->Type = UTF8_TOKEN_RAW;
  Tok->Raw  = B0;
  return 1;
}

/** 合法码点的最小编码字节数（1..4）。调用方保证 Cp 合法。 **/
STATIC UINTN CpWidth (UINT32 Cp) {
  if (Cp < 0x80) {
    return 1;
  }
  if (Cp < 0x800) {
    return 2;
  }
  if (Cp < 0x10000) {
    return 3;
  }
  return 4;
}

/** 以最小宽度 UTF-8 写入 Cp，返回写入字节数。最小宽度 → 不可能产生 overlong。 **/
STATIC UINTN WriteCp (UINT8 *Buf, UINT32 Cp) {
  UINTN W = CpWidth (Cp);

  switch (W) {
    case 1:
      Buf[0] = (UINT8)Cp;
      break;
    case 2:
      Buf[0] = (UINT8)(0xC0 | (Cp >> 6));
      Buf[1] = (UINT8)(0x80 | (Cp & 0x3F));
      break;
    case 3:
      Buf[0] = (UINT8)(0xE0 | (Cp >> 12));
      Buf[1] = (UINT8)(0x80 | ((Cp >> 6) & 0x3F));
      Buf[2] = (UINT8)(0x80 | (Cp & 0x3F));
      break;
    default:
      Buf[0] = (UINT8)(0xF0 | (Cp >> 18));
      Buf[1] = (UINT8)(0x80 | ((Cp >> 12) & 0x3F));
      Buf[2] = (UINT8)(0x80 | ((Cp >> 6) & 0x3F));
      Buf[3] = (UINT8)(0x80 | (Cp & 0x3F));
      break;
  }
  return W;
}

/**
  解码任意字节序列为 token 流。非法序列逐字节降级 RAW，不丢字节、不做规范化。
  空输入（Len=0，Bytes 可为 NULL）→ *Out=NULL、*Count=0、EFI_SUCCESS。
  callee 分配，caller 经 Utf8FreeTokens 释放。
**/
EFI_STATUS Utf8Decode (CONST UINT8 *Bytes, UINTN Len, UTF8_TOKEN **Out, UINTN *Count) {
  UINTN      I, N;
  UTF8_TOKEN *Toks;
  UTF8_TOKEN Scratch;

  if (Out == NULL || Count == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Out   = NULL;
  *Count = 0;
  if (Len > 0 && Bytes == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (Len == 0) {
    return EFI_SUCCESS;
  }

  /* 第一遍：计数（与填充共用 NextToken，判定一致）。每字节至多 1 token，N ≤ Len。 */
  N = 0;
  for (I = 0; I < Len; ) {
    I += NextToken (Bytes, Len, I, &Scratch);
    N++;
  }

  Toks = MemAlloc (N * sizeof (UTF8_TOKEN));
  if (Toks == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  /* 第二遍：填充 */
  N = 0;
  for (I = 0; I < Len; ) {
    I += NextToken (Bytes, Len, I, &Toks[N]);
    N++;
  }

  *Out   = Toks;
  *Count = N;
  return EFI_SUCCESS;
}

/**
  token 流重编码为字节序列。CP 按最小宽度编码；RAW 原样写回。
  非法 token（代理区/超 U+10FFFF/未知类型）→ EFI_INVALID_PARAMETER，不分配。
  空输入（Count=0，Toks 可为 NULL）→ *Out=NULL、*Len=0、EFI_SUCCESS。
  callee 分配，caller 经 MemFree 释放。
**/
EFI_STATUS Utf8Encode (CONST UTF8_TOKEN *Toks, UINTN Count, UINT8 **Out, UINTN *Len) {
  UINTN I, Total, Pos;
  UINT8 *Buf;

  if (Out == NULL || Len == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Out = NULL;
  *Len = 0;
  if (Count > 0 && Toks == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (Count == 0) {
    return EFI_SUCCESS;
  }

  /* 第一遍：校验 + 计算总长 */
  Total = 0;
  for (I = 0; I < Count; I++) {
    if (Toks[I].Type == UTF8_TOKEN_RAW) {
      Total += 1;
    } else if (Toks[I].Type == UTF8_TOKEN_CP) {
      UINT32 Cp = Toks[I].Cp;
      if (Cp > 0x10FFFF || (Cp >= 0xD800 && Cp <= 0xDFFF)) {
        return EFI_INVALID_PARAMETER;
      }
      Total += CpWidth (Cp);
    } else {
      return EFI_INVALID_PARAMETER;       /* 未知 token 类型 */
    }
  }

  Buf = MemAlloc (Total);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  /* 第二遍：写入 */
  Pos = 0;
  for (I = 0; I < Count; I++) {
    if (Toks[I].Type == UTF8_TOKEN_RAW) {
      Buf[Pos++] = Toks[I].Raw;
    } else {
      Pos += WriteCp (Buf + Pos, Toks[I].Cp);
    }
  }

  *Out = Buf;
  *Len = Total;
  return EFI_SUCCESS;
}

/** BOM 检测：前 3 字节为 EF BB BF。NULL/短缓冲 → FALSE。 **/
BOOLEAN Utf8HasBom (CONST UINT8 *Bytes, UINTN Len) {
  if (Bytes == NULL || Len < 3) {
    return FALSE;
  }
  return (BOOLEAN)(Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF);
}

/** 显示宽度恒 1（占位对齐）：非 ASCII 单格占位符，RAW 亦单格。 **/
UINTN Utf8TokenWidth (CONST UTF8_TOKEN *Tok) {
  (VOID)Tok;
  return 1;
}

/** 释放 Utf8Decode 产出的 token 数组。NULL 安全无操作（UEFI FreePool(NULL) 会 ASSERT）。 **/
VOID Utf8FreeTokens (UTF8_TOKEN *Toks) {
  if (Toks != NULL) {
    MemFree (Toks);
  }
}
