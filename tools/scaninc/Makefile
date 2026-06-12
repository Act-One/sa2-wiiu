CXX ?= g++

CXXFLAGS = -Wall -Werror -std=c++11 -O2

SRCS = scaninc.cpp c_file.cpp asm_file.cpp source_file.cpp

HEADERS := scaninc.h asm_file.h c_file.h source_file.h

.PHONY: all clean

HOST_OS := $(shell uname -s 2>/dev/null || echo $(OS))
ifneq ($(filter Windows_NT MINGW% MSYS% CYGWIN%,$(HOST_OS)),)
EXE := .exe
else
EXE :=
endif

all: scaninc$(EXE)
	@:

scaninc$(EXE): $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ $(LDFLAGS)

clean:
	$(RM) scaninc scaninc.exe
