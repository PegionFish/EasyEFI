/** @file
  S03 ConfigCoreLib 宿主测试（T03-1..17 + 边界补充）。
  框架：tests/host/test.h（S00）。运行：make -f host.mk test。
  T03-6 说明：DOM 无 REAL 类型（契约冻结），浮点在适配器边界拒绝 →
  断言 JsonDomFromHost 返回 EFI_INVALID_PARAMETER（见交接注记契约观察）。
**/
#include "test.h"
#include "ConfigCoreInternal.h"
#include <time.h>

#ifndef GOLDEN_PATH
#define GOLDEN_PATH "config/launcher.example.json"
#endif
#ifndef GOLDEN_DUMP_PATH
#define GOLDEN_DUMP_PATH "tests/golden-dom.txt"
#endif

/* ---- 助手 ---- */

typedef struct {
  JSON_NODE    *Dom;
  CONFIG       *Cfg;
  CONFIG_ERROR *Errs;
  CONFIG_ERROR *Warns;
  UINTN         ErrCount;
  UINTN         WarnCount;
  EFI_STATUS    DomSt;
  EFI_STATUS    ValSt;
} VRESULT;

static void ValidateText (CONST CHAR8 *Text, VRESULT *R) {
  memset (R, 0, sizeof (*R));
  R->DomSt = JsonDomFromHost (Text, &R->Dom);
  if (!EFI_ERROR (R->DomSt)) {
    R->ValSt = ConfigValidate (R->Dom, &R->Cfg, &R->Errs, &R->ErrCount,
                               &R->Warns, &R->WarnCount);
  }
}

static void FreeResult (VRESULT *R) {
  ConfigFree (R->Cfg);
  ConfigFreeErrors (R->Errs);
  ConfigFreeErrors (R->Warns);
  JsonDomFree (R->Dom);
  memset (R, 0, sizeof (*R));
}

static CONST CONFIG_ERROR *FindMsg (
  CONST CONFIG_ERROR *V, UINTN N, UINTN Idx, CONST CHAR8 *Field, UINT32 Code
  )
{
  UINTN I;
  for (I = 0; I < N; I++) {
    if (V[I].ItemIndex == Idx && strcmp (V[I].Field, Field) == 0 && V[I].Code == Code) {
      return &V[I];
    }
  }
  return NULL;
}

static char *ReadFile (const char *Path, size_t *LenOut) {
  FILE *F = fopen (Path, "rb");
  char *Buf;
  long Sz;
  if (!F) return NULL;
  fseek (F, 0, SEEK_END);
  Sz = ftell (F);
  fseek (F, 0, SEEK_SET);
  Buf = malloc ((size_t)Sz + 1);
  if (!Buf) { fclose (F); return NULL; }
  if (fread (Buf, 1, (size_t)Sz, F) != (size_t)Sz) { free (Buf); fclose (F); return NULL; }
  Buf[Sz] = '\0';
  fclose (F);
  if (LenOut) *LenOut = (size_t)Sz;
  return Buf;
}

/* 合法单条目配置模板（printf 风格 %s 替换由调用方 snprintf 完成）。 */
#define ITEM_FMT "{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"%s\",\"path\":\"%s\"}]}"

static void MakeItemJson (char *Buf, size_t Cap, const char *Action, const char *Path) {
  snprintf (Buf, Cap, ITEM_FMT, Action, Path);
}

/* ---- T03-1 黄金样例：逐字段相等 ---- */

