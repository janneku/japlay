CC = gcc
CFLAGS = -O2 -W -Wall `pkg-config gtk+-2.0 ao mad --cflags`
LDFLAGS = `pkg-config gtk+-2.0 ao mad gthread-2.0 --libs`
OBJ = main.o

all: japlay

%.o:	%.c
	$(CC) $(CFLAGS) -c $<

japlay:	$(OBJ)
	$(CC) $(OBJ) -o japlay $(LDFLAGS)

install: japlay
	install -m 755 japlay $(DESTDIR)/usr/bin

clean:
	rm -f $(OBJ)
