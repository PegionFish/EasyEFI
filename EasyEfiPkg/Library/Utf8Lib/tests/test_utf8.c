/* S01 Utf8Lib 宿主测试 T01-1..10（任务书 §3 S01 编号用例）。
 * 运行（模块目录内）: make -f host.mk test / test-leaks
 * 核心不变量：对任意字节序列 x，Utf8Encode(Utf8Decode(x)) == x。
 * 纯字节逻辑测试：不使用 CHAR16 字面量（U16 无需），字符串长度用 strlen(CHAR8*)。
 */
#include "test.h"
#include "Contract/Utf8.h"

#ifdef MEM_TRACKED
extern long MemTrackerLive (void);
#endif

/* ---- 期望 token 构造 ---- */
typedef struct { UTF8_TOKEN_TYPE Type; UINT32 Cp; UINT8 Raw; } EXP_TOK;
#define ECP(c)  { UTF8_TOKEN_CP, (UINT32)(c), 0 }
#define ERAW(b) { UTF8_TOKEN_RAW, 0, (UINT8)(b) }

/* 往返恒等 + token 级不变量（CP 合法、宽度总和 == Len，即不丢字节）。
 * 成功 TRUE；任何失败 FALSE（内部全部释放，无泄漏）。 */
static BOOLEAN check_roundtrip (CONST UINT8 *X, UINTN Len) {
  UTF8_TOKEN *Toks = NULL;
  UINT8      *Y = NULL;
  UINTN      N = 0, YLen = 0, I, Sum = 0;
  BOOLEAN    Ok = TRUE;

  if (EFI_ERROR (Utf8Decode (X, Len, &Toks, &N))) {
    return FALSE;
  }
  for (I = 0; I < N; I++) {
    UINTN W;
    if (Toks[I].Type == UTF8_TOKEN_CP) {
      UINT32 Cp = Toks[I].Cp;
      if (Cp > 0x10FFFF || (Cp >= 0xD800 && Cp <= 0xDFFF)) { Ok = FALSE; break; }
      W = Cp < 0x80 ? 1 : Cp < 0x800 ? 2 : Cp < 0x10000 ? 3 : 4;
    } else if (Toks[I].Type == UTF8_TOKEN_RAW) {
      W = 1;
    } else {
      Ok = FALSE; break;
    }
    Sum += W;
  }
  if (Ok && Sum != Len) {
    Ok = FALSE;
  }
  if (Ok && EFI_ERROR (Utf8Encode (Toks, N, &Y, &YLen))) {
    Ok = FALSE;
  }
  if (Ok && (YLen != Len || (Len > 0 && memcmp (X, Y, Len) != 0))) {
    Ok = FALSE;
  }
  Utf8FreeTokens (Toks);
  MemFree (Y);
  return Ok;
}

/* 断言 Decode 输出与期望 token 序列逐一相等（类型 + Cp/Raw）。 */
static BOOLEAN check_tokens (CONST UINT8 *X, UINTN Len, CONST EXP_TOK *Exp, UINTN ExpN) {
  UTF8_TOKEN *Toks = NULL;
  UINTN      N = 0, I;
  BOOLEAN    Ok = TRUE;

  if (EFI_ERROR (Utf8Decode (X, Len, &Toks, &N))) {
    return FALSE;
  }
  if (N != ExpN) {
    Ok = FALSE;
  }
  for (I = 0; Ok && I < N && I < ExpN; I++) {
    if (Toks[I].Type != Exp[I].Type) {
      Ok = FALSE;
    } else if (Exp[I].Type == UTF8_TOKEN_CP && Toks[I].Cp != Exp[I].Cp) {
      Ok = FALSE;
    } else if (Exp[I].Type == UTF8_TOKEN_RAW && Toks[I].Raw != Exp[I].Raw) {
      Ok = FALSE;
    }
  }
  Utf8FreeTokens (Toks);
  return Ok;
}