TEST (t03_1_golden) {
  char *Text = ReadFile (GOLDEN_PATH, NULL);
  VRESULT R;
  UINTN I;
  static const struct {
    const char *Id, *Name, *Desc;
    UINT8 Mask, Action;
    const char *Path;
    BOOLEAN Danger, Confirm;
  } Exp[] = {
    { "win-usb", "Windows installer (external)", "Find BOOTX64.EFI on other media",
      ARCH_X64_BIT, ACTION_EXEC, "auto:/EFI/BOOT/BOOTX64.EFI", FALSE, FALSE },
    { "linux-x64", "Linux LiveCD x64 (external)", "",
      ARCH_X64_BIT, ACTION_EXEC, "auto:/EFI/BOOT/BOOTX64.EFI", FALSE, FALSE },
    { "linux-arm64", "Linux LiveCD ARM64 (external)", "",
      ARCH_AARCH64_BIT, ACTION_EXEC, "auto:/EFI/BOOT/BOOTAA64.EFI", FALSE, FALSE },
    { "winpe-local", "Local WinPE (data volume)", "",
      ARCH_X64_BIT, ACTION_EXEC, "data:/entries/winpe.efi", FALSE, FALSE },
    { "fpt", "Intel FPT flash tool", "x64 only. HIGH RISK: writes SPI flash",
      ARCH_X64_BIT, ACTION_EXEC, "data:/scripts/fpt.nsh", TRUE, TRUE },
    { "afu", "AMI AFU flash tool", "x64 only. HIGH RISK: writes SPI flash",
      ARCH_X64_BIT, ACTION_EXEC, "data:/scripts/afu.nsh", TRUE, TRUE },
    { "nvram-boot", "Boot installed OS (NVRAM menu)",
      "Pick firmware boot entry. BitLocker: have recovery key ready",
      ARCH_X64_BIT | ARCH_AARCH64_BIT, ACTION_BOOT, "boot:menu", FALSE, FALSE },
    { "shell", "Return to UEFI Shell", "",
      ARCH_X64_BIT | ARCH_AARCH64_BIT, ACTION_SHELL, "", FALSE, FALSE },
    { "reboot", "Reboot", "",
      ARCH_X64_BIT | ARCH_AARCH64_BIT, ACTION_RESET, "", FALSE, TRUE },
  };
  ASSERT_TRUE (Text != NULL);
  if (!Text) return;
  ValidateText (Text, &R);
  ASSERT_OK (R.DomSt);
  ASSERT_OK (R.ValSt);
  ASSERT_EQ (R.ErrCount, 0);
  ASSERT_EQ (R.WarnCount, 0);
  ASSERT_TRUE (R.Cfg != NULL);
  if (R.Cfg) {
    ASSERT_EQ (strcmp (R.Cfg->Title, "EasyEFI Work Toolkit"), 0);
    ASSERT_EQ (strcmp (R.Cfg->Version, "1.0.0"), 0);
    ASSERT_EQ (R.Cfg->SkipTimeout, 2);
    ASSERT_EQ (R.Cfg->Count, 9);
    for (I = 0; I < 9 && I < R.Cfg->Count; I++) {
      CONFIG_ITEM *It = &R.Cfg->Items[I];
      ASSERT_EQ (strcmp (It->Id, Exp[I].Id), 0);
      ASSERT_EQ (strcmp (It->Name, Exp[I].Name), 0);
      ASSERT_EQ (strcmp (It->Desc, Exp[I].Desc), 0);
      ASSERT_EQ (It->ArchMask, Exp[I].Mask);
      ASSERT_EQ (It->Action, Exp[I].Action);
      ASSERT_EQ (strcmp (It->Path, Exp[I].Path), 0);
      ASSERT_EQ (strcmp (It->Options, ""), 0);
      ASSERT_EQ (It->Danger, Exp[I].Danger);
      ASSERT_EQ (It->Confirm, Exp[I].Confirm);
    }
  }
  FreeResult (&R);
  free (Text);
}

/* ---- T03-2 缺 id ---- */

TEST (t03_2_missing_id) {
  VRESULT R;
  ValidateText ("{\"items\":[{\"name\":\"n\",\"action\":\"shell\"}]}", &R);
  ASSERT_OK (R.DomSt);
  ASSERT_OK (R.ValSt);           /* 尽力而为：CONFIG 产出，错误入清单 */
  ASSERT_EQ (R.ErrCount, 1);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "id", CFG_ERR_MISSING_FIELD) != NULL);
  ASSERT_EQ (R.Cfg->Count, 0);
  FreeResult (&R);
  /* 空 id 同样拒绝 */
  ValidateText ("{\"items\":[{\"id\":\"\",\"name\":\"n\",\"action\":\"shell\"}]}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "id", CFG_ERR_MISSING_FIELD) != NULL);
  FreeResult (&R);
  /* 缺 name / 缺 action */
  ValidateText ("{\"items\":[{\"id\":\"a\",\"action\":\"shell\"}]}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "name", CFG_ERR_MISSING_FIELD) != NULL);
  FreeResult (&R);
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\"}]}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "action", CFG_ERR_MISSING_FIELD) != NULL);
  FreeResult (&R);
}

/* ---- T03-3 非 ASCII ---- */

TEST (t03_3_non_ascii) {
  VRESULT R;
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"\xe4\xb8\xad\xe6\x96\x87\",\"action\":\"shell\"}]}", &R);
  ASSERT_OK (R.ValSt);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "name", CFG_ERR_NON_ASCII) != NULL);
  ASSERT_EQ (R.Cfg->Count, 0);
  FreeResult (&R);
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"desc\":\"\xe4\xb8\xad\",\"action\":\"shell\"}]}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "desc", CFG_ERR_NON_ASCII) != NULL);
  FreeResult (&R);
  ValidateText ("{\"title\":\"\xe4\xb8\xad\",\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\"}]}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, (UINTN)-1, "title", CFG_ERR_NON_ASCII) != NULL);
  FreeResult (&R);
}

/* ---- T03-4 超长 ---- */

