/* S12 宿主测试：FileManager core（T12-1..11）。
 * MockTui（屏幕+按键）+ MockExec（记录器）+ 双文件后端：
 *   默认 MockFs（内存平面表）；-DUSE_POSIX_FS 切 S09 FsPosixBackend（真实临时目录，
 *   任务书"POSIX 临时树"语义）。两后端各跑一遍 T12-1..11（host.mk 两个二进制）。
 * 运行：模块目录内 make -f host.mk test
 */
#include "test.h"
#include "MockTui.h"
#include "MockFs.h"
#include "MockExec.h"
#include "../FmCore.h"

#ifdef USE_POSIX_FS
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#endif

/* ---------------- 夹具 ---------------- */

static FS_OPS *g_fs0, *g_fs1;
static CHAR8 g_root0[64], g_root1[64], g_root2[64];   /* POSIX 临时树根（MockFs 分支不用） */
static VOLUME_INFO g_vols[3];
static CONST CHAR16 *g_fsnames[3];
static FM_INIT g_init;
static FM_PLATFORM_OPS g_ops;
/* U16() 宿主侧为旋转静态缓冲（8 槽）：长期借用的字符串必须拷到稳定存储 */
static CHAR16 g_fsname_buf[3][8];
static CHAR16 g_editor_path[64];
static CHAR16 g_shell_path[64];

/* 后端创建/销毁（POSIX 分支递归清理临时树） */
static FS_OPS *backend_new (CHAR8 *RootOut) {
#ifdef USE_POSIX_FS
  char tmpl[] = "/tmp/s12fm-XXXXXX";
  char *d = mkdtemp (tmpl);
  UINTN i;
  if (d == NULL) return NULL;
  for (i = 0; d[i] != 0 && i < 63; i++) RootOut[i] = d[i];
  RootOut[i] = 0;
  return FsPosixBackend (RootOut);
#else
  RootOut[0] = 0;
  return MockFsNew ();
#endif
}

