-- speed under the crosshair, colour fades from grey to green by speed
local enabled = ui.checkbox("show speed", true)
local offset  = ui.slider_int("height", 0, 400, 120, "%d px")
local show_z  = ui.checkbox("include vertical", false)
local key     = ui.keybind("hide key", 0, 1)

callbacks.register("paint", function()
    if not enabled:get() then return end
    if key:get() then return end

    local me = entities.local_player()
    if not me or not me:is_alive() then return end

    local vx, vy, vz = me:velocity()
    local speed = show_z:get() and math.sqrt(vx * vx + vy * vy + vz * vz) or math.sqrt(vx * vx + vy * vy)

    local t = math.min(speed / 300, 1)
    local c = color(math.floor(200 - 120 * t), 200, math.floor(200 - 120 * t))

    local text = tostring(math.floor(speed + 0.5))
    local w, h = render.text_size(text, fonts.indicator)
    local sw, sh = render.screen_size()

    render.text(sw / 2 - w / 2, sh / 2 + offset:get(), text, c, fonts.indicator, text_flags.shadow)
end)
