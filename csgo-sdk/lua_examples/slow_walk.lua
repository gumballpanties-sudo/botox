-- hold a key to walk slower: scales your own move keys, never adds input
local key    = ui.keybind("slow walk key", 0x12, 1) -- 0x12 = VK_MENU (alt)
local amount = ui.slider_float("speed", 0.1, 1.0, 0.5, "%.2fx")

callbacks.register("create_move", function(cmd)
    if not key:get() then return end

    local scale = amount:get()
    cmd.forwardmove = cmd.forwardmove * scale
    cmd.sidemove    = cmd.sidemove * scale
end)
