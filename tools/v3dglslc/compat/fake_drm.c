/*
 * A fake V3D DRM device for running Mesa's V3D driver to compile shaders only.
 *
 * Answers the kernel interface the driver uses (drmIoctl, mmap, sync objects)
 * the way Mesa's drm-shim does for a V3D 4.2 (src/broadcom/drm-shim/
 * v3d_noop.c): GET_PARAM returns the Pi 4's identification registers, buffer
 * objects are ordinary memory blocks, and jobs "complete" without running.
 * Nothing touches real hardware; ogles2's runtime drives the GPU itself.
 *
 * A second descriptor (fake_drm_vc4_open) is a VideoCore IV (Raspberry Pi 3,
 * V3D 2.1) for Mesa's vc4 driver (docs/PI3_PLAN.md §4.1), after drm-shim's
 * src/broadcom/drm-shim/vc4_noop.c: its identification registers as the Pi 3
 * reads them, branches, ETC1, threaded fragment shaders and fixed render
 * order supported (as Linux and vc4core do), no madvise, no perfmon. The two
 * share the buffer objects and mmap.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>
#ifdef __linux__
/* Native PC build (build_v3dc_host.sh): mmap/munmap below replace the C
 * library's for the whole program, so other callers go to the kernel. */
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include "drm-uapi/drm.h"
#include "drm-uapi/v3d_drm.h"
#include "drm-uapi/vc4_drm.h"
#include "xf86drm.h"

#define FAKE_FD 0x5644 /* "VD" */
#define FAKE_FD_VC4 0x5643 /* "VC" */
#define MAX_BOS 4096
#define MMAP_UNIT 0x10000u /* mmap offset = handle * MMAP_UNIT */

struct fake_bo {
    void *ptr;
    uint32_t size;
    uint32_t gpu_offset;
};

static struct fake_bo bos[MAX_BOS]; /* index = handle; 0 unused */
static uint32_t next_gpu_offset = 0x1000;
static uint32_t next_syncobj = 1;

/* V3D 4.2 (Raspberry Pi 4), as drm-shim's "42" entry. */
static uint64_t ident(uint32_t param, int *ok)
{
    *ok = 1;
    switch (param) {
    case DRM_V3D_PARAM_V3D_UIFCFG:        return 0x00000045;
    case DRM_V3D_PARAM_V3D_HUB_IDENT1:    return 0x000e1124;
    case DRM_V3D_PARAM_V3D_HUB_IDENT2:    return 0x00000100;
    case DRM_V3D_PARAM_V3D_HUB_IDENT3:    return 0x00000e00;
    case DRM_V3D_PARAM_V3D_CORE0_IDENT0:  return 0x04443356;
    case DRM_V3D_PARAM_V3D_CORE0_IDENT1:  return 0x81001422;
    case DRM_V3D_PARAM_V3D_CORE0_IDENT2:  return 0x40078121;
    case DRM_V3D_PARAM_SUPPORTS_TFU:
    case DRM_V3D_PARAM_SUPPORTS_CSD:
    case DRM_V3D_PARAM_SUPPORTS_CACHE_FLUSH:
    case DRM_V3D_PARAM_SUPPORTS_PERFMON:
    case DRM_V3D_PARAM_SUPPORTS_MULTISYNC_EXT:
    case DRM_V3D_PARAM_SUPPORTS_CPU_QUEUE:  return 1;
    case DRM_V3D_PARAM_MAX_PERF_COUNTERS:
    case DRM_V3D_PARAM_GLOBAL_RESET_COUNTER:
    case DRM_V3D_PARAM_CONTEXT_RESET_COUNTER: return 0;
    default:
        *ok = 0;
        return 0;
    }
}

static struct fake_bo *lookup(uint32_t handle)
{
    if (handle == 0 || handle >= MAX_BOS || !bos[handle].ptr)
        return NULL;
    return &bos[handle];
}

int fake_drm_open(void)
{
    return FAKE_FD;
}

int fake_drm_vc4_open(void)
{
    return FAKE_FD_VC4;
}

/* A new buffer object of size bytes, zeroed; its handle, or 0. */
static uint32_t new_bo(uint32_t size)
{
    uint32_t h;
    for (h = 1; h < MAX_BOS && bos[h].ptr; h++)
        ;
    if (h == MAX_BOS)
        return 0;
    bos[h].ptr = calloc(1, size ? size : 1);
    if (!bos[h].ptr)
        return 0;
    bos[h].size = size;
    bos[h].gpu_offset = next_gpu_offset;
    next_gpu_offset += (size + 4095u) & ~4095u;
    return h;
}

