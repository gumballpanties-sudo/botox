-- counts your kills per map, chat line on every kill, reset button
local announce = ui.checkbox("chat on kill", true)
local counter  = ui.text("kills: 0")

local kills = 0

local function refresh()
    counter:set("kills: " .. kills)
end

ui.button("reset", function()
    kills = 0
    refresh()
end)

callbacks.register("player_death", function(e)
    local attacker = e:get_player("attacker")
    local victim   = e:get_player("userid")
    local me       = entities.local_player()

    if not attacker or not me or attacker ~= me or victim == me then return end

    kills = kills + 1
    refresh()

    if announce:get() and victim then
        local headshot = e:get_bool("headshot") and " (hs)" or ""
        -- leading space: csgo only parses colour bytes after the first character
        client.chat(" \x04botox\x01 killed " .. victim:name() .. headshot .. " - " .. kills .. " this map")
    end
end)

callbacks.register("level_init", function()
    kills = 0
    refresh()
end)
