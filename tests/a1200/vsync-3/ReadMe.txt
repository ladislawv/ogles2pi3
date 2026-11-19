ogles2-vsync-3: watch for tearing. m6_vsync draws a white bar crossing a
640x480 screen with GL; the white squares at the top give the way:
  5 vsync off (tears: the reference)    6 vsync switched on
  7 vsync off again                     8 a context with vsync from the start
Twice with this package's ogles2.library (the swap waits on the HVS's frame
counter), then once with the one in LIBS: (WaitBOVP). The package's library
is copied into LIBS: for its passes; the one there is put back at the end.

CD to this directory, then:   Execute Run-M1