/* The Raspberry Pi 3's VideoCore IV (docs/PI3_PLAN.md §2: IDENT0..2 as read on
 * the Pi; the driver takes version 21 from IDENT0's top byte and IDENT1's
 * revision). */
static int vc4_ioctl(unsigned long request, void *arg)
{
    if (request == DRM_IOCTL_VC4_GET_PARAM) {
        struct drm_vc4_get_param *gp = arg;
        switch (gp->param) {
        case DRM_VC4_PARAM_V3D_IDENT0: gp->value = 0x02443356; return 0;
        case DRM_VC4_PARAM_V3D_IDENT1: gp->value = 0xc1102431; return 0;
        case DRM_VC4_PARAM_V3D_IDENT2: gp->value = 0x00000121; return 0;
        case DRM_VC4_PARAM_SUPPORTS_BRANCHES:
        case DRM_VC4_PARAM_SUPPORTS_ETC1:
        case DRM_VC4_PARAM_SUPPORTS_THREADED_FS:
        case DRM_VC4_PARAM_SUPPORTS_FIXED_RCL_ORDER: gp->value = 1; return 0;
        case DRM_VC4_PARAM_SUPPORTS_MADVISE:
        case DRM_VC4_PARAM_SUPPORTS_PERFMON: gp->value = 0; return 0;
        default:
            fprintf(stderr, "fake_drm: unknown VC4 GET_PARAM %u\n", (unsigned)gp->param);
            errno = EINVAL;
            return -1;
        }
    }
    if (request == DRM_IOCTL_VC4_CREATE_BO) {
        struct drm_vc4_create_bo *c = arg;
        c->handle = new_bo(c->size);
        if (!c->handle) {
            errno = ENOMEM;
            return -1;
        }
        return 0;
    }
    if (request == DRM_IOCTL_VC4_CREATE_SHADER_BO) {
        /* The kernel copies the code in from the caller's pointer (and
         * validates it; not here). */
        struct drm_vc4_create_shader_bo *c = arg;
        c->handle = new_bo(c->size);
        if (!c->handle) {
            errno = ENOMEM;
            return -1;
        }
        memcpy(bos[c->handle].ptr, (const void *)(uintptr_t)c->data, c->size);
        return 0;
    }
    if (request == DRM_IOCTL_VC4_MMAP_BO) {
        struct drm_vc4_mmap_bo *m = arg;
        if (!lookup(m->handle)) {
            errno = ENOENT;
            return -1;
        }
        m->offset = (uint64_t)m->handle * MMAP_UNIT;
        return 0;
    }
    if (request == DRM_IOCTL_VC4_GET_TILING) {
        struct drm_vc4_get_tiling *t = arg;
        t->flags = 0;
        t->modifier = 0; /* DRM_FORMAT_MOD_LINEAR */
        return 0;
    }
    if (request == DRM_IOCTL_VC4_SUBMIT_CL) {
        static uint64_t seqno;
        struct drm_vc4_submit_cl *s = arg;
        s->seqno = ++seqno;
        return 0;
    }
    if (request == DRM_IOCTL_VC4_SET_TILING || request == DRM_IOCTL_VC4_LABEL_BO ||
        request == DRM_IOCTL_VC4_WAIT_SEQNO || request == DRM_IOCTL_VC4_WAIT_BO)
        return 0;
    if (request == DRM_IOCTL_GEM_CLOSE) {
        struct drm_gem_close *c = arg;
        struct fake_bo *bo = lookup(c->handle);
        if (bo) {
            free(bo->ptr);
            memset(bo, 0, sizeof(*bo));
        }
        return 0;
    }
    fprintf(stderr, "fake_drm: unhandled VC4 ioctl 0x%08lx\n", request);
    errno = EINVAL;
    return -1;
}

/* No sync objects (the drivers then wait by sequence number or BO). */
int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
    (void)fd;
    (void)capability;
    *value = 0;
    errno = EINVAL;
    return -1;
}

