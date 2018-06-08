--
-- RECURSIVE.LUA
--
-- Test program for Lua Lanes
--

--
-- This uses an upvalue that refers to itself
--
-- Gives error: "Recursive use of upvalues; cannot copy the function (TBD)"
--              (Lanes 13-Aug-2008)
--
if false then
    io.stderr:write( "depth A = " )
    local func
    func =
    function( depth )
        io.stderr:write(" " .. depth)
        if depth > 10 then
            return "done!"
        end

        require "lanes"
        local lane= lanes.gen("*", func)( depth+1 )
        return lane[1]
    end
    
    local v= func(0)
    assert(v=="done!")
    io.stderr:write("\n")
end


--
-- Same but using 'debug.getinfo' to get the current function
--
if true then
    io.stderr:write( "depth B = " )

    local function f( depth )
        io.stderr:write( " "..depth )
        if depth > 10 then
            return "done!"
        end
    
        assert( require )
        require "lanes"     -- gives: "'package.loaders' must be a table"

        local lane= lanes.gen("*", debug.getinfo(1,"f").func)( depth+1 )
        return lane[1]
    end
    
    local v= f(0)
    assert(v=="done!")
    io.stderr:write("\n")
end


--
-- Same with 'lanes' being passed as upvalue (it is recommended to require
-- modules separately per each lane, but this seems to work, too).
-- 
if true then
    io.stderr:write( "depth C = " )

    local lanes= require "lanes"
    assert(lanes)

    local function f( depth )
        io.stderr:write(" "..depth)
        if depth > 10 then
            return "done!"
        end
    
        local lgen= lanes.gen("*", debug.getinfo(1,"f").func)
        local lane= lgen( depth+1 )
        return lane[1]
    end
    
    local v= f(0)
    assert(v=="done!")
    io.stderr:write "\n"
end
