ogles2 M7 run 9: the disk cache's size

The first compile after the library is loaded removes the oldest compiled
programs from OGLES2Cache: when it holds more than ENV:OGLES2/CacheKB
kilobytes (64 MB without the variable, 0: no limit) or 8,192 files, down to
3/4 of that. Only the library's own files; a removed one is compiled again if
needed. This run checks it with small files in a drawer of its own.

To run:
       CD <where you copied it>/ogles2-m7-9
       Execute Run-M1
Three small windows open and close. Results are in results/.

Nothing is installed, and your own OGLES2Cache: is not touched.

bin/m7_cache         the test
libs/                the library and compiler for this run
