# eveo - live previews for multiboxed EVE Online clients on Linux

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

PKGS    := xcb xcb-composite xcb-render xcb-randr
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Isrc
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))

SRC     := src/main.c src/thumbs.c src/config.c src/x11util.c
OBJ     := $(SRC:.c=.o)

all: eveo

eveo: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): src/eveo.h
src/thumbs.o: src/font.h

# Development helper: a stand-in EVE window, so the previewer can be
# exercised without running the game.
fake-client: tools/fake-client.c
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --libs xcb)

install: eveo
	install -Dm755 eveo $(DESTDIR)$(BINDIR)/eveo

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/eveo

clean:
	rm -f eveo fake-client $(OBJ)

.PHONY: all install uninstall clean
