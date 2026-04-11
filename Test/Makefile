CXX ?= c++

ROOT := ..
FEATHER_SYS := $(ROOT)/FeatherSystem
STATIC := $(ROOT)/dist/libFeather.a

.PHONY: all clean

all: test1

$(STATIC):
	$(MAKE) -C $(ROOT) static

test1: test1.cpp $(STATIC)
	$(CXX) -std=c++17 -g -O0 -Wall -Wextra -Wpedantic -I$(FEATHER_SYS) test1.cpp $(STATIC) -o $@

clean:
	rm -f test1
