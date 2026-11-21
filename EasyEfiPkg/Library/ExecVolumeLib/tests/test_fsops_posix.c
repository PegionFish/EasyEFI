/* S09 宿主测试：FsPosixBackend 全操作 + 边界（T09-1..9）+ 内存配对（T09-12）。
 * 真实临时目录；语义对齐 MockFs（差异见 FsPosixBackend.c 头注/交接注记）。 */
#include "test.h"
#include "Contract/FsOps.h"
#include "ExecVolumeInternal.h"
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

UINTN TestMemLiveBlocks (void);
UINTN TestMemLiveBytes (void);

static CHAR8  g_root[128];
static FS_OPS *g_fs;

/* 递归清理临时树（测试收尾用，宿主 libc） */
static void rmtree (const char *path) {
  DIR *d = opendir (path);
  struct dirent *de;
  char child[1024];
  struct stat sb;
  if (d == NULL) return;
  while ((de = readdir (d)) != NULL) {
    if (strcmp (de->d_name, ".") == 0 || strcmp (de->d_name, "..") == 0) continue;
    snprintf (child, sizeof (child), "%s/%s", path, de->d_name);
    if (lstat (child, &sb) == 0 && S_ISDIR (sb.st_mode)) rmtree (child);
    else unlink (child);
  }
  closedir (d);
  rmdir (path);
}

static void setup (void) {
  strcpy (g_root, "/tmp/s09fs.XXXXXX");
  ASSERT_TRUE (mkdtemp (g_root) != NULL);
  g_fs = FsPosixBackend (g_root);
  ASSERT_TRUE (g_fs != NULL);
}

static void teardown (void) {
  if (g_fs != NULL) { g_fs->Destroy (g_fs); g_fs = NULL; }
  rmtree (g_root);
}

/* 播种一个文件（经后端自身） */
static void seed (CONST CHAR16 *path, CONST void *data, UINTN len) {
  FS_HANDLE *h = NULL;
  UINTN n = len;
  ASSERT_OK (g_fs->Open (g_fs, path, FS_MODE_WRITE | FS_MODE_CREATE, &h));
  ASSERT_OK (g_fs->Write (g_fs, h, data, &n));
  ASSERT_EQ (n, len);
  ASSERT_OK (g_fs->Close (g_fs, h));
}

TEST (t09_1_open_read_close) {
  FS_HANDLE *h = NULL;
  UINT8 buf[64];
  UINTN len;
  setup ();
  seed (U16 ("hello.txt"), "EasyEFI", 7);
  ASSERT_OK (g_fs->Open (g_fs, U16 ("hello.txt"), FS_MODE_READ, &h));
  len = sizeof (buf);
  ASSERT_OK (g_fs->Read (g_fs, h, buf, &len));
  ASSERT_EQ (len, 7);
  ASSERT_MEM_EQ (buf, "EasyEFI", 7);
  len = sizeof (buf);                       /* EOF → 0 */
  ASSERT_OK (g_fs->Read (g_fs, h, buf, &len));
  ASSERT_EQ (len, 0);
  ASSERT_OK (g_fs->Close (g_fs, h));
  /* 路径等价：反斜杠 / 前导 '/' / 重复 '/' / 尾 '/' */
  ASSERT_OK (g_fs->Open (g_fs, U16 ("\\hello.txt"), FS_MODE_READ, &h));
  ASSERT_OK (g_fs->Close (g_fs, h));
  ASSERT_OK (g_fs->Open (g_fs, U16 ("/hello.txt"), FS_MODE_READ, &h));
  ASSERT_OK (g_fs->Close (g_fs, h));
  /* 不存在 */
  ASSERT_EQ (g_fs->Open (g_fs, U16 ("nope.txt"), FS_MODE_READ, &h), EFI_NOT_FOUND);
  /* 目录不可 Open */
  ASSERT_OK (g_fs->MkDir (g_fs, U16 ("adir")));
  ASSERT_EQ (g_fs->Open (g_fs, U16 ("adir"), FS_MODE_READ, &h), EFI_INVALID_PARAMETER);
  /* 穿越拒绝 */
  ASSERT_EQ (g_fs->Open (g_fs, U16 ("../escape"), FS_MODE_READ, &h), EFI_INVALID_PARAMETER);
  ASSERT_EQ (g_fs->Open (g_fs, U16 ("a/../../escape"), FS_MODE_READ, &h), EFI_INVALID_PARAMETER);
  /* 非法模式 */
  ASSERT_EQ (g_fs->Open (g_fs, U16 ("hello.txt"), 0, &h), EFI_INVALID_PARAMETER);
  teardown ();
}

