CFLAGS = -Wall -O3
CPPFLAGS += -DUSE_RUSAGE
CPPFLAGS += -DUSE_SPLICE

textpv: textpv.c
textpv.c: Makefile
	touch textpv.c
