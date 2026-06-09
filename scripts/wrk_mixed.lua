-- Rotate through all feature routes during a single wrk run
local routes = {
    "/text",
    "/im-json",
    "/rm-json",
    "/template",
    "/template-live",
    "/static-file",
    "/write-manual",
    "/stream",
    "/async",
    "/endpoint",
    "/api/health",
    "/api/version",
    "/users/42/posts/100",
    "/hello/world",
    "/files/x/y",
    "/metrics",
    "/headers",
    "/secure",
    "/secure-async",
}

local i = 0

request = function()
    i = i + 1
    local path = routes[(i % #routes) + 1]
    if path == "/secure" or path == "/secure-async" then
        return wrk.format("GET", path, { ["X-Test-Token"] = "secret" })
    end
    return wrk.format("GET", path)
end
