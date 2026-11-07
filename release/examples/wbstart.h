/*
 * wbstart.h: the examples started from Workbench as from a Shell.
 *
 * From Workbench (main's argc 0, its argv the WBStartup message), the
 * program's icon's ToolTypes are its arguments: each one is a word of the
 * Shell's line ("FULLSCREEN", "WIDTH=800"), except those in brackets, the
 * usual way to keep one switched off ("(FULLSCREEN)"). What the program
 * prints goes to a console window, which opens only when something is
 * printed (an error) and stays until it is closed.
 *
 *   int main(int argc, char **argv)
 *   {
 *       wb_init(argc, argv, "boingocean");
 *       rda = wb_readargs("WIDTH/N,FULLSCREEN/S", args);
 *       ...
 *       wb_freeargs(rda);
 *
 * MIT licence (LICENSE).
 */
#ifndef WBSTART_H
#define WBSTART_H

#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <dos/dosextens.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>

struct Library *IconBase;
static BOOL wb_started;      /* TRUE: started from Workbench */
static char wb_line[512];    /* the ToolTypes as a Shell's line */

static void wb_init(int argc, char **argv, const char *title)
{
    struct WBStartup *wb = (struct WBStartup *)argv;
    struct DiskObject *dob;
    char con[96];
    STRPTR *tt;
    BPTR old;
    size_t n = 0, l;
    if (argc != 0)
        return;
    wb_started = TRUE;
    sprintf(con, "CON:40/40/560/160/%s/AUTO/CLOSE/WAIT", title);
    freopen(con, "w", stdout);
    if ((IconBase = OpenLibrary((STRPTR) "icon.library", 36)) != NULL) {
        old = CurrentDir(wb->sm_ArgList[0].wa_Lock);
        if ((dob = GetDiskObject((STRPTR)wb->sm_ArgList[0].wa_Name)) != NULL) {
            for (tt = (STRPTR *)dob->do_ToolTypes; tt && *tt; tt++) {
                l = strlen((const char *)*tt);
                if (!l || **tt == '(' || **tt == ';' || n + l + 2 >= sizeof(wb_line))
                    continue;
                memcpy(wb_line + n, *tt, l);
                n += l;
                wb_line[n++] = ' ';
            }
            FreeDiskObject(dob);
        }
        CurrentDir(old);
        CloseLibrary(IconBase);
        IconBase = NULL;
    }
    wb_line[n++] = '\n';
    wb_line[n] = 0;
}

/* ReadArgs on the Shell's line, or on the icon's ToolTypes: NULL when they do not fit the template */
static struct RDArgs *wb_readargs(const char *tmpl, LONG *args)
{
    struct RDArgs *rda;
    if (!wb_started)
        return ReadArgs((STRPTR)tmpl, args, NULL);
    if (!(rda = (struct RDArgs *)AllocDosObject(DOS_RDARGS, NULL)))
        return NULL;
    rda->RDA_Source.CS_Buffer = (UBYTE *)wb_line;
    rda->RDA_Source.CS_Length = strlen(wb_line);
    rda->RDA_Source.CS_CurChr = 0;
    rda->RDA_Flags |= RDAF_NOPROMPT;
    if (!ReadArgs((STRPTR)tmpl, args, rda)) {
        FreeDosObject(DOS_RDARGS, rda);
        return NULL;
    }
    return rda;
}

static void wb_freeargs(struct RDArgs *rda)
{
    FreeArgs(rda);
    if (wb_started)
        FreeDosObject(DOS_RDARGS, rda);
}

#endif
