echo -off
rem EasyEFI startup.nsh (compat layout: both arch fallbacks in P1)
rem Arch detection does NOT use load status (EDK2 load swallows failures, F25).
rem Launcher sets EASYEFI_RAN=yes via Shell protocol on any normal exit;
rem wrong-arch Launcher is rejected by firmware LoadImage and never runs.
set EASYEFI_RAN no
load %homefilesystem%\EFI\Drivers\ntfs_x64.efi
load %homefilesystem%\EFI\Drivers\ntfs_aa64.efi
map -r
if exist %homefilesystem%\noautorun then
  echo [EasyEFI] noautorun flag on boot partition - staying in UEFI Shell.
else
  %homefilesystem%\EFI\Launcher\LauncherX64.efi
  if not %EASYEFI_RAN% == yes then
    %homefilesystem%\EFI\Launcher\LauncherAA64.efi
  endif
endif