TEST (t09_2_write_modes) {
  FS_HANDLE *h = NULL;
  UINT8 buf[16];
  UINTN len;
  setup ();
  /* CREATE 创建新文件；顺序写推进位置 */
  ASSERT_OK (g_fs->Open (g_fs, U16 ("w.txt"), FS_MODE_WRITE | FS_MODE_CREATE, &h));
  len = 3; ASSERT_OK (g_fs->Write (g_fs, h, "abc", &len)); ASSERT_EQ (len, 3);
  len = 3; ASSERT_OK (g_fs->Write (g_fs, h, "def", &len)); ASSERT_EQ (len, 3);
  ASSERT_OK (g_fs->Close (g_fs, h));
  ASSERT_OK (g_fs->Open (g_fs, U16 ("w.txt"), FS_MODE_READ, &h));
  len = sizeof (buf); ASSERT_OK (g_fs->Read (g_fs, h, buf, &len));
  ASSERT_EQ (len, 6); ASSERT_MEM_EQ (buf, "abcdef", 6);
  ASSERT_OK (g_fs->Close (g_fs, h));
  /* 覆盖写不截断（对齐 mock/UEFI FAT）：重开 WRITE 写 2 字节 → 尾部保留 */
  ASSERT_OK (g_fs->Open (g_fs, U16 ("w.txt"), FS_MODE_WRITE, &h));
  len = 2; ASSERT_OK (g_fs->Write (g_fs, h, "XY", &len));
  ASSERT_OK (g_fs->Close (g_fs, h));
  ASSERT_OK (g_fs->Open (g_fs, U16 ("w.txt"), FS_MODE_READ, &h));
  len = sizeof (buf); ASSERT_OK (g_fs->Read (g_fs, h, buf, &len));
  ASSERT_EQ (len, 6); ASSERT_MEM_EQ (buf, "XYcdef", 6);
  ASSERT_OK (g_fs->Close (g_fs, h));
  /* READ 句柄写拒绝 */
  ASSERT_OK (g_fs->Open (g_fs, U16 ("w.txt"), FS_MODE_READ, &h));
  len = 1;
  ASSERT_EQ (g_fs->Write (g_fs, h, "z", &len), EFI_ACCESS_DENIED);
  ASSERT_OK (g_fs->Close (g_fs, h));
  /* CREATE 不带 WRITE：可创建，写拒绝（对齐 mock） */
  ASSERT_OK (g_fs->Open (g_fs, U16 ("c.txt"), FS_MODE_CREATE, &h));
  len = 1;
  ASSERT_EQ (g_fs->Write (g_fs, h, "z", &len), EFI_ACCESS_DENIED);
  ASSERT_OK (g_fs->Close (g_fs, h));
  /* WRITE 不带 CREATE：不存在 → NOT_FOUND */
  ASSERT_EQ (g_fs->Open (g_fs, U16 ("missing.txt"), FS_MODE_WRITE, &h), EFI_NOT_FOUND);
  teardown ();
}

TEST (t09_3_flush_close) {
  FS_HANDLE *h = NULL;
  UINT8 buf[8];
  UINTN len;
  setup ();
  ASSERT_OK (g_fs->Open (g_fs, U16 ("f.txt"), FS_MODE_WRITE | FS_MODE_CREATE, &h));
  len = 5; ASSERT_OK (g_fs->Write (g_fs, h, "flush", &len));
  ASSERT_OK (g_fs->Flush (g_fs, h));        /* fsync */
  ASSERT_OK (g_fs->Close (g_fs, h));
  ASSERT_OK (g_fs->Open (g_fs, U16 ("f.txt"), FS_MODE_READ, &h));
  ASSERT_OK (g_fs->Flush (g_fs, h));        /* 只读句柄 Flush 亦合法 */
  len = sizeof (buf); ASSERT_OK (g_fs->Read (g_fs, h, buf, &len));
  ASSERT_EQ (len, 5); ASSERT_MEM_EQ (buf, "flush", 5);
  ASSERT_OK (g_fs->Close (g_fs, h));
  teardown ();
}

