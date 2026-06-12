CC = gcc

SRCS = gbafix.c

.PHONY: all clean

HOST_OS := $(shell uname -s 2>/dev/null || echo $(OS))
ifneq ($(filter Windows_NT MINGW% MSYS% CYGWIN%,$(HOST_OS)),)
EXE := .exe
else
EXE :=
endif

all: gbafix$(EXE)
	@:

gbafix$(EXE): $(SRCS)
	$(CC) $(SRCS) -o $@ $(LDFLAGS)

clean:
	$(RM) gbafix gbafix.exe
