/* S04 ExecPathLib 宿主测试 — 任务书 T04-1..12（docs/parallel-tasks.md §3 S04）。
 * 运行：make -f host.mk test（模块目录内；ASan/UBSan）。
 * 纯逻辑：无 mock 依赖，直接测 PathParse/PathArchMatch。
 */
#include "test.h"
#include "Contract/ExecPath.h"

/* Rel 断言助手：与期望 CHAR16 串逐字符相等。 */
static void ExpectRel (RESOLVED_PATH *Rp, CONST CHAR16 *Want) {
  ASSERT_TRUE (EfiStrCmp (Rp->Rel, Want) == 0);
}

/* T04-1 四前缀正例（含黄金样例 config/launcher.example.json 的路径形态）。 */
TEST (t04_1_prefixes) {
  RESOLVED_PATH Rp;

  ASSERT_OK (PathParse ("data:/entries/winpe.efi", &Rp));
  ASSERT_EQ (Rp.Kind, PATH_DATA);
  ExpectRel (&Rp, U16 ("entries/winpe.efi"));
  ASSERT_EQ (Rp.FsIndex, 0);
  ASSERT_EQ (Rp.BootNumber, 0);
  ASSERT_FALSE (Rp.BootMenu);
  ASSERT_FALSE (Rp.IsScript);

  ASSERT_OK (PathParse ("auto:/EFI/BOOT/BOOTX64.EFI", &Rp));
  ASSERT_EQ (Rp.Kind, PATH_AUTO);
  ExpectRel (&Rp, U16 ("EFI/BOOT/BOOTX64.EFI"));
  ASSERT_FALSE (Rp.IsScript);

  ASSERT_OK (PathParse ("auto:/EFI/BOOT/BOOTAA64.EFI", &Rp));
  ASSERT_EQ (Rp.Kind, PATH_AUTO);
  ExpectRel (&Rp, U16 ("EFI/BOOT/BOOTAA64.EFI"));

  ASSERT_OK (PathParse ("fs0:/scripts/fpt.nsh", &Rp));
  ASSERT_EQ (Rp.Kind, PATH_FS);
  ASSERT_EQ (Rp.FsIndex, 0);
  ExpectRel (&Rp, U16 ("scripts/fpt.nsh"));
  ASSERT_TRUE (Rp.IsScript);

  ASSERT_OK (PathParse ("boot:0001", &Rp));
  ASSERT_EQ (Rp.Kind, PATH_BOOT);
  ASSERT_EQ (Rp.BootNumber, 1);
  ASSERT_FALSE (Rp.BootMenu);
  ASSERT_EQ (Rp.Rel[0], 0);
  ASSERT_FALSE (Rp.IsScript);

  ASSERT_OK (PathParse ("boot:menu", &Rp));
  ASSERT_EQ (Rp.Kind, PATH_BOOT);
  ASSERT_TRUE (Rp.BootMenu);
  ASSERT_EQ (Rp.BootNumber, 0);
  ASSERT_EQ (Rp.Rel[0], 0);
  ASSERT_FALSE (Rp.IsScript);
}

/* T04-2 斜杠规范化：反斜杠→正斜杠 [F15]，混合输入，Rel 无前导 /。 */
TEST (t04_2_slash_normalize) {
  RESOLVED_PATH Rp;

  ASSERT_OK (PathParse ("data:\\entries\\winpe.efi", &Rp));
  ExpectRel (&Rp, U16 ("entries/winpe.efi"));

  ASSERT_OK (PathParse ("auto:/EFI\\BOOT/BOOTX64.EFI", &Rp));
  ExpectRel (&Rp, U16 ("EFI/BOOT/BOOTX64.EFI"));

  ASSERT_OK (PathParse ("fs2:\\x\\y.nsh", &Rp));
  ASSERT_EQ (Rp.FsIndex, 2);
  ExpectRel (&Rp, U16 ("x/y.nsh"));
  ASSERT_TRUE (Rp.IsScript);

  ASSERT_OK (PathParse ("data:\\a/b\\c/d.efi", &Rp));
  ExpectRel (&Rp, U16 ("a/b/c/d.efi"));
  ASSERT_TRUE (Rp.Rel[0] != '/');
}