int drmIoctl(int fd, unsigned long request, void *arg)
{
    if (fd == FAKE_FD_VC4)
        return vc4_ioctl(request, arg);
    if (fd != FAKE_FD) {
        errno = EBADF;
        return -1;
    }

    if (request == DRM_IOCTL_V3D_GET_PARAM) {
        struct drm_v3d_get_param *gp = arg;
        int ok;
        gp->value = ident(gp->param, &ok);
        if (!ok) {
            fprintf(stderr, "fake_drm: unknown GET_PARAM %u\n", (unsigned)gp->param);
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    if (request == DRM_IOCTL_V3D_CREATE_BO) {
        struct drm_v3d_create_bo *c = arg;
        uint32_t h = new_bo(c->size);
        if (!h) {
            errno = ENOMEM;
            return -1;
        }
        c->handle = h;
        c->offset = bos[h].gpu_offset;
        return 0;
    }
    if (request == DRM_IOCTL_V3D_MMAP_BO) {
        struct drm_v3d_mmap_bo *m = arg;
        if (!lookup(m->handle)) {
            errno = ENOENT;
            return -1;
        }
        m->offset = (uint64_t)m->handle * MMAP_UNIT;
        return 0;
    }
    if (request == DRM_IOCTL_V3D_GET_BO_OFFSET) {
        struct drm_v3d_get_bo_offset *o = arg;
        struct fake_bo *bo = lookup(o->handle);
        if (!bo) {
            errno = ENOENT;
            return -1;
        }
        o->offset = bo->gpu_offset;
        return 0;
    }
    if (request == DRM_IOCTL_GEM_CLOSE) {
        struct drm_gem_close *c = arg;
        struct fake_bo *bo = lookup(c->handle);
        if (bo) {
            free(bo->ptr);
            memset(bo, 0, sizeof(*bo));
        }
        return 0;
    }
    if (request == DRM_IOCTL_V3D_SUBMIT_CL || request == DRM_IOCTL_V3D_SUBMIT_TFU ||
        request == DRM_IOCTL_V3D_SUBMIT_CSD || request == DRM_IOCTL_V3D_SUBMIT_CPU ||
        request == DRM_IOCTL_V3D_WAIT_BO ||
        request == DRM_IOCTL_V3D_PERFMON_CREATE || request == DRM_IOCTL_V3D_PERFMON_DESTROY ||
        request == DRM_IOCTL_V3D_PERFMON_GET_VALUES)
        return 0;

    fprintf(stderr, "fake_drm: unhandled ioctl 0x%08lx\n", request);
    errno = EINVAL;
    return -1;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    struct fake_bo *bo;
#ifdef __linux__
    if (fd != FAKE_FD && fd != FAKE_FD_VC4)
        return (void *)syscall(SYS_mmap, addr, len, prot, flags, fd, offset);
#endif
    (void)addr;
    (void)prot;
    (void)flags;
    if ((fd != FAKE_FD && fd != FAKE_FD_VC4) || offset % MMAP_UNIT) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    bo = lookup((uint32_t)(offset / MMAP_UNIT));
    if (!bo || len > bo->size) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    return bo->ptr;
}

int munmap(void *addr, size_t len)
{
#ifdef __linux__
    uint32_t h;
    for (h = 1; h < MAX_BOS; h++)
        if (bos[h].ptr == addr)
            return 0;
    return (int)syscall(SYS_munmap, addr, len);
#else
    (void)addr;
    (void)len;
    return 0;
#endif
}

/* Sync objects: every job is complete at once. */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
    (void)fd;
    (void)flags;
    *handle = next_syncobj++;
    return 0;
}

int drmSyncobjDestroy(int fd, uint32_t handle)
{
    (void)fd;
    (void)handle;
    return 0;
}

int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
                   int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
    (void)fd;
    (void)handles;
    (void)num_handles;
    (void)timeout_nsec;
    (void)flags;
    if (first_signaled)
        *first_signaled = 0;
    return 0;
}

int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
    (void)fd;
    (void)handle;
    *sync_file_fd = -1;
    errno = ENOSYS;
    return -1;
}

int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
    (void)fd;
    (void)handle;
    (void)sync_file_fd;
    return 0;
}

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
    (void)fd;
    (void)handle;
    (void)flags;
    *prime_fd = -1;
    errno = ENOSYS;
    return -1;
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
    (void)fd;
    (void)prime_fd;
    (void)handle;
    errno = ENOSYS;
    return -1;
}
