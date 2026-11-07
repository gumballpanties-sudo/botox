-- own world effects where you edgebug: shockwave rings on the edge, spark streaks with gravity, light pillar, sound
-- everything is simulated + drawn by this script (world_to_screen), no game particles
-- detect = fall speed snaps to one half tick of gravity mid air, then keeps falling next tick (pixelsurf stays snapped = no hit)
local enabled   = ui.checkbox("edgebug effects", true)
local fx_color  = ui.color("color", 120, 200, 255, 255)
local rainbow   = ui.checkbox("rainbow", false)
local waves_on  = ui.checkbox("shockwave", true)
local wave_size = ui.slider_int("shockwave size", 30, 300, 110, "%d u")
local spark_n   = ui.slider_int("sparks", 0, 120, 40)
local pillar_on = ui.checkbox("pillar", true)
local length    = ui.slider_float("time", 0.3, 3.0, 1.0, "%.1f s")
local sound     = ui.combo("sound", { "off", "switch", "button", "money", "beep" }, 2)
local counter   = ui.text("edgebugs: 0")

local sounds = { nil, "buttons/arena_switch_press_02.wav", "buttons/button22.wav", "survival/money_collect_01.wav", "ui/beep07.wav" }

local k_gravity, k_drag, k_max_sparks = 500, 1.5, 600

local total = 0
local bursts, sparks = {}, {}
local prev_vz, prev_air, pending = 0, false, false
local px, py, pz = 0, 0, 0
local last_paint

local function hsv(h)
    local i = math.floor(h * 6)
    local f = h * 6 - i
    local rgb = ({ { 1, f, 0 }, { 1 - f, 1, 0 }, { 0, 1, f }, { 0, 1 - f, 1 }, { f, 0, 1 }, { 1, 0, 1 - f } })[i % 6 + 1]
    return math.floor(rgb[1] * 255), math.floor(rgb[2] * 255), math.floor(rgb[3] * 255)
end

local function pick_color(hue)
    if rainbow:get() then return hsv(hue % 1) end
    local r, g, b = fx_color:get()
    return r, g, b
end

local function on_edgebug(x, y, z)
    total = total + 1
    counter:set("edgebugs: " .. total)

    local now = globals.realtime()
    local r, g, b = pick_color(now * 0.25)
    bursts[#bursts + 1] = { x = x, y = y, z = z, born = now, r = r, g = g, b = b }

    for _ = 1, spark_n:get() do
        if #sparks >= k_max_sparks then break end
        local a     = math.random() * math.pi * 2
        local speed = 120 + math.random() * 230
        local sr, sg, sb = r, g, b
        if rainbow:get() then sr, sg, sb = hsv(math.random()) end
        sparks[#sparks + 1] = {
            x = x, y = y, z = z + 2,
            vx = math.cos(a) * speed, vy = math.sin(a) * speed, vz = 80 + math.random() * 260,
            age = 0, life = length:get() * (0.5 + math.random() * 0.5),
            r = sr, g = sg, b = sb,
        }
    end

    local path = sounds[sound:get()]
    if path then engine.exec("play " .. path) end
end

callbacks.register("pre_prediction", function(cmd)
    local me = entities.local_player()
    if not enabled:get() or not me then return end

    local x, y, z = me:origin()
    local _, _, vz = me:velocity()
    local air   = (me:flags() & flags.onground) == 0 and me:move_type() == 2
    local reset = -cvar.get_float("sv_gravity") * globals.tickinterval() * 0.5 -- -6.25 at 64 tick

    if pending and air and vz < reset * 2 then on_edgebug(px, py, pz) end
    pending = prev_air and air and prev_vz < reset * 4 and math.abs(vz - reset) < 1
    if pending then px, py, pz = x, y, z end -- feet on the edge tick
    prev_vz, prev_air = vz, air
end)

-- flat ring at height z, returns the screen points (nil if any point is behind the camera)
local function ring(x, y, z, radius, c, thickness)
    local points, all = {}, true
    local lx, ly
    for s = 0, 32 do
        local a = s / 32 * math.pi * 2
        local sx, sy = render.world_to_screen(x + math.cos(a) * radius, y + math.sin(a) * radius, z)
        if sx and lx then render.line(lx, ly, sx, sy, c, thickness) end
        if not sx then all = false elseif s < 32 then points[#points + 1] = sx; points[#points + 1] = sy end
        lx, ly = sx, sy
    end
    return all and points or nil
end

local function draw_burst(w, now)
    local t = (now - w.born) / length:get()
    if t > 1 then return false end
    local fade = 1 - t

    if waves_on:get() then
        -- two rings, second one late, ease out to full size
        for k = 0, 1 do
            local tk = math.max(t * 1.4 - k * 0.25, 0)
            if tk > 0 and tk < 1 then
                local radius = 6 + wave_size:get() * (1 - (1 - tk) ^ 3)
                local alpha  = (1 - tk) * (k == 0 and 1 or 0.6)
                local points = ring(w.x, w.y, w.z, radius, color(w.r, w.g, w.b, math.floor(255 * alpha)), 1 + 3 * alpha)
                if k == 0 and points then
                    render.poly(points, color(w.r, w.g, w.b, math.floor(40 * alpha)))
                end
            end
        end
    end

    if pillar_on:get() then
        local height = 90 * (1 - (1 - math.min(t * 3, 1)) ^ 2)
        local bx, by = render.world_to_screen(w.x, w.y, w.z)
        local tx, ty = render.world_to_screen(w.x, w.y, w.z + height)
        if bx and tx then
            render.line(bx, by, tx, ty, color(w.r, w.g, w.b, math.floor(50 * fade)), 10)
            render.line(bx, by, tx, ty, color(w.r, w.g, w.b, math.floor(120 * fade)), 5)
            render.line(bx, by, tx, ty, color(255, 255, 255, math.floor(230 * fade)), 2)
        end
    end
    return true
end

callbacks.register("paint", function()
    local now = globals.realtime()
    local dt  = last_paint and math.min(now - last_paint, 0.1) or 0
    last_paint = now
    if not enabled:get() then bursts, sparks = {}, {} return end

    for i = #bursts, 1, -1 do
        if not draw_burst(bursts[i], now) then table.remove(bursts, i) end
    end

    local drag = math.max(1 - k_drag * dt, 0)
    for i = #sparks, 1, -1 do
        local p = sparks[i]
        p.age = p.age + dt
        if p.age >= p.life then
            sparks[i] = sparks[#sparks]
            sparks[#sparks] = nil
        else
            p.vx, p.vy, p.vz = p.vx * drag, p.vy * drag, p.vz * drag - k_gravity * dt
            p.x, p.y, p.z = p.x + p.vx * dt, p.y + p.vy * dt, p.z + p.vz * dt

            -- streak = head to where it was 40 ms ago, so fast sparks stretch
            local hx, hy = render.world_to_screen(p.x, p.y, p.z)
            local tx, ty = render.world_to_screen(p.x - p.vx * 0.04, p.y - p.vy * 0.04, p.z - p.vz * 0.04)
            if hx and tx then
                local alpha = math.floor(255 * (1 - p.age / p.life))
                render.line(tx, ty, hx, hy, color(p.r, p.g, p.b, alpha), 2)
                render.circle_filled(hx, hy, 1.5, color(255, 255, 255, alpha), 8)
            end
        end
    end
end)

callbacks.register("level_init", function()
    bursts, sparks, prev_air, pending = {}, {}, false, false
end)
