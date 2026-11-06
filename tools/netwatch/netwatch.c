/*
 * netwatch: resets the A1200 when its link to the PC is gone (docs/PI3_PLAN.md
 * §10). With the Pi 3 card the Amiga is on Wi-Fi, whose link drops and at times
 * does not come back until a reset, while a long test run goes on with nothing
 * reaching it; nobody can reset it from the PC then. Every EVERY seconds an ICMP
 * echo to HOST, sent again every 5 s until a reply: none within WAIT seconds, and
 * a line goes to LOG and the machine is reset.
 *   netwatch HOST [LOG=<file>] [EVERY=60] [WAIT=60] [DRYRUN]
 * DRYRUN logs what it would do and ends instead (for testing). Ctrl-C ends it.
 *
 * Two processes: the echoes are a process's own, which may block for ever in the
 * TCP/IP stack; this one only counts the seconds since a reply and touches neither
 * the network nor, until the end, the disk. (All in one, it did not reset the
 * machine twice on 2026-11-06, with the station gone: a send that never returns
 * would look so.) The reset's line, written by a third process in case the disk
 * blocks too (started beforehand: making a process needs the disk), says what the
 * echoes were doing. A reset without the line: the disk had stopped. No reset: the
 * machine itself had.
 *
 * Started by the PC while a batch runs (tests/a1200/p6_watch.sh), and on purpose
 * not from S:User-Startup: with the PC off it would reset the machine every
 * minute for ever. The PC reads LOG to learn of a reset.
 *
 * MIT licence (LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>

struct Library *SocketBase; /* the echo process's own: a task's, bsdsocket's rule */

static const char *log_path = "Stuff:amigent/netwatch.log";
static const char *host;
static unsigned long ip;
static long every = 60, wait_secs = 60;

/* Between the processes. */
enum { PHASE_IDLE, PHASE_SEND, PHASE_WAIT };
enum { ECHO_STARTING, ECHO_RUNNING, ECHO_ENDED, ECHO_FAILED };
static volatile LONG since_reply, phase, phase_secs, echo_state;
static volatile LONG log_done, log_ended;
static char log_text[120];

static void log_line(const char *text, const char *who)
{
    struct DateStamp now;
    char line[200];
    BPTR f;
    DateStamp(&now);
    snprintf(line, sizeof line, "day %ld min %ld sec %ld: %s %s\n", (long)now.ds_Days, (long)now.ds_Minute,
             (long)now.ds_Tick / TICKS_PER_SECOND, text, who);
    if (!(f = Open((STRPTR)log_path, MODE_READWRITE)) && !(f = Open((STRPTR)log_path, MODE_NEWFILE)))
        return;
    Seek(f, 0, OFFSET_END);
    Write(f, line, (LONG)strlen(line));
    Close(f);
}

/* What the file system still holds in memory, onto the disk: a line written
 * two seconds before a reset was lost (2026-11-06). */
static void flush_disk(void)
{
    struct MsgPort *fs = DeviceProc((STRPTR)log_path);
    if (fs)
        DoPkt(fs, ACTION_FLUSH, 0, 0, 0, 0, 0);
}

static unsigned short checksum(const unsigned char *p, int n)
{
    unsigned long sum = 0;
    int k;
    for (k = 0; k + 1 < n; k += 2)
        sum += (unsigned long)p[k] << 8 | p[k + 1];
    if (k < n)
        sum += (unsigned long)p[k] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (unsigned short)~sum;
}

static int parse_ip(const char *s, unsigned long *out)
{
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return -1;
    *out = (unsigned long)a << 24 | (unsigned long)b << 16 | (unsigned long)c << 8 | d;
    return 0;
}

/* One echo; 0 sent. */
static int send_echo(int s, const struct sockaddr_in *to, unsigned short id, unsigned short seq)
{
    unsigned char pkt[16];
    unsigned short c;
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 8; /* echo request */
    pkt[4] = (unsigned char)(id >> 8);
    pkt[5] = (unsigned char)id;
    pkt[6] = (unsigned char)(seq >> 8);
    pkt[7] = (unsigned char)seq;
    c = checksum(pkt, sizeof pkt);
    pkt[2] = (unsigned char)(c >> 8);
    pkt[3] = (unsigned char)c;
    return sendto(s, pkt, sizeof pkt, 0, (struct sockaddr *)to, sizeof *to) == (LONG)sizeof pkt ? 0 : -1;
}

/* Waits up to `secs` for a reply from `from` to `id`: 1 a reply, 0 none, -1 Ctrl-C. */
static int wait_reply(int s, unsigned long from, unsigned short id, long secs)
{
    for (;;) {
        fd_set r;
        struct timeval tv;
        ULONG sig = SIGBREAKF_CTRL_C;
        LONG n;
        FD_ZERO(&r);
        FD_SET(s, &r);
        tv.tv_sec = secs;
        tv.tv_usec = 0;
        n = WaitSelect(s + 1, &r, 0, 0, &tv, &sig);
        if (sig & SIGBREAKF_CTRL_C)
            return -1;
        if (n <= 0)
            return 0;
        {
            unsigned char buf[128];
            struct sockaddr_in src;
            socklen_t len = sizeof src;
            LONG got = recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&src, &len);
            int ihl;
            if (got < 28)
                continue;
            ihl = (buf[0] & 15) * 4;
            if (got < ihl + 8)
                continue;
            if ((unsigned long)src.sin_addr.s_addr == from && buf[ihl] == 0 /* echo reply */ &&
                buf[ihl + 4] == (unsigned char)(id >> 8) && buf[ihl + 5] == (unsigned char)id)
                return 1;
        }
    }
}

