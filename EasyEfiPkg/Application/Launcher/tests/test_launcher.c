/* S11 Launcher core 宿主测试 T11-1..10（任务书 §3 S11 编号用例）+ 附加覆盖。
 * 运行（模块目录内）: make -f host.mk test
 * core 只依赖契约 Ops：MockTui（屏幕/按键）+ MockExec（执行记录）+ MockFs（卷占位）
 * + S04 真实 ExecPath.c。CONFIG 夹具直接构造（core 借用，不链接 S03 符号）。
 * 纪律：CHAR16 字面量一律 U16()；U16() 宿主旋转缓冲即取即用，长期存储拷入静态缓冲。
 */
#include "test.h"
#include "LauncherCore.h"
#include "MockTui.h"
#include "MockExec.h"
#include "MockFs.h"

/* ---- 夹具 ---- */

static CHAR16 g_fmRel[64], g_edRel[64], g_shRel[64];
static LAUNCHER_SESSION g_sess;
static FS_OPS *g_p3fs = NULL, *g_bootfs = NULL;

static void u16cpy (CHAR16 *Dst, UINTN Cap, CONST CHAR16 *Src) {
  UINTN i;
  for (i = 0; Src[i] != 0 && i + 1 < Cap; i++) {
    Dst[i] = Src[i];
  }
  Dst[i] = 0;
}

static void FixtureInit (VOID) {
  MockTuiReset ();
  MockTuiSetSize (25, 80);
  MockExecReset ();
  memset (&g_sess, 0, sizeof (g_sess));
  u16cpy (g_fmRel, 64, U16("EFI/Apps/FileManager.efi"));
  u16cpy (g_edRel, 64, U16("EFI/Apps/Editor.efi"));
  u16cpy (g_shRel, 64, U16("EFI/BOOT/BOOTX64.EFI"));
  u16cpy (g_sess.P3FsName, 16, U16("fs2:"));
  u16cpy (g_sess.BootFsName, 16, U16("fs0:"));
  if (g_p3fs == NULL) {
    g_p3fs = MockFsNew ();
  }
  if (g_bootfs == NULL) {
    g_bootfs = MockFsNew ();
  }
}

static void FixtureFini (VOID) {
  if (g_p3fs != NULL) { g_p3fs->Destroy (g_p3fs); g_p3fs = NULL; }
  if (g_bootfs != NULL) { g_bootfs->Destroy (g_bootfs); g_bootfs = NULL; }
}

static CONFIG *CfgNew (CONST CHAR8 *Title, CONST CHAR8 *Ver, UINTN n) {
  CONFIG *c = MemAlloc (sizeof (CONFIG));
  memcpy (c->Title, Title, strlen (Title) + 1);
  memcpy (c->Version, Ver, strlen (Ver) + 1);
  c->SkipTimeout = 0;
  c->Count = n;
  c->Items = n > 0 ? MemAlloc (n * sizeof (CONFIG_ITEM)) : NULL;
  return c;
}

static void CfgFree (CONFIG *c) {
  if (c != NULL) {
    MemFree (c->Items);
    MemFree (c);
  }
}

static void ItemSet (CONFIG_ITEM *It, CONST CHAR8 *Id, CONST CHAR8 *Name, CONST CHAR8 *Desc,
                     CONST CHAR8 *Path, UINT8 Action, UINT8 Arch, BOOLEAN Danger, BOOLEAN Confirm,
                     CONST CHAR8 *Opts) {
  memset (It, 0, sizeof (*It));
  strcpy (It->Id, Id);
  strcpy (It->Name, Name);
  strcpy (It->Desc, Desc);
  strcpy (It->Path, Path);
  strcpy (It->Options, Opts);
  It->Action  = Action;
  It->ArchMask = Arch;
  It->Danger  = Danger;
  It->Confirm = Confirm;
}

static LAUNCHER_CORE *CoreNew (CONFIG *Cfg, BOOLEAN P3Ok, UINTN Timeout, LAUNCHER_PLATFORM_OPS *Ops) {
  LAUNCHER_INIT Init;
  g_sess.Cfg  = Cfg;
  g_sess.P3Fs = P3Ok ? g_p3fs : NULL;
  g_sess.BootFs = g_bootfs;
  memset (&Init, 0, sizeof (Init));
  Init.Sess           = &g_sess;
  Init.CurrentArch    = ARCH_X64_BIT;
  Init.SbStatus       = "SB:OFF";
  Init.FileManagerRel = g_fmRel;
  Init.EditorRel      = g_edRel;
  Init.ShellRel       = g_shRel;
  Init.Ops            = Ops;
  Init.SkipTimeout    = Timeout;
  return LauncherCoreNew (&Init);
}

static void Key (LAUNCHER_CORE *C, UINT16 Scan, CHAR16 Uni) {
  TUI_KEY k;
  k.ScanCode    = Scan;
  k.UnicodeChar = Uni;
  ASSERT_OK (LauncherCoreHandleKey (C, &k));
}

static void Chr (LAUNCHER_CORE *C, CHAR16 Ch) {
  Key (C, 0, Ch);
}

