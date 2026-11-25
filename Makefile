# Makefile for LiteBird
# Requires: g++, libfltk1.3-dev, libcurl4-openssl-dev
# On Ubuntu/Debian: apt-get install build-essential libfltk1.3-dev libcurl4-openssl-dev

CXX ?= g++
# CHANGED: -Wextra added; FLTK compile flags now go to the compile step via
# CPPFLAGS instead of only being mixed into the link libraries.
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
FLTK_CONFIG ?= fltk-config
# FIX: `fltk-config` was run in backticks by the shell; a missing fltk-config
# then produced a confusing compiler error. $(shell) + a check fails clearly.
FLTK_CXXFLAGS := $(shell $(FLTK_CONFIG) --cxxflags 2>/dev/null)
FLTK_LDFLAGS  := $(shell $(FLTK_CONFIG) --ldflags 2>/dev/null)
# FIX: -pthread is required now that page loads run on a worker thread
# (and was needed on glibc < 2.34, e.g. Ubuntu 18.04, for any std::thread use).
LIBS = $(FLTK_LDFLAGS) -lcurl -pthread

PREFIX ?= /usr/local
DESTDIR ?=

TARGET = litebird
SRC = litebird.cpp

all: check-deps $(TARGET)

check-deps:
	@command -v $(FLTK_CONFIG) >/dev/null 2>&1 || { echo "error: $(FLTK_CONFIG) not found (install libfltk1.3-dev)"; exit 1; }

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(FLTK_CXXFLAGS) $(CPPFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LIBS)

# CHANGED: unit tests for the URL / HTML / charset helpers.
tests/test_helpers: tests/test_helpers.cpp $(SRC)
	$(CXX) $(CXXFLAGS) $(FLTK_CXXFLAGS) -o $@ tests/test_helpers.cpp $(LDFLAGS) $(LIBS)

test: tests/test_helpers
	./tests/test_helpers

clean:
	rm -f $(TARGET) tests/test_helpers

# FIX: honour PREFIX/DESTDIR (packaging, chroot/ISO builds) and create the dir.
install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/

.PHONY: all clean install test check-deps