/* ---- T01-1 ASCII 往返 ---- */
TEST (t01_1_ascii) {
  UINT8      All[128];
  UINTN      I, N;
  UTF8_TOKEN *Toks;
  CONST UINT8 *Msg = (CONST UINT8 *)"Hello, EasyEFI! # comment\r\n";

  for (I = 0; I < 128; I++) {
    All[I] = (UINT8)I;                    /* 全 0x00..0x7F，含 NUL/CR/LF/TAB */
  }
  ASSERT_TRUE (check_roundtrip (All, sizeof (All)));
  ASSERT_OK (Utf8Decode (All, sizeof (All), &Toks, &N));
  ASSERT_EQ (N, 128);
  for (I = 0; I < N; I++) {
    ASSERT_EQ (Toks[I].Type, UTF8_TOKEN_CP);
    ASSERT_EQ (Toks[I].Cp, I);
  }
  Utf8FreeTokens (Toks);
  ASSERT_TRUE (check_roundtrip (Msg, strlen ((CONST CHAR8 *)Msg)));
}

/* ---- T01-2 中文往返（逐字节相等） ---- */
TEST (t01_2_chinese) {
  /* "# 中文注释\n" 手工字节 */
  CONST UINT8 Cn[] = {
    0x23, 0x20,
    0xE4, 0xB8, 0xAD,               /* 中 U+4E2D */
    0xE6, 0x96, 0x87,               /* 文 U+6587 */
    0xE6, 0xB3, 0xA8,               /* 注 U+6CE8 */
    0xE9, 0x87, 0x8A,               /* 释 U+91CA */
    0x0A
  };
  EXP_TOK ExpCn[] = { ECP (0x23), ECP (0x20), ECP (0x4E2D), ECP (0x6587),
                      ECP (0x6CE8), ECP (0x91CA), ECP (0x0A) };
  /* 3 字节边界：U+07FF / U+0800 / U+FFFF */
  CONST UINT8 B3[] = { 0xDF, 0xBF, 0xE0, 0xA0, 0x80, 0xEF, 0xBF, 0xBF };
  EXP_TOK ExpB3[] = { ECP (0x7FF), ECP (0x800), ECP (0xFFFF) };
  /* 2 字节边界：U+0080 / U+00A9(©) */
  CONST UINT8 B2[] = { 0xC2, 0x80, 0xC2, 0xA9 };
  EXP_TOK ExpB2[] = { ECP (0x80), ECP (0xA9) };

  ASSERT_TRUE (check_tokens (Cn, sizeof (Cn), ExpCn, 7));
  ASSERT_TRUE (check_roundtrip (Cn, sizeof (Cn)));
  ASSERT_TRUE (check_tokens (B3, sizeof (B3), ExpB3, 3));
  ASSERT_TRUE (check_roundtrip (B3, sizeof (B3)));
  ASSERT_TRUE (check_tokens (B2, sizeof (B2), ExpB2, 2));
  ASSERT_TRUE (check_roundtrip (B2, sizeof (B2)));
}

/* ---- T01-3 emoji（4 字节）往返 ---- */
TEST (t01_3_emoji) {
  CONST UINT8 E[] = {
    0xF0, 0x9F, 0x98, 0x80,         /* 😀 U+1F600 */
    0xF0, 0x9F, 0x92, 0xA9,         /* 💩 U+1F4A9 */
    0xF0, 0x90, 0x80, 0x80,         /* 4 字节下界 U+10000 */
    0xF4, 0x8F, 0xBF, 0xBF          /* 上界 U+10FFFF */
  };
  EXP_TOK Exp[] = { ECP (0x1F600), ECP (0x1F4A9), ECP (0x10000), ECP (0x10FFFF) };

  ASSERT_TRUE (check_tokens (E, sizeof (E), Exp, 4));
  ASSERT_TRUE (check_roundtrip (E, sizeof (E)));
}

