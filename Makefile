CC ?= gcc
CFLAGS = -Wall -O2
FUSE_FLAGS = $(shell pkg-config --cflags --libs fuse3) -lrt

ALL = fuse_growtest fuse_growtest_inval fuse_growtest_direct check_stale_mmap

all: $(ALL)

fuse_growtest: fuse_growtest.c
	$(CC) $(CFLAGS) -o $@ $< $(FUSE_FLAGS)

fuse_growtest_inval: fuse_growtest_inval.c
	$(CC) $(CFLAGS) -o $@ $< $(FUSE_FLAGS)

fuse_growtest_direct: fuse_growtest_direct.c
	$(CC) $(CFLAGS) -o $@ $< $(FUSE_FLAGS)

check_stale_mmap: check_stale_mmap.c
	$(CC) $(CFLAGS) -o $@ $< -pthread

clean:
	rm -f $(ALL)

.PHONY: all clean
