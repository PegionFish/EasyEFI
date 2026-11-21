/** @file
  UTF-8 token 编解码（契约，S00 冻结；S01 实现）。
  核心不变量：对任意字节序列 x，Encode(Decode(x)) == x（往返恒等）。
  非法序列逐字节降级为 RAW token；不做任何规范化（NFC 等禁止）。
**/
#ifndef UTF8_H
#define UTF8_H

#include "EfiCompat.h"

typedef enum {
  UTF8_TOKEN_CP  = 0,   /* 合法码点 */
  UTF8_TOKEN_RAW = 1    /* 不透明原始字节（非法序列成员） */
} UTF8_TOKEN_TYPE;

typedef struct {
  UINT32          Cp;    /* CP: 码点；RAW: 忽略 */
  UINT8           Raw;   /* RAW: 原字节；CP: 忽略 */
  UTF8_TOKEN_TYPE Type;
} UTF8_TOKEN;

EFI_STATUS Utf8Decode (CONST UINT8 *Bytes, UINTN Len, UTF8_TOKEN **Out, UINTN *Count);
EFI_STATUS Utf8Encode (CONST UTF8_TOKEN *Toks, UINTN Count, UINT8 **Out, UINTN *Len);
BOOLEAN    Utf8HasBom (CONST UINT8 *Bytes, UINTN Len);   /* EF BB BF */
UINTN      Utf8TokenWidth (CONST UTF8_TOKEN *Tok);       /* 显示宽度恒 1（占位对齐） */
VOID       Utf8FreeTokens (UTF8_TOKEN *Toks);

#endif
