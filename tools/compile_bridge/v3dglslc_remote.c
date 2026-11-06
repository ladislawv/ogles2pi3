/*
 * v3dglslc-remote: ogles2.library's shader compiler on the PC (ported from
 * ogles3's, 2026-11-05: dEQP runs compile their hundreds of programs on the
 * PC, about 80 minutes a run instead of hours). It owns the compiler's public
 * port (v3dp/v3dglsl_port.h, "v3dglsl compiler"), so the library sends its
 * requests here instead of starting "v3dglslc SERVER";
 * tools/compile_bridge/compile_bridge.py connects on TCP with amictld's token and
 * compiles them with the host v3dglslc (SERVE), which writes the same files
 * byte for byte (compile_bridge.py --check). The file comes back and is
 * written where the request says, then the request is replied. Requests of
 * version 1 (the Pi 4's) and 2 (naming the GPU, VC4's keys: the Pi 3's).
 *
 * With no bridge connected, or the link lost, the requests go to a local
 * "v3dglslc SERVER" (COMPILER) and this program ends: a run goes on, at the
 * A1200's speed; with NOFALLBACK (dEQP runs: tests/a1200/p2_deqp.sh) a request
 * waits up to 10 s for the bridge, then is refused ("compile failed") and
 * this program keeps the port, so nothing compiles on the A1200 (a compile
 * there took a dEQP run 20 minutes on one program). A QUIT request (each library expunge sends one) does not
 * end it: the bridge starts its compiler anew, as the local server would
 * be. Ctrl-C ends it. WAIT: while a local server has the port, wait for it
 * to leave (it does at the next expunge) instead of ending.
 *
 *   v3dglslc-remote [PORT=6803] [TOKEN=//amictld.token] [COMPILER=LIBS:v3dglslc] [WAIT] [NOFALLBACK]
 * Port 6803: ogles3's bridge, on the same station, uses 6802.
 *
 * TOKEN's default is amictld's, from a package's directory in amictld's
 * work/. Frames (big-endian u32s; a string is its length, 0xffffffff for
 * NULL, and its bytes):
 *   bridge:  "V2H1", token                       (once, after connecting)
 *   here:    "V2OK"
 *   here:    "V2R1", version, op, device, vs, fs, bindings, keys, file name
 *   bridge:  "V2S1", result, log, file count, then each file's name and bytes
 * (a QUIT: the request frame with op V3DGLSL_QUIT and all strings NULL,
 * answered with no files; "V2": ogles2's frames, not ogles3's "V3").
 * GCC 6 + libnix, Roadshow's bsdsocket.library (as amictld). MIT licence.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include "v3dglsl_port.h"

#define COMPILER_STACK (4UL * 1024 * 1024) /* as ogles2/compiler.c starts it */

struct Library *SocketBase;

unsigned long __stack = 65536;
extern void __stkinit(void);
void *__x_stkinit = (void *)__stkinit; /* libnix's swapstack start-up module */

static char token[80];
static const char *compiler = "LIBS:v3dglslc";
static int ls = -1, cs = -1; /* listening; the bridge's */
static struct MsgPort *port;
static unsigned long served, refused;
static int no_fallback;

/* ---- the link ---- */

static int send_all(const void *b, unsigned long n)
{
    const char *p = b;
    while (n) {
        LONG r = send(cs, (APTR)p, n > 65536 ? 65536 : n, 0);
        if (r <= 0)
            return 0;
        p += r;
        n -= (unsigned long)r;
    }
    return 1;
}

static int recv_all(void *b, unsigned long n)
{
    char *p = b;
    while (n) {
        LONG r = recv(cs, p, n > 65536 ? 65536 : n, 0);
        if (r <= 0)
            return 0;
        p += r;
        n -= (unsigned long)r;
    }
    return 1;
}

static int send_u32(ULONG v)
{
    return send_all(&v, 4); /* the 68k's order is the network's */
}

static int send_str(const void *s, ULONG n)
{
    return s ? send_u32(n) && send_all(s, n) : send_u32(0xffffffffUL);
}

static int recv_u32(ULONG *v)
{
    return recv_all(v, 4);
}

/* A string, NUL-terminated, malloc'd (NULL for NULL); FALSE: the link failed. */
static int recv_str(char **s, ULONG *n)
{
    *s = 0;
    if (!recv_u32(n))
        return 0;
    if (*n == 0xffffffffUL) {
        *n = 0;
        return 1;
    }
    if (*n > 64UL * 1024 * 1024 || !(*s = malloc(*n + 1)))
        return 0;
    (*s)[*n] = 0;
    return recv_all(*s, *n);
}

