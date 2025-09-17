CC = gcc
CFLAGS = -Wall -O2
LIBS = -lX11 -lxkbfile

all: thing

thing: wm.c
	$(CC) $(CFLAGS) wm.c -o thing $(LIBS)

clean:
	rm -f thing

