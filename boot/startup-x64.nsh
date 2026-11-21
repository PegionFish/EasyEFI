# EasyEFI compat-layout x64 script (invoked by ShellRunner trampoline as Shell file-name arg)
# NOTE: nested Shell with file-name does NOT set %homefilesystem% -> relative paths (cwd = boot fs root)
echo -off
load EFI\Drivers\ntfs_x64.efi
map -r
if exist noautorun then
  echo [EasyEFI] noautorun flag on boot partition - staying in UEFI Shell.
else
  EFI\Launcher\LauncherX64.efi
endif
