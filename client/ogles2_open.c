/*
 * libogles2.a: opening the library and the per-program-image state
 * (plan section 6.2).
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ogles2.h>

struct Library *OGLES2Base;
const struct OGLES2Dispatch *OGLES2Dispatch;
struct OGLES2Context *OGLES2CurrentContext;

/* lvo.s: the library's vectors */
extern const struct OGLES2Dispatch *OGLES2GetDispatchTableLVO(void);
extern ULONG OGLES2PrivateLogFlushLVO(void);

/* Where the library is looked for: installed (in memory, or LIBS:), then
 * beside the program, so a program can ship it in its own drawer when it is
 * not installed: PROGDIR:libs/, then PROGDIR:. One that is too old, or built
 * for another dispatch ABI, is closed and the next one tried. A copy beside
 * the program whose version.revision is higher than the installed one's is
 * tried first: the newer library wins, wherever it is. */
static const char *const where[] = {
    OGLES2NAME,
    "PROGDIR:libs/" OGLES2NAME,
    "PROGDIR:" OGLES2NAME,
};

/* exec keeps one library of a name: an OpenLibrary of any path returns the
 * ogles2.library in memory (A1200, 2026-10-15). So before the next file is
 * tried, one that would not do is expunged, as Avail FLUSH does, if no
 * program has it open; one in use stays, and the next try gets it again. */
static void expunge_unused(void)
{
    struct Library *l;
    Forbid();
    l = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)OGLES2NAME);
    if (l && l->lib_OpenCnt == 0)
        RemLibrary(l);
    Permit();
}

/* A library file's version and revision (version << 16 | revision), from its
 * "$VER: ogles2.library V.R" string; 0 when there is no such file or string. */
static ULONG file_version(const char *path)
{
    static const char tag[] = "$VER: " OGLES2NAME " ";
    enum { TAG = sizeof(tag) - 1, TAIL = TAG + 16, CHUNK = 512 }; /* $VER is near the start: one read */
    UBYTE buf[CHUNK + TAIL];
    ULONG ver = 0, rev = 0, found = 0;
    LONG n, have = 0, i, j;
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return 0;
    while (!found && (n = Read(fh, buf + have, CHUNK)) > 0) {
        have += n;
        /* a tag found here has its digits in the buffer: TAIL bytes after it */
        for (i = 0; i + TAIL <= have && !found; i++) {
            for (j = 0; j < TAG && buf[i + j] == (UBYTE)tag[j]; j++)
                ;
            if (j < TAG)
                continue;
            for (j = i + TAG; j < have && buf[j] >= '0' && buf[j] <= '9'; j++)
                ver = ver * 10 + (buf[j] - '0');
            if (j < have && buf[j] == '.')
                for (j++; j < have && buf[j] >= '0' && buf[j] <= '9'; j++)
                    rev = rev * 10 + (buf[j] - '0');
            found = 1;
        }
        if (!found && have > TAIL) { /* keep the end: a tag may straddle the chunks */
            for (i = 0; i < TAIL; i++)
                buf[i] = buf[have - TAIL + i];
            have = TAIL;
        }
    }
    Close(fh);
    return found ? ver << 16 | (rev & 0xFFFF) : 0;
}

/* The installed library's version: the one in memory, else LIBS:'s file. */
static ULONG installed_version(void)
{
    struct Library *l;
    ULONG v = 0;
    Forbid();
    if ((l = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)OGLES2NAME)) != 0)
        v = (ULONG)l->lib_Version << 16 | l->lib_Revision;
    Permit();
    return v ? v : file_version("LIBS:" OGLES2NAME);
}

/* Opens where[i] and takes it if its dispatch table fits; else leaves none open. */
static BOOL try_open(unsigned i)
{
    const struct OGLES2Dispatch *d;
    /* OpenLibrary also fails for one in memory older than OGLES2_VERSION */
    if (!(OGLES2Base = OpenLibrary((CONST_STRPTR)where[i], OGLES2_VERSION))) {
        expunge_unused();
        return FALSE;
    }
    d = OGLES2GetDispatchTableLVO();
    /* abiVersion must be EQUAL: a different value means the entries mean
     * something else. structSize "at least mine": a newer library only
     * appends entries (plan section 6.7). */
    if (d && d->abiVersion == OGLES2_DISPATCH_ABI_VERSION && d->structSize >= sizeof(struct OGLES2Dispatch)) {
        OGLES2Dispatch = d;
        return TRUE;
    }
    CloseLibrary(OGLES2Base);
    OGLES2Base = 0;
    expunge_unused();
    return FALSE;
}

BOOL OGLES2Open(void)
{
    ULONG v, best = 0;
    unsigned i, from = 0;

    if (OGLES2Base && OGLES2Dispatch)
        return TRUE;
    /* The program's own copy first when it is newer than the installed one
     * (an older one in memory that no program uses is expunged by the try;
     * one in use stays: exec keeps one library of a name). */
    for (i = 1; i < sizeof(where) / sizeof(where[0]); i++)
        if ((v = file_version(where[i])) > best) {
            best = v;
            from = i;
        }
    if (from && best > installed_version()) {
        expunge_unused();
        if (try_open(from))
            return TRUE;
    }
    for (i = 0; i < sizeof(where) / sizeof(where[0]); i++)
        if (try_open(i))
            return TRUE;
    return FALSE;
}

void OGLES2Close(void)
{
    OGLES2Dispatch = 0;
    OGLES2CurrentContext = 0;
    if (OGLES2Base) {
        /* Write the library's log while this process may still do DOS I/O
         * (the library's Close vector runs under Forbid). */
        OGLES2PrivateLogFlushLVO();
        CloseLibrary(OGLES2Base);
        OGLES2Base = 0;
    }
}