TEST (t09_4_delete) {
  FS_ENTRY ent;
  setup ();
  seed (U16 ("d.txt"), "x", 1);
  ASSERT_OK (g_fs->Delete (g_fs, U16 ("d.txt")));
  ASSERT_EQ (g_fs->Info (g_fs, U16 ("d.txt"), &ent), EFI_NOT_FOUND);
  ASSERT_EQ (g_fs->Delete (g_fs, U16 ("d.txt")), EFI_NOT_FOUND);
  /* 空目录可删 */
  ASSERT_OK (g_fs->MkDir (g_fs, U16 ("empty")));
  ASSERT_OK (g_fs->Delete (g_fs, U16 ("empty")));
  ASSERT_EQ (g_fs->Info (g_fs, U16 ("empty"), &ent), EFI_NOT_FOUND);
  /* 非空目录拒绝且保留 */
  ASSERT_OK (g_fs->MkDir (g_fs, U16 ("full")));
  seed (U16 ("full/inner.txt"), "y", 1);
  ASSERT_EQ (g_fs->Delete (g_fs, U16 ("full")), EFI_ACCESS_DENIED);
  ASSERT_OK (g_fs->Info (g_fs, U16 ("full/inner.txt"), &ent));
  /* 含子目录亦算非空 */
  ASSERT_OK (g_fs->MkDir (g_fs, U16 ("full/sub")));
  ASSERT_EQ (g_fs->Delete (g_fs, U16 ("full")), EFI_ACCESS_DENIED);
  /* 卷根不可删 */
  ASSERT_EQ (g_fs->Delete (g_fs, U16 ("/")), EFI_INVALID_PARAMETER);
  teardown ();
}

TEST (t09_5_rename) {
  FS_HANDLE *h = NULL;
  FS_ENTRY ent;
  UINT8 buf[8];
  UINTN len;
  setup ();
  seed (U16 ("old.txt"), "data", 4);
  ASSERT_OK (g_fs->Rename (g_fs, U16 ("old.txt"), U16 ("new.txt")));
  ASSERT_OK (g_fs->Open (g_fs, U16 ("new.txt"), FS_MODE_READ, &h));
  len = sizeof (buf); ASSERT_OK (g_fs->Read (g_fs, h, buf, &len));
  ASSERT_EQ (len, 4); ASSERT_MEM_EQ (buf, "data", 4);
  ASSERT_OK (g_fs->Close (g_fs, h));
  ASSERT_EQ (g_fs->Open (g_fs, U16 ("old.txt"), FS_MODE_READ, &h), EFI_NOT_FOUND);
  /* 源不存在 */
  ASSERT_EQ (g_fs->Rename (g_fs, U16 ("ghost"), U16 ("x")), EFI_NOT_FOUND);
  /* 目标已存在拒绝，双方内容不变 */
  seed (U16 ("victim.txt"), "keep", 4);
  ASSERT_EQ (g_fs->Rename (g_fs, U16 ("new.txt"), U16 ("victim.txt")), EFI_ACCESS_DENIED);
  ASSERT_OK (g_fs->Open (g_fs, U16 ("victim.txt"), FS_MODE_READ, &h));
  len = sizeof (buf); ASSERT_OK (g_fs->Read (g_fs, h, buf, &len));
  ASSERT_EQ (len, 4); ASSERT_MEM_EQ (buf, "keep", 4);
  ASSERT_OK (g_fs->Close (g_fs, h));
  /* 目录改名 + 跨目录改名（POSIX 支持） */
  ASSERT_OK (g_fs->MkDir (g_fs, U16 ("d1")));
  ASSERT_OK (g_fs->MkDir (g_fs, U16 ("d2")));
  ASSERT_OK (g_fs->Rename (g_fs, U16 ("d1"), U16 ("d1ren")));
  ASSERT_OK (g_fs->Rename (g_fs, U16 ("new.txt"), U16 ("d2/moved.txt")));
  ASSERT_OK (g_fs->Info (g_fs, U16 ("d2/moved.txt"), &ent));
  ASSERT_EQ (ent.Size, 4);
  teardown ();
}

