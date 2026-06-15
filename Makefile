WLRSC_XML := protocols/wlr-screencopy-unstable-v1.xml
LAYER_XML := protocols/wlr-layer-shell-unstable-v1.xml
XDG_XML := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml

C_FLAGS := $(shell pkg-config --cflags wayland-client) -I. -Wall
LDFLAGS := $(shell pkg-config --libs wayland-client) -lm

BASE_HDR := wlr-screencopy-unstable-v1.h wlr-layer-shell-unstable-v1.h xdg-shell.h
BASE_SRC := wlr-screencopy-unstable-v1.c wlr-layer-shell-unstable-v1.c xdg-shell.c
BASE_OBJ := $(BASE_SRC:.c=.o)

.PHONY: all clean

all: magnifier

$(BASE_HDR) $(BASE_SRC): $(WLRSC_XML) $(LAYER_XML) $(XDG_XML)
	wayland-scanner client-header $(WLRSC_XML) wlr-screencopy-unstable-v1.h
	wayland-scanner private-code $(WLRSC_XML) wlr-screencopy-unstable-v1.c
	wayland-scanner client-header $(LAYER_XML) wlr-layer-shell-unstable-v1.h
	wayland-scanner private-code $(LAYER_XML) wlr-layer-shell-unstable-v1.c
	wayland-scanner client-header $(XDG_XML) xdg-shell.h
	wayland-scanner private-code $(XDG_XML) xdg-shell.c

$(BASE_OBJ): $(BASE_SRC) $(BASE_HDR)
	$(CC) $(C_FLAGS) -c -o wlr-screencopy-unstable-v1.o wlr-screencopy-unstable-v1.c
	$(CC) $(C_FLAGS) -c -o wlr-layer-shell-unstable-v1.o wlr-layer-shell-unstable-v1.c
	$(CC) $(C_FLAGS) -c -o xdg-shell.o xdg-shell.c

magnifier: main.c $(BASE_OBJ) $(BASE_HDR)
	$(CC) $(C_FLAGS) -o $@ main.c $(BASE_OBJ) $(LDFLAGS)

clean:
	rm -f magnifier $(BASE_HDR) $(BASE_SRC) $(BASE_OBJ)
