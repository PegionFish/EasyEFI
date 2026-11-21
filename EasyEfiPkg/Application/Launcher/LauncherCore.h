/** @file
  Launcher core/glue 缝（S11 模块内部头，非契约）。

  core = 纯状态机（宿主可测）：只依赖 TuiScreen/TuiInput/ExecOps/FsOps/ExecPath/EfiMem，
  不触 gBS/gRT/Shell 协议/JsonLib。菜单/确认/帮助/错误页渲染在 core 内实现
  （编号/架构灰显/danger 标记/desc 展开属业务语义，TuiWidget 契约明确不管业务）。
  glue = UEFI 装配（LauncherGlue.c）：P3 发现/JSON 加载/变量读取/EASYEFI_RAN，
  持有 LAUNCHER_SESSION 与 CONFIG 生命周期，经 LAUNCHER_PLATFORM_OPS 提供回调。

  路径约定（对齐 S10 实际实现，见 docs/handoffs/S11.md）：
  ExecEfi = 活 FS_OPS + 根相对路径（S10 RootRel 只去前导斜杠，不剥 fsN: 前缀）；
  ExecNsh = fsN: 全限定路径（S10 强制含 ':'）。auto: 候选 Fs 经 glue AutoVol 回调
  解析活卷（S09 VolFree 销毁后端，S10 候选借用 Fs 悬空——已报 leader）。
**/
#ifndef LAUNCHER_CORE_H
#define LAUNCHER_CORE_H

#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/TuiTheme.h"
#include "Contract/TuiScreen.h"
#include "Contract/TuiInput.h"
#include "Contract/FsOps.h"
#include "Contract/ExecOps.h"
#include "Contract/ExecPath.h"
#include "Contract/ConfigSchema.h"

/* 配置文件读取上限（MVP 常量；建议 leader 增设 PcdLauncherConfigMaxBytes，见交接注记） */
#define LAUNCHER_CONFIG_MAX_BYTES  65536

typedef enum {
  LSTATE_COUNTDOWN = 0,   /* skip_timeout 倒计时 */
  LSTATE_MENU,            /* 主菜单 */
  LSTATE_CONFIRM,         /* Y/N 二次确认（默认 N） */
  LSTATE_ERROR,           /* 错误页（任意键回菜单） */
  LSTATE_BOOT_MENU,       /* boot:menu NVRAM 子菜单 */
  LSTATE_CANDIDATES,      /* auto: 多命中候选选择 */
  LSTATE_HELP             /* 帮助页（键位/警告/被跳过条目原因） */
} LAUNCHER_STATE;

typedef enum {
  LEXIT_NONE = 0,
  LEXIT_SHELL,            /* 回父 Shell（S/Q/ESC/shell 条目）；glue 置 EASYEFI_RAN 后返回 */
  LEXIT_RESET             /* 已发出 ExecReset/ExecBootNext（机器复位中），glue 直接返回 */
} LAUNCHER_EXIT;

/* boot:menu 枚举项（glue 从 BootOrder/Boot#### 变量填充，仅 ACTIVE） */
typedef struct {
  UINT16 BootNumber;
  CHAR16 Description[128];
} LAUNCHER_BOOT_ENTRY;

/* glue 持有并原地更新的会话状态；core 只读借用。Reload 回调内 glue 负责
   ConfigFree 旧 CONFIG/错误清单并换上新值（core 不链接 S03 符号）。 */
typedef struct {
  CONST CONFIG       *Cfg;        /* NULL = 内置默认菜单（JSON 缺失/失败） */
  CONST CONFIG_ERROR *Errs;       /* 被跳过非法项原因（帮助页），可 NULL */
  UINTN              ErrCount;
  CONST CONFIG_ERROR *Warns;      /* 警告（帮助页），可 NULL */
  UINTN              WarnCount;
  FS_OPS             *P3Fs;       /* NULL = DEGRADED */
  CHAR16             P3FsName[16];    /* P3 运行时映射名 L"fsN:"；空=未知 */
  CHAR16             BootFsName[16];  /* 启动 ESP 映射名；空=未知 */
  FS_OPS             *BootFs;     /* 启动 ESP 卷（可 NULL） */
  CHAR8              Banner[256]; /* 错误横幅 ASCII（空=无）；DEGRADED 横幅由 core 按 P3Fs 推导 */
} LAUNCHER_SESSION;

