/* EasyEFI 宿主测试框架（S00，leader 独占）。
 * 用法：模块 tests/host/test_<mod>.c 包含本头，写 TEST(name){...}，
 * 末尾 RUN_TESTS() 展开 main。断言失败打印文件:行并计失败。
 */
#ifndef EASYEFI_TEST_H
#define EASYEFI_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"

static int g_tests_run = 0, g_tests_failed = 0, g_cur_failed = 0;
static const char *g_cur_name = "";

#define TEST(name) static void test_##name (void)

#define ASSERT_TRUE(x) do { if (!(x)) { \
  printf ("  FAIL %s:%d: ASSERT_TRUE(%s)\n", __FILE__, __LINE__, #x); g_cur_failed = 1; } } while (0)
#define ASSERT_FALSE(x) ASSERT_TRUE (!(x))
#define ASSERT_EQ(a, b) do { UINT64 _a = (UINT64)(a), _b = (UINT64)(b); if (_a != _b) { \
  printf ("  FAIL %s:%d: ASSERT_EQ(%s,%s) %llu != %llu\n", __FILE__, __LINE__, #a, #b, \
          (unsigned long long)_a, (unsigned long long)_b); g_cur_failed = 1; } } while (0)
#define ASSERT_OK(x) do { EFI_STATUS _s = (x); if (EFI_ERROR (_s)) { \
  printf ("  FAIL %s:%d: ASSERT_OK(%s) = 0x%llx\n", __FILE__, __LINE__, #x, \
          (unsigned long long)_s); g_cur_failed = 1; } } while (0)
#define ASSERT_ERR(x) do { EFI_STATUS _s = (x); if (!EFI_ERROR (_s)) { \
  printf ("  FAIL %s:%d: ASSERT_ERR(%s) = 0x%llx (expected error)\n", __FILE__, __LINE__, #x, \
          (unsigned long long)_s); g_cur_failed = 1; } } while (0)
#define ASSERT_MEM_EQ(a, b, n) do { if (memcmp ((a), (b), (n)) != 0) { \
  printf ("  FAIL %s:%d: ASSERT_MEM_EQ(%s,%s,%s)\n", __FILE__, __LINE__, #a, #b, #n); g_cur_failed = 1; } } while (0)

#define RUN_TEST(name) do { g_cur_name = #name; g_cur_failed = 0; g_tests_run++; \
  test_##name (); if (g_cur_failed) { g_tests_failed++; printf ("[FAIL] %s\n", #name); } \
  else printf ("[ ok ] %s\n", #name); } while (0)

#define RUN_TESTS_END() do { printf ("%d tests, %d failed\n", g_tests_run, g_tests_failed); \
  return g_tests_failed ? 1 : 0; } while (0)

#endif
