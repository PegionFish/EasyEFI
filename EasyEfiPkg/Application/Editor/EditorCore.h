/** @file
  EditorCore — S13 Editor 核心内部接口（core/glue 纪律：core 只依赖契约缝）。
  缝：屏幕/按键 = TuiScreen/TuiInput 契约函数（宿主 MockTui / UEFI S06/S07 库）；
  文件 = FS_OPS vtable（宿主 MockFs / UEFI S09 后端）；模型 = EditorModelLib（S02）。
  本头仅供 EditorGlue.c 与宿主测试使用，不是公共契约。
**/
#ifndef EDITOR_CORE_H
#define EDITOR_CORE_H

#include "Contract/EfiCompat.h"
#include "Contract/FsOps.h"
#include "Contract/EditorModel.h"
#include "Contract/TuiInput.h"

typedef enum {
  ED_EDIT = 0,   /* 正常编辑 */
  ED_FIND,       /* 查找输入行（TAB 切换大小写） */
  ED_GOTO,       /* 跳行输入 */
  ED_SAVEAS,     /* 另存为输入（AsciiOnly） */
  ED_CONFIRM,    /* 修改退出三键：S=保存退出 D=放弃 ESC=取消 */
  ED_ERROR,      /* 错误页（任意键返回/退出） */
  ED_DONE        /* 已退出 */
} ED_STATE;

typedef struct EDITOR_APP EDITOR_APP;

/* MaxBytes 必须 >0（glue 传 PcdEditorMaxFileSize）；UndoDepth 传 PcdUndoDepth。
   内部 TuiInit + TuiGetSize（宿主测试先 MockTuiSetSize）。 */
EDITOR_APP   *EdNew (FS_OPS *Fs, UINTN MaxBytes, UINTN UndoDepth);
VOID          EdFree (EDITOR_APP *E);
/* 打开文件（字节经 FsOps 读 → ModelLoad）。失败 → ED_ERROR（致命，任意键退出）。 */
EFI_STATUS    EdOpen (EDITOR_APP *E, CONST CHAR16 *Path);
VOID          EdRender (EDITOR_APP *E);
EFI_STATUS    EdKey (EDITOR_APP *E, CONST TUI_KEY *Key);
/* 渲染+等键+分发循环；TuiWaitKey 错误（宿主 mock 队列空 NOT_READY）→ 返回。 */
VOID          EdRun (EDITOR_APP *E);
ED_STATE      EdState (CONST EDITOR_APP *E);
EDITOR_MODEL *EdModel (EDITOR_APP *E);
CONST CHAR16 *EdPath (CONST EDITOR_APP *E);
UINTN         EdExitCode (CONST EDITOR_APP *E);
CONST CHAR16 *EdMsg (CONST EDITOR_APP *E);

#endif
