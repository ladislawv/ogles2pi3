/*
 * The library's GPU device (docs/PI3_PLAN.md §5.1, P2 step 7): the clock
 * for the timing statistics, and the device's calls through the backend's
 * table (backend.h; V3D 4.2: ogles2/v3d/device_v3d.c). The device open is
 * where a backend is chosen: the V3D 4.2 one (the Pi 4) until the Pi 3's
 * VC4 backend exists (P5: the device tree's V3D node decides).
 */
#include <exec/execbase.h>
#include <proto/exec.h>
#include "ogles2_internal.h"
#include "backend.h"
#include "v3d_types.h"

/* ---- a clock for the timing statistics ----
 * The Pi's system timer: a free-running 1 MHz counter (CLO, peripheral
 * offset 0x3004), in the peripheral window v3dcore's registers use. One
 * read, on the Pi's side: timer.device's E-clock reads the Amiga's CIAs
 * across the PiStorm bus, which cost about 25 microseconds a call. */
#define SYSTIMER_CLO (*(volatile ULONG *)(0xF2000000 + 0x3004))

ULONG ogles2_clock_us(void)
{
    return LE32(SYSTIMER_CLO);
}

BOOL ogles2_device_open(void)
{
    return ogles2_backend->open();
}

BOOL ogles2_device_ready(void)
{
    return ogles2_backend->ready();
}

void ogles2_device_reset(const char *why)
{
    ogles2_backend->reset(why);
}

ULONG ogles2_device_bytes(void)
{
    return ogles2_backend->bytes();
}

BOOL ogles2_device_gpu_reachable(ULONG addr, ULONG size)
{
    return ogles2_backend->gpu_reachable(addr, size);
}

struct SignalSemaphore *ogles2_device_gpu_lock(void)
{
    return ogles2_backend->gpu_lock();
}

struct V3DContext *ogles2_device_context_new(UWORD w, UWORD h, int depth_bits, BOOL msaa)
{
    return ogles2_backend->context_new(w, h, depth_bits, msaa);
}

BOOL ogles2_device_context_resize(struct V3DContext *v, UWORD w, UWORD h)
{
    return ogles2_backend->context_resize(v, w, h);
}

void ogles2_device_context_free(struct V3DContext *v)
{
    ogles2_backend->context_free(v);
}
