ogles2-vsync-2: the vsync report, before and after the fix (the swap waits
on the HVS's frame counter instead of WaitBOVP). The reporter's VsyncTest
on a 640x480 32-bit screen, first with the ogles2.library in LIBS:, then
with this package's (copied into LIBS:; the one there is kept as
LIBS:ogles2.library.vsync-backup and put back at the end).

CD to this directory, then:   Execute Run-M1
Output goes to results/. About six minutes.