static void RowTextIs (UINTN Row, UINTN Col, CONST CHAR16 *Expect) {
  CHAR16 Buf[200];
  UINTN  n;
  MockTuiRowText (Row, Buf, 200);
  n = EfiStrLen (Expect);
  ASSERT_EQ (EfiStrnCmp (&Buf[Col], Expect, n), 0);   /* 前缀逐字相等 */
  ASSERT_TRUE (Buf[Col + n] == 0 || Buf[Col + n] == ' ');  /* 文本结束（清屏行 NUL / 填充行空格） */
}

static void CellAttrIs (UINTN Row, UINTN Col, UINT8 Attr) {
  ASSERT_EQ (MockTuiGetCell (Row, Col).Attr, Attr);
}

/* ---- T11-1 正常配置菜单渲染黄金快照 ---- */
TEST (t11_1_menu_golden) {
  CONFIG *Cfg;
  LAUNCHER_CORE *C;
  FixtureInit ();
  Cfg = CfgNew ("EasyEFI Work Toolkit", "1.0.0", 2);
  ItemSet (&Cfg->Items[0], "winpe", "Local WinPE", "Boot local WinPE image",
           "data:/entries/winpe.efi", ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "");
  ItemSet (&Cfg->Items[1], "fpt", "Intel FPT flash tool", "HIGH RISK: writes SPI flash",
           "data:/scripts/fpt.nsh", ACTION_EXEC, ARCH_X64_BIT, TRUE, FALSE, "");
  C = CoreNew (Cfg, TRUE, 0, NULL);
  ASSERT_TRUE (C != NULL);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  ASSERT_OK (LauncherCoreDraw (C));
  RowTextIs (0, 0, U16("EasyEFI v1.0.0 | x64 | SB:OFF | P3:OK | 80x25"));
  RowTextIs (1, 0, U16("EasyEFI Work Toolkit"));
  ASSERT_EQ (MockTuiGetCell (2, 0).Char, 0);          /* 无横幅 */
  RowTextIs (3, 1, U16("1. Local WinPE"));
  CellAttrIs (3, 1, TUI_ATTR_SELECTED);
  CellAttrIs (3, 79, TUI_ATTR_SELECTED);              /* 选中整行高亮 */
  RowTextIs (4, 1, U16("2. Intel FPT flash tool !"));
  CellAttrIs (4, 1, TUI_ATTR_DANGER);
  RowTextIs (5, 1, U16("3. FileManager"));
  RowTextIs (6, 1, U16("4. Editor"));
  RowTextIs (7, 1, U16("5. NVRAM boot menu"));
  RowTextIs (8, 1, U16("6. Return to UEFI Shell"));
  RowTextIs (9, 1, U16("7. Reboot"));
  RowTextIs (23, 1, U16("Boot local WinPE image"));   /* 选中行 desc 展开 */
  RowTextIs (24, 0, U16("Up/Dn/1-9:Select Enter:Run S:Shell R:Reload ?:Help"));
  LauncherCoreFree (C);
  CfgFree (Cfg);
  FixtureFini ();
}

/* ---- T11-2 选择+Enter → ExecOps 调用参数断言 ---- */
TEST (t11_2_exec_params) {
  CONFIG *Cfg;
  LAUNCHER_CORE *C;
  FixtureInit ();
  Cfg = CfgNew ("T", "1.0", 1);
  ItemSet (&Cfg->Items[0], "winpe", "WinPE", "", "data:/entries/winpe.efi",
           ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "arg1 arg2");
  C = CoreNew (Cfg, TRUE, 0, NULL);
  Key (C, 0, 0x0D);                                   /* Enter */
  ASSERT_EQ (MockExecCount (), 1);
  ASSERT_EQ (MockExecCall (0)->Kind, 1);              /* ExecEfi */
  ASSERT_EQ (EfiStrCmp (MockExecCall (0)->Path, U16("fs2:/entries/winpe.efi")), 0);
  ASSERT_EQ (EfiStrCmp (MockExecCall (0)->Options, U16("arg1 arg2")), 0);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);     /* 正常返回 → 回菜单 */
  ASSERT_EQ (LauncherCoreExit (C), LEXIT_NONE);
  LauncherCoreFree (C);
  CfgFree (Cfg);
  FixtureFini ();
}

/* ---- T11-3 danger → 确认 N 取消 / Y 执行 ---- */
TEST (t11_3_danger_confirm) {
  CONFIG *Cfg;
  LAUNCHER_CORE *C;
  FixtureInit ();
  Cfg = CfgNew ("T", "1.0", 1);
  ItemSet (&Cfg->Items[0], "flash", "Flash tool", "", "data:/tools/flash.efi",
           ACTION_EXEC, ARCH_X64_BIT, TRUE, FALSE, "");
  C = CoreNew (Cfg, TRUE, 0, NULL);
  Key (C, 0, 0x0D);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_CONFIRM);
  ASSERT_EQ (MockExecCount (), 0);                    /* 未执行 */
  ASSERT_OK (LauncherCoreDraw (C));
  RowTextIs (4, 2, U16("Flash tool"));
  RowTextIs (6, 2, U16("This action is marked DANGEROUS."));
  Chr (C, 'n');                                       /* N 取消 */
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  ASSERT_EQ (MockExecCount (), 0);
  Key (C, 0, 0x0D);                                   /* 再入确认 */
  ASSERT_EQ (LauncherCoreState (C), LSTATE_CONFIRM);
  Chr (C, 'y');                                       /* Y 执行 */
  ASSERT_EQ (MockExecCount (), 1);
  ASSERT_EQ (MockExecCall (0)->Kind, 1);
  ASSERT_EQ (EfiStrCmp (MockExecCall (0)->Path, U16("fs2:/tools/flash.efi")), 0);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  /* ESC 取消 + Enter 默认 N */
  Key (C, 0, 0x0D);
  Key (C, SCAN_ESC, 0);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  ASSERT_EQ (MockExecCount (), 1);
  Key (C, 0, 0x0D);
  Key (C, 0, 0x0D);                                   /* 确认态 Enter = 默认 N */
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  ASSERT_EQ (MockExecCount (), 1);
  LauncherCoreFree (C);
  CfgFree (Cfg);
  FixtureFini ();
}

