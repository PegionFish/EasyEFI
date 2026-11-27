/** @file
  S06 宿主测试专用 mock ConOut（本流独占，非 tests/host/mocks 框架件）。
  记录全部调用序列（QueryMode/SetMode/SetCursorPosition/SetAttribute/OutputString），
  并按 edk2 GraphicsConsoleDxe 的真实语义维护虚拟屏幕：
    - 画到行末（CursorColumn >= MaxColumn）立即内部 CRLF；
    - 最后一行遇 LF → 整屏上滚（scroll 计数 +1），光标停在最后一行。
  因此 Flush 若错误触碰右下角格或让输出卷绕，scroll 计数会暴露问题。
  （TerminalDxe 光标钳制不滚动，比本 mock 宽松；通过本 mock 即通过两者。）
**/
#ifndef MOCK_CONOUT_H
#define MOCK_CONOUT_H

#include "Contract/EfiCompat.h"
#include "TuiScreenInternal.h"

#define MOCK_CONOUT_MAX_ROWS   256
#define MOCK_CONOUT_MAX_COLS   512
#define MOCK_CONOUT_MAX_MODES  16
#define MOCK_CONOUT_LOG_CAP    8192
#define MOCK_CONOUT_STR_CAP    300   /* 单次 OutputString 记录上限（超长截断） */

typedef enum {
  MOCK_CALL_QUERYMODE,
  MOCK_CALL_SETMODE,
  MOCK_CALL_SETCURSOR,
  MOCK_CALL_SETATTR,
  MOCK_CALL_OUTPUT
} MOCK_CALL_TYPE;

typedef struct {
  MOCK_CALL_TYPE Type;
  UINTN          A, B;      /* SetMode: A=mode；SetCursor: A=col B=row；SetAttr: A=attr；QueryMode: A=mode */
  UINTN          StrLen;    /* OutputString 实际长度（可能 > 记录上限） */
  CHAR16         Str[MOCK_CONOUT_STR_CAP];
  EFI_STATUS     Result;
} MOCK_CALL;

/* 重置：空模式表、CurrentMode=0、屏幕空白、日志清空 */
VOID MockConOutReset (VOID);
/* 模式表：Modes[i] = {Cols, Rows}；FailMask 位 i 置位 → 该模式 QueryMode 返回 EFI_UNSUPPORTED */
VOID MockConOutSetModes (CONST UINTN (*Modes)[2], UINTN Count, UINT32 FailMask);
VOID MockConOutSetCurrentMode (INTN Mode);
/* 模拟 OVMF ConSplitter  quirks：SetCursorPosition 拒绝 Col >= Limit（即使按模式尺寸在界内）。
   默认无限制。用于验证 TuiInit 的可寻址性探测降级。 */
VOID MockConOutSetCursorColLimit (UINTN Limit);
/* 注入用后端（MaxMode/CurrentMode 每次调用时刷新） */
TUI_CONOUT *MockConOutBackend (VOID);

UINTN           MockConOutTotalCalls (VOID);
UINTN           MockConOutCallCount (MOCK_CALL_TYPE Type);
CONST MOCK_CALL *MockConOutCall (UINTN Index);            /* 时间序全部调用 */
CONST MOCK_CALL *MockConOutLastOfType (MOCK_CALL_TYPE Type); /* 最近一次，无则 NULL */
TUI_CELL        MockConOutScreen (UINTN Row, UINTN Col);
UINTN           MockConOutScrolls (VOID);
UINTN           MockConOutCursorCol (VOID);
UINTN           MockConOutCursorRow (VOID);
BOOLEAN         MockConOutLogOverflow (VOID);

#endif /* MOCK_CONOUT_H */