/* ---- T01-4 非法序列 → RAW 保留且往返恒等 ---- */
TEST (t01_4_invalid) {
  /* 截断 3 字节（尾端/中部） */
  CONST UINT8 T3[]  = { 0xE4, 0xB8 };
  EXP_TOK ExpT3[]   = { ERAW (0xE4), ERAW (0xB8) };
  CONST UINT8 T3M[] = { 0x41, 0xE4, 0xB8, 0x42 };
  EXP_TOK ExpT3M[]  = { ECP (0x41), ERAW (0xE4), ERAW (0xB8), ECP (0x42) };
  /* 截断 2/4 字节 */
  CONST UINT8 T2[]  = { 0xC2 };
  EXP_TOK ExpT2[]   = { ERAW (0xC2) };
  CONST UINT8 T4[]  = { 0xF0, 0x9F, 0x98 };
  EXP_TOK ExpT4[]   = { ERAW (0xF0), ERAW (0x9F), ERAW (0x98) };
  /* overlong：C0 AF / C1 BF / E0 80 AF / F0 80 80 AF */
  CONST UINT8 O2[]  = { 0xC0, 0xAF, 0xC1, 0xBF };
  EXP_TOK ExpO2[]   = { ERAW (0xC0), ERAW (0xAF), ERAW (0xC1), ERAW (0xBF) };
  CONST UINT8 O3[]  = { 0xE0, 0x80, 0xAF };
  EXP_TOK ExpO3[]   = { ERAW (0xE0), ERAW (0x80), ERAW (0xAF) };
  CONST UINT8 O4[]  = { 0xF0, 0x80, 0x80, 0xAF };
  EXP_TOK ExpO4[]   = { ERAW (0xF0), ERAW (0x80), ERAW (0x80), ERAW (0xAF) };
  /* 0xFF/0xFE/F5..F7 非法首字节 */
  CONST UINT8 FF[]  = { 0xFF, 0xFE, 0xF5, 0xF7 };
  EXP_TOK ExpFF[]   = { ERAW (0xFF), ERAW (0xFE), ERAW (0xF5), ERAW (0xF7) };
  /* 孤立续字节 */
  CONST UINT8 LC[]  = { 0x80, 0xBF, 0x9F };
  EXP_TOK ExpLC[]   = { ERAW (0x80), ERAW (0xBF), ERAW (0x9F) };
  /* 代理区 ED A0 80 (D800) / ED BF BF (DFFF) */
  CONST UINT8 SR[]  = { 0xED, 0xA0, 0x80, 0xED, 0xBF, 0xBF };
  EXP_TOK ExpSR[]   = { ERAW (0xED), ERAW (0xA0), ERAW (0x80),
                        ERAW (0xED), ERAW (0xBF), ERAW (0xBF) };
  /* 超 U+10FFFF：F4 90 80 80 */
  CONST UINT8 MX[]  = { 0xF4, 0x90, 0x80, 0x80 };
  EXP_TOK ExpMX[]   = { ERAW (0xF4), ERAW (0x90), ERAW (0x80), ERAW (0x80) };
  /* 续字节位置出现非续字节：E4 2D AD / F0 9F 41 80 */
  CONST UINT8 BC[]  = { 0xE4, 0x2D, 0xAD };
  EXP_TOK ExpBC[]   = { ERAW (0xE4), ECP (0x2D), ERAW (0xAD) };
  CONST UINT8 BC4[] = { 0xF0, 0x9F, 0x41, 0x80 };
  EXP_TOK ExpBC4[]  = { ERAW (0xF0), ERAW (0x9F), ECP (0x41), ERAW (0x80) };

  ASSERT_TRUE (check_tokens (T3, sizeof (T3), ExpT3, 2));
  ASSERT_TRUE (check_roundtrip (T3, sizeof (T3)));
  ASSERT_TRUE (check_tokens (T3M, sizeof (T3M), ExpT3M, 4));
  ASSERT_TRUE (check_roundtrip (T3M, sizeof (T3M)));
  ASSERT_TRUE (check_tokens (T2, sizeof (T2), ExpT2, 1));
  ASSERT_TRUE (check_roundtrip (T2, sizeof (T2)));
  ASSERT_TRUE (check_tokens (T4, sizeof (T4), ExpT4, 3));
  ASSERT_TRUE (check_roundtrip (T4, sizeof (T4)));
  ASSERT_TRUE (check_tokens (O2, sizeof (O2), ExpO2, 4));
  ASSERT_TRUE (check_roundtrip (O2, sizeof (O2)));
  ASSERT_TRUE (check_tokens (O3, sizeof (O3), ExpO3, 3));
  ASSERT_TRUE (check_roundtrip (O3, sizeof (O3)));
  ASSERT_TRUE (check_tokens (O4, sizeof (O4), ExpO4, 4));
  ASSERT_TRUE (check_roundtrip (O4, sizeof (O4)));
  ASSERT_TRUE (check_tokens (FF, sizeof (FF), ExpFF, 4));
  ASSERT_TRUE (check_roundtrip (FF, sizeof (FF)));
  ASSERT_TRUE (check_tokens (LC, sizeof (LC), ExpLC, 3));
  ASSERT_TRUE (check_roundtrip (LC, sizeof (LC)));
  ASSERT_TRUE (check_tokens (SR, sizeof (SR), ExpSR, 6));
  ASSERT_TRUE (check_roundtrip (SR, sizeof (SR)));
  ASSERT_TRUE (check_tokens (MX, sizeof (MX), ExpMX, 4));
  ASSERT_TRUE (check_roundtrip (MX, sizeof (MX)));
  ASSERT_TRUE (check_tokens (BC, sizeof (BC), ExpBC, 3));
  ASSERT_TRUE (check_roundtrip (BC, sizeof (BC)));
  ASSERT_TRUE (check_tokens (BC4, sizeof (BC4), ExpBC4, 4));
  ASSERT_TRUE (check_roundtrip (BC4, sizeof (BC4)));

  /* Encode 拒绝非法 token（代理区/超界/未知类型），错误路径不分配 */
  {
    UTF8_TOKEN Bad;
    UINT8      *Y = (UINT8 *)1;
    UINTN      YLen = 99;
    Bad.Type = UTF8_TOKEN_CP; Bad.Raw = 0;
    Bad.Cp = 0xD800;
    ASSERT_ERR (Utf8Encode (&Bad, 1, &Y, &YLen));
    ASSERT_TRUE (Y == NULL); ASSERT_EQ (YLen, 0);
    Bad.Cp = 0xDFFF;
    ASSERT_ERR (Utf8Encode (&Bad, 1, &Y, &YLen));
    Bad.Cp = 0x110000;
    ASSERT_ERR (Utf8Encode (&Bad, 1, &Y, &YLen));
    Bad.Type = (UTF8_TOKEN_TYPE)7;
    ASSERT_ERR (Utf8Encode (&Bad, 1, &Y, &YLen));
  }
  /* NULL 参数防护 */
  {
    UTF8_TOKEN *Toks;
    UINTN      N;
    UINT8      *Y;
    UINTN      YLen;
    ASSERT_ERR (Utf8Decode (NULL, 4, &Toks, &N));
    ASSERT_ERR (Utf8Decode ((CONST UINT8 *)"a", 1, NULL, &N));
    ASSERT_ERR (Utf8Decode ((CONST UINT8 *)"a", 1, &Toks, NULL));
    ASSERT_ERR (Utf8Encode (NULL, 1, &Y, &YLen));
    ASSERT_ERR (Utf8Encode (NULL, 0, NULL, &YLen));
    ASSERT_ERR (Utf8Encode (NULL, 0, &Y, NULL));
  }
}

