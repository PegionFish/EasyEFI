/** @file
  FileManager core/glue 缝（S12 模块内部头，非契约）。

  core = 纯状态机（宿主可测，S11 同构）：只依赖 TuiScreen/TuiInput/FsOps/ExecOps/
  TuiTheme/EfiMem，不触 gBS/gST/Shell 协议。渲染在 core 内自绘（业务语义：
  预览/过滤/确认内容属业务，TuiWidget 契约明确不管业务；与 S11 先例一致，
  不链接 TuiWidgetLib，stub 期即可宿主全量测试）。
  glue = FileManagerGlue.c：VolEnumerate/映射名解析/PCD/入口循环。

  路径约定（与 S11/S10 对齐）：ExecEfi/ExecNsh 的 FullPath 一律 Shell 全限定
  fsN:/...（FsNames[卷] 由 glue 提供）；Fs 参数=所属卷 FS_OPS。
  core 内部路径一律卷相对（""=根，无首尾 '/'，'/' 分隔），与 FsOps Dir 相对语义一致。

  键位（design §7.2）：↑↓/PgUp/PgDn/Home/End 导航；Enter 进入目录/执行文件；
  Backspace/ESC 上级（DIR 态先编辑/清除过滤）；可打印键入即过滤；F3 预览文本/hex；
  F4 编辑（二进制拒绝）；F5 复制（选目标卷，覆盖确认）；F6 改名；F7 新建目录/文件
  （Tab 切换类型）；F8 删除（文件或空目录，确认）；V 卷列表；S 递归文件名搜索；
  R 刷新重扫；F10 退出。确认一律默认 N（Y 执行，N/ESC 取消）。
**/
#ifndef FM_CORE_H
#define FM_CORE_H

#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "Contract/TuiTheme.h"
#include "Contract/TuiScreen.h"
#include "Contract/TuiInput.h"
#include "Contract/FsOps.h"
#include "Contract/ExecOps.h"

/* 结构上限（MVP 常量；搜索命中/路径长度边界，超限截断并提示，见交接注记） */
#define FM_MAX_PATH      512
#define FM_MAX_NAME      128
#define FM_MAX_RESULTS   256
#define FM_MAX_FILTER    128

typedef enum {
  FM_STATE_VOLUMES = 0,   /* 卷列表（CopyMode 时为复制目标选择） */
  FM_STATE_DIR,           /* 目录视图 + 右侧预览 + 键入即过滤 */
  FM_STATE_RESULTS,       /* 递归搜索结果列表 */
  FM_STATE_CONFIRM,       /* Y/N 确认（执行/删除/覆盖），默认 N */
  FM_STATE_INPUT,         /* 改名/新建/搜索词输入 */
  FM_STATE_MESSAGE        /* 错误/提示页（任意键返回） */
} FM_STATE;

typedef enum {
  FM_CONFIRM_EXEC = 0,    /* 执行文件：全路径 + 大小 */
  FM_CONFIRM_DELETE,      /* 删除文件或空目录 */
  FM_CONFIRM_OVERWRITE    /* 复制目标已存在 */
} FM_CONFIRM_KIND;

typedef enum {
  FM_INPUT_RENAME = 0,
  FM_INPUT_NEW,           /* Tab 切换 目录/文件 */
  FM_INPUT_SEARCH
} FM_INPUT_KIND;

/* glue 平台回调（core 不触固件服务）；可 NULL（R 降级为仅重读当前目录） */
typedef struct {
  VOID *Ctx;
  /* R/V 刷新：glue 重扫卷（VolFree 旧 + VolEnumerate 新），原地更新
     Init 借用指针指向的 Vols/VolCount/FsNames。core 随后钳制卷号并重读目录。 */
  EFI_STATUS (*Rescan) (VOID *Ctx);
} FM_PLATFORM_OPS;

typedef struct {
  VOLUME_INFO     *Vols;          /* glue 持有，core 借用；Count=0 → 仅 MESSAGE */
  UINTN           VolCount;
  CONST CHAR16    **FsNames;      /* 与 Vols 平行的 Shell 映射名数组 L"fsN:" */
  CONST CHAR16    *EditorPath;    /* 全限定 fsN:/EFI/Apps/Editor.efi（glue 按架构布局解析） */
  CONST CHAR16    *ShellPath;     /* 全限定 fsN:/.../Shell.efi（ExecNsh 用） */
  UINTN           PreviewMaxBytes;/* PcdPreviewMaxBytes（glue 传入，不硬编码） */
  FM_PLATFORM_OPS *Ops;
} FM_INIT;

typedef struct FM_CORE FM_CORE;

FM_CORE    *FmCoreNew (CONST FM_INIT *Init);
VOID        FmCoreFree (FM_CORE *C);
EFI_STATUS  FmCoreDraw (FM_CORE *C);                          /* 渲染当前状态 + TuiFlush */
EFI_STATUS  FmCoreHandleKey (FM_CORE *C, CONST TUI_KEY *Key); /* 纯状态转移，无内部阻塞循环 */

/* 状态访问器（宿主测试/调试用） */
FM_STATE        FmCoreState (CONST FM_CORE *C);
BOOLEAN         FmCoreExit (CONST FM_CORE *C);
UINTN           FmCoreVolIndex (CONST FM_CORE *C);
CONST CHAR16   *FmCoreCwd (CONST FM_CORE *C);          /* 当前卷相对目录，""=根 */
UINTN           FmCoreViewCount (CONST FM_CORE *C);    /* 过滤后可见条目数 */
UINTN           FmCoreSelected (CONST FM_CORE *C);     /* 过滤视图内选中下标 */
CONST FS_ENTRY *FmCoreEntry (CONST FM_CORE *C, UINTN ViewIdx); /* 视图下标→条目（越界 NULL） */
CONST CHAR16   *FmCoreFilter (CONST FM_CORE *C);
UINTN           FmCoreResultCount (CONST FM_CORE *C);
FM_CONFIRM_KIND FmCoreConfirmKind (CONST FM_CORE *C);
FM_INPUT_KIND   FmCoreInputKind (CONST FM_CORE *C);
BOOLEAN         FmCorePreviewBinary (CONST FM_CORE *C);
BOOLEAN         FmCorePreviewHex (CONST FM_CORE *C);
UINTN           FmCorePreviewLen (CONST FM_CORE *C);   /* 实际读入预览字节数（bounded 断言用） */

#endif
