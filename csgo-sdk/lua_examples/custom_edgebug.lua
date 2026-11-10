-- custom edgebug, hotwheels style, built from scratch: own csgo air movement sim on trace.hull, no botox eb, no engine prediction
-- each cmd while falling: sim up to N ticks for standing / ducking with no keys, then (advanced) your keys + your turn rate
-- first variant whose fall speed gets clipped to 0 by a floor and ends the tick airborne = edgebug
-- then cmds replay that variant until the eb tick + 1, mouse lock + strafe camera like hotwheels
-- turn botox's own edgebug off while this one runs
local enabled   = ui.checkbox("edgebug", true)
local key       = ui.keybind("edgebug key", 0, 1)
local max_ticks = ui.slider_int("predicted ticks", 0, 32, 32)
local lock_amt  = ui.slider_float("mouse lock scale", 0, 1, 0.3)
local advanced  = ui.checkbox("advanced detection (strafe)", false)
local max_turn  = ui.slider_float("max strafe turn", 0.1, 10, 3, "%.1f deg/tick")
local strafe_cam = ui.checkbox("strafe camera", true)
local show_ind  = ui.checkbox("indicator", true)
local ind_color = ui.color("indicator found", 120, 255, 120, 255)

-- csgo gamemovement constants
local STAND_H, DUCK_H, DUCK_SHIFT = 72, 54, 9 -- air duck / unduck moves origin by half the hull difference
local NON_JUMP_VELOCITY, STANDABLE_Z = 140, 0.7
local DIST_EPSILON, MIN_MOVE_FRACTION = 0.03125, 0.0001

local eb        -- found edgebug being replayed, nil = searching
local last_yaw  -- cmd yaw of the last cmd = hotwheels m_last_tick_yaw
local last_view -- view angles after our lock, last frame

local function norm(a) return (a + 180) % 360 - 180 end
local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

local function hull_trace(sx, sy, sz, ex, ey, ez, ducked)
    return trace.hull(sx, sy, sz, ex, ey, ez, -16, -16, 0, 16, 16, ducked and DUCK_H or STAND_H, masks.playersolid)
end

-- ClipVelocity, overbounce 1, then push any residual into-plane speed out
local function clip(vx, vy, vz, n)
    local back = vx * n[1] + vy * n[2] + vz * n[3]
    vx, vy, vz = vx - n[1] * back, vy - n[2] * back, vz - n[3] * back
    local adjust = vx * n[1] + vy * n[2] + vz * n[3]
    if adjust < 0 then
        adjust = math.min(adjust, -DIST_EPSILON)
        vx, vy, vz = vx - n[1] * adjust, vy - n[2] * adjust, vz - n[3] * adjust
    end
    return vx, vy, vz
end