TEST (t03_4_too_long) {
  char Buf[2048];
  char Long[300];
  VRESULT R;
  memset (Long, 'a', sizeof (Long));

  Long[41] = '\0';   /* name 41 > 40 */
  snprintf (Buf, sizeof (Buf), "{\"items\":[{\"id\":\"a\",\"name\":\"%s\",\"action\":\"shell\"}]}", Long);
  ValidateText (Buf, &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "name", CFG_ERR_TOO_LONG) != NULL);
  FreeResult (&R);

  Long[40] = '\0';   /* name 40 = 边界合法 */
  snprintf (Buf, sizeof (Buf), "{\"items\":[{\"id\":\"a\",\"name\":\"%s\",\"action\":\"shell\"}]}", Long);
  ValidateText (Buf, &R);
  ASSERT_EQ (R.ErrCount, 0);
  ASSERT_EQ (R.Cfg->Count, 1);
  FreeResult (&R);

  memset (Long, 'a', sizeof (Long)); Long[77] = '\0';   /* desc 77 > 76 */
  snprintf (Buf, sizeof (Buf), "{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"desc\":\"%s\",\"action\":\"shell\"}]}", Long);
  ValidateText (Buf, &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "desc", CFG_ERR_TOO_LONG) != NULL);
  FreeResult (&R);

  memset (Long, 'a', sizeof (Long)); Long[64] = '\0';   /* id 64 > 63 */
  snprintf (Buf, sizeof (Buf), "{\"items\":[{\"id\":\"%s\",\"name\":\"n\",\"action\":\"shell\"}]}", Long);
  ValidateText (Buf, &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "id", CFG_ERR_TOO_LONG) != NULL);
  FreeResult (&R);

  memset (Long, 'a', sizeof (Long)); Long[41] = '\0';   /* title 41 > 40（顶层） */
  snprintf (Buf, sizeof (Buf), "{\"title\":\"%s\",\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\"}]}", Long);
  ValidateText (Buf, &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, (UINTN)-1, "title", CFG_ERR_TOO_LONG) != NULL);
  FreeResult (&R);

  memset (Long, 'a', sizeof (Long)); Long[256] = '\0';  /* path 256 > 255 */
  snprintf (Buf, sizeof (Buf), "{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"exec\",\"path\":\"data:/%s\"}]}", Long);
  ValidateText (Buf, &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "path", CFG_ERR_TOO_LONG) != NULL);
  FreeResult (&R);

  memset (Long, 'a', sizeof (Long)); Long[256] = '\0';  /* options 256 > 255 */
  snprintf (Buf, sizeof (Buf), "{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"exec\",\"path\":\"data:/x\",\"options\":\"%s\"}]}", Long);
  ValidateText (Buf, &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "options", CFG_ERR_TOO_LONG) != NULL);
  FreeResult (&R);
}

/* ---- T03-5 坏 arch ---- */

TEST (t03_5_bad_arch) {
  VRESULT R;
  CONST CONFIG_ERROR *E;
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"arch\":[\"arm64\"],\"action\":\"shell\"}]}", &R);
  E = FindMsg (R.Errs, R.ErrCount, 0, "arch", CFG_ERR_BAD_ENUM);
  ASSERT_TRUE (E != NULL);
  if (E) ASSERT_TRUE (strstr (E->Msg, "aarch64") != NULL);   /* 提示正确值 */
  ASSERT_EQ (R.Cfg->Count, 0);
  FreeResult (&R);

  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"arch\":[\"x86\"],\"action\":\"shell\"}]}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "arch", CFG_ERR_BAD_ENUM) != NULL);
  FreeResult (&R);

  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"arch\":\"x64\",\"action\":\"shell\"}]}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "arch", CFG_ERR_TYPE) != NULL);
  FreeResult (&R);

  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"arch\":[1],\"action\":\"shell\"}]}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "arch", CFG_ERR_TYPE) != NULL);
  FreeResult (&R);

  /* 合法：双架构 / 缺省双架构 / 空数组掩码 0 */
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"arch\":[\"x64\",\"aarch64\"],\"action\":\"shell\"}]}", &R);
  ASSERT_EQ (R.ErrCount, 0);
  ASSERT_EQ (R.Cfg->Items[0].ArchMask, ARCH_X64_BIT | ARCH_AARCH64_BIT);
  FreeResult (&R);
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\"}]}", &R);
  ASSERT_EQ (R.Cfg->Items[0].ArchMask, ARCH_X64_BIT | ARCH_AARCH64_BIT);
  FreeResult (&R);
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"arch\":[],\"action\":\"shell\"}]}", &R);
  ASSERT_EQ (R.ErrCount, 0);
  ASSERT_EQ (R.Cfg->Items[0].ArchMask, 0);
  FreeResult (&R);
}

/* ---- T03-6 浮点（适配器边界拒绝；DOM 无 REAL 类型） ---- */