/* ---- T11-4 P3 缺失 → DEGRADED 横幅 + data: 灰显 + 系统项可用 ---- */
TEST (t11_4_degraded) {
  CONFIG *Cfg;
  LAUNCHER_CORE *C;
  FixtureInit ();
  Cfg = CfgNew ("T", "1.0", 2);
  ItemSet (&Cfg->Items[0], "winpe", "WinPE", "", "data:/entries/winpe.efi",
           ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "");
  ItemSet (&Cfg->Items[1], "usb", "USB boot", "", "auto:/EFI/BOOT/BOOTX64.EFI",
           ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "");
  C = CoreNew (Cfg, FALSE, 0, NULL);                  /* P3 缺失 */
  ASSERT_OK (LauncherCoreDraw (C));
  RowTextIs (0, 0, U16("EasyEFI v1.0 | x64 | SB:OFF | P3:DEGRADED | 80x25"));
  RowTextIs (2, 0, U16("DATA VOLUME UNAVAILABLE - NTFS driver failed or P3 missing"));
  CellAttrIs (3, 1, TUI_ATTR_DISABLED);               /* data: 灰显 */
  RowTextIs (3, 1, U16("1. WinPE [P3 unavailable]")); /* 灰显原因标注 */
  ASSERT_EQ (LauncherCoreSelected (C), 1);            /* 导航跳过灰显 → auto: 项 */
  CellAttrIs (4, 1, TUI_ATTR_SELECTED);
  RowTextIs (23, 1, U16("usb"));                      /* 选中行（auto: 项）detail = Id */
  Chr (C, '3');                                       /* 数字直选 FileManager（行 idx2） */
  ASSERT_EQ (LauncherCoreSelected (C), 2);
  Key (C, 0, 0x0D);
  ASSERT_EQ (MockExecCount (), 1);                    /* 系统项可用 */
  ASSERT_EQ (EfiStrCmp (MockExecCall (0)->Path, U16("fs0:/EFI/Apps/FileManager.efi")), 0);
  LauncherCoreFree (C);
  CfgFree (Cfg);
  FixtureFini ();
}

/* ---- T11-5 坏 JSON → 内置菜单 + 错误横幅（core 层：cfg=NULL+Banner 输入） ---- */
TEST (t11_5_bad_json_builtin) {
  LAUNCHER_CORE *C;
  FixtureInit ();
  strcpy (g_sess.Banner, "launcher.json: parse error at line 3");
  C = CoreNew (NULL, FALSE, 0, NULL);
  ASSERT_OK (LauncherCoreDraw (C));
  RowTextIs (2, 0, U16("launcher.json: parse error at line 3"));
  RowTextIs (1, 0, U16("EasyEFI Launcher"));          /* 默认标题 */
  RowTextIs (3, 1, U16("1. External media: EFI/BOOT/BOOTX64.EFI"));
  RowTextIs (4, 1, U16("2. FileManager"));
  RowTextIs (5, 1, U16("3. Editor"));
  RowTextIs (6, 1, U16("4. NVRAM boot menu"));
  RowTextIs (7, 1, U16("5. Return to UEFI Shell"));
  RowTextIs (8, 1, U16("6. Reboot"));
  Chr (C, '5');                                       /* Shell 系统项 */
  Key (C, 0, 0x0D);
  ASSERT_EQ (LauncherCoreExit (C), LEXIT_SHELL);
  LauncherCoreFree (C);
  FixtureFini ();
}

/* ---- T11-6 倒计时 ESC → 退出；任意键跳过；归零进菜单 ---- */
TEST (t11_6_countdown) {
  CONFIG *Cfg;
  LAUNCHER_CORE *C;
  FixtureInit ();
  Cfg = CfgNew ("T", "1.0", 0);
  C = CoreNew (Cfg, TRUE, 5, NULL);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_COUNTDOWN);
  ASSERT_EQ (LauncherCoreCountdown (C), 5);
  ASSERT_OK (LauncherCoreDraw (C));
  RowTextIs (4, 2, U16("Auto-start in 5 s"));
  ASSERT_OK (LauncherCoreTick (C));
  ASSERT_EQ (LauncherCoreCountdown (C), 4);
  ASSERT_OK (LauncherCoreDraw (C));
  RowTextIs (4, 2, U16("Auto-start in 4 s"));
  Key (C, SCAN_ESC, 0);
  ASSERT_EQ (LauncherCoreExit (C), LEXIT_SHELL);
  LauncherCoreFree (C);
  /* 任意其他键跳过倒计时 */
  C = CoreNew (Cfg, TRUE, 3, NULL);
  Chr (C, 'x');
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  ASSERT_EQ (LauncherCoreExit (C), LEXIT_NONE);
  LauncherCoreFree (C);
  /* 归零自动进菜单 */
  C = CoreNew (Cfg, TRUE, 2, NULL);
  ASSERT_OK (LauncherCoreTick (C));
  ASSERT_EQ (LauncherCoreState (C), LSTATE_COUNTDOWN);
  ASSERT_OK (LauncherCoreTick (C));
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  LauncherCoreFree (C);
  CfgFree (Cfg);
  FixtureFini ();
}

