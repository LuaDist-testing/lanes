-- This file was automatically generated for the LuaDist project.

--
-- Lanes rockspec
--
-- Ref:  
--      <http://luarocks.org/en/Rockspec_format>
--
-- History:
--  AKa 18-Aug-2008: 2.0-1
--

package = "lanes"

version = "2.0-1"

-- LuaDist source
source = {
  tag = "2.0-1",
  url = "git://github.com/LuaDist-testing/lanes.git"
}
-- Original source
-- source= {
--     url= "http://akauppi.googlepages.com/lanes-2.0.tgz",
--     md5= "27a807828de0bda3787dbcd2d4947019"
-- }

description = {
	summary= "Multithreading support for Lua",
	detailed= [[
        Lua Lanes is a portable, message passing multithreading library 
        providing the possibility to run multiple Lua states in parallel. 
    ]],
	license= "MIT/X11",
	homepage="http://kotisivu.dnainternet.net/askok/lanes/",
	maintainer="Asko Kauppi <akauppi@gmail.com>"	
}

-- Q: What is the difference of "windows" and "win32"?
--
supported_platforms= { "win32",     -- Q: what's the difference to "windows"?
                       "macosx", 
                       "linux", 
                       "freebsd",   -- TBD: not tested recently
                       --"msys",    -- TBD: not supported by LuaRocks 0.6
}

dependencies= {
    "lua >= 5.1, < 5.2",
}

--
-- Non-Win32: build using the Makefile
-- Win32: build using 'make-vc.cmd' and "manual" copy of products
--
-- TBD: How is MSYS treated?  We'd like (really) it to use the Makefile.
--      It should be a target like "cygwin", not defining "windows". 
--      "windows" should actually guarantee Visual C++ as the compiler.
--
-- Q: Does "win32" guarantee we have Visual C++ 2005/2008 command line tools?
--
-- Note: Cannot use the simple "module" build type, because we need to precompile
--       'src/keeper.lua' -> keeper.lch and bake it into lanes.c.
--
build = {

    -- Win32 (Visual C++) uses 'make-vc.cmd' for building
    --
    platforms= {
        windows= {
            type= "command",
            build_command= "make-vc.cmd",
            install= {
                lua = { "src/lanes.lua" },
                lib = { "src/lua51-lanes.dll" }
            }
        }
    },

    -- Other platforms use the Makefile
    --
    -- LuaRocks defines CFLAGS, LIBFLAG and LUA_INCDIR for 'make rock',
    --          defines LIBDIR, LUADIR for 'make install'
    --
    -- Ref: <http://www.luarocks.org/en/Paths_and_external_dependencies>
    --
    type = "make",
    
    build_target = "rock",
    build_variables= {
        CFLAGS= "$(CFLAGS) -I$(LUA_INCDIR)",
        LIBFLAG= "$(LIBFLAG)",
    },

    install_target = "install",
    install_variables= {
        LUA_LIBDIR= "$(LIBDIR)",
        LUA_SHAREDIR= "$(LUADIR)",
    }
}
