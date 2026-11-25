# Makefile for LiteBird
# Requires: g++, libfltk1.3-dev, libcurl4-openssl-dev
# On Ubuntu/Debian: apt-get install build-essential libfltk1.3-dev libcurl4-openssl-dev

CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall
LIBS = `fltk-config --cxxflags --ldflags` -lcurl

TARGET = litebird
SRC = litebird.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC) $(LIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

.PHONY: all clean install