/* ---- T11-7 arch 不匹配灰显不可选 ---- */
TEST (t11_7_arch_disabled) {
  CONFIG *Cfg;
  LAUNCHER_CORE *C;
  FixtureInit ();
  Cfg = CfgNew ("T", "1.0", 1);
  ItemSet (&Cfg->Items[0], "arm", "ARM only tool", "", "data:/tools/arm.efi",
           ACTION_EXEC, ARCH_AARCH64_BIT, FALSE, FALSE, "");
  C = CoreNew (Cfg, TRUE, 0, NULL);                   /* 当前 x64 */
  ASSERT_OK (LauncherCoreDraw (C));
  CellAttrIs (3, 1, TUI_ATTR_DISABLED);
  RowTextIs (3, 1, U16("1. ARM only tool [arch mismatch]"));
  ASSERT_EQ (LauncherCoreSelected (C), 1);            /* 跳过灰显 → FileManager */
  RowTextIs (23, 1, U16("Browse volumes and execute firmware tools"));
  Chr (C, '1');                                       /* 数字直选灰显项 → 忽略 */
  ASSERT_EQ (LauncherCoreSelected (C), 1);
  Key (C, SCAN_UP, 0);                                /* 上 nav 无可用行 → 钳制 */
  ASSERT_EQ (LauncherCoreSelected (C), 1);
  Key (C, 0, 0x0D);                                   /* Enter 执行的是 FileManager */
  ASSERT_EQ (MockExecCount (), 1);
  ASSERT_EQ (EfiStrCmp (MockExecCall (0)->Path, U16("fs0:/EFI/Apps/FileManager.efi")), 0);
  LauncherCoreFree (C);
  CfgFree (Cfg);
  FixtureFini ();
}

/* ---- T11-8 R 重载配置 ---- */
static CONFIG *g_reloadNewCfg = NULL;
static UINTN g_reloadCalls = 0;
static EFI_STATUS ReloadMock (VOID *Ctx) {
  CONFIG *Old = (CONFIG *)g_sess.Cfg;
  g_reloadCalls++;
  g_sess.Cfg = g_reloadNewCfg;                        /* glue 语义：原地换配置 */
  CfgFree (Old);
  return EFI_SUCCESS;
}

TEST (t11_8_reload) {
  CONFIG *CfgOld;
  LAUNCHER_CORE *C;
  LAUNCHER_PLATFORM_OPS Ops;
  FixtureInit ();
  CfgOld = CfgNew ("Old Title", "1.0", 1);
  ItemSet (&CfgOld->Items[0], "a", "Old item", "", "data:/a.efi",
           ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "");
  g_reloadNewCfg = CfgNew ("New Title", "2.0", 2);
  ItemSet (&g_reloadNewCfg->Items[0], "b", "New item B", "", "data:/b.efi",
           ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "");
  ItemSet (&g_reloadNewCfg->Items[1], "c", "New item C", "", "data:/c.efi",
           ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "");
  memset (&Ops, 0, sizeof (Ops));
  Ops.Reload = ReloadMock;
  g_reloadCalls = 0;
  C = CoreNew (CfgOld, TRUE, 0, &Ops);
  Chr (C, 'r');
  ASSERT_EQ (g_reloadCalls, 1);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  ASSERT_OK (LauncherCoreDraw (C));
  RowTextIs (1, 0, U16("New Title"));
  RowTextIs (0, 0, U16("EasyEFI v2.0 | x64 | SB:OFF | P3:OK | 80x25"));
  RowTextIs (3, 1, U16("1. New item B"));
  RowTextIs (4, 1, U16("2. New item C"));
  RowTextIs (5, 1, U16("3. FileManager"));
  /* 重载后执行新条目 */
  Key (C, 0, 0x0D);
  ASSERT_EQ (MockExecCount (), 1);
  ASSERT_EQ (EfiStrCmp (MockExecCall (0)->Path, U16("fs2:/b.efi")), 0);
  LauncherCoreFree (C);
  CfgFree (g_reloadNewCfg);
  g_reloadNewCfg = NULL;
  FixtureFini ();
}

