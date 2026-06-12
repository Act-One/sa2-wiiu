CXX := g++

CXXFLAGS := -I ext/gbagfx -std=c++11 -O3 -Wall -Wno-switch -Werror

SRCS := asm_file.cpp c_file.cpp charmap.cpp preproc.cpp string_parser.cpp \
	utf8.cpp ext/gbagfx/jasc_pal.cpp

HEADERS := asm_file.h c_file.h char_util.h charmap.h preproc.h string_parser.h \
	utf8.h

.PHONY: all clean

HOST_OS := $(shell uname -s 2>/dev/null || echo $(OS))
ifneq ($(filter Windows_NT MINGW% MSYS% CYGWIN%,$(HOST_OS)),)
EXE := .exe
else
EXE :=
endif

all: preproc$(EXE)
	@:

preproc$(EXE): $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ $(LDFLAGS)

clean:
	$(RM) preproc preproc.exe
