/** @file
  ExecImageLib 内部平台缝（internal seam —— 非冻结契约，仅本库内部使用）。

  双环境核心 ExecImage.c 只经由此表触碰平台服务：
  - UEFI 构建：ExecImageUefi.c 提供 gBS/gRT/gST 真实实现；
  - 宿主测试：tests/HostSeam.c 提供 mock 记录器（T10-1..4 断言调用序列/参数）。

  契约面（ExecOps.h）不变；本头是 core/glue 纪律在库内的落地缝。
**/
#ifndef EXEC_IMAGE_SEAM_H
#define EXEC_IMAGE_SEAM_H

#include "Contract/EfiCompat.h"

/* EFI_RESET_TYPE 值（UEFI Spec；核心不 include Uefi.h） */
#define EXEC_RESET_COLD      0
#define EXEC_RESET_WARM      1
#define EXEC_RESET_SHUTDOWN  2

/* EFI_VARIABLE_* 属性位 */
#define EXEC_VAR_NV  0x00000001
#define EXEC_VAR_BS  0x00000002
#define EXEC_VAR_RT  0x00000004

typedef struct {
  /* gBS->LoadImage(BootPolicy=FALSE, Parent=自身) + 经 LoadedImage 协议设置
     LoadOptions/LoadOptionsSize。Dp=完整设备路径字节（卷前缀+文件节点+END）。
     Options 可为 NULL（OptionsSize=0）。成功时 *ImageHandle 有效。 */
  EFI_STATUS (*LoadImage)(CONST UINT8 *Dp, UINTN DpLen, CONST CHAR16 *Options,
                          UINTN OptionsSize, VOID **ImageHandle);

  /* gBS->StartImage；*ImageReturn=目标返回码（StartImage 返回值即目标 Exit 状态，
     已核实 MdeModulePkg/Core/Dxe/Image/Image.c CoreStartImage）。 */
  EFI_STATUS (*StartImage)(VOID *ImageHandle, EFI_STATUS *ImageReturn);

  /* StartImage 前：保存 ConOut 模式 / 关闭自身定时器事件；返回后：恢复 ConOut 模式 */
  EFI_STATUS (*ConsoleSave)(VOID);
  EFI_STATUS (*EventsClose)(VOID);
  EFI_STATUS (*ConsoleRestore)(VOID);

  /* gRT->SetVariable；Guid16=EFI_GUID 的 16 字节内存布局（小端） */
  EFI_STATUS (*SetVariable)(CONST CHAR16 *Name, CONST UINT8 *Guid16,
                            UINT32 Attributes, CONST VOID *Data, UINTN Size);

  /* gRT->ResetSystem；UEFI 上不返回 */
  VOID (*ResetSystem)(UINT32 ResetType);

  /* 启动卷（本镜像加载来源卷）设备路径，含 END 节点；callee 以 MemAlloc 分配 */
  EFI_STATUS (*BootVolumeDp)(UINT8 **Dp, UINTN *DpLen);
} EXEC_SEAM;

extern CONST EXEC_SEAM *gExecSeam;

#endif /* EXEC_IMAGE_SEAM_H */