CFLAGS = -Wall -O3
CPPFLAGS = -DUSE_RUSAGE

textpv: textpv.c
textpv.c: Makefile
