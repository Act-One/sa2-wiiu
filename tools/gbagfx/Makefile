CC = gcc

# TODO: why does error-maybe-uninitialized appear on some peoples systems
CFLAGS = -Wall -Wextra -Werror -Wno-sign-compare -Wno-maybe-uninitialized -Wno-unknown-warning-option -std=c11 -O2 -DPNG_SKIP_SETJMP_CHECK
CFLAGS += $(shell pkg-config --cflags libpng)

LIBS = -lpng -lz
LDFLAGS += $(shell pkg-config --libs-only-L libpng)

SRCS = main.c convert_png.c gfx.c jasc_pal.c lz.c rl.c util.c font.c huff.c

.PHONY: all clean

HOST_OS := $(shell uname -s 2>/dev/null || echo $(OS))
ifneq ($(filter Windows_NT MINGW% MSYS% CYGWIN%,$(HOST_OS)),)
EXE := .exe
else
EXE :=
endif

all: gbagfx$(EXE)
	@:

gbagfx-debug$(EXE): $(SRCS) convert_png.h gfx.h global.h jasc_pal.h lz.h rl.h util.h font.h
	$(CC) $(CFLAGS) -DDEBUG $(SRCS) -o $@ $(LDFLAGS) $(LIBS)

gbagfx$(EXE): $(SRCS) convert_png.h gfx.h global.h jasc_pal.h lz.h rl.h util.h font.h
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS) $(LIBS)

clean:
	$(RM) gbagfx gbagfx.exe gbagfx-debug gbagfx-debug.exe