/* ---- T01-5 BOM 检测（有/无/仅 BOM） ---- */
TEST (t01_5_bom) {
  CONST UINT8 BomAbc[]  = { 0xEF, 0xBB, 0xBF, 0x61, 0x62, 0x63 };
  CONST UINT8 BomOnly[] = { 0xEF, 0xBB, 0xBF };
  CONST UINT8 NoBom[]   = { 0x61, 0x62, 0x63 };
  CONST UINT8 BomTrunc[] = { 0xEF, 0xBB };
  EXP_TOK Exp[] = { ECP (0xFEFF), ECP (0x61), ECP (0x62), ECP (0x63) };

  ASSERT_TRUE (Utf8HasBom (BomAbc, sizeof (BomAbc)));
  ASSERT_TRUE (Utf8HasBom (BomOnly, sizeof (BomOnly)));
  ASSERT_FALSE (Utf8HasBom (NoBom, sizeof (NoBom)));
  ASSERT_FALSE (Utf8HasBom (BomTrunc, sizeof (BomTrunc)));
  ASSERT_FALSE (Utf8HasBom (NULL, 0));
  ASSERT_FALSE (Utf8HasBom (BomAbc, 2));      /* Len 短于 BOM */
  /* BOM 解码为普通 CP U+FEFF（独立标记，上层处置），往返保留 */
  ASSERT_TRUE (check_tokens (BomAbc, sizeof (BomAbc), Exp, 4));
  ASSERT_TRUE (check_roundtrip (BomAbc, sizeof (BomAbc)));
  ASSERT_TRUE (check_roundtrip (BomOnly, sizeof (BomOnly)));
}

