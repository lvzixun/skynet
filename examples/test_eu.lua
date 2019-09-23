local skynet = require "skynet"
local service = require "skynet.service"


local function echo_service()
    local skynet = require "skynet"
    local b, e = false, false
    local function do_echo(x, y, z)
        if x == 1 then
            b = skynet.now()
            print("b = ", b)
        end

        if x == 1000000 then
            e = skynet.now()
            print("e = ", e)
        end
        local d = x + y + z
        if b and e then
            print("cost:",e - b, "count:", x)
        end
        -- print("skynet.now()", skynet.now())
    end

    skynet.dispatch("lua", function(_,source,cmd,...)
        do_echo(...)
    end)
end

skynet.start(function ()
    local echo_addr = service.new("echo_service", echo_service)
    local len = 1000000
    print(skynet.now(), "###")
    for i=1,len do
        local a, b, c = i, i+1, i+2
        skynet.send(echo_addr, "lua", "do_echo", a, b, c)
    end
end)