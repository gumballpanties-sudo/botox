-- viewmodel_swap.lua: your AWP becomes a model rigged its own way (own bones + animations, arms built in), the case
-- draw_model's model.model can't do. Put the .mdl + .vvd + .dx90.vtx (+ its materials) under csgo\, keep the paths below
-- in sync. sv_pure servers: settings > sv_pure bypass. Client side only, everyone else still sees the stock AWP.

local STOCK_V  = "models/weapons/v_snip_awp.mdl"
local CUSTOM_V = "models/weapons/awp_dragon/v_snip_awp.mdl"
local CUSTOM_W = "models/weapons/awp_dragon/w_snip_awp.mdl" -- "" = keep the stock world model
local AWP      = 9

local on        = ui.checkbox("awp dragon", true)
local hide_arms = ui.checkbox("hide stock arms (the model has its own)", true)

local stock_v, custom_v, custom_w -- model indexes, per map
local failed = false              -- a path didn't load: engine.precache_model printed why in console
local world                       -- { wm = world model entity, stock = its own index } while ours is on it

local function holding_awp()
    local me = entities.local_player()
    local wep = me and me:is_alive() and me:weapon()
    return wep and wep:weapon_id() == AWP and wep or nil
end

local function restore_world()
    if world and world.wm:is_valid() and world.wm:get_prop("CBaseEntity->m_nModelIndex") == custom_w then
        world.wm:set_model_index(world.stock)
    end
    world = nil
end

local function off()
    if stock_v then engine.swap_viewmodel(stock_v, nil) end -- botox puts the stock viewmodel back
    restore_world()
    stock_v, custom_v, custom_w = nil, nil, nil
end

-- postdataupdate_start: before prediction copies the network state ( the world model is predicted, a write after it is
-- undone every packet ). render_start: before the frame draws
callbacks.register("frame_stage", function(stage)
    if stage ~= stages.net_update_postdataupdate_start and stage ~= stages.render_start then return end
    if not on:get() then off() return end
    if not engine.in_game() or failed then return end

    if not stock_v then
        stock_v, custom_v = engine.precache_model(STOCK_V), engine.precache_model(CUSTOM_V)
        custom_w = CUSTOM_W ~= "" and engine.precache_model(CUSTOM_W) or nil
        if not stock_v or not custom_v or (CUSTOM_W ~= "" and not custom_w) then
            failed, stock_v = true, nil
            client.log("model didn't load, see console")
            return
        end
        engine.swap_viewmodel(stock_v, custom_v) -- botox keeps it on from here, animations renumbered
    end

    -- the world model ( third person, the shadow ) has no swap of its own: set it whenever the game put its own back
    local wep = custom_w and holding_awp()
    local wm = wep and wep:get_prop("CBaseCombatWeapon->m_hWeaponWorldModel", "handle")
    local current = wm and wm:get_prop("CBaseEntity->m_nModelIndex")
    if current and current ~= custom_w then
        if not world or world.wm ~= wm then world = { wm = wm, stock = current } end
        wm:set_model_index(custom_w)
    end
end)

callbacks.register("draw_model", function(m)
    if hide_arms:get() and custom_v and m.name:find("v_models/arms/", 1, true) and holding_awp() then
        m.hide = true
    end
end)

-- indexes are per map: botox already dropped the swap, look the models up again
callbacks.register("level_init", function()
    stock_v, custom_v, custom_w, world, failed = nil, nil, nil, nil, false
end)

callbacks.register("unload", off)