/* ---- T11-9 boot:menu 子菜单（mock BootOrder） ---- */
static EFI_STATUS BootEntriesMock (VOID *Ctx, LAUNCHER_BOOT_ENTRY **Out, UINTN *Count) {
  LAUNCHER_BOOT_ENTRY *L = MemAlloc (2 * sizeof (LAUNCHER_BOOT_ENTRY));
  L[0].BootNumber = 0x0001;
  u16cpy (L[0].Description, 128, U16("Windows Boot Manager"));
  L[1].BootNumber = 0x0002;
  u16cpy (L[1].Description, 128, U16("UEFI Shell"));
  *Out = L;
  *Count = 2;
  return EFI_SUCCESS;
}

TEST (t11_9_boot_submenu) {
  LAUNCHER_CORE *C;
  LAUNCHER_PLATFORM_OPS Ops;
  FixtureInit ();
  memset (&Ops, 0, sizeof (Ops));
  Ops.BootEntries = BootEntriesMock;
  C = CoreNew (NULL, TRUE, 0, &Ops);                  /* 内置菜单：bootmenu = idx3 */
  Chr (C, '4');
  ASSERT_EQ (LauncherCoreSelected (C), 3);
  Key (C, 0, 0x0D);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_BOOT_MENU);
  ASSERT_OK (LauncherCoreDraw (C));
  RowTextIs (1, 0, U16("NVRAM Boot Entries"));
  RowTextIs (3, 1, U16("0001 Windows Boot Manager"));
  RowTextIs (4, 1, U16("0002 UEFI Shell"));
  CellAttrIs (3, 1, TUI_ATTR_SELECTED);
  Key (C, SCAN_DOWN, 0);
  Key (C, 0, 0x0D);
  ASSERT_EQ (MockExecCount (), 1);
  ASSERT_EQ (MockExecCall (0)->Kind, 3);              /* ExecBootNext */
  ASSERT_EQ (MockExecCall (0)->BootNumber, 0x0002);
  ASSERT_EQ (LauncherCoreExit (C), LEXIT_RESET);
  LauncherCoreFree (C);
  /* ESC 返回路径（列表释放配对） */
  C = CoreNew (NULL, TRUE, 0, &Ops);
  Chr (C, '4');
  Key (C, 0, 0x0D);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_BOOT_MENU);
  Key (C, SCAN_ESC, 0);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  ASSERT_EQ (MockExecCount (), 1);                    /* 未新增执行 */
  LauncherCoreFree (C);
  FixtureFini ();
}

/* ---- T11-10 .nsh 条目 → ExecNsh 参数断言 ---- */
TEST (t11_10_nsh_params) {
  CONFIG *Cfg;
  LAUNCHER_CORE *C;
  static EXEC_CANDIDATE Cand;
  FixtureInit ();
  Cfg = CfgNew ("T", "1.0", 2);
  ItemSet (&Cfg->Items[0], "fpt", "FPT", "", "data:/scripts/fpt.nsh",
           ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "-x -y");
  ItemSet (&Cfg->Items[1], "tool", "Tool", "", "auto:/scripts/tool.nsh",
           ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "opt");
  C = CoreNew (Cfg, TRUE, 0, NULL);
  Key (C, 0, 0x0D);                                   /* data: .nsh */
  ASSERT_EQ (MockExecCount (), 1);
  ASSERT_EQ (MockExecCall (0)->Kind, 2);              /* ExecNsh */
  ASSERT_EQ (EfiStrCmp (MockExecCall (0)->Path, U16("fs2:/scripts/fpt.nsh")), 0);
  ASSERT_EQ (EfiStrCmp (MockExecCall (0)->Options, U16("-x -y")), 0);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  /* auto: .nsh 单候选 → ExecNsh（候选全限定路径） */
  memset (&Cand, 0, sizeof (Cand));
  u16cpy (Cand.Display, 128, U16("USB: tool.nsh"));
  u16cpy (Cand.FullPath, 512, U16("fs5:/scripts/tool.nsh"));
  Cand.Fs = g_p3fs;
  MockExecSetCandidates (&Cand, 1);
  Chr (C, '2');
  Key (C, 0, 0x0D);
  ASSERT_EQ (MockExecCount (), 2);
  ASSERT_EQ (MockExecCall (1)->Kind, 2);
  ASSERT_EQ (EfiStrCmp (MockExecCall (1)->Path, U16("fs5:/scripts/tool.nsh")), 0);
  ASSERT_EQ (EfiStrCmp (MockExecCall (1)->Options, U16("opt")), 0);
  MockExecSetCandidates (NULL, 0);
  LauncherCoreFree (C);
  CfgFree (Cfg);
  FixtureFini ();
}

