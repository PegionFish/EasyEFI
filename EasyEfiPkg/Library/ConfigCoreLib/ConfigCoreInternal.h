/** @file
  ConfigCoreLib 内部头（非契约；S03 私有）。
  - UEFI 构建：化解 JsonLib.h 与 JsonDom.h 的 JsonArrayCount 同名冲突（见下）。
  - 双环境 CHAR8 助手 / 常量 / 内部附加 API（DOM 规范 dumps，供 T03-15 同构对比）。
**/
#ifndef CONFIG_CORE_INTERNAL_H
#define CONFIG_CORE_INTERNAL_H

#ifdef UEFI_BUILD
/* 契约冲突 workaround（S03 交接注记有提案，leader 裁决前冻结头不改）：
   JsonLib.h 声明 UINTN JsonArrayCount(EDKII_JSON_ARRAY)，即 VOID 指针参数；
   JsonDom.h 声明 UINTN JsonArrayCount(CONST JSON_NODE*) —— 同 TU 内冲突。
   先把 JsonLib 的声明改名；JsonLib.h 是 pragma once，JsonDom.h 内的二次
   include 成为 no-op，DOM 助手声明得以保留。本库绝不调用 JsonLib 的
   JsonArrayCount（EDK2 适配器以 JsonArrayGetValue 迭代到 NULL 计数）。
   JsonLib.h 不自带基础类型，须先 include Uefi.h。 */
#include <Uefi.h>
#define JsonArrayCount Edk2JsonArrayCount
#include <Library/JsonLib.h>
#undef JsonArrayCount
#endif

#include "Contract/ConfigSchema.h"
#include "Contract/EfiMem.h"

/* skip_timeout 缺省值：UEFI 侧走 PCD（可配置，EasyEfiPkg.dec）；
   宿主侧镜像 PCD 默认值（纯逻辑核心不链接 PcdLib 宿主实现）。 */
#ifdef UEFI_BUILD
#include <Library/PcdLib.h>
#define CFG_SKIP_TIMEOUT_DEFAULT  PcdGet32 (PcdSkipTimeoutDefault)
#else
#define CFG_SKIP_TIMEOUT_DEFAULT  2u
#endif

#define CFG_SKIP_TIMEOUT_MAX  30u

/* DOM 转换/规范化 dumps 的最大递归深度（配置实际深度 ≤4；防御恶意深嵌套）。 */
#define CFG_MAX_DOM_DEPTH  64u

/* 警告码约定：契约枚举无警告专用码。未知字段警告 Code=0（"无错误"）；
   skip_timeout 越界钳制警告复用 CFG_ERR_RANGE。见交接注记。 */
#define CFG_WARN_UNKNOWN  0u

/* 顶层错误 ItemIndex 约定值（契约注释：(UINTN)-1）。 */
#define CFG_TOP_INDEX  ((UINTN)-1)

/* ---- CHAR8 助手（纯逻辑：不依赖 libc / UEFI 库） ---- */

static inline UINTN CfgStrLen (CONST CHAR8 *S) {
  UINTN N = 0;
  if (S == NULL) return 0;
  while (S[N] != '\0') N++;
  return N;
}

static inline BOOLEAN CfgStrEq (CONST CHAR8 *A, CONST CHAR8 *B) {
  UINTN I = 0;
  if (A == NULL || B == NULL) return (A == B) ? TRUE : FALSE;
  while (A[I] != '\0' && A[I] == B[I]) I++;
  return (A[I] == B[I]) ? TRUE : FALSE;
}

/* 有界拷贝：Dst 容量 Cap（含 NUL 位），恒 NUL 终止。 */
static inline VOID CfgStrCopy (CHAR8 *Dst, UINTN Cap, CONST CHAR8 *Src) {
  UINTN I = 0;
  if (Cap == 0) return;
  if (Src != NULL) {
    while (I + 1 < Cap && Src[I] != '\0') { Dst[I] = Src[I]; I++; }
  }
  Dst[I] = '\0';
}

/* 可打印 ASCII（0x20..0x7E）；name/desc/title/id/version 的 ASCII 纪律。
   控制字符与 ≥0x80（含 UTF-8 多字节）一律拒绝（ConOut 无 CJK 字形 [F04]）。 */
static inline BOOLEAN CfgIsAscii (CONST CHAR8 *S) {
  UINTN I;
  for (I = 0; S[I] != '\0'; I++) {
    if ((UINT8)S[I] < 0x20 || (UINT8)S[I] > 0x7E) return FALSE;
  }
  return TRUE;
}

/* INT64 → 十进制 ASCII（含 INT64_MIN；Buf 容量 ≥22）。 */
VOID CfgI64ToA (INT64 V, CHAR8 *Buf, UINTN Cap);

/* ---- 内部附加 API（非契约；宿主测试与 QEMU 冒烟 app 共用） ---- */

/** 规范单行 dumps（T03-15 双适配器同构对比的黄金表示）。
    格式：null / true / false / <int> / str(<escaped>) / arr[<e>,...] / obj{<k>:<v>,...}
    转义：'\\'→"\\\\"、')'→"\\)"、','→"\\,"、换行→"\\n"、回车→"\\r"、制表→"\\t"。
    callee 分配，caller MemFree。深度超 CFG_MAX_DOM_DEPTH → EFI_BAD_BUFFER_SIZE。 **/
EFI_STATUS JsonDomDump (CONST JSON_NODE *N, CHAR8 **Out);

#endif /* CONFIG_CORE_INTERNAL_H */