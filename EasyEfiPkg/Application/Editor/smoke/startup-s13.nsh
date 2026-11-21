echo -off
load %homefilesystem%\EFI\Drivers\ntfs.efi
map -r
echo [S13] smoke begin
%homefilesystem%\EFI\Apps\Editor.efi fs1:\s13test.nsh
echo [S13] first session done
type fs1:\s13test.nsh
echo [S13] readback-1 done
%homefilesystem%\EFI\Apps\Editor.efi s13test.nsh
echo [S13] second session done
type fs1:\s13test.nsh
echo [S13] readback-2 done
