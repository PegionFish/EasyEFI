/* S09 宿主测试：卷层纯逻辑核心。
 * T09-10 P3 发现算法（合成设备路径 + POSIX 后端临时目录卷集）；
 * T09-11 卷枚举排序与 isP3 标记（VolSortMark）+ VolFree 释放配对。
 * VolumeModel 契约函数：S05 合入前链接 VolumeModelInterim.c（host.mk 自动选择），
 * 合入后自动链接真实 VolumeModelLib——本文件断言对两者同样成立。 */
#include "test.h"
#include "TestUtil.h"
#include "DpSynth.h"
#include "Contract/FsOps.h"
#include "Contract/VolumeModel.h"
#include "ExecVolumeInternal.h"

UINTN TestMemLiveBlocks (void);
UINTN TestMemLiveBytes (void);

/* ---- 合成卷集 ---- */

#define MAXV 8

typedef struct {
  CHAR8  Root[128];
  FS_OPS *Fs;
  UINT8  Dp[256];
  UINTN  DpLen;
} TVOL;

static TVOL        g_tv[MAXV];
static UINTN       g_ntv;
static VOLUME_INFO g_vi[MAXV];

static UINT8 SIG_B1[16] = { 0xB1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
static UINT8 SIG_B3[16] = { 0xB3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3 };
static UINT8 SIG_O3[16] = { 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9 };

static void tv_reset (void) {
  UINTN i;
  for (i = 0; i < g_ntv; i++) {
    if (g_tv[i].Fs != NULL) g_tv[i].Fs->Destroy (g_tv[i].Fs);
    s09_rmtree (g_tv[i].Root);
  }
  g_ntv = 0;
}

/* 添加一卷：usbPort 区分物理盘（克隆盘=不同 port），partNum 分区号，
 * marker/.easyefi，json/launcher.json。设备路径 = USB 前缀 + HD(GPT) + END。 */
static TVOL *tv_add (UINT8 usbPort, UINT32 partNum, CONST UINT8 sig[16],
                     BOOLEAN marker, BOOLEAN json) {
  TVOL   *v = &g_tv[g_ntv++];
  DP_BUF b;
  strcpy (v->Root, "/tmp/s09vol.XXXXXX");
  ASSERT_TRUE (mkdtemp (v->Root) != NULL);
  v->Fs = FsPosixBackend (v->Root);
  ASSERT_TRUE (v->Fs != NULL);
  if (marker) s09_seed (v->Fs, U16 (".easyefi"), "easyefi-test-image v0\n");
  if (json)   s09_seed (v->Fs, U16 ("launcher.json"), "{}\n");
  DpInit (&b);
  DpDiskPrefixUsb (&b, usbPort);
  DpHdGpt (&b, partNum, 0x800, 0x100000, sig);
  DpEnd (&b);
  ASSERT_TRUE (b.Len <= sizeof (v->Dp));
  memcpy (v->Dp, b.Buf, b.Len);
  v->DpLen = b.Len;
  return v;
}

static void vi_build (void) {
  UINTN i;
  for (i = 0; i < g_ntv; i++) {
    memset (&g_vi[i], 0, sizeof (VOLUME_INFO));
    g_vi[i].Fs    = g_tv[i].Fs;
    g_vi[i].Dp    = g_tv[i].Dp;
    g_vi[i].DpLen = g_tv[i].DpLen;
  }
}

static void boot_dp (DP_BUF *b, UINT8 usbPort, UINT32 partNum, CONST UINT8 sig[16]) {
  DpInit (b);
  DpDiskPrefixUsb (b, usbPort);
  DpHdGpt (b, partNum, 0x800, 0x80000, sig);
  DpEnd (b);
}

/* 验证返回的 Fs 确实绑定预期卷：写入探针文件后经独立后端读回 */
static void probe_fs (FS_OPS *fs, CONST CHAR8 *root) {
  FS_OPS   *check;
  FS_ENTRY ent;
  s09_seed (fs, U16 ("probe.txt"), "p3-probe");
  check = FsPosixBackend (root);
  ASSERT_TRUE (check != NULL);
  ASSERT_OK (check->Info (check, U16 ("probe.txt"), &ent));
  ASSERT_EQ (ent.Size, 8);
  check->Destroy (check);
}

TEST (t09_10_p3_discovery) {
  DP_BUF     boot;
  UINTN      idx;
  EFI_STATUS st;
  TVOL       *p3, *p1, *clone, *other;

  /* 契约函数自检（合成路径）：S05 真实实现合入后同样必须通过 */
  {
    GPT_NODE_INFO gi;
    UINT8         *pref = NULL;
    UINTN         plen = 0;
    DP_BUF        a, b2;
    boot_dp (&a, 1, 1, SIG_B1);
    boot_dp (&b2, 1, 3, SIG_B3);
    ASSERT_OK (DpFindGpt (b2.Buf, b2.Len, &gi));
    ASSERT_EQ (gi.PartNumber, 3);
    ASSERT_EQ (gi.PartStartLba, 0x800);
    ASSERT_EQ (gi.PartEndLba, 0x800 + 0x80000 - 1);   /* boot_dp 尺寸 0x80000 */
    ASSERT_MEM_EQ (gi.DiskSignature, SIG_B3, 16);
    ASSERT_OK (DpDiskPrefix (a.Buf, a.Len, &pref, &plen));
    ASSERT_EQ (plen, 24);           /* ACPI(12)+PCI(6)+USB(6) */
    MemFree (pref);
    ASSERT_TRUE (VolSameDisk (a.Buf, a.Len, b2.Buf, b2.Len));
    ASSERT_TRUE (VolIsP3 (a.Buf, a.Len, b2.Buf, b2.Len));
    ASSERT_FALSE (VolIsP3 (b2.Buf, b2.Len, a.Buf, a.Len));  /* 卷是 P1 */
    /* MBR 拒绝 */
    {
      DP_BUF m;
      DpInit (&m);
      DpDiskPrefixUsb (&m, 1);
      DpHdMbr (&m, 3, 0x12345678);
      DpEnd (&m);
      ASSERT_EQ (DpFindGpt (m.Buf, m.Len, &gi), EFI_UNSUPPORTED);
      ASSERT_FALSE (VolIsP3 (a.Buf, a.Len, m.Buf, m.Len));
    }
    /* 截断不越界 */
    ASSERT_FALSE (VolIsP3 (a.Buf, a.Len, b2.Buf, 10));
    ASSERT_ERR (DpFindGpt (b2.Buf, 10, &gi));
  }

  /* a) 正常：启动盘 P3 带标记 */
  boot_dp (&boot, 1, 1, SIG_B1);
  p1 = tv_add (1, 1, SIG_B1, FALSE, FALSE);
  p3 = tv_add (1, 3, SIG_B3, TRUE, FALSE);
  tv_add (2, 1, SIG_O3, FALSE, FALSE);
  vi_build ();
  st = VolFindP3InSet (g_vi, g_ntv, boot.Buf, boot.Len, &idx);
  ASSERT_OK (st);
  ASSERT_TRUE (g_vi[idx].Fs == p3->Fs);
  probe_fs (g_vi[idx].Fs, p3->Root);
  tv_reset ();

  /* b) 多 NTFS 盘：启动盘 P3 优先于它盘 P3 */
  p3    = tv_add (1, 3, SIG_B3, TRUE, FALSE);
  other = tv_add (2, 3, SIG_O3, TRUE, FALSE);
  vi_build ();
  st = VolFindP3InSet (g_vi, g_ntv, boot.Buf, boot.Len, &idx);
  ASSERT_OK (st);
  ASSERT_TRUE (g_vi[idx].Fs == p3->Fs);
  tv_reset ();

  /* c) 克隆盘（同分区签名、不同控制器路径）：
   *    启动盘 P3 无标记 → 步骤2 不认克隆盘；兜底扫到克隆盘标记 */
  p3    = tv_add (1, 3, SIG_B3, FALSE, FALSE);
  clone = tv_add (2, 3, SIG_B3, TRUE, FALSE);   /* 同签名 SIG_B3 */
  vi_build ();
  ASSERT_FALSE (VolSameDisk (boot.Buf, boot.Len, clone->Dp, clone->DpLen));
  st = VolFindP3InSet (g_vi, g_ntv, boot.Buf, boot.Len, &idx);
  ASSERT_OK (st);
  ASSERT_TRUE (g_vi[idx].Fs == clone->Fs);
  tv_reset ();

  /* c2) 克隆盘也有标记时启动盘仍优先 */
  p3    = tv_add (1, 3, SIG_B3, TRUE, FALSE);
  clone = tv_add (2, 3, SIG_B3, TRUE, FALSE);
  vi_build ();
  st = VolFindP3InSet (g_vi, g_ntv, boot.Buf, boot.Len, &idx);
  ASSERT_OK (st);
  ASSERT_TRUE (g_vi[idx].Fs == p3->Fs);
  tv_reset ();

  /* d) 无标记 → NOT_FOUND */
  tv_add (1, 1, SIG_B1, FALSE, FALSE);
  tv_add (1, 3, SIG_B3, FALSE, FALSE);
  tv_add (2, 3, SIG_O3, FALSE, FALSE);
  vi_build ();
  st = VolFindP3InSet (g_vi, g_ntv, boot.Buf, boot.Len, &idx);
  ASSERT_EQ (st, EFI_NOT_FOUND);
  tv_reset ();

  /* e) 无 P3 分区：兜底扫到启动盘 P1 的标记 */
  p1 = tv_add (1, 1, SIG_B1, TRUE, FALSE);
  tv_add (1, 2, SIG_B1, FALSE, FALSE);
  vi_build ();
  st = VolFindP3InSet (g_vi, g_ntv, boot.Buf, boot.Len, &idx);
  ASSERT_OK (st);
  ASSERT_TRUE (g_vi[idx].Fs == p1->Fs);
  tv_reset ();

  /* f) P3 无标记，启动盘 P2 仅 launcher.json → 兜底接受 */
  tv_add (1, 3, SIG_B3, FALSE, FALSE);
  p1 = tv_add (1, 2, SIG_B1, FALSE, TRUE);
  vi_build ();
  st = VolFindP3InSet (g_vi, g_ntv, boot.Buf, boot.Len, &idx);
  ASSERT_OK (st);
  ASSERT_TRUE (g_vi[idx].Fs == p1->Fs);
  tv_reset ();

  /* g) 空卷集 → NOT_FOUND */
  vi_build ();
  st = VolFindP3InSet (g_vi, 0, boot.Buf, boot.Len, &idx);
  ASSERT_EQ (st, EFI_NOT_FOUND);

  /* h) BootDp=NULL：无启动上下文，兜底仍可用 */
  other = tv_add (2, 3, SIG_O3, TRUE, FALSE);
  vi_build ();
  st = VolFindP3InSet (g_vi, g_ntv, NULL, 0, &idx);
  ASSERT_OK (st);
  ASSERT_TRUE (g_vi[idx].Fs == other->Fs);
  tv_reset ();

  /* i) 畸形卷设备路径：不越界、不误判，正常卷仍命中 */
  other = tv_add (2, 3, SIG_O3, TRUE, FALSE);
  p3 = tv_add (1, 3, SIG_B3, TRUE, FALSE);
  vi_build ();
  g_vi[0].DpLen = 10;               /* 截断 other 盘 dp（首个被评估） */
  st = VolFindP3InSet (g_vi, g_ntv, boot.Buf, boot.Len, &idx);
  ASSERT_OK (st);
  ASSERT_TRUE (g_vi[idx].Fs == p3->Fs);
  tv_reset ();
}

TEST (t09_11_sort_mark_free) {
  DP_BUF      boot;
  VOLUME_INFO *vols;
  UINTN       i;
  FS_OPS      *fsP3, *fsP1, *fsClone, *fsOther;

  boot_dp (&boot, 1, 1, SIG_B1);
  fsP3    = tv_add (1, 3, SIG_B3, TRUE, FALSE)->Fs;   /* IsP3 */
  fsP1    = tv_add (1, 1, SIG_B1, FALSE, FALSE)->Fs;
  fsClone = tv_add (2, 1, SIG_B1, FALSE, FALSE)->Fs;
  fsOther = tv_add (3, 3, SIG_O3, TRUE, FALSE)->Fs;   /* 它盘 P3：非 IsP3 */

  /* 堆 owned 卷集（Dp 拷贝 + Fs），供 VolFree 完整释放 */
  vols = (VOLUME_INFO *)MemAlloc (g_ntv * sizeof (VOLUME_INFO));
  ASSERT_TRUE (vols != NULL);
  for (i = 0; i < g_ntv; i++) {
    memset (&vols[i], 0, sizeof (VOLUME_INFO));
    vols[i].Fs    = g_tv[i].Fs;
    vols[i].DpLen = g_tv[i].DpLen;
    vols[i].Dp    = (UINT8 *)MemAlloc (g_tv[i].DpLen);
    ASSERT_TRUE (vols[i].Dp != NULL);
    memcpy (vols[i].Dp, g_tv[i].Dp, g_tv[i].DpLen);
    g_tv[i].Fs = NULL;              /* 所有权移交 vols，tv_reset 不再 Destroy */
  }

  ASSERT_OK (VolSortMark (vols, g_ntv, boot.Buf, boot.Len));
  /* IsP3 优先；其余按 dp 字节序：usb1/P1 < usb2/P1 < usb3/P3 */
  ASSERT_TRUE (vols[0].Fs == fsP3);
  ASSERT_TRUE (vols[0].IsP3);
  ASSERT_TRUE (vols[1].Fs == fsP1);
  ASSERT_FALSE (vols[1].IsP3);
  ASSERT_TRUE (vols[2].Fs == fsClone);
  ASSERT_FALSE (vols[2].IsP3);
  ASSERT_TRUE (vols[3].Fs == fsOther);
  ASSERT_FALSE (vols[3].IsP3);      /* 它盘 P3 不算本镜像数据区 */

  /* 无启动上下文：IsP3 全 FALSE，纯字节序（usb1/P1 < usb1/P3 < usb2 < usb3） */
  ASSERT_OK (VolSortMark (vols, g_ntv, NULL, 0));
  for (i = 0; i < g_ntv; i++) ASSERT_FALSE (vols[i].IsP3);
  ASSERT_TRUE (vols[0].Fs == fsP1);
  ASSERT_TRUE (vols[1].Fs == fsP3);
  ASSERT_TRUE (vols[2].Fs == fsClone);
  ASSERT_TRUE (vols[3].Fs == fsOther);

  /* launcher.json 单独存在：FsHasMarker 接受，但 P1 非分区3 → IsP3 仍 FALSE */
  ASSERT_OK (VolSortMark (vols, g_ntv, boot.Buf, boot.Len));
  ASSERT_TRUE (vols[0].IsP3);
  ASSERT_TRUE (vols[1].Fs == fsP1);
  s09_seed (vols[1].Fs, U16 ("launcher.json"), "{}\n");
  ASSERT_OK (VolSortMark (vols, g_ntv, boot.Buf, boot.Len));
  ASSERT_TRUE (vols[0].Fs == fsP3 && vols[0].IsP3);
  ASSERT_TRUE (vols[1].Fs == fsP1);
  ASSERT_FALSE (vols[1].IsP3);

  VolFree (vols, g_ntv);
  tv_reset ();
  ASSERT_EQ (TestMemLiveBlocks (), 0);
  ASSERT_EQ (TestMemLiveBytes (), 0);
}

int main (void) {
  RUN_TEST (t09_10_p3_discovery);
  RUN_TEST (t09_11_sort_mark_free);
  RUN_TESTS_END ();
}
