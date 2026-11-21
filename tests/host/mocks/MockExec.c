/* MockExec — ExecOps 宿主 mock 记录器（S00，leader 独占）。 */
#include "Contract/ExecOps.h"
#include "Contract/EfiMem.h"
#include "MockExec.h"
#include <string.h>

#define MOCK_EXEC_MAX 64

static MOCK_EXEC_CALL g_calls[MOCK_EXEC_MAX];
static UINTN g_count = 0;
static EFI_STATUS g_image_return = EFI_SUCCESS;
static EXEC_CANDIDATE *g_candidates = NULL;
static UINTN g_candidate_count = 0;

void MockExecReset (VOID) { g_count = 0; g_image_return = EFI_SUCCESS; }
void MockExecSetImageReturn (EFI_STATUS S) { g_image_return = S; }
UINTN MockExecCount (VOID) { return g_count; }
CONST MOCK_EXEC_CALL *MockExecCall (UINTN I) { return I < g_count ? &g_calls[I] : NULL; }
void MockExecSetCandidates (EXEC_CANDIDATE *C, UINTN N) { g_candidates = C; g_candidate_count = N; }

static void record (UINT32 Kind, CONST CHAR16 *Path, CONST CHAR16 *Opt) {
  if (g_count >= MOCK_EXEC_MAX) return;
  MOCK_EXEC_CALL *C = &g_calls[g_count++];
  memset (C, 0, sizeof (*C));
  C->Kind = Kind;
  if (Path) { UINTN i; for (i = 0; Path[i] && i < 511; i++) C->Path[i] = Path[i]; }
  if (Opt) { UINTN i; for (i = 0; Opt[i] && i < 511; i++) C->Options[i] = Opt[i]; }
}

EFI_STATUS ExecEfi (FS_OPS *Fs, CONST CHAR16 *FullPath, CONST CHAR16 *LoadOptions, EFI_STATUS *ImageReturn) {
  record (1, FullPath, LoadOptions);
  if (ImageReturn) *ImageReturn = g_image_return;
  return EFI_SUCCESS;
}
EFI_STATUS ExecNsh (CONST CHAR16 *ShellFullPath, CONST CHAR16 *ScriptFullPath, CONST CHAR16 *Args) {
  record (2, ScriptFullPath, Args);
  return EFI_SUCCESS;
}
EFI_STATUS ExecBootNext (UINT16 Number) {
  record (3, NULL, NULL);
  if (g_count) g_calls[g_count - 1].BootNumber = Number;
  return EFI_SUCCESS;
}
EFI_STATUS ExecReset (VOID) { record (4, NULL, NULL); return EFI_SUCCESS; }
EFI_STATUS ExecAutoCandidates (CONST CHAR16 *RelPath, CONST UINT8 *BootDp, UINTN BootLen,
                               EXEC_CANDIDATE **Out, UINTN *Count) {
  if (!g_candidate_count) return EFI_NOT_FOUND;
  *Out = g_candidates; *Count = g_candidate_count;
  return EFI_SUCCESS;
}
VOID ExecFreeCandidates (EXEC_CANDIDATE *List, UINTN Count) { /* mock 不拥有 */ }