/* ---- 附加：auto: 多候选选择 / 零命中 ---- */
TEST (extra_auto_candidates) {
  CONFIG *Cfg;
  LAUNCHER_CORE *C;
  static EXEC_CANDIDATE Cands[2];
  FixtureInit ();
  Cfg = CfgNew ("T", "1.0", 1);
  ItemSet (&Cfg->Items[0], "usb", "USB boot", "", "auto:/EFI/BOOT/BOOTX64.EFI",
           ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "copt");
  memset (Cands, 0, sizeof (Cands));
  u16cpy (Cands[0].Display, 128, U16("KINGSTON: BOOTX64.EFI"));
  u16cpy (Cands[0].FullPath, 512, U16("fs3:/EFI/BOOT/BOOTX64.EFI"));
  Cands[0].Fs = g_bootfs;
  u16cpy (Cands[1].Display, 128, U16("SANCRUZER: BOOTX64.EFI"));
  u16cpy (Cands[1].FullPath, 512, U16("fs4:/EFI/BOOT/BOOTX64.EFI"));
  Cands[1].Fs = g_p3fs;
  MockExecSetCandidates (Cands, 2);
  C = CoreNew (Cfg, TRUE, 0, NULL);
  Key (C, 0, 0x0D);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_CANDIDATES);
  ASSERT_EQ (MockExecCount (), 0);
  ASSERT_OK (LauncherCoreDraw (C));
  RowTextIs (3, 1, U16("KINGSTON: BOOTX64.EFI"));
  RowTextIs (4, 1, U16("SANCRUZER: BOOTX64.EFI"));
  Key (C, SCAN_DOWN, 0);
  Key (C, 0, 0x0D);
  ASSERT_EQ (MockExecCount (), 1);
  ASSERT_EQ (MockExecCall (0)->Kind, 1);
  ASSERT_EQ (EfiStrCmp (MockExecCall (0)->Path, U16("fs4:/EFI/BOOT/BOOTX64.EFI")), 0);
  ASSERT_EQ (EfiStrCmp (MockExecCall (0)->Options, U16("copt")), 0);  /* 原条目 options 保留 */
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  /* ESC 取消路径 */
  Key (C, 0, 0x0D);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_CANDIDATES);
  Key (C, SCAN_ESC, 0);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  ASSERT_EQ (MockExecCount (), 1);
  MockExecSetCandidates (NULL, 0);
  LauncherCoreFree (C);
  /* 零命中 → 错误页 */
  C = CoreNew (Cfg, TRUE, 0, NULL);
  Key (C, 0, 0x0D);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_ERROR);
  ASSERT_OK (LauncherCoreDraw (C));
  RowTextIs (4, 2, U16("No external media found with that file"));
  Chr (C, ' ');
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  LauncherCoreFree (C);
  CfgFree (Cfg);
  FixtureFini ();
}

/* ---- 附加：fsN: / boot:XXXX / shell / reset 动作 ---- */
TEST (extra_action_variants) {
  CONFIG *Cfg;
  LAUNCHER_CORE *C;
  FixtureInit ();
  Cfg = CfgNew ("T", "1.0", 4);
  ItemSet (&Cfg->Items[0], "fs", "Explicit vol", "", "fs7:/tool.efi",
           ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "");
  ItemSet (&Cfg->Items[1], "boot", "Boot OS", "", "boot:000A",
           ACTION_BOOT, ARCH_X64_BIT, FALSE, FALSE, "");
  ItemSet (&Cfg->Items[2], "sh", "Shell", "", "",
           ACTION_SHELL, ARCH_X64_BIT, FALSE, FALSE, "");
  ItemSet (&Cfg->Items[3], "rst", "Reset", "", "",
           ACTION_RESET, ARCH_X64_BIT, FALSE, FALSE, "");
  C = CoreNew (Cfg, TRUE, 0, NULL);
  Key (C, 0, 0x0D);                                   /* fs7: */
  ASSERT_EQ (MockExecCount (), 1);
  ASSERT_EQ (EfiStrCmp (MockExecCall (0)->Path, U16("fs7:/tool.efi")), 0);
  Chr (C, '2');
  Key (C, 0, 0x0D);                                   /* boot:000A */
  ASSERT_EQ (MockExecCount (), 2);
  ASSERT_EQ (MockExecCall (1)->Kind, 3);
  ASSERT_EQ (MockExecCall (1)->BootNumber, 0x000A);
  ASSERT_EQ (LauncherCoreExit (C), LEXIT_RESET);
  LauncherCoreFree (C);
  C = CoreNew (Cfg, TRUE, 0, NULL);
  Chr (C, '3');
  Key (C, 0, 0x0D);                                   /* shell 动作 */
  ASSERT_EQ (LauncherCoreExit (C), LEXIT_SHELL);
  LauncherCoreFree (C);
  C = CoreNew (Cfg, TRUE, 0, NULL);
  Chr (C, '4');
  Key (C, 0, 0x0D);                                   /* reset 动作 → 强制确认 */
  ASSERT_EQ (LauncherCoreState (C), LSTATE_CONFIRM);
  ASSERT_EQ (MockExecCount (), 2);
  Chr (C, 'y');
  ASSERT_EQ (MockExecCount (), 3);
  ASSERT_EQ (MockExecCall (2)->Kind, 4);              /* ExecReset */
  ASSERT_EQ (LauncherCoreExit (C), LEXIT_RESET);
  LauncherCoreFree (C);
  CfgFree (Cfg);
  FixtureFini ();
}

/* ---- 附加：exec 失败 / 目标返回错误 → 错误页 ---- */
TEST (extra_exec_errors) {
  CONFIG *Cfg;
  LAUNCHER_CORE *C;
  FixtureInit ();
  Cfg = CfgNew ("T", "1.0", 1);
  ItemSet (&Cfg->Items[0], "x", "X", "", "data:/x.efi",
           ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "");
  MockExecSetImageReturn (EFI_LOAD_ERROR);
  C = CoreNew (Cfg, TRUE, 0, NULL);
  Key (C, 0, 0x0D);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_ERROR);
  ASSERT_OK (LauncherCoreDraw (C));
  RowTextIs (1, 0, U16("ERROR"));
  RowTextIs (4, 2, U16("Image returned: 0x8000000000000001"));
  Chr (C, ' ');
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  MockExecSetImageReturn (EFI_SUCCESS);
  LauncherCoreFree (C);
  CfgFree (Cfg);
  FixtureFini ();
}

