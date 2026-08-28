PREFIX ?= /usr/local
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=gnu23 -Wall -Wno-unused-function $(shell pkg-config --cflags glfw3 gl)
LDLIBS = $(shell pkg-config --libs glfw3 gl) -lm

VENDOR = vendor/stb_truetype.h vendor/stb_image_write.h vendor/Inter.ttf vendor/Excalifont.ttf
APPS = $(DESTDIR)$(PREFIX)/share/applications
ICONS = $(DESTDIR)$(PREFIX)/share/icons/hicolor

paintboard: paintboard.c $(VENDOR)
	$(CC) $(CFLAGS) $(LDFLAGS) paintboard.c $(LDLIBS) -o $@

test: paintboard
	./paintboard --test

install: paintboard
	install -Dm755 paintboard $(DESTDIR)$(PREFIX)/bin/paintboard
	install -Dm644 assets/paintboard.desktop $(APPS)/paintboard.desktop
	install -Dm644 assets/paintboard.svg $(ICONS)/scalable/apps/paintboard.svg
	install -Dm644 assets/paintboard.png $(ICONS)/256x256/apps/paintboard.png

# Regenerate the raster icon after editing the svg. Needs resvg.
icon:
	resvg --width 256 --height 256 assets/paintboard.svg assets/paintboard.png

clean:
	rm -f paintboard

.PHONY: test install icon clean
