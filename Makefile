CC      = gcc
CFLAGS  = -Wall -O2 -g -W
ALL_CFLAGS = $(CFLAGS) -D_GNU_SOURCE -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64

PROGS = run
ALL = $(PROGS)

all: $(ALL)

run: run.o
	$(CC) $(ALL_CFLAGS) -o $@ $(filter %.o,$^) -lpthread

clean:
	-rm -f *.o $(PROGS)