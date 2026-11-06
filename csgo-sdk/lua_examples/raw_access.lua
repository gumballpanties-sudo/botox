-- the "everything" layer: botox config vars, netvars, raw memory, vtable calls
local info = ui.text("press a button")

ui.button("count botox variables", function()
    local count = 0
    for _ in pairs(config.list()) do count = count + 1 end
    info:set(count .. " botox variables")
    print("menu key:", config.get("m_menu_key"))
end)

ui.button("toggle debug log", function()
    config.set("m_debug_log", not config.get("m_debug_log"))
    info:set("debug log " .. (config.get("m_debug_log") and "on" or "off"))
end)

ui.button("health three ways", function()
    local me = entities.local_player()
    if not me then info:set("not in game") return end

    -- 1: helper, 2: netvar by name, 3: raw read at entity + netvar offset
    local a = me:health()
    local b = me:get_prop("CBasePlayer->m_iHealth", "int")
    local c = mem.read_i32(me:ptr() + mem.netvar("CBasePlayer->m_iHealth"))
    info:set(string.format("health %d / %d / %d", a, b, c))
end)

ui.button("local index via vtable", function()
    -- IVEngineClient::GetLocalPlayer = vtable slot 12, __thiscall, no args
    local engine_client = mem.interfaces().engine_client
    local fn = mem.vfunc(engine_client, 12)
    local index = mem.call(fn, "thiscall", engine_client)
    info:set("local index " .. index .. " (engine.local_index() = " .. engine.local_index() .. ")")
end)

callbacks.register("unload", function()
    print("raw_access unloaded")
end)
