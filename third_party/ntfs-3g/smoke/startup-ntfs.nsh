echo -off
load %homefilesystem%\EFI\Drivers\ntfs.efi
map -r
%homefilesystem%\EFI\Apps\NtfsProbe.efi