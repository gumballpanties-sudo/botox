-- simple 2d box + name + health bar from the player's collision hull
local enabled   = ui.checkbox("boxes", true)
local teammates = ui.checkbox("teammates", false)
local box_color = ui.color("box color", 255, 255, 255, 255)
local style     = ui.combo("health text", { "off", "number", "percent" }, 2)

local function screen_box(player)
    local ox, oy, oz = player:origin()
    local x1, y1, z1, x2, y2, z2 = player:mins_maxs()
    if not x1 then return nil end

    local top_x, top_y = render.world_to_screen(ox, oy, oz + z2)
    local bot_x, bot_y = render.world_to_screen(ox, oy, oz + z1)
    if not top_x or not bot_x then return nil end

    local h = bot_y - top_y
    local w = h * 0.5
    return top_x - w / 2, top_y, top_x + w / 2, bot_y
end

callbacks.register("paint", function()
    if not enabled:get() or not engine.in_game() then return end

    local me = entities.local_player()

    for _, player in ipairs(entities.players()) do
        if player ~= me and (teammates:get() or player:is_enemy()) then
            local x1, y1, x2, y2 = screen_box(player)
            if x1 then
                render.rect(x1 - 1, y1 - 1, x2 + 1, y2 + 1, color(0, 0, 0, 160), 3)
                render.rect(x1, y1, x2, y2, color(box_color:get()))

                local name = player:name()
                local nw   = render.text_size(name, fonts.verdana)
                render.text((x1 + x2) / 2 - nw / 2, y1 - 14, name, color(255, 255, 255), fonts.verdana)

                local hp   = math.max(0, math.min(player:health(), 100))
                local fill = (y2 - y1) * hp / 100
                render.rect_filled(x1 - 6, y1 - 1, x1 - 2, y2 + 1, color(0, 0, 0, 160))
                render.rect_filled(x1 - 5, y2 - fill, x1 - 3, y2, color(math.floor(255 - hp * 2.55), math.floor(hp * 2.55), 0))

                local mode = style:get()
                if mode > 1 and hp < 100 then
                    local label = mode == 2 and tostring(hp) or (hp .. "%")
                    render.text(x1 - 6, y2 - fill - 6, label, color(255, 255, 255), fonts.verdana, text_flags.outline)
                end
            end
        end
    end
end)
