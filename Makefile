CFLAGS = -Wall -O3
#CPPFLAGS += -DNDEBUG
CPPFLAGS += -DUSE_RUSAGE
#CPPFLAGS += -DUSE_SPLICE

.PHONY: all
all: textpv-read textpv-splice

textpv-read: textpv.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^
textpv-splice: textpv.c
	$(CC) -DUSE_SPLICE $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^
textpv.c: Makefile
	touch textpv.c
