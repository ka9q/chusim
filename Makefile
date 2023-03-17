# $Id$ Makefile for standalone CHU program
BINDIR=/usr/local/bin
CFLAGS=-g -O2

all:	chusim

clean:
	rm -f *.o chusim

install: chusim	
	install -D --target-directory=$(BINDIR) chusim

chusim: chusim.o
	$(CC) -g -o $@ $^  -lm
