ogles2-vsync-1: the vsync report ("aglSwapBuffers with OGLES3_CCT_VSYNC
sometimes waits two vblanks"). A 640x480 32-bit screen opens for each step.

  bin/vsync_probe   WaitBOVP, WaitTOF and the HVS's frame counter timed
                    against the Pi's display scaler (only reads of it)
  bin/VsyncTest     the reporter's program (ogles2.library, VSYNC on)

CD to this directory, then:   Execute Run-M1
Output goes to results/. About four minutes.
