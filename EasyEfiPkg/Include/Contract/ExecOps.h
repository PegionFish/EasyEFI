/** @file
  执行 Ops（契约，S00 冻结；S10 实现 UEFI 后端，S00 提供 mock 记录器）。
  语义依据 design §5.6 与评审 F01/F03/F20：
  - .nsh 走嵌套 Shell：LoadOptions = L"<脚本全限定路径> [参数]"（file-name 隐式 -nostartup）
  - auto: 候选排除启动盘全部分区
**/
#ifndef EXEC_OPS_H
#define EXEC_OPS_H

#include "EfiCompat.h"
#include "FsOps.h"

typedef struct {
  CHAR16  Display[128];    /* 候选展示名（卷标签+路径） */
  CHAR16  FullPath[512];   /* 运行时全限定路径 */
  FS_OPS  *Fs;             /* 所在卷（不拥有） */
} EXEC_CANDIDATE;

/* LoadImage+StartImage；调用前清理自身事件/保存控制台，返回后恢复；
   ImageReturn 捕获目标返回状态（目标 ExitBootServices 则不返回） */
EFI_STATUS ExecEfi (FS_OPS *Fs, CONST CHAR16 *FullPath, CONST CHAR16 *LoadOptions,
                    EFI_STATUS *ImageReturn);

/* 嵌套 Shell 执行脚本；ShellFullPath=本 ESP 的 Shell.efi；脚本结束后停在嵌套 Shell 提示符 */
EFI_STATUS ExecNsh (CONST CHAR16 *ShellFullPath, CONST CHAR16 *ScriptFullPath,
                    CONST CHAR16 *Args);

EFI_STATUS ExecBootNext (UINT16 Number);   /* BootNext(BS|RT) + ResetSystem(EfiResetCold) */
EFI_STATUS ExecReset (VOID);

/* auto: 候选枚举；BootDp/BootLen=启动盘身份（排除同盘）；0 命中返回 EFI_NOT_FOUND */
EFI_STATUS ExecAutoCandidates (CONST CHAR16 *RelPath, CONST UINT8 *BootDp, UINTN BootLen,
                               EXEC_CANDIDATE **Out, UINTN *Count);
VOID       ExecFreeCandidates (EXEC_CANDIDATE *List, UINTN Count);

#endif