TEST (t03_6_float_rejected) {
  JSON_NODE *Dom = NULL;
  EFI_STATUS S;
  S = JsonDomFromHost ("{\"skip_timeout\":1.5,\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\"}]}", &Dom);
  ASSERT_EQ (S, EFI_INVALID_PARAMETER);
  ASSERT_TRUE (Dom == NULL);
  S = JsonDomFromHost ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\",\"x\":2.0}]}", &Dom);
  ASSERT_EQ (S, EFI_INVALID_PARAMETER);   /* 未知字段浮点同样拒绝（整数 only 全文档） */
  ASSERT_TRUE (Dom == NULL);
  S = JsonDomFromHost ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\",\"x\":1e2}]}", &Dom);
  ASSERT_EQ (S, EFI_INVALID_PARAMETER);   /* 科学计数 = real */
  ASSERT_TRUE (Dom == NULL);
  S = JsonDomFromHost ("{\"skip_timeout\":99999999999999999999999,\"items\":[]}", &Dom);
  ASSERT_ERR (S);                          /* 整数溢出 → 解析失败 */
  ASSERT_TRUE (Dom == NULL);
  /* 整数合法边界 */
  S = JsonDomFromHost ("{\"skip_timeout\":-9223372036854775808,\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\"}]}", &Dom);
  ASSERT_OK (S);
  JsonDomFree (Dom);
}

/* ---- T03-7 重复 id ---- */

TEST (t03_7_dup_id) {
  VRESULT R;
  ValidateText ("{\"items\":["
                "{\"id\":\"x\",\"name\":\"one\",\"action\":\"shell\"},"
                "{\"id\":\"x\",\"name\":\"two\",\"action\":\"shell\"}]}", &R);
  ASSERT_OK (R.ValSt);
  ASSERT_EQ (R.ErrCount, 1);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 1, "id", CFG_ERR_DUP_ID) != NULL);
  ASSERT_EQ (R.Cfg->Count, 1);
  ASSERT_EQ (strcmp (R.Cfg->Items[0].Name, "one"), 0);
  FreeResult (&R);
  /* 被跳过项的 id 不占位：非法项 + 同 id 合法项 → 合法项保留 */
  ValidateText ("{\"items\":["
                "{\"id\":\"x\",\"name\":\"bad\",\"action\":\"nope\"},"
                "{\"id\":\"x\",\"name\":\"good\",\"action\":\"shell\"}]}", &R);
  ASSERT_EQ (R.Cfg->Count, 1);
  ASSERT_EQ (strcmp (R.Cfg->Items[0].Name, "good"), 0);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "action", CFG_ERR_BAD_ENUM) != NULL);
  FreeResult (&R);
}

/* ---- T03-8 坏 path 前缀/文法（委托 PathParse） ---- */

TEST (t03_8_bad_path) {
  static const char *Bad[] = {
    "usb:/x.efi", "data:", "data:/../etc/passwd", "data:/a/../../b",
    "fsX:/a", "fs:/a", "boot:GGGG", "boot:123", "boot:12345", "",
    "data:/a//b", "data:/", "http://x", "boot:",
  };
  char Buf[1024];
  VRESULT R;
  UINTN I;
  for (I = 0; I < sizeof (Bad) / sizeof (Bad[0]); I++) {
    MakeItemJson (Buf, sizeof (Buf), "exec", Bad[I]);
    ValidateText (Buf, &R);
    ASSERT_OK (R.ValSt);
    ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "path", CFG_ERR_BAD_PATH) != NULL);
    ASSERT_EQ (R.Cfg->Count, 0);
    FreeResult (&R);
  }
  /* 合法路径文法 */
  static const char *Good[] = {
    "data:/entries/winpe.efi", "auto:/EFI/BOOT/BOOTX64.EFI",
    "fs0:/x.efi", "fs12:/x.efi", "boot:0001", "boot:FFFF", "boot:menu",
    "data:\\\\scripts\\\\fpt.nsh",   /* JSON 转义后 = data:\scripts\fpt.nsh，反斜杠规范化接受 */
  };
  for (I = 0; I < sizeof (Good) / sizeof (Good[0]); I++) {
    const char *Act = (strncmp (Good[I], "boot:", 5) == 0) ? "boot" : "exec";
    MakeItemJson (Buf, sizeof (Buf), Act, Good[I]);
    ValidateText (Buf, &R);
    ASSERT_OK (R.DomSt);
    ASSERT_EQ (R.ErrCount, 0);
    ASSERT_EQ (R.Cfg->Count, 1);
    FreeResult (&R);
  }
  /* exec/boot 缺 path → MISSING_FIELD；shell/reset 无 path 合法 */
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"exec\"}]}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "path", CFG_ERR_MISSING_FIELD) != NULL);
  FreeResult (&R);
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"boot\"}]}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "path", CFG_ERR_MISSING_FIELD) != NULL);
  FreeResult (&R);
  /* shell 带 path / 非 exec 带 options → 警告忽略 */
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\",\"path\":\"data:/x\",\"options\":\"o\"}]}", &R);
  ASSERT_EQ (R.ErrCount, 0);
  ASSERT_EQ (R.WarnCount, 2);
  ASSERT_EQ (R.Cfg->Count, 1);
  FreeResult (&R);
}

