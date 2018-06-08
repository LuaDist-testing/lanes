#
# Lanes/Makefile
#
#   make
#   make test
#   make basic|fifo|keeper|...
#
#   make perftest[-odd|-even|-plain]
#   make launchtest
#
#   make install DESTDIR=path
#   make tar|tgz VERSION=x.x
#   make clean
#

MODULE = lanes

N=1000

_SO=.so
TIME=time

ifeq "$(findstring MINGW32,$(shell uname -s))" "MINGW32"
  # MinGW MSYS on XP
  #
  LUA=lua
  LUAC=luac
  _SO=.dll
  TIME=timeit.exe
else
  # Autodetect LUA & LUAC
  #
  LUA=$(word 1,$(shell which lua5.1) $(shell which lua51) lua)
  LUAC=$(word 1,$(shell which luac5.1) $(shell which luac51) luac)
endif

PREFIX=LUA_CPATH=./src/?$(_SO) LUA_PATH="src/?.lua;./tests/?.lua"

#---
all: src/lua51-lanes$(_SO)

src/lua51-lanes$(_SO): src/*.lua src/*.c src/*.h
	cd src && $(MAKE) LUA=$(LUA) LUAC=$(LUAC)

clean:
	cd src && $(MAKE) clean

debug: src/lua51-lanes$(_SO)
	gdb $(LUA) tests/basic.lua


#--- LuaRocks automated build ---
#
rock:
	cd src && $(MAKE) LUAROCKS=1 CFLAGS="$(CFLAGS)" LIBFLAG="$(LIBFLAG)" LUA=$(LUA) LUAC=$(LUAC)


#--- Testing ---
#
test:
	$(MAKE) basic
	$(MAKE) fifo
	$(MAKE) keeper
	$(MAKE) timer
	$(MAKE) atomic
	$(MAKE) cyclic
	$(MAKE) objects
	$(MAKE) fibonacci
	$(MAKE) recursive

basic: tests/basic.lua src/lua51-lanes$(_SO)
	$(PREFIX) $(LUA) $<

fifo: tests/fifo.lua src/lua51-lanes$(_SO)
	$(PREFIX) $(LUA) $<

keeper: tests/keeper.lua src/lua51-lanes$(_SO)
	$(PREFIX) $(LUA) $<

fibonacci: tests/fibonacci.lua src/lua51-lanes$(_SO)
	$(PREFIX) $(LUA) $<

timer: tests/timer.lua src/lua51-lanes$(_SO)
	$(PREFIX) $(LUA) $<

atomic: tests/atomic.lua src/lua51-lanes$(_SO)
	$(PREFIX) $(LUA) $<

cyclic: tests/cyclic.lua src/lua51-lanes$(_SO)
	$(PREFIX) $(LUA) $<

recursive: tests/recursive.lua src/lua51-lanes$(_SO)
	$(PREFIX) $(LUA) $<

hangtest: tests/hangtest.lua src/lua51-lanes$(_SO)
	$(PREFIX) $(LUA) $<

ehynes: tests/ehynes.lua src/lua51-lanes$(_SO)
	$(PREFIX) $(LUA) $<

require: tests/require.lua src/lua51-lanes$(_SO)
	$(PREFIX) $(LUA) $<

objects: tests/objects.lua src/lua51-lanes$(_SO)
	$(PREFIX) $(LUA) $<

#---
perftest-plain: tests/perftest.lua src/lua51-lanes$(_SO)
	$(MAKE) _perftest ARGS="$< $(N) -plain"

perftest: tests/perftest.lua src/lua51-lanes$(_SO)
	$(MAKE) _perftest ARGS="$< $(N)"

perftest-odd: tests/perftest.lua src/lua51-lanes$(_SO)
	$(MAKE) _perftest ARGS="$< $(N) -prio=+2"

perftest-even: tests/perftest.lua src/lua51-lanes$(_SO)
	$(MAKE) _perftest ARGS="$< $(N) -prio=-2"

#---
launchtest: tests/launchtest.lua src/lua51-lanes$(_SO)
	$(MAKE) _perftest ARGS="$< $(N)"

_perftest:
	$(PREFIX) $(TIME) $(LUA) $(ARGS)


#--- Installing ---
#
# This is for installing to '/usr/local' or similar; through LuaRocks or manually.
#
# LUA_DIR, LUA_LIBDIR and LUA_SHAREDIR are overwritten by LuaRocks (don't change
# the names!)
#
LUA_DIR=/usr/local
LUA_LIBDIR=$(LUA_DIR)/lib/lua/5.1
LUA_SHAREDIR=$(LUA_DIR)/share/lua/5.1

install: src/lua51-lanes$(_SO) src/lanes.lua
	mkdir -p $(LUA_LIBDIR) $(LUA_SHAREDIR)
	cp src/lua51-lanes$(_SO) $(LUA_LIBDIR)
	cp src/lanes.lua $(LUA_SHAREDIR)


#--- Packaging ---
#
# Make a folder of the same name as tgz, good manners (for the manual
# expander)
#
# "make tgz VERSION=yyyymmdd"
#
VERSION=

tar tgz:
ifeq "$(VERSION)" ""
	echo "Usage: make tar VERSION=x.x"; false
else
	$(MAKE) clean 
	-rm -rf $(MODULE)-$(VERSION)
	mkdir $(MODULE)-$(VERSION)
	tar c --exclude=.svn --exclude=.DS_Store --exclude="_*" \
	        --exclude="*.tgz" --exclude="*.rockspec" \
	        --exclude=lanes.dev --exclude="$(MODULE)-*" --exclude=xcode * \
		--exclude="*.obj" --exclude="*.dll" --exclude=timeit.dat \
	   | (cd $(MODULE)-$(VERSION) && tar x)
	tar czvf $(MODULE)-$(VERSION).tgz $(MODULE)-$(VERSION)
	md5sum $(MODULE)-$(VERSION).tgz
endif
	
	
#--- Undocumented ---
#
run: src/lua51-lanes$(_SO)
	$(PREFIX) $(LUA) -e "require '$(MODULE)'" -i

echo:
	@echo $(PROGRAMFILES:C=X)

.PROXY:	all clean test require debug _nodemo _notest

