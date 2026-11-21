# ExecVolumeLib 模块碎片（leader 聚合进 EasyEfiPkg.dsc；流负责人只改本文件）
# VolumeModelLib 走全局解析（S05 真实实现已合入 main；S09 interim 已移除）。
[LibraryClasses]
  ExecVolumeLib|EasyEfiPkg/Library/ExecVolumeLib/ExecVolumeLib.inf

[Components]
  EasyEfiPkg/Library/ExecVolumeLib/ExecVolumeLib.inf {
    <LibraryClasses>
      # TEMPORARY：EfiMem.c ASSERT 缺 DebugLib.h include（main 84c6f4c），
      # wrapper 强制包含；leader 修复后删除覆盖 + EfiCompatLibS09.inf。
      EfiCompatLib|EasyEfiPkg/Library/ExecVolumeLib/EfiCompatLibS09.inf
  }
  # S09 冒烟应用：fat / ntfs 变体自适应（卷枚举 + FsOps 读写闭环 + P3 发现/降级）
  EasyEfiPkg/Library/ExecVolumeLib/smoke/SmokeS09.inf {
    <LibraryClasses>
      EfiCompatLib|EasyEfiPkg/Library/ExecVolumeLib/EfiCompatLibS09.inf
  }
