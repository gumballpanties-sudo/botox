-- custom models: draw your own knife / gun model instead of the stock one, tint enemies, hide arms.
-- put the model files under csgo\ (models\weapons\..., materials\models\weapons\...) and turn on
-- settings > sv_pure bypass for sv_pure servers. the model must be rigged to the stock skeleton.
local knife_input = ui.input("knife model", "models/weapons/v_knife_custom.mdl")
local from_input  = ui.input("gun to replace", "v_rif_ak47")
local gun_input   = ui.input("gun model", "")
local tint        = ui.checkbox("tint enemies", true)
local tint_color  = ui.color("enemy color", 255, 80, 80, 255)
local no_arms     = ui.checkbox("hide arms", false)

-- paths only change on the button: every path botox loads stays loaded, half typed ones too
local knife, gun_from, gun_to = knife_input:get(), from_input:get(), gun_input:get()
ui.button("apply models", function()
    knife, gun_from, gun_to = knife_input:get(), from_input:get(), gun_input:get()
end)

-- draw_model runs hundreds of times a frame: read the ui once a frame here
local want_tint, enemy_color, hide_arms = false, nil, false
callbacks.register("paint", function()
    want_tint, enemy_color, hide_arms = tint:get(), color(tint_color:get()), no_arms:get()
end)

callbacks.register("draw_model", function(model)
    local name = model.name

    if knife ~= "" and name:find("weapons/v_knife", 1, true) then
        model.model = knife
    elseif gun_to ~= "" and gun_from ~= "" and name:find(gun_from, 1, true) then
        model.model = gun_to
    elseif hide_arms and name:find("v_models/arms/", 1, true) then
        model.hide = true
    elseif want_tint then
        local player = model.entity
        if player and player:is_player() and player:is_enemy() then
            model.color = enemy_color
        end
    end
end)