/* The echoes' process: Ctrl-C from the main one ends it. */
static void echo_entry(void)
{
    struct sockaddr_in to;
    unsigned short id = (unsigned short)((ULONG)FindTask(0) >> 4), seq = 0;
    int s;

    if (!(SocketBase = OpenLibrary((STRPTR) "bsdsocket.library", 4))) {
        echo_state = ECHO_FAILED;
        return;
    }
    if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) < 0) {
        CloseLibrary(SocketBase);
        echo_state = ECHO_FAILED;
        return;
    }
    memset(&to, 0, sizeof to);
    to.sin_len = sizeof to;
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = ip;
    echo_state = ECHO_RUNNING;
    for (;;) {
        int r = 0;
        long left;
        ULONG got = 0;
        while (!r) { /* the main process decides when it has been too long */
            phase_secs = 0;
            phase = PHASE_SEND;
            send_echo(s, &to, id, ++seq); /* a failed send: as no reply (the link is down) */
            phase_secs = 0;
            phase = PHASE_WAIT;
            r = wait_reply(s, ip, id, 5);
            phase = PHASE_IDLE;
            if (r < 0)
                goto done;
        }
        since_reply = 0;
        /* the rest of the interval, in pieces Ctrl-C can end */
        for (left = every; left > 0 && !got; left--) {
            Delay(TICKS_PER_SECOND);
            got = SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C;
        }
        if (got)
            goto done;
    }
done:
    CloseSocket(s);
    CloseLibrary(SocketBase);
    Forbid(); /* the main process unloads this code at ECHO_ENDED; the exit breaks the Forbid */
    echo_state = ECHO_ENDED;
}

/* The reset's line, by a process of its own, waiting from the start: the disk may
 * be what has stopped, and starting a process then would talk to it too (its
 * current directory's lock). Ctrl-F: write the line; Ctrl-C: end. */
static void log_entry(void)
{
    if (Wait(SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_F) {
        log_line(log_text, host);
        flush_disk();
        log_done = 1;
    }
    Forbid(); /* as echo_entry */
    log_ended = 1;
}

int main(int argc, char **argv)
{
    int dry = 0, i;
    struct Process *echo, *logger;

    for (i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "LOG=", 4))
            log_path = argv[i] + 4;
        else if (!strncmp(argv[i], "EVERY=", 6))
            every = atol(argv[i] + 6);
        else if (!strncmp(argv[i], "WAIT=", 5))
            wait_secs = atol(argv[i] + 5);
        else if (!strcmp(argv[i], "DRYRUN"))
            dry = 1;
        else
            host = argv[i];
    }
    if (!host || parse_ip(host, &ip) < 0 || every < 1 || wait_secs < 1) {
        printf("usage: netwatch HOST [LOG=<file>] [EVERY=60] [WAIT=60] [DRYRUN]\n");
        return 20;
    }
    /* Above the run it watches: a busy program must not look like a lost link. */
    SetTaskPri(FindTask(0), 5);
    logger = CreateNewProcTags(NP_Entry, (ULONG)log_entry, NP_Name, (ULONG) "netwatch log", NP_Priority, 5,
                               NP_StackSize, 16384, TAG_DONE);
    if (!logger) {
        printf("netwatch: no process for the log\n");
        return 20;
    }
    echo = CreateNewProcTags(NP_Entry, (ULONG)echo_entry, NP_Name, (ULONG) "netwatch echoes", NP_Priority, 5,
                             NP_StackSize, 16384, TAG_DONE);
    if (!echo) {
        printf("netwatch: no process for the echoes\n");
        Signal(&logger->pr_Task, SIGBREAKF_CTRL_C);
        while (!log_ended)
            Delay(TICKS_PER_SECOND / 5);
        return 20;
    }
    for (i = 0; i < 100 && echo_state == ECHO_STARTING; i++)
        Delay(TICKS_PER_SECOND / 5);
    if (echo_state != ECHO_RUNNING) {
        printf("netwatch: no bsdsocket.library or no raw socket\n");
        while (echo_state == ECHO_STARTING)
            Delay(TICKS_PER_SECOND);
        Signal(&logger->pr_Task, SIGBREAKF_CTRL_C);
        while (!log_ended)
            Delay(TICKS_PER_SECOND / 5);
        return 20;
    }
    log_line("watching", host);
    printf("netwatch: watching %s, every %ld s, %ld s for a reply%s\n", host, every, wait_secs, dry ? " (dry run)" : "");
    fflush(stdout);
    for (;;) {
        Delay(TICKS_PER_SECOND);
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
            break;
        phase_secs++;
        if (++since_reply >= every + wait_secs) {
            LONG ph = phase, secs = phase_secs;
            snprintf(log_text, sizeof log_text, "no reply: %s (the echoes %s for %ld s), from",
                     dry ? "would reset (dry run)" : "resetting",
                     ph == PHASE_SEND ? "stuck in a send" : ph == PHASE_WAIT ? "waiting for a reply" : "between sends",
                     (long)secs);
            Signal(&logger->pr_Task, SIGBREAKF_CTRL_F);
            for (i = 0; i < 10 && !log_done; i++)
                Delay(TICKS_PER_SECOND);
            if (dry)
                break;
            Delay(5 * TICKS_PER_SECOND);
            ColdReboot();
        }
    }
    /* A send that never returns keeps this waiting: the code must stay for it. */
    Signal(&echo->pr_Task, SIGBREAKF_CTRL_C);
    while (echo_state != ECHO_ENDED)
        Delay(TICKS_PER_SECOND / 5);
    Signal(&logger->pr_Task, SIGBREAKF_CTRL_C);
    while (!log_ended)
        Delay(TICKS_PER_SECOND / 5);
    log_line("stopped", host);
    return 0;
}