static void drop_link(const char *why)
{
    if (cs >= 0) {
        printf("v3dglslc-remote: bridge gone (%s)\n", why);
        CloseSocket(cs);
        cs = -1;
    }
}

/* A connection: the bridge, if it knows the token and speaks these frames;
 * it replaces the link only then (another bridge, ogles3's say, knocking on
 * the port leaves a working link alone). */
static void take_connection(void)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    struct timeval tv;
    char magic[4], *t = 0;
    ULONG len;
    LONG one = 1;
    int old = cs, s = accept(ls, (struct sockaddr *)&a, &n);
    if (s < 0)
        return;
    tv.tv_sec = 10; /* the handshake; a compile on the PC takes milliseconds */
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    cs = s; /* recv_all and send_all use cs */
    if (!recv_all(magic, 4) || memcmp(magic, "V2H1", 4) || !recv_str(&t, &len) || !t || strcmp(t, token) ||
        !send_all("V2OK", 4)) {
        free(t);
        CloseSocket(s);
        cs = old;
        printf("v3dglslc-remote: a connection without the token or these frames, closed\n");
        return;
    }
    free(t);
    if (old >= 0) {
        printf("v3dglslc-remote: bridge gone (a new one connected)\n");
        CloseSocket(old);
    }
    tv.tv_sec = 60;
    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    /* whose: the newest bridge takes the requests, with its own compiler, so a
     * stray one's programs can look like a bug (2026-11-06) */
    {
        const unsigned char *ip = (const unsigned char *)&a.sin_addr.s_addr;
        printf("v3dglslc-remote: bridge connected from %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
    }
}

/* ---- requests ---- */

static void done(struct V3DGLSLRequest *r)
{
    if (r->msg.mn_ReplyPort)
        ReplyMsg(&r->msg);
    else
        FreeVec(r); /* a QUIT from an expunge */
}

/* The file at path, malloc'd; NULL if there is none. */
static char *read_file(const char *path, ULONG *n)
{
    BPTR f = Open((STRPTR)path, MODE_OLDFILE);
    char *b = 0;
    LONG size;
    *n = 0;
    if (!f)
        return 0;
    if (Seek(f, 0, OFFSET_END) >= 0 && (size = Seek(f, 0, OFFSET_BEGINNING)) >= 0 &&
        (b = malloc((ULONG)size + 1)) && Read(f, b, size) == size)
        *n = (ULONG)size;
    else {
        free(b);
        b = 0;
    }
    Close(f);
    return b;
}

static int write_file(const char *path, const char *b, ULONG n)
{
    BPTR f = Open((STRPTR)path, MODE_NEWFILE);
    int ok;
    if (!f)
        return 0;
    ok = Write(f, (APTR)b, n) == (LONG)n;
    Close(f);
    if (!ok)
        DeleteFile((STRPTR)path);
    return ok;
}

/* The request compiled on the PC: FALSE if the link failed (r not replied). */
static int forward(struct V3DGLSLRequest *r)
{
    const char *name = 0, *p;
    char path[512], magic[4], *log = 0;
    ULONG dir = 0, result, nlog, nfiles, i;
    /* version 2's fields only from a version-2 request (an older sender's
     * ends before them); bindings only with LINK_BOUND */
    int v2 = r->version == V3DGLSL_VERSION_DEVICE;
    ULONG device = v2 ? r->device : V3DGLSL_DEVICE_V3D42;
    const char *bindings = r->op == V3DGLSL_LINK_BOUND ? r->bindings : 0;
    const void *keys = v2 && device == V3DGLSL_DEVICE_VC4 ? r->vc4_keys : 0;
    ULONG keys_size = keys ? r->vc4_keys_size : 0;
    int ok = 1;

    if (r->out_path) {
        name = r->out_path;
        for (p = r->out_path; *p; p++)
            if (*p == '/' || *p == ':')
                name = p + 1;
        dir = (ULONG)(name - r->out_path);
        if (strlen(r->out_path) + 8 > sizeof(path))
            return 0;
    }
    ok = send_all("V2R1", 4) && send_u32(r->version) && send_u32(r->op) && send_u32(device) &&
         send_str(r->vs, r->vs ? strlen(r->vs) : 0) && send_str(r->fs, r->fs ? strlen(r->fs) : 0) &&
         send_str(bindings, bindings ? strlen(bindings) : 0) && send_str(keys, keys_size) &&
         send_str(name, name ? strlen(name) : 0);
    if (!ok || !recv_all(magic, 4) || memcmp(magic, "V2S1", 4) || !recv_u32(&result) || !recv_str(&log, &nlog) ||
        !recv_u32(&nfiles))
        goto failed;
    for (i = 0; i < nfiles; i++) {
        char *fname, *data;
        ULONG fn, dn;
        if (!recv_str(&fname, &fn) || !recv_str(&data, &dn) || !fname) {
            free(fname);
            goto failed;
        }
        /* Next to out_path, under the name the PC's compiler gave it. */
        if (r->out_path && dir + fn < sizeof(path) && !strchr(fname, '/') && !strchr(fname, ':')) {
            memcpy(path, r->out_path, dir);
            strcpy(path + dir, fname);
            if (!data || !write_file(path, data, dn))
                result = 2;
        } else
            result = 2;
        free(fname);
        free(data);
    }
    if (r->log && r->log_size) {
        ULONG n = nlog < r->log_size - 1 ? nlog : r->log_size - 1;
        if (log)
            memcpy(r->log, log, n);
        r->log[log ? n : 0] = 0;
    }
    free(log);
    r->result = (LONG)result;
    served++;
    done(r);
    return 1;
failed:
    free(log);
    drop_link("a request failed");
    return 0;
}

