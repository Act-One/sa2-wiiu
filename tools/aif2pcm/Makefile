CC = gcc

CFLAGS = -Wall -Wextra -Wno-switch -Werror -std=c11 -O2 -s

LIBS = -lm

SRCS = main.c extended.c

.PHONY: all clean

HOST_OS := $(shell uname -s 2>/dev/null || echo $(OS))
ifneq ($(filter Windows_NT MINGW% MSYS% CYGWIN%,$(HOST_OS)),)
EXE := .exe
else
EXE :=
endif

all: aif2pcm$(EXE)
	@:

aif2pcm$(EXE): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS) $(LIBS)

clean:
	$(RM) aif2pcm aif2pcm.exe
