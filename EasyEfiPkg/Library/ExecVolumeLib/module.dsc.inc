# ExecVolumeLib 模块碎片（leader 聚合进 EasyEfiPkg.dsc；流负责人只改本文件）
[LibraryClasses]
  ExecVolumeLib|EasyEfiPkg/Library/ExecVolumeLib/ExecVolumeLib.inf

[Components]
  EasyEfiPkg/Library/ExecVolumeLib/ExecVolumeLib.inf {
    <LibraryClasses>
      # INTERIM（S09）：S05 VolumeModelLib 真实实现合入 main 后删除本覆盖
      # （连同 VolumeModelInterim.c/.inf），恢复全局 VolumeModelLib 解析。
      VolumeModelLib|EasyEfiPkg/Library/ExecVolumeLib/VolumeModelInterim.inf
  }
  # S09 fat 变体冒烟应用（卷枚举 + FAT 读写 + P3 降级路径）
  EasyEfiPkg/Library/ExecVolumeLib/smoke/SmokeS09.inf {
    <LibraryClasses>
      VolumeModelLib|EasyEfiPkg/Library/ExecVolumeLib/VolumeModelInterim.inf
  }
