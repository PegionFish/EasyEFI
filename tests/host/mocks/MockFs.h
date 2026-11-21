/* MockFs 测试接口（S00） */
#ifndef MOCK_FS_H
#define MOCK_FS_H
#include "Contract/FsOps.h"
FS_OPS *MockFsNew (VOID);
void MockFsAddFile (FS_OPS *Fs, CONST CHAR16 *Path, CONST UINT8 *Data, UINTN Len);
void MockFsAddDir (FS_OPS *Fs, CONST CHAR16 *Path);
#endif
