# Feather: build and install as a C++ library (static + shared).
# Consumption: -I<prefix>/include/feather -L<prefix>/lib -lfeather

CXX      ?= c++
AR       ?= ar
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2 -fPIC
LDFLAGS  ?=

PREFIX      ?= /usr/local
INCLUDEDIR  ?= $(PREFIX)/include
LIBDIR      ?= $(PREFIX)/lib

FEATHER_SYS := FeatherSystem

SRCS := \
	$(FEATHER_SYS)/Feather.cpp \
	$(FEATHER_SYS)/FeatherRuntime/FSScheduler.cpp \
	$(FEATHER_SYS)/FeatherRuntime/FSTime.cpp

INCLUDES := -I$(FEATHER_SYS) -I$(FEATHER_SYS)/FeatherRuntime

DIST    := dist
OBJDIR  := $(DIST)/obj
OBJS    := $(SRCS:%.cpp=$(OBJDIR)/%.o)
STATIC  := $(DIST)/libfeather.a

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  SHARED  := $(DIST)/libfeather.dylib
  SHFLAGS := -dynamiclib -install_name @rpath/libfeather.dylib
else
  SHARED  := $(DIST)/libfeather.so
  SHFLAGS := -shared -Wl,-soname,libfeather.so
endif

.PHONY: all static shared clean install uninstall

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

clean:
	rm -rf $(DIST)

install: all
	install -d $(DESTDIR)$(LIBDIR)
	install -m 644 $(STATIC) $(DESTDIR)$(LIBDIR)/
	install -m 755 $(SHARED) $(DESTDIR)$(LIBDIR)/
	install -d $(DESTDIR)$(INCLUDEDIR)/feather/FeatherRuntime
	install -m 644 $(FEATHER_SYS)/Feather.hpp $(DESTDIR)$(INCLUDEDIR)/feather/
	install -m 644 $(FEATHER_SYS)/FeatherRuntime/*.hpp $(DESTDIR)$(INCLUDEDIR)/feather/FeatherRuntime/

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/libfeather.a
	rm -f $(DESTDIR)$(LIBDIR)/libfeather.dylib
	rm -f $(DESTDIR)$(LIBDIR)/libfeather.so
	rm -rf $(DESTDIR)$(INCLUDEDIR)/feather
