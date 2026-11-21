/* S10 宿主测试：合成卷集 fixture 接口（TEST-ONLY，实现 S09 契约 VolEnumerate/VolFree）。 */
#ifndef HOST_VOL_H
#define HOST_VOL_H

#include "Contract/EfiCompat.h"
#include "Contract/FsOps.h"

void HostVolReset (VOID);
/* 注册一个卷：Dp 拷贝入注册表；Fs 实例指针保持稳定（模拟 S09 按卷缓存） */
void HostVolAdd (CONST CHAR16 *Label, CONST UINT8 *Dp, UINTN DpLen, FS_OPS *Fs);

#endif