/* ---- T01-6 空输入 ---- */
TEST (t01_6_empty) {
  UTF8_TOKEN *Toks = (UTF8_TOKEN *)1;         /* 毒化初值，验证被写出 */
  UINTN      N = 99;
  UINT8      *Y = (UINT8 *)1;
  UINTN      YLen = 99;

  ASSERT_OK (Utf8Decode (NULL, 0, &Toks, &N));
  ASSERT_TRUE (Toks == NULL);
  ASSERT_EQ (N, 0);
  ASSERT_OK (Utf8Decode ((CONST UINT8 *)"x", 0, &Toks, &N));
  ASSERT_TRUE (Toks == NULL);
  ASSERT_EQ (N, 0);
  ASSERT_OK (Utf8Encode (NULL, 0, &Y, &YLen));
  ASSERT_TRUE (Y == NULL);
  ASSERT_EQ (YLen, 0);
  Utf8FreeTokens (NULL);                      /* 安全无操作 */
  MemFree (NULL);
  ASSERT_TRUE (check_roundtrip (NULL, 0));
}

/* ---- T01-7 混合合法+非法 ---- */
TEST (t01_7_mixed) {
  /* A 中 FF ©(C2 A9) 80 😀 代理区 z */
  CONST UINT8 M[] = {
    0x41,
    0xE4, 0xB8, 0xAD,
    0xFF,
    0xC2, 0xA9,
    0x80,
    0xF0, 0x9F, 0x98, 0x80,
    0xED, 0xA0, 0x80,
    0x7A
  };
  EXP_TOK Exp[] = {
    ECP (0x41), ECP (0x4E2D), ERAW (0xFF), ECP (0xA9), ERAW (0x80),
    ECP (0x1F600), ERAW (0xED), ERAW (0xA0), ERAW (0x80), ECP (0x7A)
  };

  ASSERT_TRUE (check_tokens (M, sizeof (M), Exp, 10));
  ASSERT_TRUE (check_roundtrip (M, sizeof (M)));
}