/* ---- T03-8b S04 语义裁决边界（main docs/handoffs/S04.md）----
   前缀小写敏感/值大小写不敏感；前缀后强制分隔符；拒 ./空分量；
   BMP 非 ASCII 路径接受；星面/非法 UTF-8/控制字符拒绝。 */

TEST (t03_8b_path_s04_semantics) {
  static const struct { const char *Path; BOOLEAN Ok; const char *Act; } Cases[] = {
    { "data:/\xe8\x84\x9a\xe6\x9c\xac.nsh", TRUE, "exec" },   /* 中文 BMP 路径接受 */
    { "data:\\\\\xe8\x84\x9a\xe6\x9c\xac.nsh", TRUE, "exec" }, /* 反斜杠+中文 */
    { "data:/a.NSH", TRUE, "exec" },                          /* .nsh 值大小写不敏感 */
    { "fs007:/x.efi", TRUE, "exec" },                         /* fs 前导零 */
    { "boot:ffff", TRUE, "boot" },                            /* hex 小写 */
    { "boot:MENU", TRUE, "boot" },                            /* menu 大小写不敏感 */
    { "data:/foo..bar", TRUE, "exec" },                       /* 含点文件名合法 */
    { "data:/..hidden", TRUE, "exec" },                       /* ..hidden 合法 */
    { "DATA:/x.efi", FALSE, "exec" },                         /* 前缀大小写敏感 */
    { "data:x.efi", FALSE, "exec" },                          /* 缺分隔符 */
    { "data:/./a", FALSE, "exec" },                           /* '.' 分量拒绝 */
    { "data:/a/", FALSE, "exec" },                            /* 尾随空分量 */
    { "data:/a\\u0001b", FALSE, "exec" },                     /* 控制字符（JSON 转义） */
    { "data:/\xf0\x9f\x98\x80.nsh", FALSE, "exec" },          /* 星面 emoji（合法 UTF-8，UCS-2 不可表示→PathParse 拒） */
    { "fs-1:/x", FALSE, "exec" },                             /* fs 索引非数字 */
    { "boot:men", FALSE, "boot" },                            /* 非 menu 非 4 位 hex */
  };
  char Buf[1024];
  VRESULT R;
  UINTN I;
  JSON_NODE *Dom = NULL;
  /* 非法 UTF-8（代理区/截断）：jansson 解析层即拒（到不了 PathParse），双适配器一致 */
  ASSERT_ERR (JsonDomFromHost ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"exec\",\"path\":\"data:/\xed\xa0\x80\"}]}", &Dom));
  ASSERT_TRUE (Dom == NULL);
  ASSERT_ERR (JsonDomFromHost ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"exec\",\"path\":\"data:/\xe8\x84\"}]}", &Dom));
  ASSERT_TRUE (Dom == NULL);
  for (I = 0; I < sizeof (Cases) / sizeof (Cases[0]); I++) {
    MakeItemJson (Buf, sizeof (Buf), Cases[I].Act, Cases[I].Path);
    ValidateText (Buf, &R);
    ASSERT_OK (R.DomSt);
    ASSERT_OK (R.ValSt);
    if (Cases[I].Ok) {
      ASSERT_EQ (R.ErrCount, 0);
      ASSERT_EQ (R.Cfg->Count, 1);
      if (R.Cfg && R.Cfg->Count == 1 && strchr (Cases[I].Path, '\\') == NULL) {
        /* Path 存原始输入串（UTF-8 逐字节）；含 JSON 转义反斜杠的例除外
           （表值为编码形，存储值为解码形） */
        ASSERT_EQ (strcmp (R.Cfg->Items[0].Path, Cases[I].Path), 0);
      }
    } else {
      ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "path", CFG_ERR_BAD_PATH) != NULL);
      ASSERT_EQ (R.Cfg->Count, 0);
    }
    FreeResult (&R);
  }
}

/* ---- T03-9 未知 action ---- */

TEST (t03_9_bad_action) {
  VRESULT R;
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"launch\",\"path\":\"data:/x\"}]}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "action", CFG_ERR_BAD_ENUM) != NULL);
  ASSERT_EQ (R.Cfg->Count, 0);
  FreeResult (&R);
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":5}]}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "action", CFG_ERR_TYPE) != NULL);
  FreeResult (&R);
}

