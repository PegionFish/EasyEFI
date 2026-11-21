/** @file
  S11 冒烟目标应用：打印标记 + 返回 EFI_SUCCESS。
  用作 ExecEfi 目标（fat 场景置于 EFI/Apps/FileManager.efi 供内置项执行；
  ntfs 场景复制为 P3 entries/target.efi 供 data: 项与 danger 项执行）。
  S12/S13 合并后 FM/Editor 为真实应用，冒烟改用本专用目标（标记稳定）。
**/
#include <Uefi.h>
#include <Library/UefiLib.h>

EFI_STATUS EFIAPI UefiMain (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
#if defined(MDE_CPU_X64)
  Print (L"[S11-TARGET] ran x64\n");
#elif defined(MDE_CPU_AARCH64)
  Print (L"[S11-TARGET] ran aarch64\n");
#else
  Print (L"[S11-TARGET] ran unknown-arch\n");
#endif
  return EFI_SUCCESS;
}
