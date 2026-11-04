/*
 * vc4core's device (docs/PI3_PLAN.md §3.1): the 3D core taken over in its
 * reset state, its identity, its registers.
 *
 * The firmware counts SET_ENABLE_QPU (PI3_PLAN §2, "Power"): a user that did
 * not disable it (a crash, another program) leaves the next enable a core in
 * that user's state, which loses a tile's primitives now and then (with VPM
 * errors in ERRSTAT). So the core is disabled until its registers stop
 * answering (0xdeadbeef), enabled once, and VPMBASE must read its reset value.
 *
 * MIT licence (LICENSE).
 */
#include "vc4core.h"
#include "vc4_os.h"
#include "vc4_regs.h"

static volatile uint32_t *regs;
static int enabled; /* we hold one SET_ENABLE_QPU */

uint32_t vc4_read(uint32_t reg)
{
    return vc4_le32(regs[reg / 4]);
}

void vc4_write(uint32_t reg, uint32_t v)
{
    regs[reg / 4] = vc4_le32(v);
}

static int enable_qpu(int on)
{
    uint32_t v[1];
    v[0] = on ? 1 : 0;
    return vc4_os_mbox(TAG_SET_ENABLE_QPU, v, 1) < 0 ? -1 : 0;
}

/* Off until the registers read 0xdeadbeef, then on once: the reset state. */
static int take_over(void)
{
    int k;
    for (k = 0; k < 32 && vc4_read(V3D_IDENT0) != V3D_UNPOWERED; k++)
        if (enable_qpu(0) < 0)
            return -1;
    if (k > 1)
        vc4_os_log("the core was enabled %d times (a user that did not disable it): reset", k);
    if (enable_qpu(1) < 0)
        return -1;
    enabled = 1;
    if (vc4_read(V3D_VPMBASE) != V3D_VPMBASE_RESET) {
        vc4_os_log("VPMBASE 0x%08x after power-on, not its reset value 0x%x", vc4_read(V3D_VPMBASE),
                   V3D_VPMBASE_RESET);
        return -1;
    }
    /* None of the VPM kept for user QPU programs: all of it for the vertex
     * pipeline, as Linux's vc4_v3d_init_hw leaves it for Mesa's shaders */
    vc4_write(V3D_VPMBASE, 0);
    return 0;
}

/* Polled: every interrupt of the core disabled (enabling the binner's
 * out-of-memory one, as Linux does, hung the firmware's mailbox in the bare
 * boot, 2026-11-04: its line reaches the VideoCore), old requests cleared. */
static void interrupts(void)
{
    vc4_write(V3D_INTDIS, 0xffffffffu);
    vc4_write(V3D_INTCTL, 0xffffffffu);
}

int vc4_open(void)
{
    uint32_t i0, i1;
    if (vc4_os_open() < 0)
        return -1;
    regs = vc4_os_regs();
    if (take_over() < 0) {
        vc4_close();
        return -1;
    }
    i0 = vc4_read(V3D_IDENT0);
    i1 = vc4_read(V3D_IDENT1);
    if (i0 != V3D_IDENT0_VALUE) {
        vc4_os_log("IDENT0 0x%08x: not a VideoCore IV 3D core", i0);
        vc4_close();
        return -1;
    }
    vc4_os_log("V3D %u.%u: %u slices of %u QPUs, VPM %u KB", i0 >> 24, i1 & 15, (i1 >> 4) & 15,
               (i1 >> 8) & 15, i1 >> 28);
    interrupts();
    return 0;
}

void vc4_close(void)
{
    if (enabled)
        enable_qpu(0);
    enabled = 0;
    vc4_os_close();
    regs = 0;
}

int vc4_reset(const char *why)
{
    vc4_os_log("reset after %s: CT0CS %08x CT1CS %08x CT0CA %08x CT1CA %08x ERRSTAT %08x", why,
               vc4_read(V3D_CT0CS), vc4_read(V3D_CT1CS), vc4_read(V3D_CT0CA), vc4_read(V3D_CT1CA),
               vc4_read(V3D_ERRSTAT));
    vc4_os_log("  INTCTL %08x BPCA %08x BPCS %08x BPOA %08x BPOS %08x PCS %08x BFC %08x RFC %08x",
               vc4_read(V3D_INTCTL), vc4_read(V3D_BPCA), vc4_read(V3D_BPCS), vc4_read(V3D_BPOA),
               vc4_read(V3D_BPOS), vc4_read(V3D_PCS), vc4_read(V3D_BFC), vc4_read(V3D_RFC));
    vc4_write(V3D_CT0CS, V3D_CTCS_RESET);
    vc4_write(V3D_CT1CS, V3D_CTCS_RESET);
    if (enabled) {
        enable_qpu(0);
        enabled = 0;
    }
    if (take_over() < 0)
        return -1;
    interrupts();
    return 0;
}
