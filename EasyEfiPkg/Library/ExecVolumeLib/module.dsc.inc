# ExecVolumeLib 模块碎片（leader 聚合进 EasyEfiPkg.dsc；流负责人只改本文件）
# VolumeModelLib / EfiCompatLib 均走全局解析（S05 真实实现与 EfiMem DebugLib
# 修复已合入 main；S09 的 interim/wrapper 已全部移除）。
[LibraryClasses]
  ExecVolumeLib|EasyEfiPkg/Library/ExecVolumeLib/ExecVolumeLib.inf

[Components]
  EasyEfiPkg/Library/ExecVolumeLib/ExecVolumeLib.inf
  # S09 冒烟应用：fat / ntfs 变体自适应（卷枚举 + FsOps 读写闭环 + P3 发现/降级）
  EasyEfiPkg/Library/ExecVolumeLib/smoke/SmokeS09.inf