#ifdef USE_POSIX_FS
static void rm_tree (CONST CHAR8 *Root) {
  DIR *d = opendir (Root);
  struct dirent *e;
  CHAR8 p[512];
  struct stat sb;
  if (d == NULL) return;
  while ((e = readdir (d)) != NULL) {
    UINTN i;
    if (e->d_name[0] == '.' &&
        (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
    for (i = 0; Root[i] != 0 && i < 400; i++) p[i] = Root[i];
    p[i++] = '/';
    {
      UINTN j;
      for (j = 0; e->d_name[j] != 0 && i + j < 500; j++) p[i + j] = e->d_name[j];
      p[i + j] = 0;
    }
    if (stat (p, &sb) == 0 && S_ISDIR (sb.st_mode)) rm_tree (p);
    else unlink (p);
  }
  closedir (d);
  rmdir (Root);
}
#endif

static void backend_free (FS_OPS *Fs, CHAR8 *Root) {
  if (Fs == NULL) return;
  Fs->Destroy (Fs);
#ifdef USE_POSIX_FS
  if (Root[0] != 0) rm_tree (Root);
#else
  (void)Root;
#endif
}

/* 播种：目录/文件（文件先 Delete 保证截断语义，两后端一致）。
   MockFs 分支用 MockFsAddFile 绕过其 64KiB Write 上限（big.img 1MB 播种）。 */
static void seed_dir (FS_OPS *Fs, CONST CHAR16 *Path) {
#ifdef USE_POSIX_FS
  Fs->MkDir (Fs, Path);
#else
  MockFsAddDir (Fs, Path);
#endif
}

static void seed_file (FS_OPS *Fs, CONST CHAR16 *Path, CONST UINT8 *Data, UINTN Len) {
#ifdef USE_POSIX_FS
  FS_HANDLE *H = NULL;
  UINTN w = Len;
  Fs->Delete (Fs, Path);   /* 忽略结果：不存在亦正常 */
  if (!EFI_ERROR (Fs->Open (Fs, Path, FS_MODE_WRITE | FS_MODE_CREATE, &H))) {
    Fs->Write (Fs, H, Data, &w);
    Fs->Close (Fs, H);
  }
#else
  MockFsAddFile (Fs, Path, Data, Len);
#endif
}

/* T12-4 读取量计数包装：委托 thunk（MockFs 函数把 FS_OPS* 当 MOCKFS* 容器首成员，
   不能拷贝 vtable 自立——必须转调内层指针） */
static FS_OPS g_countops;
static FS_OPS *g_countinner;
static UINTN g_readbytes;
static EFI_STATUS count_open (FS_OPS *Fs, CONST CHAR16 *P, UINT32 M, FS_HANDLE **H) {
  return g_countinner->Open (g_countinner, P, M, H);
}
static EFI_STATUS count_read (FS_OPS *Fs, FS_HANDLE *H, VOID *Buf, UINTN *Len) {
  EFI_STATUS s = g_countinner->Read (g_countinner, H, Buf, Len);
  g_readbytes += *Len;
  return s;
}
static EFI_STATUS count_close (FS_OPS *Fs, FS_HANDLE *H) {
  return g_countinner->Close (g_countinner, H);
}
static EFI_STATUS count_enum (FS_OPS *Fs, CONST CHAR16 *D, FS_ENTRY **E, UINTN *N) {
  return g_countinner->Enum (g_countinner, D, E, N);
}
static EFI_STATUS count_info (FS_OPS *Fs, CONST CHAR16 *P, FS_ENTRY *O) {
  return g_countinner->Info (g_countinner, P, O);
}

/* CHAR16 标签拷贝（夹具助手） */
static void str_copy_label (CHAR16 *Dst, CONST CHAR16 *Src) {
  UINTN i;
  for (i = 0; Src[i] && i < 63; i++) Dst[i] = Src[i];
  Dst[i] = 0;
}

static void seed_volumes (void) {
  UINT8 bin[64];
  UINT8 *big;
  UINTN i;
  g_fs0 = backend_new (g_root0);
  g_fs1 = backend_new (g_root1);
  seed_dir (g_fs0, U16("EFI"));
  seed_dir (g_fs0, U16("EFI/Apps"));
  for (i = 0; i < sizeof (bin); i++) bin[i] = (UINT8)(i * 7);
  bin[0] = 0x4D; bin[1] = 0x5A; bin[2] = 0;      /* MZ + NUL → 二进制 */
  seed_file (g_fs0, U16("EFI/Apps/Editor.efi"), bin, sizeof (bin));
  seed_file (g_fs0, U16("startup.nsh"), (CONST UINT8 *)"echo hi", 7);

  seed_dir (g_fs1, U16("scripts"));
  seed_dir (g_fs1, U16("scripts/sub"));
  seed_dir (g_fs1, U16("emptydir"));
  seed_file (g_fs1, U16("scripts/fpt.nsh"),
             (CONST UINT8 *)"# \xe4\xb8\xad\xe6\x96\x87 comment\nfpt -f firmware.bin\n", 37);
  seed_file (g_fs1, U16("scripts/sub/deep.nsh"), (CONST UINT8 *)"echo deep", 9);
  seed_file (g_fs1, U16("firmware.bin"), bin, sizeof (bin));
  seed_file (g_fs1, U16("readme.txt"), (CONST UINT8 *)"hello toolkit", 13);
  big = MemAlloc (1048576);
  for (i = 0; i < 1048576; i++) big[i] = (UINT8)(i & 0xFF);
  big[100] = 0;                                   /* NUL → 二进制 */
  seed_file (g_fs1, U16("big.img"), big, 1048576);
  MemFree (big);

  /* 卷 0 root 追加 30 个文件（视口钳制用） */
  for (i = 0; i < 30; i++) {
    CHAR16 name[32];
    UINTN j;
    CONST CHAR16 *pfx = U16("file");
    for (j = 0; pfx[j]; j++) name[j] = pfx[j];
    name[j++] = (CHAR16)('a' + i / 26);
    name[j++] = (CHAR16)('a' + i % 26);
    name[j++] = L'.'; name[j++] = L't'; name[j++] = L'x'; name[j++] = L't';
    name[j] = 0;
    seed_file (g_fs0, name, (CONST UINT8 *)"x", 1);
  }

  g_vols[0].Label[0] = 0;
  str_copy_label (g_vols[0].Label, U16("TESTESP"));
  str_copy_label (g_vols[0].FsName, U16("FAT32"));
  g_vols[0].Capacity = 64ULL * 1024 * 1024;
  g_vols[0].Free = 32ULL * 1024 * 1024;
  g_vols[0].IsP3 = FALSE;
  g_vols[0].Dp = NULL; g_vols[0].DpLen = 0;
  g_vols[0].Fs = g_fs0;
  str_copy_label (g_vols[1].Label, U16("TOOLKIT"));
  str_copy_label (g_vols[1].FsName, U16("NTFS"));
  g_vols[1].Capacity = 512ULL * 1024 * 1024;
  g_vols[1].Free = 100ULL * 1024 * 1024;
  g_vols[1].IsP3 = TRUE;
  g_vols[1].Dp = NULL; g_vols[1].DpLen = 0;
  g_vols[1].Fs = g_fs1;
  str_copy_label (g_fsname_buf[0], U16("fs0:"));
  str_copy_label (g_fsname_buf[1], U16("fs1:"));
  g_fsnames[0] = g_fsname_buf[0];
  g_fsnames[1] = g_fsname_buf[1];
}

static FM_CORE *new_core (void) {
  FM_CORE *C;
  MockTuiReset ();
  MockTuiSetSize (25, 80);
  TuiInit ();
  MockExecReset ();
  seed_volumes ();
  g_init.Vols = g_vols;
  g_init.VolCount = 2;
  g_init.FsNames = g_fsnames;
  str_copy_label (g_editor_path, U16("fs0:/EFI/Apps/Editor.efi"));
  str_copy_label (g_shell_path, U16("fs0:/EFI/BOOT/Shell.efi"));
  g_init.EditorPath = g_editor_path;
  g_init.ShellPath = g_shell_path;
  g_init.PreviewMaxBytes = 16384;
  g_init.Ops = NULL;
  C = FmCoreNew (&g_init);
  return C;
}

static void key (FM_CORE *C, UINT16 Scan, CHAR16 Uni) {
  TUI_KEY k;
  k.ScanCode = Scan;
  k.UnicodeChar = Uni;
  ASSERT_OK (FmCoreHandleKey (C, &k));
}
static void ch (FM_CORE *C, CHAR16 c) { key (C, 0, c); }
static void type_str (FM_CORE *C, CONST CHAR8 *s) {
  UINTN i;
  for (i = 0; s[i]; i++) ch (C, (CHAR16)(UINT8)s[i]);
}

/* 屏幕任意行含子串（CHAR16 手工匹配） */
static BOOLEAN screen_has (CONST CHAR16 *Needle) {
  CHAR16 row[256];
  UINTN r, i, j;
  for (r = 0; r < 25; r++) {
    MockTuiRowText (r, row, 256);
    for (i = 0; row[i]; i++) {
      for (j = 0; Needle[j]; j++) if (row[i + j] != Needle[j]) break;
      if (!Needle[j]) return TRUE;
    }
  }
  return FALSE;
}

static UINTN find_view (FM_CORE *C, CONST CHAR16 *Name) {
  UINTN i;
  for (i = 0; i < FmCoreViewCount (C); i++) {
    CONST FS_ENTRY *e = FmCoreEntry (C, i);
    UINTN j;
    for (j = 0; Name[j] && e->Name[j] == Name[j]; j++);
    if (!Name[j] && !e->Name[j]) return i;
  }
  return (UINTN)-1;
}

static void select_name (FM_CORE *C, CONST CHAR16 *Name) {
  UINTN idx = find_view (C, Name);
  ASSERT_TRUE (idx != (UINTN)-1);
  if (idx == (UINTN)-1) return;
  while (FmCoreSelected (C) < idx) key (C, SCAN_DOWN, 0);
  while (FmCoreSelected (C) > idx) key (C, SCAN_UP, 0);
}

/* ---------------- T12-1 导航/排序/上级 ---------------- */
TEST (t12_1_navigation) {
  FM_CORE *C = new_core ();
  ASSERT_EQ (FmCoreState (C), FM_STATE_DIR);
  ASSERT_EQ (FmCoreVolIndex (C), 1);                 /* 默认进 P3 */
  ASSERT_EQ (FmCoreCwd (C)[0], 0);
  /* 目录优先按名：emptydir/ scripts/ 然后文件按名 */
  ASSERT_TRUE (FmCoreEntry (C, 0)->IsDir);
  ASSERT_EQ (EfiStrCmp (FmCoreEntry (C, 0)->Name, U16("emptydir")), 0);
  ASSERT_EQ (EfiStrCmp (FmCoreEntry (C, 1)->Name, U16("scripts")), 0);
  ASSERT_FALSE (FmCoreEntry (C, 2)->IsDir);
  ASSERT_EQ (EfiStrCmp (FmCoreEntry (C, 2)->Name, U16("big.img")), 0);
  /* 进入 scripts */
  select_name (C, U16("scripts"));
  key (C, 0, 0x0D);
  ASSERT_EQ (EfiStrCmp (FmCoreCwd (C), U16("scripts")), 0);
  ASSERT_EQ (EfiStrCmp (FmCoreEntry (C, 0)->Name, U16("sub")), 0);
  ASSERT_EQ (EfiStrCmp (FmCoreEntry (C, 1)->Name, U16("fpt.nsh")), 0);
  /* Backspace 上级 */
  key (C, 0, 0x08);
  ASSERT_EQ (FmCoreCwd (C)[0], 0);
  /* ESC 到卷列表 */
  key (C, SCAN_ESC, 0);
  ASSERT_EQ (FmCoreState (C), FM_STATE_VOLUMES);
  /* 选卷 0 */
  key (C, SCAN_UP, 0);
  key (C, 0, 0x0D);
  ASSERT_EQ (FmCoreVolIndex (C), 0);
  ASSERT_EQ (FmCoreState (C), FM_STATE_DIR);
  /* 视口钳制：30+ 文件，END/PGDN 越界不崩 */
  key (C, SCAN_END, 0);
  ASSERT_EQ (FmCoreSelected (C), FmCoreViewCount (C) - 1);
  key (C, SCAN_PGDN, 0);
  ASSERT_EQ (FmCoreSelected (C), FmCoreViewCount (C) - 1);
  key (C, SCAN_HOME, 0);
  ASSERT_EQ (FmCoreSelected (C), 0);
  key (C, SCAN_PGUP, 0);
  ASSERT_EQ (FmCoreSelected (C), 0);
  ASSERT_OK (FmCoreDraw (C));
  FmCoreFree (C);
  backend_free (g_fs0, g_root0);
  backend_free (g_fs1, g_root1);
}

/* ---------------- T12-2 过滤 ---------------- */
TEST (t12_2_filter) {
  FM_CORE *C = new_core ();
  UINTN all = FmCoreViewCount (C);
  ch (C, L'f');                                     /* 键入即过滤 */
  ASSERT_EQ (EfiStrCmp (FmCoreFilter (C), U16("f")), 0);
  ASSERT_EQ (FmCoreViewCount (C), 1);
  ASSERT_EQ (EfiStrCmp (FmCoreEntry (C, 0)->Name, U16("firmware.bin")), 0);
  ch (C, L'i');
  ASSERT_EQ (FmCoreViewCount (C), 1);
  key (C, 0, 0x08);                                 /* 退格编辑过滤 */
  ASSERT_EQ (EfiStrCmp (FmCoreFilter (C), U16("f")), 0);
  key (C, SCAN_ESC, 0);                             /* ESC 清除过滤（不上级） */
  ASSERT_EQ (FmCoreFilter (C)[0], 0);
  ASSERT_EQ (FmCoreViewCount (C), all);
  ASSERT_EQ (FmCoreState (C), FM_STATE_DIR);
  /* '/' 显式过滤 + 激活后 v/s/r 作为普通字符 */
  ch (C, L'/');
  ch (C, L'r');
  ASSERT_EQ (EfiStrCmp (FmCoreFilter (C), U16("r")), 0);
  ASSERT_TRUE (FmCoreViewCount (C) >= 2);           /* readme/scripts/firmware 含 r */
  ASSERT_EQ (FmCoreState (C), FM_STATE_DIR);        /* 'r' 未触发刷新/状态变化 */
  /* 过滤态 Enter 进入匹配目录 */
  key (C, SCAN_ESC, 0);
  ch (C, L'/');
  type_str (C, "scripts");
  ASSERT_EQ (FmCoreViewCount (C), 1);
  key (C, 0, 0x0D);
  ASSERT_EQ (EfiStrCmp (FmCoreCwd (C), U16("scripts")), 0);
  ASSERT_EQ (FmCoreFilter (C)[0], 0);               /* 进入目录清过滤 */
  FmCoreFree (C);
  backend_free (g_fs0, g_root0);
  backend_free (g_fs1, g_root1);
}

/* ---------------- T12-3 递归搜索命中与跳转 ---------------- */
TEST (t12_3_search) {
  FM_CORE *C = new_core ();
  ch (C, L's');
  ASSERT_EQ (FmCoreState (C), FM_STATE_INPUT);
  ASSERT_EQ (FmCoreInputKind (C), FM_INPUT_SEARCH);
  type_str (C, "deep");
  key (C, 0, 0x0D);
  ASSERT_EQ (FmCoreState (C), FM_STATE_RESULTS);
  ASSERT_EQ (FmCoreResultCount (C), 1);
  ASSERT_OK (FmCoreDraw (C));
  ASSERT_TRUE (screen_has (U16("scripts/sub/deep.nsh")));
  key (C, 0, 0x0D);                           /* 跳转 */
  ASSERT_EQ (FmCoreState (C), FM_STATE_DIR);
  ASSERT_EQ (EfiStrCmp (FmCoreCwd (C), U16("scripts/sub")), 0);
  ASSERT_EQ (EfiStrCmp (FmCoreEntry (C, FmCoreSelected (C))->Name, U16("deep.nsh")), 0);
  /* 无命中 → 提示 */
  key (C, SCAN_ESC, 0);                             /* sub → scripts */
  key (C, SCAN_ESC, 0);                             /* scripts → root */
  ch (C, L's');
  type_str (C, "zzz_no_match");
  key (C, 0, 0x0D);
  ASSERT_EQ (FmCoreState (C), FM_STATE_MESSAGE);
  FmCoreFree (C);
  backend_free (g_fs0, g_root0);
  backend_free (g_fs1, g_root1);
}

/* ---------------- T12-4 预览 bounded（1MB 只读 16KiB） ---------------- */
TEST (t12_4_preview_bounded) {
  FM_CORE *C;
  g_readbytes = 0;
  C = new_core ();
  g_countinner = g_vols[1].Fs;
  g_countops.Open = count_open;
  g_countops.Read = count_read;
  g_countops.Close = count_close;
  g_countops.Enum = count_enum;
  g_countops.Info = count_info;
  g_vols[1].Fs = &g_countops;
  /* 重新加载当前目录以走包装 vtable（触发一次导航） */
  key (C, SCAN_DOWN, 0);
  key (C, SCAN_UP, 0);
  select_name (C, U16("big.img"));
  ASSERT_EQ (FmCorePreviewLen (C), 16384);
  ASSERT_EQ (g_readbytes, 16384);                   /* 断言读取量 */
  ASSERT_TRUE (FmCorePreviewBinary (C));
  g_vols[1].Fs = g_countinner;
  FmCoreFree (C);
  backend_free (g_fs0, g_root0);
  backend_free (g_fs1, g_root1);
}

/* ---------------- T12-5 二进制 hex 头渲染 ---------------- */
TEST (t12_5_hex_preview) {
  FM_CORE *C = new_core ();
  select_name (C, U16("firmware.bin"));
  ASSERT_TRUE (FmCorePreviewBinary (C));
  ASSERT_OK (FmCoreDraw (C));
  ASSERT_TRUE (screen_has (U16("[binary]")));
  ASSERT_TRUE (screen_has (U16("00000000")));
  ASSERT_TRUE (screen_has (U16("4d 5a 00")));       /* MZ + NUL 的 hex */
  key (C, SCAN_F3, 0);                              /* 文本/hex 切换 */
  ASSERT_TRUE (FmCorePreviewHex (C));
  ASSERT_OK (FmCoreDraw (C));
  ASSERT_TRUE (screen_has (U16("F3:text")));
  FmCoreFree (C);
  backend_free (g_fs0, g_root0);
  backend_free (g_fs1, g_root1);
}

/* ---------------- T12-6 执行确认含全路径+大小 ---------------- */
TEST (t12_6_exec_confirm) {
  FM_CORE *C = new_core ();
  select_name (C, U16("scripts"));
  key (C, 0, 0x0D);
  select_name (C, U16("fpt.nsh"));
  key (C, 0, 0x0D);
  ASSERT_EQ (FmCoreState (C), FM_STATE_CONFIRM);
  ASSERT_EQ (FmCoreConfirmKind (C), FM_CONFIRM_EXEC);
  ASSERT_OK (FmCoreDraw (C));
  ASSERT_TRUE (screen_has (U16("fs1:/scripts/fpt.nsh")));
  ASSERT_TRUE (screen_has (U16("37 bytes")));
  ASSERT_TRUE (screen_has (U16("default: N")));
  ASSERT_EQ (MockExecCount (), 0);                  /* 确认前不执行 */
  ch (C, L'n');                                     /* N 取消 */
  ASSERT_EQ (FmCoreState (C), FM_STATE_DIR);
  ASSERT_EQ (MockExecCount (), 0);
  key (C, 0, 0x0D);
  ch (C, L'y');                                     /* Y 执行 .nsh → 嵌套 Shell */
  ASSERT_EQ (MockExecCount (), 1);
  ASSERT_EQ (MockExecCall (0)->Kind, 2);
  ASSERT_EQ (EfiStrCmp (MockExecCall (0)->Path, U16("fs1:/scripts/fpt.nsh")), 0);
  /* .efi 执行 */
  key (C, SCAN_ESC, 0);                             /* scripts → root */
  key (C, SCAN_ESC, 0);                             /* root → volumes */
  key (C, SCAN_UP, 0);
  key (C, 0, 0x0D);                           /* vol0 */
  select_name (C, U16("EFI"));
  key (C, 0, 0x0D);
  select_name (C, U16("Apps"));
  key (C, 0, 0x0D);
  select_name (C, U16("Editor.efi"));
  key (C, 0, 0x0D);
  ch (C, L'y');
  ASSERT_EQ (MockExecCount (), 2);
  ASSERT_EQ (MockExecCall (1)->Kind, 1);
  ASSERT_EQ (EfiStrCmp (MockExecCall (1)->Path, U16("fs0:/EFI/Apps/Editor.efi")), 0);
  FmCoreFree (C);
  backend_free (g_fs0, g_root0);
  backend_free (g_fs1, g_root1);
}

/* ---------------- T12-7 复制/覆盖/改名/删除/mkdir/新建 ---------------- */
TEST (t12_7_file_ops) {
  FM_CORE *C = new_core ();
  FS_ENTRY info;
  /* 复制 readme.txt → vol0 根 */
  select_name (C, U16("readme.txt"));
  key (C, SCAN_F5, 0);
  ASSERT_EQ (FmCoreState (C), FM_STATE_VOLUMES);
  key (C, SCAN_UP, 0);                              /* 选 vol0 */
  key (C, 0, 0x0D);
  ASSERT_EQ (FmCoreState (C), FM_STATE_MESSAGE);    /* 复制完成提示 */
  ASSERT_OK (g_fs0->Info (g_fs0, U16("readme.txt"), &info));
  ASSERT_EQ (info.Size, 13);
  ch (C, L' ');                                     /* 任意键回 DIR */
  ASSERT_EQ (FmCoreState (C), FM_STATE_DIR);
  /* 覆盖确认：源改短 → 覆盖后目标必须截断 */
  seed_file (g_fs1, U16("readme.txt"), (CONST UINT8 *)"short", 5);
  select_name (C, U16("readme.txt"));               /* 回 DIR 后 Sel 在卷选择器位置，须重选 */
  key (C, SCAN_F5, 0);
  key (C, SCAN_UP, 0);
  key (C, 0, 0x0D);
  ASSERT_EQ (FmCoreState (C), FM_STATE_CONFIRM);
  ASSERT_EQ (FmCoreConfirmKind (C), FM_CONFIRM_OVERWRITE);
  ASSERT_OK (FmCoreDraw (C));
  ASSERT_TRUE (screen_has (U16("fs0:/readme.txt")));
  ch (C, L'n');                                     /* 取消 → 目标不变 */
  ASSERT_OK (g_fs0->Info (g_fs0, U16("readme.txt"), &info));
  ASSERT_EQ (info.Size, 13);
  key (C, SCAN_F5, 0);
  key (C, SCAN_UP, 0);
  key (C, 0, 0x0D);
  ch (C, L'y');                                     /* 确认覆盖 */
  ASSERT_OK (g_fs0->Info (g_fs0, U16("readme.txt"), &info));
  ASSERT_EQ (info.Size, 5);                         /* 截断语义 */
  ch (C, L' ');
  /* 改名 */
  select_name (C, U16("readme.txt"));
  key (C, SCAN_F6, 0);
  ASSERT_EQ (FmCoreState (C), FM_STATE_INPUT);
  ASSERT_EQ (FmCoreInputKind (C), FM_INPUT_RENAME);
  { UINTN i; for (i = 0; i < 10; i++) key (C, 0, 0x08); }
  type_str (C, "readme2.txt");
  key (C, 0, 0x0D);
  ASSERT_EQ (FmCoreState (C), FM_STATE_DIR);
  ASSERT_TRUE (find_view (C, U16("readme2.txt")) != (UINTN)-1);
  ASSERT_TRUE (find_view (C, U16("readme.txt")) == (UINTN)-1);
  /* 删除文件（确认） */
  select_name (C, U16("readme2.txt"));
  key (C, SCAN_F8, 0);
  ASSERT_EQ (FmCoreConfirmKind (C), FM_CONFIRM_DELETE);
  ch (C, L'y');
  ASSERT_TRUE (find_view (C, U16("readme2.txt")) == (UINTN)-1);
  /* mkdir（F7 + Tab 切 DIR） */
  key (C, SCAN_F7, 0);
  key (C, 0, 0x09);
  type_str (C, "newdir");
  key (C, 0, 0x0D);
  ASSERT_OK (g_fs1->Info (g_fs1, U16("newdir"), &info));
  ASSERT_TRUE (info.IsDir);
  /* 新建空文件 */
  key (C, SCAN_F7, 0);
  type_str (C, "newfile.txt");
  key (C, 0, 0x0D);
  ASSERT_OK (g_fs1->Info (g_fs1, U16("newfile.txt"), &info));
  ASSERT_EQ (info.Size, 0);
  /* 空目录删除 */
  select_name (C, U16("newdir"));
  key (C, SCAN_F8, 0);
  ch (C, L'y');
  ASSERT_TRUE (find_view (C, U16("newdir")) == (UINTN)-1);
  /* 非空目录拒绝 */
  select_name (C, U16("scripts"));
  key (C, SCAN_F8, 0);
  ch (C, L'y');
  ASSERT_EQ (FmCoreState (C), FM_STATE_MESSAGE);
  ASSERT_TRUE (find_view (C, U16("scripts")) != (UINTN)-1);
  FmCoreFree (C);
  backend_free (g_fs0, g_root0);
  backend_free (g_fs1, g_root1);
}

/* ---------------- T12-8 F4 文本→Editor 参数 / 二进制拒绝 ---------------- */
TEST (t12_8_f4_edit) {
  FM_CORE *C = new_core ();
  select_name (C, U16("scripts"));
  key (C, 0, 0x0D);
  select_name (C, U16("fpt.nsh"));
  key (C, SCAN_F4, 0);
  ASSERT_EQ (MockExecCount (), 1);
  ASSERT_EQ (MockExecCall (0)->Kind, 1);
  ASSERT_EQ (EfiStrCmp (MockExecCall (0)->Path, U16("fs0:/EFI/Apps/Editor.efi")), 0);
  ASSERT_EQ (EfiStrCmp (MockExecCall (0)->Options, U16("fs1:/scripts/fpt.nsh")), 0);
  /* 二进制拒绝 */
  key (C, SCAN_ESC, 0);
  select_name (C, U16("firmware.bin"));
  key (C, SCAN_F4, 0);
  ASSERT_EQ (FmCoreState (C), FM_STATE_MESSAGE);
  ASSERT_EQ (MockExecCount (), 1);                  /* 未新增 exec */
  ASSERT_OK (FmCoreDraw (C));
  ASSERT_TRUE (screen_has (U16("Binary")));
  FmCoreFree (C);
  backend_free (g_fs0, g_root0);
  backend_free (g_fs1, g_root1);
}

/* ---------------- T12-9 卷切换（每卷 cwd 保持） ---------------- */
TEST (t12_9_volume_switch) {
  FM_CORE *C = new_core ();
  select_name (C, U16("scripts"));
  key (C, 0, 0x0D);                           /* vol1 cwd=scripts */
  ch (C, L'v');
  ASSERT_EQ (FmCoreState (C), FM_STATE_VOLUMES);
  key (C, SCAN_UP, 0);
  key (C, 0, 0x0D);                           /* → vol0 */
  ASSERT_EQ (FmCoreVolIndex (C), 0);
  ASSERT_EQ (FmCoreCwd (C)[0], 0);
  ch (C, L'v');
  key (C, SCAN_DOWN, 0);
  key (C, 0, 0x0D);                           /* → vol1 */
  ASSERT_EQ (FmCoreVolIndex (C), 1);
  ASSERT_EQ (EfiStrCmp (FmCoreCwd (C), U16("scripts")), 0);
  FmCoreFree (C);
  backend_free (g_fs0, g_root0);
  backend_free (g_fs1, g_root1);
}

/* ---------------- T12-10 80x25 布局快照 ---------------- */
TEST (t12_10_layout) {
  FM_CORE *C = new_core ();
  CHAR16 row[256];
  ASSERT_OK (FmCoreDraw (C));
  /* 标题行：应用名 + 卷标签 + P3 */
  MockTuiRowText (0, row, 256);
  ASSERT_TRUE (screen_has (U16("EasyEFI FileManager")));
  ASSERT_TRUE (screen_has (U16("TOOLKIT")));
  ASSERT_TRUE (screen_has (U16("P3")));
  /* 状态栏（row 23）= 当前全路径（行尾空格填充，比前缀） */
  MockTuiRowText (23, row, 256);
  ASSERT_EQ (EfiStrnCmp (row, U16("fs1:/"), 5), 0);
  /* 键位条（row 24）常驻 */
  MockTuiRowText (24, row, 256);
  ASSERT_TRUE (EfiStrChr (row, L'F') != NULL);
  ASSERT_TRUE (screen_has (U16("F10:quit")));
  /* 列表首行 = emptydir/（目录优先） */
  MockTuiRowText (1, row, 256);
  ASSERT_TRUE (EfiStrnCmp (row, U16("emptydir/"), 9) == 0);
  /* 预览分隔列 44 = '|'，预览头含选中项名 */
  ASSERT_EQ (MockTuiGetCell (1, 44).Char, L'|');
  ASSERT_TRUE (screen_has (U16("emptydir")));
  /* 选中行高亮属性 */
  ASSERT_EQ (MockTuiGetCell (1, 0).Attr, TUI_ATTR_SELECTED);
  ASSERT_EQ (MockTuiGetCell (2, 0).Attr, TUI_ATTR_NORMAL);
  /* 状态栏/键位条属性 */
  ASSERT_EQ (MockTuiGetCell (23, 0).Attr, TUI_ATTR_STATUSBAR);
  ASSERT_EQ (MockTuiGetCell (24, 0).Attr, TUI_ATTR_KEYBAR);
  FmCoreFree (C);
  backend_free (g_fs0, g_root0);
  backend_free (g_fs1, g_root1);
}

/* ---------------- T12-11 刷新后外部新增可见 + Rescan 卷数变化 ---------------- */
static EFI_STATUS rescan_add_vol (VOID *Ctx) {
  FS_OPS *fs2 = backend_new (g_root2);
  seed_file (fs2, U16("extra.txt"), (CONST UINT8 *)"e", 1);
  str_copy_label (g_vols[2].Label, U16("USBSTICK"));
  str_copy_label (g_vols[2].FsName, U16("FAT32"));
  g_vols[2].Capacity = 1024 * 1024;
  g_vols[2].Free = 1024;
  g_vols[2].IsP3 = FALSE;
  g_vols[2].Dp = NULL; g_vols[2].DpLen = 0;
  g_vols[2].Fs = fs2;
  str_copy_label (g_fsname_buf[2], U16("fs2:"));
  g_fsnames[2] = g_fsname_buf[2];
  g_init.VolCount = 3;                              /* glue 原地更新借用结构 */
  return EFI_SUCCESS;
}

TEST (t12_11_refresh) {
  FM_CORE *C = new_core ();
  ASSERT_TRUE (find_view (C, U16("added.txt")) == (UINTN)-1);
  seed_file (g_fs1, U16("added.txt"), (CONST UINT8 *)"new", 3);
  ch (C, L'r');                                     /* 刷新重读 */
  ASSERT_TRUE (find_view (C, U16("added.txt")) != (UINTN)-1);
  /* Rescan 回调：卷数 2→3，cwd 数组同步不越界 */
  g_ops.Ctx = NULL;
  g_ops.Rescan = rescan_add_vol;
  g_init.Ops = &g_ops;
  ch (C, L'v');
  ch (C, L'r');
  ASSERT_EQ (g_init.VolCount, 3);
  ASSERT_OK (FmCoreDraw (C));
  ASSERT_TRUE (screen_has (U16("USBSTICK")));
  key (C, SCAN_END, 0);
  key (C, 0, 0x0D);                           /* 进新卷 */
  ASSERT_EQ (FmCoreVolIndex (C), 2);
  ASSERT_TRUE (find_view (C, U16("extra.txt")) != (UINTN)-1);
  /* F10 退出 */
  key (C, SCAN_F10, 0);
  ASSERT_TRUE (FmCoreExit (C));
  FmCoreFree (C);
  backend_free (g_fs0, g_root0);
  backend_free (g_fs1, g_root1);
  backend_free (g_vols[2].Fs, g_root2);
}

int main (void) {
  setvbuf (stdout, NULL, _IONBF, 0);
  RUN_TEST (t12_1_navigation);
  RUN_TEST (t12_2_filter);
  RUN_TEST (t12_3_search);
  RUN_TEST (t12_4_preview_bounded);
  RUN_TEST (t12_5_hex_preview);
  RUN_TEST (t12_6_exec_confirm);
  RUN_TEST (t12_7_file_ops);
  RUN_TEST (t12_8_f4_edit);
  RUN_TEST (t12_9_volume_switch);
  RUN_TEST (t12_10_layout);
  RUN_TEST (t12_11_refresh);
  RUN_TESTS_END ();
}
