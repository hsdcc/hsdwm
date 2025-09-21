CC = gcc
CFLAGS = -Wall -O2 -std=c11 -Wextra
LIBS = -lX11 -lxkbfile -lXft -lfontconfig -lm

all: thing

thing: wm.c
	$(CC) $(CFLAGS) wm.c -o thing $(LIBS)

clean:
	rm -f thing

