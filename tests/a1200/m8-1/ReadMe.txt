ogles2 M8 run 1: a framebuffer object's job queued behind the running render.

On a 640x480 screen of its own, every frame clears a small texture to a
colour of its own first, then draws it over the screen; every second frame
reads the screen back and checks the colour. Step 1 with the queue (the
library's default), step 2 with ENV:OGLES2/NoQueue (the old way) as the
control. Each step shows colours changing for about a second.

  CD to this directory, then:   Execute Run-M1