/* T04-3 fs 索引 / boot 十六进制解析（0001/FFFF/menu；大小写不敏感值）。 */
TEST (t04_3_index_hex) {
  RESOLVED_PATH Rp;

  ASSERT_OK (PathParse ("fs0:/a", &Rp));   ASSERT_EQ (Rp.FsIndex, 0);
  ASSERT_OK (PathParse ("fs7:/a", &Rp));   ASSERT_EQ (Rp.FsIndex, 7);
  ASSERT_OK (PathParse ("fs12:/a", &Rp));  ASSERT_EQ (Rp.FsIndex, 12);
  ASSERT_OK (PathParse ("fs123:/a", &Rp)); ASSERT_EQ (Rp.FsIndex, 123);
  ASSERT_OK (PathParse ("fs0007:/a", &Rp)); ASSERT_EQ (Rp.FsIndex, 7);  /* 前导零按十进制 */

  ASSERT_OK (PathParse ("boot:0000", &Rp)); ASSERT_EQ (Rp.BootNumber, 0x0000);
  ASSERT_OK (PathParse ("boot:0001", &Rp)); ASSERT_EQ (Rp.BootNumber, 0x0001);
  ASSERT_OK (PathParse ("boot:FFFF", &Rp)); ASSERT_EQ (Rp.BootNumber, 0xFFFF);
  ASSERT_OK (PathParse ("boot:ffff", &Rp)); ASSERT_EQ (Rp.BootNumber, 0xFFFF);
  ASSERT_OK (PathParse ("boot:ABCD", &Rp)); ASSERT_EQ (Rp.BootNumber, 0xABCD);
  ASSERT_OK (PathParse ("boot:aBcD", &Rp)); ASSERT_EQ (Rp.BootNumber, 0xABCD);
  ASSERT_FALSE (Rp.BootMenu);

  ASSERT_OK (PathParse ("boot:menu", &Rp)); ASSERT_TRUE (Rp.BootMenu);
  ASSERT_OK (PathParse ("boot:MENU", &Rp)); ASSERT_TRUE (Rp.BootMenu);  /* 值大小写不敏感 */
  ASSERT_OK (PathParse ("boot:Menu", &Rp)); ASSERT_TRUE (Rp.BootMenu);
}

/* T04-4 拒绝：空路径（含各前缀下的空 rel）与 NULL 参数。 */
TEST (t04_4_empty) {
  RESOLVED_PATH Rp;
  ASSERT_ERR (PathParse ("", &Rp));
  ASSERT_ERR (PathParse ("data:", &Rp));
  ASSERT_ERR (PathParse ("data:/", &Rp));
  ASSERT_ERR (PathParse ("data:\\", &Rp));
  ASSERT_ERR (PathParse ("auto:", &Rp));
  ASSERT_ERR (PathParse ("auto:/", &Rp));
  ASSERT_ERR (PathParse ("fs0:", &Rp));
  ASSERT_ERR (PathParse ("fs0:/", &Rp));
  ASSERT_ERR (PathParse ("fs3:\\", &Rp));
  ASSERT_ERR (PathParse ("boot:", &Rp));
  ASSERT_ERR (PathParse (NULL, &Rp));
  ASSERT_ERR (PathParse ("data:/x", NULL));
  ASSERT_ERR (PathParse (NULL, NULL));
}

/* T04-5 拒绝：未知前缀（含大小写敏感关键字、无前缀、缺分隔符）。 */
TEST (t04_5_unknown_prefix) {
  RESOLVED_PATH Rp;
  ASSERT_ERR (PathParse ("foo:/bar", &Rp));
  ASSERT_ERR (PathParse ("http://x", &Rp));
  ASSERT_ERR (PathParse ("usb:/x", &Rp));
  ASSERT_ERR (PathParse ("/abs/path", &Rp));
  ASSERT_ERR (PathParse ("rel/path", &Rp));
  ASSERT_ERR (PathParse ("datax:/x", &Rp));
  ASSERT_ERR (PathParse ("dat:/x", &Rp));
  ASSERT_ERR (PathParse (":/x", &Rp));
  ASSERT_ERR (PathParse ("DATA:/x", &Rp));     /* 前缀关键字小写敏感 */
  ASSERT_ERR (PathParse ("Auto:/x", &Rp));
  ASSERT_ERR (PathParse ("BOOT:0001", &Rp));
  ASSERT_ERR (PathParse ("FS0:/x", &Rp));
  ASSERT_ERR (PathParse ("data::/x", &Rp));
  ASSERT_ERR (PathParse ("data:rel.efi", &Rp)); /* 严格文法：前缀后必须 '/' */
  ASSERT_ERR (PathParse ("auto:rel.efi", &Rp));
  ASSERT_ERR (PathParse ("fs0:rel.efi", &Rp));
}

