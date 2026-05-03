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
CMAKEPKGDIR ?= $(LIBDIR)/cmake/Feather
VERSION     ?= 0.1.0

FEATHER_SYS := FeatherSystem
ARDUINO_NAME ?= Feather
ARDUINO_OUT  ?= dist/arduino/$(ARDUINO_NAME)
ARDUINO_LIBDIR ?= $(HOME)/Documents/Arduino/libraries

CORE_SRCS := \
	$(FEATHER_SYS)/FeatherRuntime/FSTime.cpp

SCHEDULER_SRCS := \
	$(FEATHER_SYS)/FeatherRuntime/FSScheduler.cpp

EVENT_SRCS :=

FACADE_SRCS := \
	$(FEATHER_SYS)/Feather.cpp

SRCS := \
	$(FACADE_SRCS) \
	$(CORE_SRCS) \
	$(SCHEDULER_SRCS) \
	$(EVENT_SRCS)

CORE_HDRS := \
	$(FEATHER_SYS)/Core/Clock.hpp

SCHEDULER_HDRS := \
	$(FEATHER_SYS)/Scheduler/Scheduler.hpp

EVENT_HDRS := \
	$(FEATHER_SYS)/Events/Event.hpp

PLATFORM_HDRS := \
	$(FEATHER_SYS)/Platform/Platform.hpp

UI_HDRS := \
	$(FEATHER_SYS)/UI/Display.hpp \
	$(FEATHER_SYS)/FeatherUI/FSDisplay.hpp

COMPAT_HDRS := \
	$(FEATHER_SYS)/FeatherRuntime/FSEvent.hpp \
	$(FEATHER_SYS)/FeatherRuntime/FSScheduler.hpp \
	$(FEATHER_SYS)/FeatherRuntime/FSTime.hpp

HDRS := \
	$(FEATHER_SYS)/Feather.hpp \
	$(CORE_HDRS) \
	$(SCHEDULER_HDRS) \
	$(EVENT_HDRS) \
	$(PLATFORM_HDRS) \
	$(UI_HDRS) \
	$(COMPAT_HDRS)

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
	install install-libs install-headers install-pkgconfig install-cmake \
	uninstall uninstall-libs uninstall-headers uninstall-pkgconfig uninstall-cmake \
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

install: install-libs install-headers install-pkgconfig install-cmake

install-libs: static shared
	install -d $(DESTDIR)$(LIBDIR)
	install -m 644 $(STATIC) $(DESTDIR)$(LIBDIR)/
	install -m 755 $(SHARED) $(DESTDIR)$(LIBDIR)/

install-headers:
	install -d $(DESTDIR)$(INCLUDEDIR)/Feather
	install -d $(DESTDIR)$(INCLUDEDIR)/Feather/Core
	install -d $(DESTDIR)$(INCLUDEDIR)/Feather/Scheduler
	install -d $(DESTDIR)$(INCLUDEDIR)/Feather/Events
	install -d $(DESTDIR)$(INCLUDEDIR)/Feather/Platform
	install -d $(DESTDIR)$(INCLUDEDIR)/Feather/UI
	install -d $(DESTDIR)$(INCLUDEDIR)/Feather/FeatherUI
	install -d $(DESTDIR)$(INCLUDEDIR)/Feather/FeatherRuntime
	install -m 644 $(FEATHER_SYS)/Feather.hpp $(DESTDIR)$(INCLUDEDIR)/Feather/
	install -m 644 $(CORE_HDRS) $(DESTDIR)$(INCLUDEDIR)/Feather/Core/
	install -m 644 $(SCHEDULER_HDRS) $(DESTDIR)$(INCLUDEDIR)/Feather/Scheduler/
	install -m 644 $(EVENT_HDRS) $(DESTDIR)$(INCLUDEDIR)/Feather/Events/
	install -m 644 $(PLATFORM_HDRS) $(DESTDIR)$(INCLUDEDIR)/Feather/Platform/
	install -m 644 $(FEATHER_SYS)/UI/Display.hpp $(DESTDIR)$(INCLUDEDIR)/Feather/UI/
	install -m 644 $(FEATHER_SYS)/FeatherUI/FSDisplay.hpp $(DESTDIR)$(INCLUDEDIR)/Feather/FeatherUI/
	install -m 644 $(COMPAT_HDRS) $(DESTDIR)$(INCLUDEDIR)/Feather/FeatherRuntime/

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

