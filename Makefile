CC=clang
OFLAGS=-O2
WFLAGS=-Wall -Wpedantic -Wextra -Wconversion -Wstrict-prototypes -Werror=implicit-function-declaration -Werror=implicit-int -Werror=incompatible-pointer-types -Werror=int-conversion
CFLAGS=-std=c99 `pkg-config --cflags --libs libcurl yajl mpv`
DFLAGS=-g -O1 -fno-omit-frame-pointer -fno-optimize-sibling-calls -fsanitize=address -fsanitize=undefined

all: src/main.c src/network.c
	$(CC) $(CFLAGS) $^ -o jftui

debug: src/main.c src/network.c
	$(CC) $(CFLAGS) $(DFLAGS) $^ -o jftui_debug

clean:
	rm -f jftui jftui_debug