/* A library expunge's QUIT: the bridge starts its compiler again, as the
 * local server would start anew, so a program's file never depends on
 * what an earlier library compiled (a program's default variants can
 * depend on the one compiled before it in the same process). */
static void pass_quit(void)
{
    char magic[4], *log;
    ULONG result, nlog, nfiles;
    if (cs < 0)
        return;
    if (!send_all("V2R1", 4) || !send_u32(V3DGLSL_VERSION) || !send_u32(V3DGLSL_QUIT) ||
        !send_u32(V3DGLSL_DEVICE_V3D42) || !send_str(0, 0) || !send_str(0, 0) || !send_str(0, 0) ||
        !send_str(0, 0) || !send_str(0, 0) || !recv_all(magic, 4) || memcmp(magic, "V2S1", 4) ||
        !recv_u32(&result) || !recv_str(&log, &nlog) || !recv_u32(&nfiles) || nfiles) {
        drop_link("the QUIT failed");
        return;
    }
    free(log);
}

/* NOFALLBACK: up to 10 s for the bridge to connect (it tries every 5 s);
 * TRUE if it has. */
static int await_bridge(void)
{
    int t;
    for (t = 0; t < 10 && cs < 0; t++) {
        fd_set rd;
        struct timeval tv;
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        if (WaitSelect(ls + 1, &rd, 0, 0, &tv, 0) > 0 && FD_ISSET(ls, &rd))
            take_connection();
    }
    return cs >= 0;
}

/* NOFALLBACK without a bridge: r fails, the port stays ours. */
static void refuse(struct V3DGLSLRequest *r)
{
    r->result = 2;
    if (r->log && r->log_size)
        snprintf(r->log, r->log_size, "v3dglslc-remote: no bridge to the PC; not compiled on the A1200 (NOFALLBACK)");
    done(r);
    if (!refused++)
        printf("v3dglslc-remote: no bridge: requests refused (NOFALLBACK)\n");
}

/* No bridge: the local compiler takes r and everything after it, and this
 * program ends. The port goes first (the server starts only without one). */
static void fall_back(struct V3DGLSLRequest *r)
{
    struct MsgPort *local = 0;
    char cmd[300];
    BPTR in, out;
    int t;
    Forbid();
    RemPort(port);
    Permit();
    printf("v3dglslc-remote: no bridge: %s takes the requests\n", compiler);
    snprintf(cmd, sizeof(cmd), "\"%s\" SERVER", compiler);
    in = Open((STRPTR) "NIL:", MODE_OLDFILE);
    out = Open((STRPTR) "NIL:", MODE_NEWFILE);
    if (in && out &&
        SystemTags((STRPTR)cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE, NP_StackSize, COMPILER_STACK,
                   NP_Name, (ULONG)V3DGLSL_PORT_NAME, TAG_DONE) != -1) {
        for (t = 0; t < 30 * 50 && !local; t += 5) { /* 30 s */
            Forbid();
            local = FindPort((STRPTR)V3DGLSL_PORT_NAME);
            Permit();
            if (!local)
                Delay(5);
        }
    } else {
        if (in)
            Close(in);
        if (out)
            Close(out);
    }
    do {
        if (r->op == V3DGLSL_QUIT)
            done(r);
        else if (local) {
            Forbid(); /* the server leaves only at a QUIT, which nobody sends meanwhile */
            if ((local = FindPort((STRPTR)V3DGLSL_PORT_NAME)))
                PutMsg(local, &r->msg);
            Permit();
        }
        if (!local && r->op != V3DGLSL_QUIT) {
            r->result = 2;
            if (r->log && r->log_size)
                snprintf(r->log, r->log_size, "v3dglslc-remote: no compiler (%s did not start)", compiler);
            done(r);
        }
    } while ((r = (struct V3DGLSLRequest *)GetMsg(port)));
}

