CFLAGS = -Wall -O3 -g
#CPPFLAGS += -DNDEBUG
CPPFLAGS += -DUSE_RUSAGE
#CPPFLAGS += -DUSE_SPLICE

.PHONY: all
all: tpv tpv-read tpv-splice

tpv:
	ln -s tpv-read tpv
tpv-read: tpv.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^
tpv-splice: tpv.c
	$(CC) -DUSE_SPLICE $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^
tpv.c: Makefile
	touch tpv.c
