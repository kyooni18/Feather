# Feather: build and install as a C++ library (static + shared).
#
# Build:
#   make -r                 # recommended: no built-in implicit rules (avoids stray .o in the tree)
#   make                    # static + shared into dist/
#
# Install (system default /usr/local):
#   sudo make install
#
# Install to a staging tree (packaging):
#   make install DESTDIR=/tmp/stage PREFIX=/usr
#
# Install only headers or only libraries:
#   make install-headers
#   make install-libs
#
# Consumption (Feather/ include dir avoids unrelated top-level Feather.hpp / FeatherC):
#   c++ ... -std=c++17 -I<prefix>/include -L<prefix>/lib -lFeather
#   #include <Feather/Feather.hpp>
#   pkg-config --cflags --libs Feather   # after install (PKG_CONFIG_PATH if needed)

CXX      ?= c++
AR       ?= ar
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2 -fPIC
LDFLAGS  ?=

PREFIX      ?= /usr/local
INCLUDEDIR  ?= $(PREFIX)/include
LIBDIR      ?= $(PREFIX)/lib
VERSION     ?= 0.1.0

FEATHER_SYS := FeatherSystem

SRCS := \
	$(FEATHER_SYS)/Feather.cpp \
	$(FEATHER_SYS)/FeatherRuntime/FSScheduler.cpp \
	$(FEATHER_SYS)/FeatherRuntime/FSTime.cpp

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
	uninstall uninstall-libs uninstall-headers uninstall-pkgconfig

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

# Test binary for local run/debug (links static lib; -g -O0 for debugging test code).
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

# Written with final PREFIX (not DESTDIR); standard for relocatable .pc after unpack.
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