/* T04-6 拒绝：'..' 穿越、'.' 分量、空分量（前导/尾随/连续斜杠）；
 * 含点合法文件名不受影响。 */
TEST (t04_6_traversal) {
  RESOLVED_PATH Rp;
  ASSERT_ERR (PathParse ("data:/../x", &Rp));
  ASSERT_ERR (PathParse ("data:/a/../b", &Rp));
  ASSERT_ERR (PathParse ("data:/a/..", &Rp));
  ASSERT_ERR (PathParse ("data:/..", &Rp));
  ASSERT_ERR (PathParse ("auto:/..", &Rp));
  ASSERT_ERR (PathParse ("auto:/a/../b", &Rp));
  ASSERT_ERR (PathParse ("fs0:/a/../../b", &Rp));
  ASSERT_ERR (PathParse ("data:/a\\..\\b", &Rp));  /* 规范化后穿越同样拒绝 */
  ASSERT_ERR (PathParse ("data:\\..\\x", &Rp));
  ASSERT_ERR (PathParse ("data:/./a", &Rp));
  ASSERT_ERR (PathParse ("data:/a/.", &Rp));
  ASSERT_ERR (PathParse ("data:/a//b", &Rp));
  ASSERT_ERR (PathParse ("data://a", &Rp));
  ASSERT_ERR (PathParse ("data:/a/", &Rp));
  ASSERT_ERR (PathParse ("data:/a\\", &Rp));

  /* 含点文件名合法 */
  ASSERT_OK (PathParse ("data:/foo..bar", &Rp));
  ExpectRel (&Rp, U16 ("foo..bar"));
  ASSERT_OK (PathParse ("data:/..hidden", &Rp));
  ExpectRel (&Rp, U16 ("..hidden"));
  ASSERT_OK (PathParse ("data:/a...b", &Rp));
  ASSERT_OK (PathParse ("data:/x..", &Rp));
  ASSERT_OK (PathParse ("data:/.nsh", &Rp));
  ASSERT_TRUE (Rp.IsScript);
}

/* T04-7 拒绝：fs 索引非数字 / 溢出。 */
TEST (t04_7_fs_bad_index) {
  RESOLVED_PATH Rp;
  ASSERT_ERR (PathParse ("fsX:/a", &Rp));
  ASSERT_ERR (PathParse ("fs:/a", &Rp));
  ASSERT_ERR (PathParse ("fs-1:/a", &Rp));
  ASSERT_ERR (PathParse ("fs1.5:/a", &Rp));
  ASSERT_ERR (PathParse ("fs 0:/a", &Rp));
  ASSERT_ERR (PathParse ("fs0x1:/a", &Rp));
  ASSERT_ERR (PathParse ("fs+1:/a", &Rp));
  ASSERT_ERR (PathParse ("fs99999999999999999999999999:/a", &Rp));
  ASSERT_ERR (PathParse ("fs18446744073709551616:/a", &Rp));  /* 2^64，UINTN 溢出 */
  /* 边界：UINTN 最大值本身可解析（64 位） */
  ASSERT_OK (PathParse ("fs18446744073709551615:/a", &Rp));
  ASSERT_EQ (Rp.FsIndex, ~(UINTN)0);
}

/* T04-8 拒绝：boot 非 4 位十六进制（menu 除外）。 */
TEST (t04_8_boot_bad) {
  RESOLVED_PATH Rp;
  ASSERT_ERR (PathParse ("boot:GGGG", &Rp));
  ASSERT_ERR (PathParse ("boot:1", &Rp));
  ASSERT_ERR (PathParse ("boot:00001", &Rp));
  ASSERT_ERR (PathParse ("boot:12X4", &Rp));
  ASSERT_ERR (PathParse ("boot:men", &Rp));
  ASSERT_ERR (PathParse ("boot:menux", &Rp));
  ASSERT_ERR (PathParse ("boot:MENUx", &Rp));
  ASSERT_ERR (PathParse ("boot:0x10", &Rp));
  ASSERT_ERR (PathParse ("boot:-001", &Rp));
  ASSERT_ERR (PathParse ("boot: 001", &Rp));
  ASSERT_ERR (PathParse ("boot:0001 ", &Rp));
  ASSERT_ERR (PathParse ("boot:00 1", &Rp));
}

