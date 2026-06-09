-- wrk POST helper: wrk -s wrk_post.lua URL -- METHOD HEADER BODY
local method = "POST"
local header_key = "Content-Type"
local header_val = "application/json"
local body = "{}"

function init(args)
    if #args >= 3 then
        method = args[1]
        local header_line = args[2]
        body = args[3]
        local colon = string.find(header_line, ":")
        header_key = string.sub(header_line, 1, colon - 1)
        header_val = string.sub(header_line, colon + 2)
    end
end

request = function()
    return wrk.format(method, wrk.path, { [header_key] = header_val }, body)
end