/* glue 平台回调（core 不触固件服务） */
typedef struct {
  VOID *Ctx;
  /* R 键：重扫卷 + 重载 launcher.json，原地更新 Sess（含 P3Fs/映射名/Banner）。
     glue 内部 ConfigFree 旧配置。返回错误 → core 显示错误页。 */
  EFI_STATUS (*Reload) (VOID *Ctx);
  /* boot:menu 枚举（BootOrder/Boot####，仅 ACTIVE）。callee MemAlloc 数组，core MemFree。 */
  EFI_STATUS (*BootEntries) (VOID *Ctx, LAUNCHER_BOOT_ENTRY **Out, UINTN *Count);
  /* 显式 fsN: 卷号 → 活 FS_OPS（glue 经 Shell 映射/活 Vols 解析）。失败 → 错误页。
     可 NULL（宿主 mock 无此回调时 fsN: 执行降级错误页）。 */
  EFI_STATUS (*FsByIndex) (VOID *Ctx, UINTN Index, FS_OPS **Fs);
  /* auto: 候选 → 活卷解析（S10 候选 Fs 借用自已释放枚举集，不可信；glue 在活 Vols
     中按相对路径存在性+排除启动盘+Display 标签优先匹配）。FsName 供 .nsh 全限定构造。
     可 NULL（宿主 mock 直接用候选 Fs/全限定 FullPath）。 */
  EFI_STATUS (*AutoVol) (VOID *Ctx, CONST CHAR16 *RelPath, CONST CHAR16 *Display,
                         FS_OPS **Fs, CHAR16 *FsName, UINTN FsNameCap);
} LAUNCHER_PLATFORM_OPS;

typedef struct {
  LAUNCHER_SESSION      *Sess;            /* glue 持有，必填 */
  CONST UINT8           *BootDp;          /* 启动盘身份（auto: 排除用），可 NULL/0 */
  UINTN                 BootDpLen;
  UINT8                 CurrentArch;      /* ARCH_X64_BIT / ARCH_AARCH64_BIT 单比特 */
  CONST CHAR8           *SbStatus;        /* ASCII "SB:ON"/"SB:OFF"/"SB:?"，NULL→"SB:?" */
  CONST CHAR16          *FileManagerRel;  /* 启动 ESP 相对路径（glue 按布局/架构解析） */
  CONST CHAR16          *EditorRel;
  CONST CHAR16          *ShellRel;        /* 本 ESP Shell.efi 相对路径（ExecNsh 用） */
  LAUNCHER_PLATFORM_OPS *Ops;             /* 可 NULL（R/boot:menu 降级为错误页） */
  UINTN                 SkipTimeout;      /* 生效倒计时秒（glue: cfg?cfg->SkipTimeout:PcdDefault），0=禁用 */
} LAUNCHER_INIT;

typedef struct LAUNCHER_CORE LAUNCHER_CORE;

LAUNCHER_CORE  *LauncherCoreNew (CONST LAUNCHER_INIT *Init);
VOID            LauncherCoreFree (LAUNCHER_CORE *C);
LAUNCHER_STATE  LauncherCoreState (CONST LAUNCHER_CORE *C);
LAUNCHER_EXIT   LauncherCoreExit (CONST LAUNCHER_CORE *C);
UINTN           LauncherCoreSelected (CONST LAUNCHER_CORE *C);
UINTN           LauncherCoreCountdown (CONST LAUNCHER_CORE *C);
EFI_STATUS      LauncherCoreDraw (LAUNCHER_CORE *C);              /* 渲染当前状态 + TuiFlush */
EFI_STATUS      LauncherCoreHandleKey (LAUNCHER_CORE *C, CONST TUI_KEY *Key);
EFI_STATUS      LauncherCoreTick (LAUNCHER_CORE *C);              /* 倒计时 -1s；归零 → MENU */

#endif