/* ---- T03-10 skip_timeout 越界钳制 + 警告 ---- */

TEST (t03_10_skip_timeout) {
  VRESULT R;
  ValidateText ("{\"skip_timeout\":31,\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\"}]}", &R);
  ASSERT_OK (R.ValSt);
  ASSERT_EQ (R.Cfg->SkipTimeout, 30);
  ASSERT_EQ (R.ErrCount, 0);
  ASSERT_TRUE (FindMsg (R.Warns, R.WarnCount, (UINTN)-1, "skip_timeout", CFG_ERR_RANGE) != NULL);
  FreeResult (&R);

  ValidateText ("{\"skip_timeout\":-1,\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\"}]}", &R);
  ASSERT_EQ (R.Cfg->SkipTimeout, 0);
  ASSERT_TRUE (FindMsg (R.Warns, R.WarnCount, (UINTN)-1, "skip_timeout", CFG_ERR_RANGE) != NULL);
  FreeResult (&R);

  ValidateText ("{\"skip_timeout\":0,\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\"}]}", &R);
  ASSERT_EQ (R.Cfg->SkipTimeout, 0);
  ASSERT_EQ (R.WarnCount, 0);
  FreeResult (&R);
  ValidateText ("{\"skip_timeout\":30,\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\"}]}", &R);
  ASSERT_EQ (R.Cfg->SkipTimeout, 30);
  ASSERT_EQ (R.WarnCount, 0);
  FreeResult (&R);
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\"}]}", &R);
  ASSERT_EQ (R.Cfg->SkipTimeout, 2);   /* 缺省 = PCD 默认 */
  ASSERT_EQ (R.WarnCount, 0);
  FreeResult (&R);
  ValidateText ("{\"skip_timeout\":\"2\",\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\"}]}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, (UINTN)-1, "skip_timeout", CFG_ERR_TYPE) != NULL);
  FreeResult (&R);
}

/* ---- T03-11 空 items ---- */

TEST (t03_11_empty_items) {
  VRESULT R;
  ValidateText ("{\"items\":[]}", &R);
  ASSERT_OK (R.ValSt);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, (UINTN)-1, "items", CFG_ERR_MISSING_FIELD) != NULL);
  ASSERT_EQ (R.Cfg->Count, 0);
  FreeResult (&R);
  ValidateText ("{\"title\":\"t\"}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, (UINTN)-1, "items", CFG_ERR_MISSING_FIELD) != NULL);
  FreeResult (&R);
  ValidateText ("{\"items\":{}}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, (UINTN)-1, "items", CFG_ERR_TYPE) != NULL);
  FreeResult (&R);
}

/* ---- T03-12 根非 object ---- */

TEST (t03_12_root_type) {
  VRESULT R;
  ValidateText ("[1,2,3]", &R);
  ASSERT_OK (R.DomSt);            /* 数组是合法 JSON 根（适配器接受） */
  ASSERT_ERR (R.ValSt);
  ASSERT_TRUE (R.Cfg == NULL);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, (UINTN)-1, "root", CFG_ERR_ROOT_TYPE) != NULL);
  FreeResult (&R);
  /* 标量根：适配器与 JsonLoadString 一致拒绝（无 DECODE_ANY） */
  ValidateText ("\"str\"", &R);
  ASSERT_ERR (R.DomSt);
  FreeResult (&R);
  ValidateText ("5", &R);
  ASSERT_ERR (R.DomSt);
  FreeResult (&R);
}

/* ---- T03-13 未知字段 → 警告且解析成功 ---- */

TEST (t03_13_unknown_fields) {
  VRESULT R;
  ValidateText ("{\"future_top\":true,\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\",\"future_field\":1}]}", &R);
  ASSERT_OK (R.ValSt);
  ASSERT_EQ (R.ErrCount, 0);
  ASSERT_EQ (R.WarnCount, 2);
  ASSERT_TRUE (FindMsg (R.Warns, R.WarnCount, (UINTN)-1, "future_top", CFG_WARN_UNKNOWN) != NULL);
  ASSERT_TRUE (FindMsg (R.Warns, R.WarnCount, 0, "future_field", CFG_WARN_UNKNOWN) != NULL);
  ASSERT_EQ (R.Cfg->Count, 1);
  FreeResult (&R);
}

/* ---- T03-14 danger 隐含 confirm ---- */