/* T04-9 拒绝：超长（Rel 容量 255 CHAR16 + NUL）；多字节按码点计数。 */
TEST (t04_9_overlong) {
  RESOLVED_PATH Rp;
  char Buf[12000];
  UINTN i;

  /* 恰好 255 字符 → 成功 */
  memcpy (Buf, "data:/", 6);
  memset (Buf + 6, 'a', 255);
  Buf[6 + 255] = '\0';
  ASSERT_OK (PathParse (Buf, &Rp));
  ASSERT_EQ (EfiStrLen (Rp.Rel), 255);

  /* 256 字符 → EFI_BAD_BUFFER_SIZE */
  memset (Buf + 6, 'a', 256);
  Buf[6 + 256] = '\0';
  ASSERT_EQ (PathParse (Buf, &Rp), EFI_BAD_BUFFER_SIZE);

  /* 255 个 CJK 码点（765 UTF-8 字节）→ 成功（按码点而非字节计长） */
  memcpy (Buf, "data:/", 6);
  for (i = 0; i < 255; i++) {
    Buf[6 + i * 3]     = (char)0xE4;   /* U+4E2D 中 */
    Buf[6 + i * 3 + 1] = (char)0xB8;
    Buf[6 + i * 3 + 2] = (char)0xAD;
  }
  Buf[6 + 255 * 3] = '\0';
  ASSERT_OK (PathParse (Buf, &Rp));
  ASSERT_EQ (EfiStrLen (Rp.Rel), 255);
  ASSERT_EQ (Rp.Rel[0], 0x4E2D);
  ASSERT_EQ (Rp.Rel[254], 0x4E2D);

  /* 256 个 CJK 码点 → EFI_BAD_BUFFER_SIZE */
  for (i = 0; i < 256; i++) {
    Buf[6 + i * 3]     = (char)0xE4;
    Buf[6 + i * 3 + 1] = (char)0xB8;
    Buf[6 + i * 3 + 2] = (char)0xAD;
  }
  Buf[6 + 256 * 3] = '\0';
  ASSERT_EQ (PathParse (Buf, &Rp), EFI_BAD_BUFFER_SIZE);

  /* 10KB 垃圾输入 → 错误返回，不崩 */
  memcpy (Buf, "data:/", 6);
  memset (Buf + 6, 'z', 10000);
  Buf[10006] = '\0';
  ASSERT_ERR (PathParse (Buf, &Rp));
}

/* T04-10 IsScript 判定（.nsh 后缀，大小写不敏感；boot 恒 FALSE）。 */
TEST (t04_10_isscript) {
  RESOLVED_PATH Rp;
  ASSERT_OK (PathParse ("data:/a.nsh", &Rp));   ASSERT_TRUE (Rp.IsScript);
  ASSERT_OK (PathParse ("data:/a.NSH", &Rp));   ASSERT_TRUE (Rp.IsScript);
  ASSERT_OK (PathParse ("data:/a.NsH", &Rp));   ASSERT_TRUE (Rp.IsScript);
  ASSERT_OK (PathParse ("auto:/x.nsh", &Rp));   ASSERT_TRUE (Rp.IsScript);
  ASSERT_OK (PathParse ("fs1:/x.nsh", &Rp));    ASSERT_TRUE (Rp.IsScript);
  ASSERT_OK (PathParse ("data:/.nsh", &Rp));    ASSERT_TRUE (Rp.IsScript);
  ASSERT_OK (PathParse ("data:/a.efi", &Rp));   ASSERT_FALSE (Rp.IsScript);
  ASSERT_OK (PathParse ("data:/a.EFI", &Rp));   ASSERT_FALSE (Rp.IsScript);
  ASSERT_OK (PathParse ("data:/a.nshx", &Rp));  ASSERT_FALSE (Rp.IsScript);
  ASSERT_OK (PathParse ("data:/a.nsh.", &Rp));  ASSERT_FALSE (Rp.IsScript);
  ASSERT_OK (PathParse ("data:/nsh", &Rp));     ASSERT_FALSE (Rp.IsScript);
  ASSERT_OK (PathParse ("data:/a.nsh/b.efi", &Rp)); ASSERT_FALSE (Rp.IsScript);
  ASSERT_OK (PathParse ("boot:0001", &Rp));     ASSERT_FALSE (Rp.IsScript);
  ASSERT_OK (PathParse ("boot:menu", &Rp));     ASSERT_FALSE (Rp.IsScript);
}