TEST (t09_6_mkdir) {
  FS_ENTRY ent;
  setup ();
  ASSERT_OK (g_fs->MkDir (g_fs, U16 ("nd")));
  ASSERT_OK (g_fs->Info (g_fs, U16 ("nd"), &ent));
  ASSERT_TRUE (ent.IsDir);
  ASSERT_EQ (g_fs->MkDir (g_fs, U16 ("nd")), EFI_ACCESS_DENIED);
  /* 父目录缺失 */
  ASSERT_EQ (g_fs->MkDir (g_fs, U16 ("no/such/parent")), EFI_NOT_FOUND);
  /* 同名文件占位 */
  seed (U16 ("afile"), "x", 1);
  ASSERT_EQ (g_fs->MkDir (g_fs, U16 ("afile")), EFI_ACCESS_DENIED);
  /* 逐级创建后可写文件 */
  ASSERT_OK (g_fs->MkDir (g_fs, U16 ("nd/sub")));
  seed (U16 ("nd/sub/f.txt"), "ok", 2);
  ASSERT_OK (g_fs->Info (g_fs, U16 ("nd/sub/f.txt"), &ent));
  ASSERT_EQ (ent.Size, 2);
  teardown ();
}

TEST (t09_7_enum) {
  FS_ENTRY *ents = NULL;
  UINTN n = 0, i;
  setup ();
  ASSERT_OK (g_fs->MkDir (g_fs, U16 ("zdir")));
  ASSERT_OK (g_fs->MkDir (g_fs, U16 ("adir")));
  seed (U16 ("b.txt"), "bb", 2);
  seed (U16 ("a.txt"), "a", 1);
  seed (U16 ("c.txt"), "ccc", 3);
  ASSERT_OK (g_fs->Enum (g_fs, U16 (""), &ents, &n));
  ASSERT_EQ (n, 5);
  /* 目录优先按名排序 */
  ASSERT_TRUE (ents[0].IsDir);  ASSERT_TRUE (EfiStrCmp (ents[0].Name, U16 ("adir")) == 0);
  ASSERT_TRUE (ents[1].IsDir);  ASSERT_TRUE (EfiStrCmp (ents[1].Name, U16 ("zdir")) == 0);
  ASSERT_FALSE (ents[2].IsDir); ASSERT_TRUE (EfiStrCmp (ents[2].Name, U16 ("a.txt")) == 0);
  ASSERT_FALSE (ents[3].IsDir); ASSERT_TRUE (EfiStrCmp (ents[3].Name, U16 ("b.txt")) == 0);
  ASSERT_FALSE (ents[4].IsDir); ASSERT_TRUE (EfiStrCmp (ents[4].Name, U16 ("c.txt")) == 0);
  ASSERT_EQ (ents[2].Size, 1);
  ASSERT_EQ (ents[4].Size, 3);
  ASSERT_TRUE (ents[2].Mtime > 0);
  MemFree (ents); ents = NULL;
  /* 根路径等价写法 */
  ASSERT_OK (g_fs->Enum (g_fs, U16 ("/"), &ents, &n));
  ASSERT_EQ (n, 5);
  MemFree (ents); ents = NULL;
  /* 子目录 + 反斜杠 + 尾斜杠；无 "." / ".." */
  seed (U16 ("zdir/inner.txt"), "i", 1);
  ASSERT_OK (g_fs->Enum (g_fs, U16 ("\\zdir\\"), &ents, &n));
  ASSERT_EQ (n, 1);
  ASSERT_TRUE (EfiStrCmp (ents[0].Name, U16 ("inner.txt")) == 0);
  for (i = 0; i < n; i++) {
    ASSERT_TRUE (EfiStrCmp (ents[i].Name, U16 (".")) != 0);
    ASSERT_TRUE (EfiStrCmp (ents[i].Name, U16 ("..")) != 0);
  }
  MemFree (ents); ents = NULL;
  /* 空目录 → 0 条 */
  ASSERT_OK (g_fs->Enum (g_fs, U16 ("adir"), &ents, &n));
  ASSERT_EQ (n, 0);
  MemFree (ents); ents = NULL;
  /* 不存在目录 → NOT_FOUND（真实 FS 语义；mock 平面表返回空集，有意差异） */
  ASSERT_EQ (g_fs->Enum (g_fs, U16 ("ghostdir"), &ents, &n), EFI_NOT_FOUND);
  /* 文件当目录 → NOT_FOUND */
  ASSERT_EQ (g_fs->Enum (g_fs, U16 ("a.txt"), &ents, &n), EFI_NOT_FOUND);
  teardown ();
}

