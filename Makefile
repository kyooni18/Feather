# Feather: build and install as a C++ library (static + shared + Arduino export).
#
# Build:
#   make -r
#   make
#
# Install:
#   sudo make install
#   make install DESTDIR=/tmp/stage PREFIX=/usr
#
# Arduino export:
#   make arduino-export
#   make arduino-install ARDUINO_LIBDIR=~/Documents/Arduino/libraries
#
# Consumption:
#   c++ ... -std=c++17 -I<prefix>/include -L<prefix>/lib -lFeather
#   #include <Feather/Feather.hpp>
#   pkg-config --cflags --libs Feather

CXX      ?= c++
AR       ?= ar
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2 -fPIC
LDFLAGS  ?=

PREFIX      ?= /usr/local
INCLUDEDIR  ?= $(PREFIX)/include
LIBDIR      ?= $(PREFIX)/lib
VERSION     ?= 0.1.0

FEATHER_SYS := FeatherSystem
ARDUINO_NAME ?= Feather
ARDUINO_OUT  ?= dist/arduino/$(ARDUINO_NAME)
ARDUINO_LIBDIR ?= $(HOME)/Documents/Arduino/libraries

SRCS := \
	$(FEATHER_SYS)/Feather.cpp \
	$(FEATHER_SYS)/FeatherRuntime/FSScheduler.cpp \
	$(FEATHER_SYS)/FeatherRuntime/FSTime.cpp

HDRS := \
	$(FEATHER_SYS)/Feather.hpp \
	$(FEATHER_SYS)/FeatherRuntime/FSScheduler.hpp \
	$(FEATHER_SYS)/FeatherRuntime/FSTime.hpp

INCLUDES := -I$(FEATHER_SYS) -I$(FEATHER_SYS)/FeatherRuntime

DIST    := dist
OBJDIR  := $(DIST)/obj
OBJS    := $(SRCS:%.cpp=$(OBJDIR)/%.o)
STATIC  := $(DIST)/libFeather.a

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  SHARED  := $(DIST)/libFeather.dylib
  SHFLAGS := -dynamiclib -install_name @rpath/libFeather.dylib
else
  SHARED  := $(DIST)/libFeather.so
  SHFLAGS := -shared -Wl,-soname,libFeather.so
endif

TEST_BIN := $(DIST)/test1

.PHONY: all static shared test1 clean \
	install install-libs install-headers install-pkgconfig \
	uninstall uninstall-libs uninstall-headers uninstall-pkgconfig \
	arduino-export arduino-install arduino-clean

all: static shared

static: $(STATIC)