-- TryPlayerMove (airborne), returns true when a floor plane (normal z > 0.7) was hit
local function try_move(s, dt)
    local planes = {}
    local ox, oy, oz = s.vx, s.vy, s.vz
    local px, py, pz = ox, oy, oz
    local time_left, all_fraction, floor = dt, 0, false

    for _ = 1, 4 do
        if s.vx == 0 and s.vy == 0 and s.vz == 0 then break end
        local t = hull_trace(s.x, s.y, s.z, s.x + s.vx * time_left, s.y + s.vy * time_left, s.z + s.vz * time_left, s.ducked)
        local frac = t.fraction
        if frac > 0 and frac < MIN_MOVE_FRACTION then frac = 0 end
        all_fraction = all_fraction + frac
        if t.all_solid then
            s.vx, s.vy, s.vz = 0, 0, 0
            return floor
        end
        if frac > 0 then
            s.x, s.y, s.z = t.x, t.y, t.z
            ox, oy, oz = s.vx, s.vy, s.vz
            planes = {}
        end
        if frac == 1 then break end

        s.touched = true
        local n = { t.normal_x, t.normal_y, math.abs(t.normal_z) < 0.0001 and 0 or t.normal_z }
        if n[3] > 0.7 then floor = true end
        time_left = time_left - time_left * frac
        if #planes >= 5 then
            s.vx, s.vy, s.vz = 0, 0, 0
            break
        end
        planes[#planes + 1] = n

        if #planes == 1 then
            -- first plane in air = reflect, sv_bounce 0 makes it a plain clip
            ox, oy, oz = clip(ox, oy, oz, n)
            s.vx, s.vy, s.vz = ox, oy, oz
        else
            local ok
            for i = 1, #planes do
                s.vx, s.vy, s.vz = clip(ox, oy, oz, planes[i])
                ok = true
                for j = 1, #planes do
                    local p = planes[j]
                    if j ~= i and s.vx * p[1] + s.vy * p[2] + s.vz * p[3] < 0 then ok = false break end
                end
                if ok then break end
            end
            if not ok then
                -- crease between two planes, anything else = stop
                if #planes ~= 2 then
                    s.vx, s.vy, s.vz = 0, 0, 0
                    break
                end
                local a, b = planes[1], planes[2]
                local dx, dy, dz = a[2] * b[3] - a[3] * b[2], a[3] * b[1] - a[1] * b[3], a[1] * b[2] - a[2] * b[1]
                local len = math.sqrt(dx * dx + dy * dy + dz * dz)
                if len > 0 then dx, dy, dz = dx / len, dy / len, dz / len end
                local d = dx * s.vx + dy * s.vy + dz * s.vz
                s.vx, s.vy, s.vz = dx * d, dy * d, dz * d
            end
            if s.vx * px + s.vy * py + s.vz * pz <= 0 then
                s.vx, s.vy, s.vz = 0, 0, 0
                break
            end
        end
    end

    if all_fraction == 0 then s.vx, s.vy, s.vz = 0, 0, 0 end
    return floor
end

-- CategorizePosition (airborne): hull 2 units down, standable = land, landing zeroes fall speed
local function categorize(s)
    s.onground = false
    if s.vz > NON_JUMP_VELOCITY then return end
    local t = hull_trace(s.x, s.y, s.z, s.x, s.y, s.z - 2, s.ducked)
    local hit = t.fraction < 1 or t.start_solid
    if hit then s.touched = true end
    if hit and t.entity and t.normal_z >= STANDABLE_Z then
        s.onground = true
        s.vz = 0
    end
end

-- one tick of csgo FullWalkMove in air, returns true when this tick is an edgebug
local function sim_tick(s, fmove, smove, yaw, duck, e)
    if duck ~= s.ducked then
        if duck then
            s.z, s.ducked = s.z + DUCK_SHIFT, true
            categorize(s)
        else
            local t = hull_trace(s.x, s.y, s.z, s.x, s.y, s.z - DUCK_SHIFT, false) -- CanUnduck
            if not t.start_solid and t.fraction == 1 then
                s.z, s.ducked = s.z - DUCK_SHIFT, false
                categorize(s)
            else
                s.touched = true
            end
        end
        if s.onground then return false end
    end

    s.vz = s.vz - e.gravity * 0.5 * e.dt -- StartGravity

    -- AirMove + AirAccelerate (wish capped at 30)
    local r = math.rad(yaw)
    local cy, sy = math.cos(r), math.sin(r)
    local wx, wy = cy * fmove + sy * smove, sy * fmove - cy * smove
    local wishspeed = math.sqrt(wx * wx + wy * wy)
    if wishspeed > 0 then
        local dx, dy = wx / wishspeed, wy / wishspeed
        wishspeed = math.min(wishspeed, e.maxspeed)
        local add = math.min(wishspeed, 30) - (s.vx * dx + s.vy * dy)
        if add > 0 then
            local accel = math.min(e.airaccel * wishspeed * e.dt, add)
            s.vx, s.vy = s.vx + accel * dx, s.vy + accel * dy
        end
    end

    local vz_before = s.vz
    local floor = try_move(s, e.dt)
    local killed = floor and vz_before < 0 and s.vz > -1 -- a floor ate the fall speed
    categorize(s)
    s.vz = s.vz - e.gravity * 0.5 * e.dt -- FinishGravity
    if s.onground then s.vz = 0 end
    return killed and not s.onground -- slid off the edge in the same tick
end

local function search(me, cmd)
    local x, y, z = me:origin()
    local vx, vy, vz = me:velocity()
    local ticks = max_ticks:get()
    local e = {
        dt = globals.tickinterval(),
        gravity = cvar.get_float("sv_gravity") or 800,
        airaccel = cvar.get_float("sv_airaccelerate") or 12,
        maxspeed = me:get_prop("CBasePlayer->m_flMaxspeed", "float"),
    }
    if not e.maxspeed or e.maxspeed <= 0 then e.maxspeed = 250 end
    local ducked = me:flags() & flags.ducking ~= 0
    local yd = clamp(norm(cmd.yaw - (last_yaw or cmd.yaw)), -180 / ticks, 180 / ticks)

    local variants = { { false, false }, { true, false } }
    if advanced:get() and math.abs(yd) <= max_turn:get() then
        variants[3], variants[4] = { false, true }, { true, true }
    end

    local stand_touched
    for vi, v in ipairs(variants) do
        local duck, strafe = v[1], v[2]
        -- duck hull rides inside the standing hull on the same keyless path: standing touched nothing = duck cannot either
        if not (vi == 2 and stand_touched == false) then
            local s = { x = x, y = y, z = z, vx = vx, vy = vy, vz = vz, ducked = ducked, onground = false, touched = false }
            for i = 0, ticks do
                local yaw = strafe and norm(cmd.yaw + yd * i) or cmd.yaw
                local fm, sm = 0, 0
                if strafe then fm, sm = cmd.forwardmove, cmd.sidemove end
                if sim_tick(s, fm, sm, yaw, duck, e) then
                    return {
                        cmd0 = cmd.command_number, ticks = i + 1, duck = duck, strafe = strafe,
                        fmove = fm, smove = sm, yaw = cmd.yaw, yd = strafe and yd or 0,
                        mdx = math.abs(cmd.mousedx), t0 = globals.realtime(),
                    }
                end
                if s.onground or s.vz >= 0 then break end
            end
            if vi == 1 then stand_touched = s.touched end
        end
    end
end

local function apply(cmd)
    cmd.buttons = cmd.buttons & ~(buttons.jump | buttons.forward | buttons.back | buttons.moveleft | buttons.moveright)
    if eb.strafe then
        cmd.forwardmove, cmd.sidemove = eb.fmove, eb.smove
        cmd.yaw = norm(eb.yaw + eb.yd * (cmd.command_number - eb.cmd0))
    else
        cmd.forwardmove, cmd.sidemove = 0, 0
    end
    if eb.duck then
        cmd.buttons = cmd.buttons | buttons.duck
    else
        cmd.buttons = cmd.buttons & ~buttons.duck
    end
end

callbacks.register("pre_prediction", function(cmd)
    local me = entities.local_player()
    if not (enabled:get() and key:get() and me and me:move_type() == 2) then
        eb, last_yaw = nil, cmd.yaw
        return
    end

    if eb and cmd.command_number >= eb.cmd0 + eb.ticks + 1 then eb = nil end
    if not eb and me:flags() & flags.onground == 0 then
        local _, _, vz = me:velocity()
        if vz < 0 then eb = search(me, cmd) end
    end

    last_yaw = cmd.yaw
    if eb then apply(cmd) end
end)

-- botox features after prediction (autostrafe ...) may touch the cmd again, put the replay back on top
callbacks.register("create_move", function(cmd)
    if eb and cmd.command_number >= eb.cmd0 and cmd.command_number < eb.cmd0 + eb.ticks + 1 then apply(cmd) end
end)

-- hotwheels override_mouse_input lock + fsn strafe camera, done on view angles once per frame
callbacks.register("frame_stage", function(stage)
    if stage ~= stages.start then return end
    local p, y, r = engine.view_angles()
    if eb and last_view then
        local lock = lock_amt:get()
        local elapsed = clamp((globals.realtime() - eb.t0) / globals.tickinterval(), 0, eb.ticks + 1)
        if eb.strafe and strafe_cam:get() then
            y = norm(eb.yaw + eb.yd * elapsed)
        else
            local dyaw = norm(y - last_view.y)
            if dyaw ~= 0 then
                local mx = -dyaw / (cvar.get_float("m_yaw") or 0.022) -- view turn back to mouse x
                local factor = 100 / (eb.ticks ^ ((eb.ticks - elapsed) / mx) + 100 + lock * eb.mdx)
                if factor == factor then y = norm(last_view.y + dyaw * factor) end
            end
        end
        p = last_view.p + (p - last_view.p) * (1 - lock)
        engine.set_view_angles(p, y, r)
    end
    last_view = { p = p, y = y }
end)

callbacks.register("paint", function()
    if not (show_ind:get() and enabled:get() and entities.local_player()) then return end
    local w, h = render.screen_size()
    local held = key:get()
    local c = eb and color(ind_color:get()) or color(255, 255, 255, held and 255 or 90)
    local tw = render.text_size("eb", fonts.indicator)
    render.text(math.floor(w / 2 - tw / 2), math.floor(h * 0.72), "eb", c, fonts.indicator)
    if eb then
        local info = (eb.duck and "duck" or "stand") .. (eb.strafe and string.format(" | strafe %.2f", eb.yd) or "") .. " | " .. eb.ticks .. "t"
        local iw = render.text_size(info)
        render.text(math.floor(w / 2 - iw / 2), math.floor(h * 0.72) + 34, info, c)
    end
end)

callbacks.register("level_init", function()
    eb, last_yaw, last_view = nil, nil, nil
end)

-- for tools/custom_edgebug_check.lua, botox ignores a script's return value
return { sim_tick = sim_tick }
