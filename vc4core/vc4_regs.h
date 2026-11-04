/*
 * The 3D core's registers vc4core uses (byte offsets; Broadcom's VideoCore
 * IV 3D Architecture Reference Guide, section 10; Linux's vc4_regs.h).
 *
 * MIT licence (LICENSE).
 */
#ifndef VC4_REGS_H
#define VC4_REGS_H

#define V3D_IDENT0 0x000
#define V3D_IDENT1 0x004
#define V3D_IDENT2 0x008
#define V3D_L2CACTL 0x020
#define V3D_SLCACTL 0x024
#define V3D_INTCTL 0x030
#define V3D_INTENA 0x034
#define V3D_INTDIS 0x038
#define V3D_CT0CS 0x100
#define V3D_CT1CS 0x104
#define V3D_CT0EA 0x108
#define V3D_CT1EA 0x10c
#define V3D_CT0CA 0x110
#define V3D_CT1CA 0x114
#define V3D_PCS 0x130
#define V3D_BFC 0x134
#define V3D_RFC 0x138
#define V3D_BPCA 0x300
#define V3D_BPCS 0x304
#define V3D_BPOA 0x308
#define V3D_BPOS 0x30c
#define V3D_VPMBASE 0x504
#define V3D_DBGE 0xf00
#define V3D_FDBGO 0xf04
#define V3D_FDBGB 0xf08
#define V3D_FDBGR 0xf0c
#define V3D_FDBGS 0xf10
#define V3D_ERRSTAT 0xf20

#define V3D_IDENT0_VALUE 0x02443356u /* "V3D", version 2 */
#define V3D_UNPOWERED 0xdeadbeefu    /* what the registers read while the core is off */
#define V3D_VPMBASE_RESET 0x10u
#define V3D_INT_OUTOMEM (1u << 2)    /* the binner needs more overflow memory */
#define V3D_CTCS_RESET (1u << 15)
#define V3D_L2CACTL_L2CCLR (1u << 2)
#define V3D_ERRSTAT_IDLE 0x1000u     /* what it reads after good jobs (PI3_PLAN §2) */

#define TAG_SET_ENABLE_QPU 0x00030012u

#endif