shared: $(SHARED)

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(STATIC): $(OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(SHARED): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(SHFLAGS) $(LDFLAGS) -o $@ $^

test1: static
	@mkdir -p $(DIST)
	$(CXX) -std=c++17 -g -O0 -Wall -Wextra -Wpedantic -I$(FEATHER_SYS) Test/test1.cpp $(STATIC) -o $(TEST_BIN)

clean:
	rm -rf $(DIST)
	rm -f $(CURDIR)/-.o

# --- installation ---

install: install-libs install-headers install-pkgconfig

install-libs: static shared
	install -d $(DESTDIR)$(LIBDIR)
	install -m 644 $(STATIC) $(DESTDIR)$(LIBDIR)/
	install -m 755 $(SHARED) $(DESTDIR)$(LIBDIR)/

install-headers:
	install -d $(DESTDIR)$(INCLUDEDIR)/Feather
	install -d $(DESTDIR)$(INCLUDEDIR)/Feather/FeatherRuntime
	install -m 644 $(FEATHER_SYS)/Feather.hpp $(DESTDIR)$(INCLUDEDIR)/Feather/
	install -m 644 $(FEATHER_SYS)/FeatherRuntime/FSScheduler.hpp $(DESTDIR)$(INCLUDEDIR)/Feather/FeatherRuntime/
	install -m 644 $(FEATHER_SYS)/FeatherRuntime/FSTime.hpp $(DESTDIR)$(INCLUDEDIR)/Feather/FeatherRuntime/

install-pkgconfig:
	install -d $(DESTDIR)$(LIBDIR)/pkgconfig
	@echo "prefix=$(PREFIX)" > $(DESTDIR)$(LIBDIR)/pkgconfig/Feather.pc
	@echo 'exec_prefix=$${prefix}' >> $(DESTDIR)$(LIBDIR)/pkgconfig/Feather.pc
	@echo 'libdir=$${exec_prefix}/lib' >> $(DESTDIR)$(LIBDIR)/pkgconfig/Feather.pc
	@echo 'includedir=$${prefix}/include' >> $(DESTDIR)$(LIBDIR)/pkgconfig/Feather.pc
	@echo '' >> $(DESTDIR)$(LIBDIR)/pkgconfig/Feather.pc
	@echo 'Name: Feather' >> $(DESTDIR)$(LIBDIR)/pkgconfig/Feather.pc
	@echo 'Description: Feather C++ library' >> $(DESTDIR)$(LIBDIR)/pkgconfig/Feather.pc
	@echo 'Version: $(VERSION)' >> $(DESTDIR)$(LIBDIR)/pkgconfig/Feather.pc
	@echo 'Libs: -L$${libdir} -lFeather -Wl,-rpath,$${libdir}' >> $(DESTDIR)$(LIBDIR)/pkgconfig/Feather.pc
	@echo 'Cflags: -I$${includedir}' >> $(DESTDIR)$(LIBDIR)/pkgconfig/Feather.pc

uninstall: uninstall-libs uninstall-headers uninstall-pkgconfig

uninstall-libs:
	rm -f $(DESTDIR)$(LIBDIR)/libFeather.a
	rm -f $(DESTDIR)$(LIBDIR)/libFeather.dylib
	rm -f $(DESTDIR)$(LIBDIR)/libFeather.so

uninstall-headers:
	rm -rf $(DESTDIR)$(INCLUDEDIR)/Feather

uninstall-pkgconfig:
	rm -f $(DESTDIR)$(LIBDIR)/pkgconfig/Feather.pc

# --- Arduino export ---

arduino-export: arduino-clean
	install -d $(ARDUINO_OUT)
	install -d $(ARDUINO_OUT)/src
	install -d $(ARDUINO_OUT)/src/FeatherRuntime
	@if [ -d examples ]; then cp -R examples $(ARDUINO_OUT)/; fi
	@cp $(FEATHER_SYS)/Feather.hpp $(ARDUINO_OUT)/src/
	@cp $(FEATHER_SYS)/Feather.cpp $(ARDUINO_OUT)/src/
	@cp $(FEATHER_SYS)/FeatherRuntime/FSScheduler.hpp $(ARDUINO_OUT)/src/FeatherRuntime/
	@cp $(FEATHER_SYS)/FeatherRuntime/FSScheduler.cpp $(ARDUINO_OUT)/src/FeatherRuntime/
	@cp $(FEATHER_SYS)/FeatherRuntime/FSTime.hpp $(ARDUINO_OUT)/src/FeatherRuntime/
	@cp $(FEATHER_SYS)/FeatherRuntime/FSTime.cpp $(ARDUINO_OUT)/src/FeatherRuntime/
	@printf '%s\n' \
		'name=$(ARDUINO_NAME)' \
		'version=$(VERSION)' \
		'author=' \
		'maintainer=' \
		'sentence=Feather task scheduling library' \
		'paragraph=Feather runtime and scheduler for Arduino.' \
		'category=Timing' \
		'url=' \
		'architectures=*' \
	> $(ARDUINO_OUT)/library.properties
	@printf '%s\n' \
		'# $(ARDUINO_NAME)' \
		'' \
		'Arduino export of Feather.' \
		'' \
		'Include with:' \
		'' \
		'```cpp' \
		'#include <Feather.hpp>' \
		'```' \
	> $(ARDUINO_OUT)/README.md

arduino-install: arduino-export
	rm -rf $(ARDUINO_LIBDIR)/$(ARDUINO_NAME)
	install -d $(ARDUINO_LIBDIR)
	cp -R $(ARDUINO_OUT) $(ARDUINO_LIBDIR)/

arduino-clean:
	rm -rf $(DIST)/arduino
