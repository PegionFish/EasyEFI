/* S09 宿主测试：合成设备路径构造器（UEFI Spec 2.10 §10 字节格式，手工小端）。
 * 节点：Type(1) SubType(1) Length(2 LE，含 4 字节头) + 载荷。
 * HD(GPT)：MEDIA(0x04)/HD(0x01)，42 字节：PartNumber(4) Start(8) Size(8)
 *          Signature[16] MBRType(1)=0 SignatureType(1)=2。 */
#ifndef DP_SYNTH_H
#define DP_SYNTH_H

#include "Contract/EfiCompat.h"
#include <string.h>

typedef struct {
  UINT8 Buf[512];
  UINTN Len;
} DP_BUF;

static inline void DpInit (DP_BUF *b) { b->Len = 0; }

static inline void DpRaw (DP_BUF *b, UINT8 Type, UINT8 Sub, CONST UINT8 *Payload, UINTN n) {
  UINTN nl = 4 + n;
  b->Buf[b->Len++] = Type;
  b->Buf[b->Len++] = Sub;
  b->Buf[b->Len++] = (UINT8)(nl & 0xFF);
  b->Buf[b->Len++] = (UINT8)(nl >> 8);
  if (n) memcpy (b->Buf + b->Len, Payload, n);
  b->Len += n;
}

static inline void DpEnd (DP_BUF *b) { DpRaw (b, 0x7F, 0xFF, NULL, 0); }

static inline void DpU32 (UINT8 *p, UINT32 v) {
  p[0] = (UINT8)v; p[1] = (UINT8)(v >> 8); p[2] = (UINT8)(v >> 16); p[3] = (UINT8)(v >> 24);
}
static inline void DpU64 (UINT8 *p, UINT64 v) {
  DpU32 (p, (UINT32)v); DpU32 (p + 4, (UINT32)(v >> 32));
}

static inline void DpAcpi (DP_BUF *b, UINT32 Hid, UINT32 Uid) {
  UINT8 p[8]; DpU32 (p, Hid); DpU32 (p + 4, Uid);
  DpRaw (b, 0x02, 0x01, p, 8);
}
static inline void DpPci (DP_BUF *b, UINT8 Dev, UINT8 Func) {
  UINT8 p[2] = { Func, Dev };
  DpRaw (b, 0x01, 0x01, p, 2);
}
static inline void DpUsb (DP_BUF *b, UINT8 Port, UINT8 Iface) {
  UINT8 p[2] = { Port, Iface };
  DpRaw (b, 0x03, 0x05, p, 2);
}
static inline void DpHdGpt (DP_BUF *b, UINT32 PartNum, UINT64 Start, UINT64 Size, CONST UINT8 Sig[16]) {
  UINT8 p[38];
  DpU32 (p, PartNum);
  DpU64 (p + 4, Start);
  DpU64 (p + 12, Size);
  memcpy (p + 20, Sig, 16);
  p[36] = 2;                    /* MBRType = GPT（edk2 PartitionDxe/Gpt.c 实证） */
  p[37] = 2;                    /* SignatureType = GPT */
  DpRaw (b, 0x04, 0x01, p, 38);
}
static inline void DpHdMbr (DP_BUF *b, UINT32 PartNum, UINT32 MbrSig) {
  UINT8 p[38];
  memset (p, 0, sizeof (p));
  DpU32 (p, PartNum);
  DpU32 (p + 20, MbrSig);       /* MBR 签名在 Signature 前 4 字节 */
  p[36] = 1;                    /* MBRType = legacy */
  p[37] = 1;                    /* SignatureType = MBR */
  DpRaw (b, 0x04, 0x01, p, 38);
}

/* 常用盘前缀：ACPI(PNP0A03,0)/PCI(0x14,0)/USB(port,0) */
static inline void DpDiskPrefixUsb (DP_BUF *b, UINT8 Port) {
  DpAcpi (b, 0x030A41D0, 0);    /* PNP0A03（EISA 压缩 HID，小端 UINT32） */
  DpPci (b, 0x14, 0);
  DpUsb (b, Port, 0);
}

#endif