TEST (t03_14_danger_confirm) {
  VRESULT R;
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"exec\",\"path\":\"data:/x\",\"danger\":true}]}", &R);
  ASSERT_EQ (R.ErrCount, 0);
  ASSERT_EQ (R.Cfg->Items[0].Danger, TRUE);
  ASSERT_EQ (R.Cfg->Items[0].Confirm, TRUE);
  FreeResult (&R);
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\",\"confirm\":true}]}", &R);
  ASSERT_EQ (R.Cfg->Items[0].Danger, FALSE);
  ASSERT_EQ (R.Cfg->Items[0].Confirm, TRUE);
  FreeResult (&R);
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\"}]}", &R);
  ASSERT_EQ (R.Cfg->Items[0].Danger, FALSE);
  ASSERT_EQ (R.Cfg->Items[0].Confirm, FALSE);
  FreeResult (&R);
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"shell\",\"danger\":\"yes\"}]}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "danger", CFG_ERR_TYPE) != NULL);
  FreeResult (&R);
}

/* ---- T03-15 双适配器同构 DOM（黄金对比；EDK2 侧在 QEMU 冒烟跑） ---- */

TEST (t03_15_dom_isomorphism) {
  char *Text = ReadFile (GOLDEN_PATH, NULL);
  char *Expect = ReadFile (GOLDEN_DUMP_PATH, NULL);
  JSON_NODE *Dom = NULL;
  CHAR8 *Dump = NULL;
  ASSERT_TRUE (Text != NULL);
  ASSERT_TRUE (Expect != NULL);   /* 先运行 tests/gen_golden 生成（见 host.mk 注释） */
  if (Text && Expect) {
    ASSERT_OK (JsonDomFromHost (Text, &Dom));
    ASSERT_OK (JsonDomDump (Dom, &Dump));
    /* 去掉文件尾换行后逐字节相等 */
    size_t EL = strlen (Expect);
    while (EL > 0 && (Expect[EL-1] == '\n' || Expect[EL-1] == '\r')) Expect[--EL] = '\0';
    ASSERT_TRUE (Dump != NULL && strcmp (Dump, Expect) == 0);
    if (Dump && strcmp (Dump, Expect) != 0) {
      printf ("  host dump: %.200s...\n  expect   : %.200s...\n", Dump, Expect);
    }
  }
  MemFree (Dump);
  JsonDomFree (Dom);
  free (Text);
  free (Expect);
  /* 小文档规范形黄金 */
  {
    JSON_NODE *D2 = NULL;
    CHAR8 *S2 = NULL;
    ASSERT_OK (JsonDomFromHost ("{\"a\":[1,true,null,\"x\"],\"b\":{\"c\":\"d,e)\"}}", &D2));
    ASSERT_OK (JsonDomDump (D2, &S2));
    ASSERT_TRUE (S2 != NULL && strcmp (S2, "obj{a:arr[1,true,null,str(x)],b:obj{c:str(d\\,e\\))}}") == 0);
    MemFree (S2);
    JsonDomFree (D2);
  }
}

/* ---- T03-16 大配置（200 项）性能与内存 ---- */

TEST (t03_16_large_config) {
  char *Buf = malloc (200 * 256 + 256);
  size_t Off = 0;
  UINTN I;
  VRESULT R;
  struct timespec T0, T1;
  double Ms;
  ASSERT_TRUE (Buf != NULL);
  if (!Buf) return;
  Off += (size_t)snprintf (Buf + Off, 200 * 256 + 256 - Off, "{\"title\":\"big\",\"items\":[");
  for (I = 0; I < 200; I++) {
    Off += (size_t)snprintf (Buf + Off, 200 * 256 + 256 - Off,
      "%s{\"id\":\"item-%u\",\"name\":\"Item %u\",\"arch\":[\"x64\",\"aarch64\"],"
      "\"action\":\"exec\",\"path\":\"data:/entries/img%u.efi\",\"options\":\"-f %u\"}",
      (I == 0) ? "" : ",", (unsigned)I, (unsigned)I, (unsigned)I, (unsigned)I);
  }
  snprintf (Buf + Off, 200 * 256 + 256 - Off, "]}");
  clock_gettime (CLOCK_MONOTONIC, &T0);
  ValidateText (Buf, &R);
  clock_gettime (CLOCK_MONOTONIC, &T1);
  Ms = (double)(T1.tv_sec - T0.tv_sec) * 1000.0 + (double)(T1.tv_nsec - T0.tv_nsec) / 1e6;
  ASSERT_OK (R.ValSt);
  ASSERT_EQ (R.ErrCount, 0);
  ASSERT_EQ (R.Cfg->Count, 200);
  ASSERT_TRUE (Ms < 2000.0);   /* ASan -O1 下宽限 2s */
  printf ("  200 items validated in %.1f ms\n", Ms);
  ASSERT_EQ (strcmp (R.Cfg->Items[199].Id, "item-199"), 0);
  FreeResult (&R);
  free (Buf);
}

/* ---- 补充边界 ---- */