int main(int argc, char **argv)
{
    const char *token_file = "//amictld.token";
    int listen_port = 6803, wait_free = 0, i;
    struct sockaddr_in a;
    LONG one = 1;
    char *t;
    ULONG n;

    for (i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "PORT=", 5))
            listen_port = atoi(argv[i] + 5);
        else if (!strncmp(argv[i], "TOKEN=", 6))
            token_file = argv[i] + 6;
        else if (!strncmp(argv[i], "COMPILER=", 9))
            compiler = argv[i] + 9;
        else if (!strcmp(argv[i], "WAIT"))
            wait_free = 1;
        else if (!strcmp(argv[i], "NOFALLBACK"))
            no_fallback = 1;
        else {
            printf("usage: v3dglslc-remote [PORT=6803] [TOKEN=//amictld.token] [COMPILER=LIBS:v3dglslc] [WAIT] "
                   "[NOFALLBACK]\n");
            return 10;
        }
    }
    if (!(t = read_file(token_file, &n)) || n >= sizeof(token)) {
        printf("v3dglslc-remote: no token in %s\n", token_file);
        free(t);
        return 10;
    }
    memcpy(token, t, n);
    free(t);
    while (n && (token[n - 1] == '\n' || token[n - 1] == '\r' || token[n - 1] == ' '))
        n--;
    token[n] = 0;
    if (!(SocketBase = OpenLibrary((STRPTR) "bsdsocket.library", 4))) {
        printf("v3dglslc-remote: no bsdsocket.library\n");
        return 20;
    }
    ls = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = AF_INET;
    a.sin_port = (unsigned short)listen_port;
    a.sin_addr.s_addr = INADDR_ANY;
    if (ls < 0 || setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0 ||
        bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 2) < 0) {
        printf("v3dglslc-remote: cannot listen on port %d\n", listen_port);
        if (ls >= 0)
            CloseSocket(ls);
        CloseLibrary(SocketBase);
        return 20;
    }
    /* WAIT: until a local server leaves (the next library expunge). */
    for (;;) {
        Forbid();
        if (!FindPort((STRPTR)V3DGLSL_PORT_NAME))
            break;
        Permit();
        if (!wait_free || (SetSignal(0, 0) & SIGBREAKF_CTRL_C)) {
            printf("v3dglslc-remote: a compiler server is running already\n");
            CloseSocket(ls);
            CloseLibrary(SocketBase);
            return 5;
        }
        Delay(25);
    }
    if (!(port = CreateMsgPort())) {
        Permit();
        CloseSocket(ls);
        CloseLibrary(SocketBase);
        return 20;
    }
    port->mp_Node.ln_Name = (char *)V3DGLSL_PORT_NAME;
    port->mp_Node.ln_Pri = 0;
    AddPort(port);
    Permit();
    printf("v3dglslc-remote: serving \"%s\", bridge port %d\n", V3DGLSL_PORT_NAME, listen_port);

    for (;;) {
        struct V3DGLSLRequest *r;
        fd_set rd;
        ULONG sig = SIGBREAKF_CTRL_C | (1UL << port->mp_SigBit);
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        if (WaitSelect(ls + 1, &rd, 0, 0, 0, &sig) > 0 && FD_ISSET(ls, &rd))
            take_connection();
        if (sig & SIGBREAKF_CTRL_C)
            break;
        while ((r = (struct V3DGLSLRequest *)GetMsg(port))) {
            if (r->version != V3DGLSL_VERSION && r->version != V3DGLSL_VERSION_DEVICE) {
                r->result = 3;
                done(r);
            } else if (r->op == V3DGLSL_QUIT) {
                r->result = 0;
                done(r); /* a library expunge: the next library finds us again */
                pass_quit();
            } else if (no_fallback) {
                /* a link lost mid-request: once more, on a new one */
                if ((cs >= 0 || await_bridge()) && forward(r))
                    ;
                else if (!await_bridge() || !forward(r))
                    refuse(r);
            } else if (cs < 0 || !forward(r)) {
                fall_back(r);
                goto out;
            }
        }
    }
    /* Ctrl-C: gone from the list first, then fail what came meanwhile (the
     * library starts its own server at its next program). */
    {
        struct V3DGLSLRequest *r;
        Forbid();
        RemPort(port);
        Permit();
        while ((r = (struct V3DGLSLRequest *)GetMsg(port))) {
            r->result = 2;
            done(r);
        }
    }
out:
    printf("v3dglslc-remote: done, %lu request(s) compiled on the PC, %lu refused\n", served, refused);
    DeleteMsgPort(port);
    drop_link("ending");
    CloseSocket(ls);
    CloseLibrary(SocketBase);
    return 0;
}
