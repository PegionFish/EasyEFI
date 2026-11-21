/* S10 宿主测试：内部缝 mock 记录器接口（TEST-ONLY）。 */
#ifndef HOST_SEAM_H
#define HOST_SEAM_H

#include "Contract/EfiCompat.h"

typedef enum {
  SEAM_LOAD_IMAGE = 1,
  SEAM_START_IMAGE,
  SEAM_CONSOLE_SAVE,
  SEAM_EVENTS_CLOSE,
  SEAM_CONSOLE_RESTORE,
  SEAM_SET_VARIABLE,
  SEAM_RESET_SYSTEM,
  SEAM_BOOT_VOLUME_DP
} SEAM_CALL_KIND;

typedef struct {
  UINT32 Kind;
  /* LoadImage */
  UINT8  Dp[1024];
  UINTN  DpLen;
  CHAR16 Options[512];
  UINTN  OptionsSize;
  /* StartImage：写入 *ImageReturn 的值 */
  EFI_STATUS ChildWritten;
  /* SetVariable */
  CHAR16 VarName[64];
  UINT8  Guid[16];
  UINT32 Attr;
  UINT8  Data[64];
  UINTN  DataSize;
  /* ResetSystem */
  UINT32 ResetType;
} SEAM_CALL;

void SeamReset (VOID);
UINTN SeamCount (VOID);
CONST SEAM_CALL *SeamCall (UINTN I);
void SeamSetLoadStatus (EFI_STATUS S);      /* LoadImage 返回（默认 SUCCESS） */
void SeamSetChildStatus (EFI_STATUS S);     /* StartImage 返回 + *ImageReturn（默认 SUCCESS） */
void SeamSetVarStatus (EFI_STATUS S);       /* SetVariable 返回（默认 SUCCESS） */
void SeamSetBootVolDp (CONST UINT8 *Dp, UINTN Len);

#endif