TEST (t09_8_info) {
  FS_ENTRY ent;
  setup ();
  ASSERT_OK (g_fs->MkDir (g_fs, U16 ("dir")));
  seed (U16 ("dir/f.txt"), "12345", 5);
  memset (&ent, 0xAA, sizeof (ent));
  ASSERT_OK (g_fs->Info (g_fs, U16 ("\\dir\\f.txt"), &ent));
  ASSERT_EQ (ent.Size, 5);
  ASSERT_FALSE (ent.IsDir);
  ASSERT_TRUE (ent.Mtime > 0);
  /* Name = 规范化输入路径（对齐 mock） */
  ASSERT_TRUE (EfiStrCmp (ent.Name, U16 ("dir/f.txt")) == 0);
  ASSERT_OK (g_fs->Info (g_fs, U16 ("dir"), &ent));
  ASSERT_TRUE (ent.IsDir);
  ASSERT_EQ (g_fs->Info (g_fs, U16 ("ghost"), &ent), EFI_NOT_FOUND);
  ASSERT_EQ (g_fs->Info (g_fs, U16 ("../up"), &ent), EFI_INVALID_PARAMETER);
  teardown ();
}

TEST (t09_9_large_file) {
  FS_HANDLE *h = NULL;
  UINTN total = 1024 * 1024, chunk = 64 * 1024, rchunk = 32 * 1024;
  UINTN off, len, got;
  UINT8 *wbuf, *rbuf;
  FS_ENTRY ent;
  setup ();
  wbuf = (UINT8 *)MemAlloc (total);
  rbuf = (UINT8 *)MemAlloc (total);
  ASSERT_TRUE (wbuf != NULL && rbuf != NULL);
  for (off = 0; off < total; off++) wbuf[off] = (UINT8)(off * 31 + 7);
  ASSERT_OK (g_fs->Open (g_fs, U16 ("big.bin"), FS_MODE_WRITE | FS_MODE_CREATE, &h));
  for (off = 0; off < total; off += chunk) {
    len = chunk;
    ASSERT_OK (g_fs->Write (g_fs, h, wbuf + off, &len));
    ASSERT_EQ (len, chunk);
  }
  ASSERT_OK (g_fs->Flush (g_fs, h));
  ASSERT_OK (g_fs->Close (g_fs, h));
  ASSERT_OK (g_fs->Info (g_fs, U16 ("big.bin"), &ent));
  ASSERT_EQ (ent.Size, total);
  ASSERT_OK (g_fs->Open (g_fs, U16 ("big.bin"), FS_MODE_READ, &h));
  got = 0;
  for (;;) {
    len = rchunk;
    ASSERT_OK (g_fs->Read (g_fs, h, rbuf + got, &len));
    if (len == 0) break;
    got += len;
    ASSERT_TRUE (got <= total);
  }
  ASSERT_EQ (got, total);
  ASSERT_MEM_EQ (rbuf, wbuf, total);
  ASSERT_OK (g_fs->Close (g_fs, h));
  MemFree (wbuf);
  MemFree (rbuf);
  teardown ();
}

TEST (t09_12_mem_pairing) {
  UINTN round;
  /* 反复创建/操作/销毁，验证 callee 分配全部可被 caller 释放 */
  for (round = 0; round < 8; round++) {
    FS_ENTRY *ents = NULL, ent;
    UINTN n = 0;
    setup ();
    seed (U16 ("m.txt"), "mem", 3);
    ASSERT_OK (g_fs->MkDir (g_fs, U16 ("md")));
    ASSERT_OK (g_fs->Enum (g_fs, U16 (""), &ents, &n));
    ASSERT_EQ (n, 2);
    MemFree (ents);
    ASSERT_OK (g_fs->Info (g_fs, U16 ("m.txt"), &ent));
    ASSERT_OK (g_fs->Rename (g_fs, U16 ("m.txt"), U16 ("m2.txt")));
    ASSERT_OK (g_fs->Delete (g_fs, U16 ("m2.txt")));
    ASSERT_OK (g_fs->Delete (g_fs, U16 ("md")));
    teardown ();
  }
  /* 全套用例跑完后全局分配必须归零（任何泄漏都会在此暴露） */
  ASSERT_EQ (TestMemLiveBlocks (), 0);
  ASSERT_EQ (TestMemLiveBytes (), 0);
}

int main (void) {
  RUN_TEST (t09_1_open_read_close);
  RUN_TEST (t09_2_write_modes);
  RUN_TEST (t09_3_flush_close);
  RUN_TEST (t09_4_delete);
  RUN_TEST (t09_5_rename);
  RUN_TEST (t09_6_mkdir);
  RUN_TEST (t09_7_enum);
  RUN_TEST (t09_8_info);
  RUN_TEST (t09_9_large_file);
  RUN_TEST (t09_12_mem_pairing);
  RUN_TESTS_END ();
}