/* T04-11 架构掩码过滤全组合（bit0=x64 bit1=aarch64；含 0..255 穷举性质）。 */
TEST (t04_11_archmask) {
  UINTN m, c;
  /* 规范组合表 */
  ASSERT_FALSE (PathArchMatch (0, ARCH_X64_BIT));
  ASSERT_FALSE (PathArchMatch (0, ARCH_AARCH64_BIT));
  ASSERT_TRUE  (PathArchMatch (ARCH_X64_BIT, ARCH_X64_BIT));
  ASSERT_FALSE (PathArchMatch (ARCH_X64_BIT, ARCH_AARCH64_BIT));
  ASSERT_FALSE (PathArchMatch (ARCH_AARCH64_BIT, ARCH_X64_BIT));
  ASSERT_TRUE  (PathArchMatch (ARCH_AARCH64_BIT, ARCH_AARCH64_BIT));
  ASSERT_TRUE  (PathArchMatch (ARCH_X64_BIT | ARCH_AARCH64_BIT, ARCH_X64_BIT));
  ASSERT_TRUE  (PathArchMatch (ARCH_X64_BIT | ARCH_AARCH64_BIT, ARCH_AARCH64_BIT));
  /* 当前架构位为 0 / 双位 / 未知位的边界 */
  ASSERT_FALSE (PathArchMatch (ARCH_X64_BIT | ARCH_AARCH64_BIT, 0));
  ASSERT_TRUE  (PathArchMatch (ARCH_X64_BIT, ARCH_X64_BIT | ARCH_AARCH64_BIT));
  ASSERT_FALSE (PathArchMatch (ARCH_X64_BIT, 0x80));
  /* 穷举 256×256：结果恒等于按位与判定 */
  for (m = 0; m < 256; m++) {
    for (c = 0; c < 256; c++) {
      ASSERT_EQ (PathArchMatch ((UINT8)m, (UINT8)c), ((m & c) != 0));
    }
  }
}

/* ---- T04-12 辅助：确定性 PRNG（xorshift32，固定种子可复现） ---- */
static UINT32 g_rng = 0x50443034u;
static UINT32 Rng (void) {
  UINT32 x = g_rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  g_rng = x;
  return x;
}

/* 成功解析的不变量独立复核（与实现不同的第二套扫描）。 */
static void CheckInvariants (RESOLVED_PATH *Rp) {
  UINTN n, k, start;
  ASSERT_TRUE (Rp->Kind == PATH_DATA || Rp->Kind == PATH_AUTO ||
               Rp->Kind == PATH_FS   || Rp->Kind == PATH_BOOT);
  n = EfiStrLen (Rp->Rel);
  ASSERT_TRUE (n <= 255);
  ASSERT_TRUE (Rp->BootNumber <= 0xFFFF);
  if (Rp->Kind == PATH_BOOT) {
    ASSERT_EQ (n, 0);
    ASSERT_FALSE (Rp->IsScript);
    return;
  }
  ASSERT_TRUE (n > 0);
  ASSERT_TRUE (Rp->Rel[0] != '/');
  ASSERT_TRUE (Rp->Rel[n - 1] != '/');
  for (k = 0; k < n; k++) {
    ASSERT_TRUE (Rp->Rel[k] != '\\');
    ASSERT_TRUE (Rp->Rel[k] >= 0x20 && Rp->Rel[k] != 0x7F);
  }
  /* 分量非空且非 '.'/'..' */
  start = 0;
  for (k = 0; k <= n; k++) {
    if (k == n || Rp->Rel[k] == '/') {
      UINTN len = k - start;
      ASSERT_TRUE (len > 0);
      ASSERT_FALSE (len == 1 && Rp->Rel[start] == '.');
      ASSERT_FALSE (len == 2 && Rp->Rel[start] == '.' && Rp->Rel[start + 1] == '.');
      start = k + 1;
    }
  }
}

