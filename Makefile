CC = gcc
CFLAGS = -O2 -W -Wall `pkg-config gtk+-2.0 ao --cflags`
LDFLAGS = `pkg-config gtk+-2.0 ao gthread-2.0 --libs`
PLUGIN_CFLAGS = $(CFLAGS) -fPIC
PLUGIN_LDFLAGS = $(LDCFLAGS) -shared

OBJ = main.o
PLUGIN_OBJS = in_mad.o in_mikmod.o in_vorbis.o
BINARY = japlay
PLUGINS = in_mad.so in_mikmod.so in_vorbis.so

all: $(BINARY) $(PLUGINS)

in_mad.o:	in_mad.c
	$(CC) $(PLUGIN_CFLAGS) `pkg-config mad --cflags` -c $<

in_mikmod.o:	in_mikmod.c
	$(CC) $(PLUGIN_CFLAGS) `libmikmod-config --cflags` -c $<

in_vorbis.o:	in_vorbis.c
	$(CC) $(PLUGIN_CFLAGS) `pkg-config vorbisfile --cflags` -c $<

%.o:	%.c
	$(CC) $(CFLAGS) -c $<

japlay:	$(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

install:	$(BINARY) $(PLUGINS)
	install -m 755 japlay $(DESTDIR)/usr/bin
	mkdir -p -m 755 $(DESTDIR)/usr/lib/japlay
	install -m 755 $(PLUGINS) $(DESTDIR)/usr/lib/japlay

clean:
	rm -f $(OBJ) $(PLUGIN_OBJS) $(BINARY) $(PLUGINS)

in_mad.so:	in_mad.o
	$(CC) in_mad.o -o $@ $(PLUGIN_LDFLAGS) `pkg-config mad --libs`

in_mikmod.so:	in_mikmod.o
	$(CC) in_mikmod.o -o $@ $(PLUGIN_LDFLAGS) `libmikmod-config --libs`

in_vorbis.so:	in_vorbis.o
	$(CC) in_vorbis.o -o $@ $(PLUGIN_LDFLAGS) `pkg-config vorbisfile --libs`
