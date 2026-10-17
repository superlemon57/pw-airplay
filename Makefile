PREFIX ?= $(HOME)/.local
PKGS = libpipewire-0.3 libcurl json-c avahi-client libsystemd
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
	  $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lm -lpthread

SRCS = src/pw-airplayd.c src/sink.c src/engine.c src/owntone.c src/config.c src/mdns.c \
       src/mpris.c src/nowplaying.c
OBJS = $(SRCS:.c=.o)

all: pw-airplayd

pw-airplayd: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

src/%.o: src/%.c src/*.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: pw-airplayd
	install -Dm755 pw-airplayd $(DESTDIR)$(PREFIX)/bin/pw-airplayd

clean:
	rm -f pw-airplayd $(OBJS)

.PHONY: all install clean