/* ---- 附加：帮助页（键位/警告/被跳过条目原因） ---- */
TEST (extra_help_page) {
  CONFIG *Cfg;
  LAUNCHER_CORE *C;
  CONFIG_ERROR Errs[1];
  FixtureInit ();
  Cfg = CfgNew ("T", "1.0", 0);
  memset (Errs, 0, sizeof (Errs));
  Errs[0].ItemIndex = 2;
  strcpy (Errs[0].Field, "arch");
  strcpy (Errs[0].Msg, "bad enum value");
  g_sess.Errs = Errs;
  g_sess.ErrCount = 1;
  C = CoreNew (Cfg, TRUE, 0, NULL);
  Chr (C, '?');
  ASSERT_EQ (LauncherCoreState (C), LSTATE_HELP);
  ASSERT_OK (LauncherCoreDraw (C));
  RowTextIs (1, 0, U16("Help"));
  RowTextIs (3, 1, U16("Keys: Up/Dn/1-9 select  Enter run  S/Q/ESC Shell  R reload"));
  RowTextIs (7, 1, U16("Skipped config items:"));
  RowTextIs (8, 1, U16(" - item 2.arch: bad enum value"));
  Key (C, SCAN_ESC, 0);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  LauncherCoreFree (C);
  CfgFree (Cfg);
  FixtureFini ();
}

/* ---- 附加：导航（钳制/数字/Home/End/PgDn） ---- */
TEST (extra_navigation) {
  CONFIG *Cfg;
  LAUNCHER_CORE *C;
  UINTN i;
  FixtureInit ();
  Cfg = CfgNew ("T", "1.0", 8);
  for (i = 0; i < 8; i++) {
    CHAR8 Id[8], Name[16], Path[32];
    sprintf (Id, "i%u", (unsigned)i);
    sprintf (Name, "Item %u", (unsigned)i);
    sprintf (Path, "data:/f%u.efi", (unsigned)i);
    ItemSet (&Cfg->Items[i], Id, Name, "", Path, ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "");
  }
  C = CoreNew (Cfg, TRUE, 0, NULL);                   /* 13 行（8 工作 + 5 系统） */
  ASSERT_EQ (LauncherCoreSelected (C), 0);
  Key (C, SCAN_UP, 0);
  ASSERT_EQ (LauncherCoreSelected (C), 0);            /* 顶端钳制 */
  Key (C, SCAN_DOWN, 0);
  Key (C, SCAN_DOWN, 0);
  Key (C, SCAN_DOWN, 0);
  ASSERT_EQ (LauncherCoreSelected (C), 3);
  Chr (C, '9');
  ASSERT_EQ (LauncherCoreSelected (C), 8);            /* 数字直选 */
  Key (C, SCAN_HOME, 0);
  ASSERT_EQ (LauncherCoreSelected (C), 0);
  Key (C, SCAN_END, 0);
  ASSERT_EQ (LauncherCoreSelected (C), 12);
  Key (C, SCAN_DOWN, 0);
  ASSERT_EQ (LauncherCoreSelected (C), 12);           /* 底端钳制 */
  Key (C, SCAN_PGUP, 0);
  ASSERT_EQ (LauncherCoreSelected (C), 0);            /* 13 行 < 视口 20 → 页翻到顶 */
  LauncherCoreFree (C);
  CfgFree (Cfg);
  FixtureFini ();
}

/* ---- 附加：小屏不越界 ---- */
TEST (extra_small_screen) {
  CONFIG *Cfg;
  LAUNCHER_CORE *C;
  FixtureInit ();
  MockTuiSetSize (10, 40);
  Cfg = CfgNew ("T", "1.0", 3);
  ItemSet (&Cfg->Items[0], "a", "A", "", "data:/a.efi", ACTION_EXEC, ARCH_X64_BIT, TRUE, FALSE, "");
  ItemSet (&Cfg->Items[1], "b", "B", "", "auto:/b.efi", ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "");
  ItemSet (&Cfg->Items[2], "c", "C", "", "fs1:/c.efi", ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "");
  C = CoreNew (Cfg, TRUE, 2, NULL);
  ASSERT_OK (LauncherCoreDraw (C));                   /* COUNTDOWN */
  ASSERT_OK (LauncherCoreTick (C));
  ASSERT_OK (LauncherCoreTick (C));
  ASSERT_OK (LauncherCoreDraw (C));                   /* MENU */
  Chr (C, '1');
  Key (C, 0, 0x0D);
  ASSERT_EQ (LauncherCoreState (C), LSTATE_CONFIRM);
  ASSERT_OK (LauncherCoreDraw (C));                   /* CONFIRM */
  Chr (C, 'y');
  ASSERT_EQ (LauncherCoreState (C), LSTATE_MENU);
  ASSERT_OK (LauncherCoreDraw (C));
  Chr (C, '?');
  ASSERT_EQ (LauncherCoreState (C), LSTATE_HELP);
  ASSERT_OK (LauncherCoreDraw (C));                   /* HELP */
  LauncherCoreFree (C);
  CfgFree (Cfg);
  FixtureFini ();
}

