/** @file
  EasyEFI 宿主/UEFI 双环境兼容类型（契约，S00 冻结）。
  纯逻辑库只允许 include 本头与 EfiMem.h，禁止 MdePkg/协议头。
  UEFI 构建定义 UEFI_BUILD 宏（.inf BuildOptions），宿主构建不定义。
**/
#ifndef EFI_COMPAT_H
#define EFI_COMPAT_H

#ifdef UEFI_BUILD

#include <Uefi.h>
#include <Library/BaseLib.h>

#define EfiStrLen  StrLen
#define EfiStrCmp  StrCmp
#define EfiStrnCmp StrnCmp

#else /* host build */

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef int8_t    INT8;
typedef int16_t   INT16;
typedef int32_t   INT32;
typedef int64_t   INT64;
typedef uintptr_t UINTN;
typedef intptr_t  INTN;
typedef char      CHAR8;
typedef uint16_t  CHAR16;   /* UCS-2，宿主不用 wchar_t（Linux 上 4 字节） */
typedef UINT8     BOOLEAN;

#ifndef VOID
#define VOID void
#endif
#ifndef TRUE
#define TRUE  ((BOOLEAN)1)
#endif
#ifndef FALSE
#define FALSE ((BOOLEAN)0)
#endif
#ifndef NULL
#define NULL ((VOID *)0)
#endif
#ifndef CONST
#define CONST const
#endif
#ifndef STATIC
#define STATIC static
#endif
#ifndef IN
#define IN
#endif
#ifndef OUT
#define OUT
#endif
#ifndef OPTIONAL
#define OPTIONAL
#endif
#ifndef EFIAPI
#define EFIAPI
#endif
#ifndef MAX_BIT
#define MAX_BIT 0x8000000000000000ULL
#endif
#ifndef MAX_2_BITS
#define MAX_2_BITS 0xC000000000000000ULL
#endif

typedef UINTN EFI_STATUS;
#define EFI_ERROR(A) (((INTN)(A)) < 0)
#define EFI_SUCCESS              0ULL
#define EFI_LOAD_ERROR           (1ULL  | MAX_BIT)
#define EFI_INVALID_PARAMETER    (2ULL  | MAX_BIT)
#define EFI_UNSUPPORTED          (3ULL  | MAX_BIT)
#define EFI_BAD_BUFFER_SIZE      (4ULL  | MAX_BIT)
#define EFI_BUFFER_TOO_SMALL     (5ULL  | MAX_BIT)
#define EFI_NOT_READY            (6ULL  | MAX_BIT)
#define EFI_DEVICE_ERROR         (7ULL  | MAX_BIT)
#define EFI_WRITE_PROTECTED      (8ULL  | MAX_BIT)
#define EFI_OUT_OF_RESOURCES     (9ULL  | MAX_BIT)
#define EFI_NOT_FOUND            (14ULL | MAX_BIT)
#define EFI_ACCESS_DENIED        (15ULL | MAX_BIT)
#define EFI_ABORTED              (21ULL | MAX_BIT)
#define EFI_SECURITY_VIOLATION   (26ULL | MAX_BIT)

#endif /* UEFI_BUILD */

/* CHAR16 字符串字面量：双环境代码一律用 U16("...")，禁止裸 L""（宿主 wchar_t 32 位）。
   UEFI 侧展开为 L 字面量；宿主侧运行时转换（旋转静态缓冲，单线程测试安全）。 */
#ifdef UEFI_BUILD
#define U16(s) L##s
#else
CONST CHAR16 *EfiAsciiToU16 (CONST CHAR8 *S);
#define U16(s) EfiAsciiToU16 (s)
#endif

/* CHAR16 字符串助手：宿主 wchar_t 为 4 字节，禁止对 CHAR16 用 wcs* 函数。
   UEFI 侧 EfiStrChr 与宿主同实现（BaseLib 无 StrChr）。 */
static inline CHAR16 *EfiStrChr (CONST CHAR16 *S, CHAR16 C) {
  UINTN i;
  for (i = 0; S[i]; i++) if (S[i] == C) return (CHAR16 *)&S[i];
  return (CHAR16 *)0;
}
#ifndef UEFI_BUILD
static inline UINTN EfiStrLen (CONST CHAR16 *S) { UINTN n = 0; while (S[n]) n++; return n; }
static inline INTN EfiStrCmp (CONST CHAR16 *A, CONST CHAR16 *B) {
  while (*A && *A == *B) { A++; B++; }
  return (INTN)*A - (INTN)*B;
}
static inline INTN EfiStrnCmp (CONST CHAR16 *A, CONST CHAR16 *B, UINTN N) {
  UINTN i;
  for (i = 0; i < N; i++) { if (A[i] != B[i]) return (INTN)A[i] - (INTN)B[i]; if (!A[i]) break; }
  return 0;
}
#endif
#endif /* EFI_COMPAT_H */
