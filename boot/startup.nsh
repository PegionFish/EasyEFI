echo -off
# EasyEFI startup.nsh (strict layout, identical on both ESPs)
# Semantics verified against edk2-stable202608 Shell source + QEMU (design-review F02/F14/F25):
# - %homefilesystem% = boot partition map name, set by Shell before script runs
# - failed ordinary commands (load) do NOT abort scripts; no load-status detection here
# - degradation is Launcher's job (P3 discovery at runtime)
load %homefilesystem%\EFI\Drivers\ntfs.efi
map -r
if exist %homefilesystem%\noautorun then
  echo [EasyEFI] noautorun flag on boot partition; staying in UEFI Shell.
else
  %homefilesystem%\EFI\Launcher\Launcher.efi
endif