/* ---- 附加：随机按键 fuzz（状态机不崩/状态合法） ---- */
static UINT32 g_rng = 0x11A0BEEFu;
static UINT32 rnd (VOID) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

/* fuzz 专用 reload：不换配置（夹具共享，不能释放） */
static EFI_STATUS ReloadNoopMock (VOID *Ctx) {
  return EFI_SUCCESS;
}

TEST (extra_fuzz_keys) {
  CONFIG *Cfg;
  LAUNCHER_CORE *C;
  LAUNCHER_PLATFORM_OPS Ops;
  UINTN r;
  static EXEC_CANDIDATE Cands[2];
  FixtureInit ();
  Cfg = CfgNew ("Fuzz", "1.0", 4);
  ItemSet (&Cfg->Items[0], "a", "A", "", "data:/a.efi", ACTION_EXEC, ARCH_X64_BIT, FALSE, FALSE, "");
  ItemSet (&Cfg->Items[1], "b", "B", "", "data:/b.nsh", ACTION_EXEC, ARCH_X64_BIT, TRUE, FALSE, "");
  ItemSet (&Cfg->Items[2], "c", "C", "", "auto:/c.efi", ACTION_EXEC, ARCH_AARCH64_BIT, FALSE, FALSE, "");
  ItemSet (&Cfg->Items[3], "d", "D", "", "boot:0001", ACTION_BOOT, ARCH_X64_BIT, FALSE, FALSE, "");
  memset (Cands, 0, sizeof (Cands));
  u16cpy (Cands[0].Display, 128, U16("M1"));
  u16cpy (Cands[0].FullPath, 512, U16("fs3:/c.efi"));
  u16cpy (Cands[1].Display, 128, U16("M2"));
  u16cpy (Cands[1].FullPath, 512, U16("fs4:/c.efi"));
  memset (&Ops, 0, sizeof (Ops));
  Ops.Reload = ReloadNoopMock;
  Ops.BootEntries = BootEntriesMock;
  for (r = 0; r < 3000; r++) {
    TUI_KEY k;
    UINTN Mode = rnd () % 5;
    /* 每 500 轮重建 core（覆盖倒计时/DEGRADED 初态） */
    if (r % 500 == 0) {
      if (r > 0) {
        LauncherCoreFree (C);
      }
      MockExecSetCandidates (Cands, 2);
      C = CoreNew (Cfg, (BOOLEAN)((r / 500) % 2 == 0), (r / 500) % 3, &Ops);
      ASSERT_TRUE (C != NULL);
    }
    k.ScanCode = 0;
    k.UnicodeChar = 0;
    switch (Mode) {
      case 0: k.ScanCode = (UINT16)(1 + rnd () % 0x17); break;   /* 扫描码含未映射 */
      case 1: k.UnicodeChar = (CHAR16)('0' + rnd () % 10); break;
      case 2: k.UnicodeChar = (CHAR16)('a' + rnd () % 26); break;
      case 3: k.UnicodeChar = (CHAR16)(0x0D); break;
      default: k.UnicodeChar = (CHAR16)(rnd () % 0x80); break;
    }
    ASSERT_OK (LauncherCoreHandleKey (C, &k));
    ASSERT_TRUE (LauncherCoreState (C) <= LSTATE_HELP);
    ASSERT_TRUE (LauncherCoreExit (C) <= LEXIT_RESET);
    if (rnd () % 7 == 0) {
      ASSERT_OK (LauncherCoreDraw (C));
    }
    if (rnd () % 11 == 0) {
      ASSERT_OK (LauncherCoreTick (C));
    }
    if (LauncherCoreExit (C) != LEXIT_NONE) {
      LauncherCoreFree (C);
      MockExecSetCandidates (Cands, 2);
      C = CoreNew (Cfg, TRUE, 0, &Ops);
    }
  }
  LauncherCoreFree (C);
  MockExecSetCandidates (NULL, 0);
  g_reloadNewCfg = NULL;
  CfgFree (Cfg);
  FixtureFini ();
}

int main (void) {
  RUN_TEST (t11_1_menu_golden);
  RUN_TEST (t11_2_exec_params);
  RUN_TEST (t11_3_danger_confirm);
  RUN_TEST (t11_4_degraded);
  RUN_TEST (t11_5_bad_json_builtin);
  RUN_TEST (t11_6_countdown);
  RUN_TEST (t11_7_arch_disabled);
  RUN_TEST (t11_8_reload);
  RUN_TEST (t11_9_boot_submenu);
  RUN_TEST (t11_10_nsh_params);
  RUN_TEST (extra_auto_candidates);
  RUN_TEST (extra_action_variants);
  RUN_TEST (extra_exec_errors);
  RUN_TEST (extra_help_page);
  RUN_TEST (extra_navigation);
  RUN_TEST (extra_small_screen);
  RUN_TEST (extra_fuzz_keys);
  RUN_TESTS_END ();
}
