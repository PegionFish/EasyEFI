/* S10 宿主测试：内部缝 mock 记录器（TEST-ONLY）。记录调用序列与参数，
   供 T10-1..4 断言。gExecSeam 在宿主构建中由本文件定义。 */
#include "Contract/EfiCompat.h"
#include "Contract/EfiMem.h"
#include "ExecImageSeam.h"
#include "HostSeam.h"

#define SEAM_MAX 64

static SEAM_CALL g_calls[SEAM_MAX];
static UINTN g_count = 0;
static EFI_STATUS g_load_status = EFI_SUCCESS;
static EFI_STATUS g_child_status = EFI_SUCCESS;
static EFI_STATUS g_var_status = EFI_SUCCESS;
static UINT8 g_boot_dp[1024];
static UINTN g_boot_dp_len = 0;
static UINTN g_handle_seq = 0;

void SeamReset (VOID) {
  g_count = 0;
  g_load_status = EFI_SUCCESS;
  g_child_status = EFI_SUCCESS;
  g_var_status = EFI_SUCCESS;
  g_boot_dp_len = 0;
  g_handle_seq = 0;
}
UINTN SeamCount (VOID) { return g_count; }
CONST SEAM_CALL *SeamCall (UINTN I) { return I < g_count ? &g_calls[I] : NULL; }
void SeamSetLoadStatus (EFI_STATUS S) { g_load_status = S; }
void SeamSetChildStatus (EFI_STATUS S) { g_child_status = S; }
void SeamSetVarStatus (EFI_STATUS S) { g_var_status = S; }
void SeamSetBootVolDp (CONST UINT8 *Dp, UINTN Len) {
  UINTN i;
  if (Len > sizeof (g_boot_dp)) Len = sizeof (g_boot_dp);
  for (i = 0; i < Len; i++) g_boot_dp[i] = Dp[i];
  g_boot_dp_len = Len;
}

static SEAM_CALL *rec (UINT32 Kind) {
  SEAM_CALL *C;
  if (g_count >= SEAM_MAX) return NULL;
  C = &g_calls[g_count++];
  { UINTN i; UINT8 *p = (UINT8 *)C; for (i = 0; i < sizeof (*C); i++) p[i] = 0; }
  C->Kind = Kind;
  return C;
}

static EFI_STATUS mLoadImage (CONST UINT8 *Dp, UINTN DpLen, CONST CHAR16 *Options,
                              UINTN OptionsSize, VOID **ImageHandle) {
  SEAM_CALL *C = rec (SEAM_LOAD_IMAGE);
  UINTN i;
  if (C == NULL) return EFI_OUT_OF_RESOURCES;
  if (DpLen > sizeof (C->Dp)) return EFI_BAD_BUFFER_SIZE;
  for (i = 0; i < DpLen; i++) C->Dp[i] = Dp[i];
  C->DpLen = DpLen;
  if (Options != NULL) {
    for (i = 0; Options[i] != 0 && i < 511; i++) C->Options[i] = Options[i];
    C->Options[i] = 0;
  }
  C->OptionsSize = OptionsSize;
  if (EFI_ERROR (g_load_status)) return g_load_status;
  *ImageHandle = (VOID *)(UINTN)(0x1000 + ++g_handle_seq);
  return EFI_SUCCESS;
}

static EFI_STATUS mStartImage (VOID *ImageHandle, EFI_STATUS *ImageReturn) {
  SEAM_CALL *C = rec (SEAM_START_IMAGE);
  (VOID)ImageHandle;
  if (C == NULL) return EFI_OUT_OF_RESOURCES;
  C->ChildWritten = g_child_status;
  if (ImageReturn != NULL) *ImageReturn = g_child_status;
  return g_child_status;
}

static EFI_STATUS mConsoleSave (VOID) { rec (SEAM_CONSOLE_SAVE); return EFI_SUCCESS; }
static EFI_STATUS mEventsClose (VOID) { rec (SEAM_EVENTS_CLOSE); return EFI_SUCCESS; }
static EFI_STATUS mConsoleRestore (VOID) { rec (SEAM_CONSOLE_RESTORE); return EFI_SUCCESS; }

static EFI_STATUS mSetVariable (CONST CHAR16 *Name, CONST UINT8 *Guid16,
                                UINT32 Attributes, CONST VOID *Data, UINTN Size) {
  SEAM_CALL *C = rec (SEAM_SET_VARIABLE);
  UINTN i;
  CONST UINT8 *D = (CONST UINT8 *)Data;
  if (C == NULL) return EFI_OUT_OF_RESOURCES;
  for (i = 0; Name[i] != 0 && i < 63; i++) C->VarName[i] = Name[i];
  C->VarName[i] = 0;
  for (i = 0; i < 16; i++) C->Guid[i] = Guid16[i];
  C->Attr = Attributes;
  if (Size > sizeof (C->Data)) return EFI_BAD_BUFFER_SIZE;
  for (i = 0; i < Size; i++) C->Data[i] = D[i];
  C->DataSize = Size;
  return g_var_status;
}

static VOID mResetSystem (UINT32 ResetType) {
  SEAM_CALL *C = rec (SEAM_RESET_SYSTEM);
  if (C != NULL) C->ResetType = ResetType;
}

static EFI_STATUS mBootVolumeDp (UINT8 **Dp, UINTN *DpLen) {
  UINT8 *Copy;
  UINTN i;
  rec (SEAM_BOOT_VOLUME_DP);
  if (g_boot_dp_len == 0) return EFI_NOT_FOUND;
  Copy = MemAlloc (g_boot_dp_len);
  if (Copy == NULL) return EFI_OUT_OF_RESOURCES;
  for (i = 0; i < g_boot_dp_len; i++) Copy[i] = g_boot_dp[i];
  *Dp = Copy;
  *DpLen = g_boot_dp_len;
  return EFI_SUCCESS;
}

static CONST EXEC_SEAM mHostSeam = {
  mLoadImage, mStartImage, mConsoleSave, mEventsClose, mConsoleRestore,
  mSetVariable, mResetSystem, mBootVolumeDp
};

CONST EXEC_SEAM *gExecSeam = &mHostSeam;