install-cmake:
	install -d $(DESTDIR)$(CMAKEPKGDIR)
	@printf '%s\n' \
		'include(CMakeFindDependencyMacro)' \
		'get_filename_component(_FEATHER_PREFIX "$${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)' \
		'' \
		'if(NOT TARGET Feather::Core)' \
		'  add_library(Feather::Core INTERFACE IMPORTED)' \
		'  set_target_properties(Feather::Core PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "$${_FEATHER_PREFIX}/include")' \
		'endif()' \
		'' \
		'if(NOT TARGET Feather::Scheduler)' \
		'  add_library(Feather::Scheduler INTERFACE IMPORTED)' \
		'  target_link_libraries(Feather::Scheduler INTERFACE Feather::Core)' \
		'endif()' \
		'' \
		'if(NOT TARGET Feather::Events)' \
		'  add_library(Feather::Events INTERFACE IMPORTED)' \
		'  target_link_libraries(Feather::Events INTERFACE Feather::Scheduler)' \
		'endif()' \
		'' \
		'if(NOT TARGET Feather::Feather)' \
		'  add_library(Feather::Feather UNKNOWN IMPORTED)' \
		'  set_target_properties(Feather::Feather PROPERTIES IMPORTED_LOCATION "$${_FEATHER_PREFIX}/lib/libFeather.a" INTERFACE_INCLUDE_DIRECTORIES "$${_FEATHER_PREFIX}/include")' \
		'endif()' \
		'' \
		'if(NOT TARGET Feather::Static)' \
		'  add_library(Feather::Static INTERFACE IMPORTED)' \
		'  target_link_libraries(Feather::Static INTERFACE Feather::Feather)' \
		'endif()' \
		'' \
		'if(NOT TARGET Feather::Shared)' \
		'  add_library(Feather::Shared UNKNOWN IMPORTED)' \
		'  if(EXISTS "$${_FEATHER_PREFIX}/lib/libFeather.dylib")' \
		'    set(_FEATHER_SHARED "$${_FEATHER_PREFIX}/lib/libFeather.dylib")' \
		'  else()' \
		'    set(_FEATHER_SHARED "$${_FEATHER_PREFIX}/lib/libFeather.so")' \
		'  endif()' \
		'  set_target_properties(Feather::Shared PROPERTIES IMPORTED_LOCATION "$${_FEATHER_SHARED}" INTERFACE_INCLUDE_DIRECTORIES "$${_FEATHER_PREFIX}/include")' \
		'endif()' \
		'' \
		'if(NOT TARGET Feather::UI)' \
		'  add_library(Feather::UI INTERFACE IMPORTED)' \
		'  target_link_libraries(Feather::UI INTERFACE Feather::Feather)' \
		'endif()' \
		'' \
		'unset(_FEATHER_SHARED)' \
		'unset(_FEATHER_PREFIX)' \
	> $(DESTDIR)$(CMAKEPKGDIR)/FeatherConfig.cmake
	@printf '%s\n' \
		'set(PACKAGE_VERSION "$(VERSION)")' \
		'if(PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)' \
		'  set(PACKAGE_VERSION_COMPATIBLE FALSE)' \
		'else()' \
		'  set(PACKAGE_VERSION_COMPATIBLE TRUE)' \
		'  if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)' \
		'    set(PACKAGE_VERSION_EXACT TRUE)' \
		'  endif()' \
		'endif()' \
	> $(DESTDIR)$(CMAKEPKGDIR)/FeatherConfigVersion.cmake

uninstall: uninstall-libs uninstall-headers uninstall-pkgconfig uninstall-cmake

uninstall-libs:
	rm -f $(DESTDIR)$(LIBDIR)/libFeather.a
	rm -f $(DESTDIR)$(LIBDIR)/libFeather.dylib
	rm -f $(DESTDIR)$(LIBDIR)/libFeather.so

uninstall-headers:
	rm -rf $(DESTDIR)$(INCLUDEDIR)/Feather

uninstall-pkgconfig:
	rm -f $(DESTDIR)$(LIBDIR)/pkgconfig/Feather.pc

uninstall-cmake:
	rm -rf $(DESTDIR)$(CMAKEPKGDIR)

# --- Arduino export ---

arduino-export: arduino-clean
	install -d $(ARDUINO_OUT)
	install -d $(ARDUINO_OUT)/src
	install -d $(ARDUINO_OUT)/src/Core
	install -d $(ARDUINO_OUT)/src/Scheduler
	install -d $(ARDUINO_OUT)/src/Events
	install -d $(ARDUINO_OUT)/src/Platform
	install -d $(ARDUINO_OUT)/src/UI
	install -d $(ARDUINO_OUT)/src/FeatherUI
	install -d $(ARDUINO_OUT)/src/FeatherRuntime
	@if [ -d examples ]; then cp -R examples $(ARDUINO_OUT)/; fi
	@cp $(FEATHER_SYS)/Feather.hpp $(ARDUINO_OUT)/src/
	@cp $(FEATHER_SYS)/Feather.cpp $(ARDUINO_OUT)/src/
	@cp $(CORE_HDRS) $(ARDUINO_OUT)/src/Core/
	@cp $(SCHEDULER_HDRS) $(ARDUINO_OUT)/src/Scheduler/
	@cp $(EVENT_HDRS) $(ARDUINO_OUT)/src/Events/
	@cp $(PLATFORM_HDRS) $(ARDUINO_OUT)/src/Platform/
	@cp $(FEATHER_SYS)/UI/Display.hpp $(ARDUINO_OUT)/src/UI/
	@cp $(FEATHER_SYS)/FeatherUI/FSDisplay.hpp $(ARDUINO_OUT)/src/FeatherUI/
	@cp $(FEATHER_SYS)/FeatherRuntime/FSEvent.hpp $(ARDUINO_OUT)/src/FeatherRuntime/
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
