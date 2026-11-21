# EasyEFI compat-layout x64 script (invoked by ShellRunner trampoline as Shell file-name arg)
# NOTE: nested Shell with file-name does NOT set %homefilesystem%; NTFS driver preloaded by ShellRunner
echo -off
map -r
if exist noautorun then
  echo [EasyEFI] noautorun flag on boot partition; staying in UEFI Shell.
else
  EFI\Launcher\LauncherX64.efi
endif
