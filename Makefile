CC     ?= gcc
CFLAGS ?= -O2 -std=gnu99 -Wall -Itamp
SRC     = simple.c compressors.c tamp/compressor.c tamp/decompressor.c tamp/common.c

simple: $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: simple
	./simple test

clean:
	rm -f simple

.PHONY: test clean