/* ---- T01-8 性质测试：随机字节 fuzz ≥10000 轮 Encode(Decode(x)) == x ---- */
STATIC UINT32 g_rng = 0x5EED1234u;
STATIC UINT32 rnd (VOID) {                    /* xorshift32，确定性可复现 */
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

TEST (t01_8_fuzz_roundtrip) {
  UINT8       Buf[600];
  UINTN       r;
  CONST UINTN Rounds = 12000;                 /* 任务书要求 ≥10000 */
  /* 模式 2 碎片池：合法/非法/边界片段 */
  static CONST UINT8 F0[]  = { 0x41, 0x42 };
  static CONST UINT8 F1[]  = { 0xE4, 0xB8, 0xAD };
  static CONST UINT8 F2[]  = { 0xF0, 0x9F, 0x98, 0x80 };
  static CONST UINT8 F3[]  = { 0xEF, 0xBB, 0xBF };
  static CONST UINT8 F4[]  = { 0xC0, 0xAF };
  static CONST UINT8 F5[]  = { 0xE0, 0x80, 0xAF };
  static CONST UINT8 F6[]  = { 0xED, 0xA0, 0x80 };
  static CONST UINT8 F7[]  = { 0xF4, 0x90, 0x80, 0x80 };
  static CONST UINT8 F8[]  = { 0xFF };
  static CONST UINT8 F9[]  = { 0x80, 0xBF };
  static CONST UINT8 F10[] = { 0xE4, 0xB8 };
  static CONST UINT8 F11[] = { 0xC2 };
  static CONST UINT8 F12[] = { 0xDF, 0xBF };
  static CONST UINT8 F13[] = { 0xF0, 0x90, 0x80, 0x80 };
  static CONST UINT8 F14[] = { 0xF4, 0x8F, 0xBF, 0xBF };
  static CONST UINT8 F15[] = { 0x0A, 0x0D, 0x09 };
  static CONST UINT8 *Frags[] = { F0, F1, F2, F3, F4, F5, F6, F7,
                                  F8, F9, F10, F11, F12, F13, F14, F15 };
  static CONST UINT8 FragN[] = { 2, 3, 4, 3, 2, 3, 3, 4, 1, 2, 2, 1, 2, 4, 4, 3 };

  for (r = 0; r < Rounds; r++) {
    UINTN Len = 0, I;
    UINTN Mode = r % 3;

    if (Mode == 0) {                          /* 均匀随机字节 */
      Len = rnd () % 300;
      for (I = 0; I < Len; I++) {
        Buf[I] = (UINT8)(rnd () & 0xFF);
      }
    } else if (Mode == 1) {                   /* 随机合法 UTF-8 + 随机破坏 */
      while (Len < 280) {
        UINT32 Cp = rnd () % 0x110000;
        if (Cp >= 0xD800 && Cp <= 0xDFFF) {
          Cp = 0x41;                          /* 避开代理区 */
        }
        /* 独立手工编码（不复用被测库，避免自证） */
        if (Cp < 0x80) {
          Buf[Len++] = (UINT8)Cp;
        } else if (Cp < 0x800) {
          Buf[Len++] = (UINT8)(0xC0 | (Cp >> 6));
          Buf[Len++] = (UINT8)(0x80 | (Cp & 0x3F));
        } else if (Cp < 0x10000) {
          Buf[Len++] = (UINT8)(0xE0 | (Cp >> 12));
          Buf[Len++] = (UINT8)(0x80 | ((Cp >> 6) & 0x3F));
          Buf[Len++] = (UINT8)(0x80 | (Cp & 0x3F));
        } else {
          Buf[Len++] = (UINT8)(0xF0 | (Cp >> 18));
          Buf[Len++] = (UINT8)(0x80 | ((Cp >> 12) & 0x3F));
          Buf[Len++] = (UINT8)(0x80 | ((Cp >> 6) & 0x3F));
          Buf[Len++] = (UINT8)(0x80 | (Cp & 0x3F));
        }
      }
      switch (rnd () % 4) {
        case 0:                               /* 随机截断 */
          Len = rnd () % (Len + 1);
          break;
        case 1:                               /* 翻转一位 */
          Buf[rnd () % Len] ^= (UINT8)(1u << (rnd () % 8));
          break;
        case 2:                               /* 替换一字节 */
          Buf[rnd () % Len] = (UINT8)(rnd () & 0xFF);
          break;
        default:                              /* 保持合法 */
          break;
      }
    } else {                                  /* 合法/非法碎片拼接 + 噪声 */
      while (Len < 280) {
        UINTN K = rnd () % 16;
        if (Len + FragN[K] + 1 > sizeof (Buf)) {
          break;
        }
        memcpy (Buf + Len, Frags[K], FragN[K]);
        Len += FragN[K];
        if (rnd () % 4 == 0) {
          Buf[Len++] = (UINT8)(rnd () & 0xFF);
        }
      }
    }

    if (!check_roundtrip (Buf, Len)) {
      printf ("  fuzz round %llu mode %llu len %llu FAILED\n",
              (unsigned long long)r, (unsigned long long)Mode, (unsigned long long)Len);
      ASSERT_TRUE (FALSE);
      return;
    }
  }
  printf ("  fuzz: %llu rounds, Encode(Decode(x))==x all hold\n",
          (unsigned long long)Rounds);
}

/* ---- T01-9 宽度恒 1 ---- */
TEST (t01_9_width_always_one) {
  UTF8_TOKEN T;
  CONST UINT8 M[] = { 0x41, 0xE4, 0xB8, 0xAD, 0xFF, 0xF0, 0x9F, 0x98, 0x80 };
  UTF8_TOKEN *Toks;
  UINTN      N, I;

  T.Type = UTF8_TOKEN_CP; T.Raw = 0;
  T.Cp = 0x41;    ASSERT_EQ (Utf8TokenWidth (&T), 1);
  T.Cp = 0x4E2D;  ASSERT_EQ (Utf8TokenWidth (&T), 1);
  T.Cp = 0x1F600; ASSERT_EQ (Utf8TokenWidth (&T), 1);
  T.Cp = 0xFEFF;  ASSERT_EQ (Utf8TokenWidth (&T), 1);
  T.Type = UTF8_TOKEN_RAW; T.Cp = 0; T.Raw = 0xFF;
  ASSERT_EQ (Utf8TokenWidth (&T), 1);

  ASSERT_OK (Utf8Decode (M, sizeof (M), &Toks, &N));
  for (I = 0; I < N; I++) {
    ASSERT_EQ (Utf8TokenWidth (&Toks[I]), 1);
  }
  Utf8FreeTokens (Toks);
}

/* ---- T01-10 内存释放配对（ASan/UBSan 干净；test-leaks 目标另断言零存活分配） ---- */
TEST (t01_10_mem_pairing) {
  UINT8 Buf[256];
  UINTN r;

  for (r = 0; r < 2000; r++) {
    UINTN      I, Len = rnd () % 256;
    UTF8_TOKEN *Toks;
    UINTN      N;
    UINT8      *Y;
    UINTN      YLen;
    for (I = 0; I < Len; I++) {
      Buf[I] = (UINT8)(rnd () & 0xFF);
    }
    ASSERT_OK (Utf8Decode (Buf, Len, &Toks, &N));
    if (g_cur_failed) { return; }
    ASSERT_OK (Utf8Encode (Toks, N, &Y, &YLen));
    if (g_cur_failed) { Utf8FreeTokens (Toks); return; }
    Utf8FreeTokens (Toks);
    MemFree (Y);
  }
  Utf8FreeTokens (NULL);
  MemFree (NULL);
#ifdef MEM_TRACKED
  ASSERT_EQ (MemTrackerLive (), 0);           /* 全部配对，零残留 */
#endif
}

/* ---- 附加：Encode 独立黄金对比（不经 Decode，手工 token 流） ---- */
TEST (extra_encode_manual) {
  UTF8_TOKEN Toks[4];
  UINT8      *Y = NULL;
  UINTN      YLen = 0;
  CONST UINT8 Exp[] = { 0xE4, 0xB8, 0xAD, 0xFF, 0xF0, 0x9F, 0x98, 0x80, 0x0A };

  Toks[0].Type = UTF8_TOKEN_CP;  Toks[0].Cp = 0x4E2D;  Toks[0].Raw = 0;
  Toks[1].Type = UTF8_TOKEN_RAW; Toks[1].Cp = 0;       Toks[1].Raw = 0xFF;
  Toks[2].Type = UTF8_TOKEN_CP;  Toks[2].Cp = 0x1F600; Toks[2].Raw = 0;
  Toks[3].Type = UTF8_TOKEN_CP;  Toks[3].Cp = 0x0A;    Toks[3].Raw = 0;
  ASSERT_OK (Utf8Encode (Toks, 4, &Y, &YLen));
  ASSERT_EQ (YLen, sizeof (Exp));
  ASSERT_MEM_EQ (Y, Exp, sizeof (Exp));
  MemFree (Y);
}

/* ---- 附加：大输入（128KiB，编辑器上限的一半量级）往返 ---- */
TEST (extra_large_input) {
  static UINT8 Big[128 * 1024];
  /* 16 字节模式块：ASCII + 中文 + RAW + emoji + 换行 */
  CONST UINT8 Pat[16] = {
    0x61, 0x62, 0xE4, 0xB8, 0xAD, 0xFF, 0xF0, 0x9F,
    0x98, 0x80, 0x0A, 0x20, 0xC2, 0xA9, 0x80, 0x7A
  };
  UINTN I;

  for (I = 0; I < sizeof (Big); I++) {
    Big[I] = Pat[I % 16];
  }
  ASSERT_TRUE (check_roundtrip (Big, sizeof (Big)));
  /* 全 ASCII 最坏 token 数（N == Len） */
  for (I = 0; I < sizeof (Big); I++) {
    Big[I] = (UINT8)('A' + (I % 26));
  }
  ASSERT_TRUE (check_roundtrip (Big, sizeof (Big)));
}

int main (void) {
  RUN_TEST (t01_1_ascii);
  RUN_TEST (t01_2_chinese);
  RUN_TEST (t01_3_emoji);
  RUN_TEST (t01_4_invalid);
  RUN_TEST (t01_5_bom);
  RUN_TEST (t01_6_empty);
  RUN_TEST (t01_7_mixed);
  RUN_TEST (t01_8_fuzz_roundtrip);
  RUN_TEST (t01_9_width_always_one);
  RUN_TEST (t01_10_mem_pairing);
  RUN_TEST (extra_encode_manual);
  RUN_TEST (extra_large_input);
  RUN_TESTS_END ();
}
