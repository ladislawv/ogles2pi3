ogles2 P5 run 1
===============

What this is
------------
The first run of ogles2 on the Raspberry Pi 3 card (VideoCore IV): the
PiStorm with the Pi 3 in it, Emu68 as on the Pi 4 card.

Steps 1 and 2 use the 3D core directly, without the library: they take the
core over, read Emu68's device tree (the facts go into the results: where
the peripherals, the mailbox and the RAM are), draw the test pictures into
memory and compare each with the picture Mesa's own driver drew on the same
Pi 3 under Linux. Nothing is shown on screen.

Steps 3 and 4 are the library's first use of the Pi 3: a smoke test, then
clears of an application's bitmap through the VC4 backend, each read back
and checked (nothing is shown).

Nothing is installed. libs/ is added to LIBS: for steps 3 and 4 and removed
at the end; the library's log goes to results/ogles2.log (this build writes
every log line at once, so a hang loses nothing).

How to run
----------
1. With the Pi 3 card in the PiStorm, open a Shell and type:
       CD <where you copied it>/ogles2-p5-1
       Execute Run-P5
2. If the machine hangs, reset it and continue with the next step:
       Execute Run-P5 FROM=<the next step>
   results/00-progress.txt says which steps finished.
3. Send the results directory back.
