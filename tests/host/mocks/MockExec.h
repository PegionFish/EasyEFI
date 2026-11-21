/* MockExec 测试接口（S00） */
#ifndef MOCK_EXEC_H
#define MOCK_EXEC_H
#include "Contract/ExecOps.h"

typedef struct {
  UINT32 Kind;              /* 1=ExecEfi 2=ExecNsh 3=BootNext 4=Reset */
  CHAR16 Path[512];
  CHAR16 Options[512];
  UINT16 BootNumber;
} MOCK_EXEC_CALL;

void MockExecReset (VOID);
void MockExecSetImageReturn (EFI_STATUS S);
UINTN MockExecCount (VOID);
CONST MOCK_EXEC_CALL *MockExecCall (UINTN I);
void MockExecSetCandidates (EXEC_CANDIDATE *C, UINTN N);
#endif