/* T04-12 fuzz 不崩：≥10000 轮随机输入 + 定向 UTF-8 边界。ASan/UBSan 护航。 */
TEST (t04_12_fuzz) {
  RESOLVED_PATH Rp;
  char Buf[512];
  UINTN round, i, len;
  CONST CHAR8 *Prefixes[4] = { "data:/", "auto:/", "fs0:/", "boot:" };

  /* 20000 轮随机 fuzz：4 种生成模式 */
  for (round = 0; round < 20000; round++) {
    UINT32 mode = Rng () % 4;
    len = Rng () % 300;
    if (mode == 2 || mode == 3) {
      CONST CHAR8 *p = Prefixes[Rng () % 4];
      UINTN pl = 0;
      while (p[pl]) { Buf[pl] = p[pl]; pl++; }
      for (i = pl; i < len; i++) {
        Buf[i] = (mode == 2) ? (char)(1 + Rng () % 255)
                             : (char)(0x20 + Rng () % 0x5F);
      }
    } else if (mode == 0) {
      for (i = 0; i < len; i++) Buf[i] = (char)(1 + Rng () % 255);  /* 任意非 NUL 字节 */
    } else {
      for (i = 0; i < len; i++) Buf[i] = (char)(0x20 + Rng () % 0x5F); /* 可打印 ASCII */
    }
    Buf[len] = '\0';
    if (!EFI_ERROR (PathParse (Buf, &Rp))) {
      CheckInvariants (&Rp);
    }
  }

  /* 定向：合法 BMP 多字节（中文路径） */
  ASSERT_OK (PathParse ("data:/\xE4\xB8\xAD\xE6\x96\x87/x.nsh", &Rp));  /* 中文 */
  {
    CHAR16 Want[] = { 0x4E2D, 0x6587, '/', 'x', '.', 'n', 's', 'h', 0 };
    ExpectRel (&Rp, Want);
    ASSERT_TRUE (Rp.IsScript);
  }
  /* BMP 边界码点：U+0080 / U+07FF / U+0800 / U+FFFF */
  ASSERT_OK (PathParse ("data:/\xC2\x80", &Rp));       ASSERT_EQ (Rp.Rel[0], 0x0080);
  ASSERT_OK (PathParse ("data:/\xDF\xBF", &Rp));       ASSERT_EQ (Rp.Rel[0], 0x07FF);
  ASSERT_OK (PathParse ("data:/\xE0\xA0\x80", &Rp));   ASSERT_EQ (Rp.Rel[0], 0x0800);
  ASSERT_OK (PathParse ("data:/\xEF\xBF\xBF", &Rp));   ASSERT_EQ (Rp.Rel[0], 0xFFFF);

  /* 定向：非法 UTF-8 / 超 UCS-2 / 控制字符 → 拒绝不崩 */
  ASSERT_ERR (PathParse ("data:/\xFF", &Rp));              /* 非法字节 */
  ASSERT_ERR (PathParse ("data:/\xC0\xAF", &Rp));          /* overlong 2 字节 */
  ASSERT_ERR (PathParse ("data:/\xE0\x80\x80", &Rp));      /* overlong 3 字节 */
  ASSERT_ERR (PathParse ("data:/\xE4\xB8", &Rp));          /* 截断 3 字节 */
  ASSERT_ERR (PathParse ("data:/\xE4\xB8\xAD\xE6\x96", &Rp)); /* 尾部截断 */
  ASSERT_ERR (PathParse ("data:/\x80", &Rp));              /* 孤立续字节 */
  ASSERT_ERR (PathParse ("data:/\xED\xA0\x80", &Rp));      /* UTF-16 代理区 */
  ASSERT_ERR (PathParse ("data:/\xF0\x9F\x98\x80", &Rp));  /* emoji（星面，UCS-2 不可表示） */
  ASSERT_ERR (PathParse ("data:/\xF4\x8F\xBF\xBF", &Rp));  /* U+10FFFF（星面） */
  ASSERT_ERR (PathParse ("data:/\xF5\x80\x80\x80", &Rp));  /* 非法前导 */
  ASSERT_ERR (PathParse ("data:/\x01", &Rp));              /* C0 控制 */
  ASSERT_ERR (PathParse ("data:/\x7F", &Rp));              /* DEL */
  ASSERT_ERR (PathParse ("data:/a\tb", &Rp));              /* TAB */
  ASSERT_ERR (PathParse ("data:/a\nb", &Rp));              /* LF */
}

int main (void) {
  RUN_TEST (t04_1_prefixes);
  RUN_TEST (t04_2_slash_normalize);
  RUN_TEST (t04_3_index_hex);
  RUN_TEST (t04_4_empty);
  RUN_TEST (t04_5_unknown_prefix);
  RUN_TEST (t04_6_traversal);
  RUN_TEST (t04_7_fs_bad_index);
  RUN_TEST (t04_8_boot_bad);
  RUN_TEST (t04_9_overlong);
  RUN_TEST (t04_10_isscript);
  RUN_TEST (t04_11_archmask);
  RUN_TEST (t04_12_fuzz);
  RUN_TESTS_END ();
}