TEST (extra_adapter_edges) {
  JSON_NODE *Dom = NULL;
  VRESULT R;
  /* 重复 JSON 键拒绝（design §5.3 REJECT_DUPLICATES，双端一致） */
  ASSERT_ERR (JsonDomFromHost ("{\"items\":[],\"items\":[]}", &Dom));
  ASSERT_TRUE (Dom == NULL);
  /* 语法错误 */
  ASSERT_ERR (JsonDomFromHost ("{broken", &Dom));
  ASSERT_ERR (JsonDomFromHost ("", &Dom));
  ASSERT_ERR (JsonDomFromHost (NULL, &Dom));
  ASSERT_EQ (JsonDomFromHost ("{}", NULL), EFI_INVALID_PARAMETER);
  /* 空 object 根 → items 缺失错误 */
  ValidateText ("{}", &R);
  ASSERT_OK (R.DomSt);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, (UINTN)-1, "items", CFG_ERR_MISSING_FIELD) != NULL);
  FreeResult (&R);
  /* item 非 object */
  ValidateText ("{\"items\":[5]}", &R);
  ASSERT_TRUE (FindMsg (R.Errs, R.ErrCount, 0, "(item)", CFG_ERR_TYPE) != NULL);
  FreeResult (&R);
  /* options 允许 UTF-8（非 ASCII 不拒绝；glue 侧转 UCS-2） */
  ValidateText ("{\"items\":[{\"id\":\"a\",\"name\":\"n\",\"action\":\"exec\",\"path\":\"data:/x\",\"options\":\"\xe5\x8f\x82\xe6\x95\xb0\"}]}", &R);
  ASSERT_EQ (R.ErrCount, 0);
  ASSERT_EQ (strcmp (R.Cfg->Items[0].Options, "\xe5\x8f\x82\xe6\x95\xb0"), 0);
  FreeResult (&R);
  /* 非法参数 / NULL 释放安全 */
  ASSERT_EQ (ConfigValidate (NULL, NULL, NULL, NULL, NULL, NULL), EFI_INVALID_PARAMETER);
  ConfigFree (NULL);
  ConfigFreeErrors (NULL);
  JsonDomFree (NULL);
  /* DOM 助手边界 */
  ASSERT_TRUE (JsonObjectGet (NULL, "x") == NULL);
  ASSERT_EQ (JsonDomArrayCount (NULL), 0);
  ASSERT_TRUE (JsonArrayGet (NULL, 0) == NULL);
  /* INT64_MIN dumps */
  {
    CHAR8 *S = NULL;
    ASSERT_OK (JsonDomFromHost ("{\"v\":-9223372036854775808}", &Dom));
    ASSERT_OK (JsonDomDump (Dom, &S));
    ASSERT_TRUE (S != NULL && strcmp (S, "obj{v:-9223372036854775808}") == 0);
    MemFree (S);
    JsonDomFree (Dom);
    Dom = NULL;
  }
  /* 深嵌套防御（>64 层拒绝，不栈溢出） */
  {
    char Deep[512];
    size_t J;
    for (J = 0; J < 200; J++) Deep[J] = '[';
    for (J = 0; J < 200; J++) Deep[200 + J] = ']';
    Deep[400] = '\0';
    /* 200 层合法嵌套数组：jansson 接受（<2048），适配器深度守卫拒绝 */
    ASSERT_ERR (JsonDomFromHost (Deep, &Dom));
    ASSERT_TRUE (Dom == NULL);
  }
}

/* ---- T03-17 内存配对（计数分配器变体断言平衡；ASan 变体查越界/UAF） ---- */

#ifdef CFG_COUNT_MEM
extern INTN g_mem_balance;   /* tests/MemCount.c 计数分配器 */
#else
INTN g_mem_balance = 0;      /* ASan 变体占位；泄漏平衡由计数变体实证 */
#endif

TEST (t03_17_mem_balance) {
  ASSERT_EQ (g_mem_balance, 0);
}

int main (void) {
  RUN_TEST (t03_1_golden);
  RUN_TEST (t03_2_missing_id);
  RUN_TEST (t03_3_non_ascii);
  RUN_TEST (t03_4_too_long);
  RUN_TEST (t03_5_bad_arch);
  RUN_TEST (t03_6_float_rejected);
  RUN_TEST (t03_7_dup_id);
  RUN_TEST (t03_8_bad_path);
  RUN_TEST (t03_8b_path_s04_semantics);
  RUN_TEST (t03_9_bad_action);
  RUN_TEST (t03_10_skip_timeout);
  RUN_TEST (t03_11_empty_items);
  RUN_TEST (t03_12_root_type);
  RUN_TEST (t03_13_unknown_fields);
  RUN_TEST (t03_14_danger_confirm);
  RUN_TEST (t03_15_dom_isomorphism);
  RUN_TEST (t03_16_large_config);
  RUN_TEST (extra_adapter_edges);
  RUN_TEST (t03_17_mem_balance);
  RUN_TESTS_END ();
}