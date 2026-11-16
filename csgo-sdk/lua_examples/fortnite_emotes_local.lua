-- fortnite_emotes_local.lua  |  botox lua (csgo)
-- Fortnite emotes on your own player. Local only: nobody else sees them, the server never knows.
--
-- Why swapping m_nModelIndex made you invisible: fortnite_emotes_v2.mdl has no mesh at all ( its .vvd is 64 bytes ),
-- it is only a skeleton with 97 dances. The server plugin parents the player to an invisible prop that plays the
-- dance and bone merges the player onto it ( EF_BONEMERGE ). entity:animate does that bone merge on your client:
-- your own bones take the emote pose, so your model, the gun in your hand and the holstered gear ( pistol, knife,
-- grenades, c4 ) all dance. Hitboxes, movement and what the server sees don't change.
--
-- Needs the model files from the Fortnite Emotes Extended plugin ( github.com/Franc1sco/Fortnite-Emotes-Extended ):
--   csgo\models\player\custom_player\kodua\fortnite_emotes_v2.mdl + .vvd + .dx90.vtx
-- sv_pure servers reject loose files: settings > sv_pure bypass, or play offline / with bots.

local MODEL = "models/player/custom_player/kodua/fortnite_emotes_v2.mdl"

-- { menu name, first sequence, sequence that loops after it ( nil = plays once ) }.
-- generated from the plugin's CreateEmote calls + the model's looping flags
local EMOTES = {
    { "DanceMoves", "DanceMoves", nil },
    { "ElectroShuffle", "ElectroShuffle", "ElectroShuffle" },
    { "AerobicChamp", "Emote_AerobicChamp", "Emote_AerobicChamp" },
    { "BandOfTheFort", "Emote_BandOfTheFort", "Emote_BandOfTheFort" },
    { "Bendy", "Emote_Bendy", "Emote_Bendy" },
    { "BlowKiss", "Emote_BlowKiss", nil },
    { "Boogie Down", "Emote_Boogie_Down_Intro", "Emote_Boogie_Down" },
    { "Bring It On", "Emote_Bring_It_On", nil },
    { "Calculated", "Emote_Calculated", nil },
    { "Capoeira", "Emote_Capoeira", nil },
    { "Celebration", "Emote_Celebration_Loop", "Emote_Celebration_Loop" },
    { "Charleston", "Emote_Charleston", "Emote_Charleston" },
    { "Chicken", "Emote_Chicken", "Emote_Chicken" },
    { "Chug", "Emote_Chug", "Emote_Chug" },
    { "Confused", "Emote_Confused", nil },
    { "Cry", "Emote_Cry", nil },
    { "Dab", "Emote_Dab", nil },
    { "Dance Breakdance", "Emote_Dance_Breakdance", nil },
    { "Dance Disco T3", "Emote_Dance_Disco_T3", "Emote_Dance_Disco_T3" },
    { "Dance Loser", "Emote_Dance_Loser", "Emote_Dance_Loser_CT" },
    { "Dance NoBones", "Emote_Dance_NoBones", "Emote_Dance_NoBones" },
    { "Dance Pump", "Emote_Dance_Pump", "Emote_Dance_Pump" },
    { "Dance RideThePony", "Emote_Dance_RideThePony", nil },
    { "Dance Shoot", "Emote_Dance_Shoot", "Emote_Dance_Shoot" },
    { "Dance SwipeIt", "Emote_Dance_SwipeIt", "Emote_Dance_SwipeIt" },
    { "Dance Worm", "Emote_Dance_Worm", nil },
    { "DG Disco", "Emote_DG_Disco", "Emote_DG_Disco" },
    { "DustingOffHands", "Emote_DustingOffHands", nil },
    { "DustOffShoulders", "Emote_DustOffShoulders", nil },
    { "EasternBloc", "Emote_EasternBloc_Start", "Emote_EasternBloc" },
    { "Facepalm", "Emote_Facepalm", nil },
    { "FancyFeet", "Emote_FancyFeet", "Emote_FancyFeet_CT" },
    { "Fishing", "Emote_Fishing", nil },
    { "Flex", "Emote_Flex", nil },
    { "FlippnSexy", "Emote_FlippnSexy", nil },
    { "FlossDance", "Emote_FlossDance", "Emote_FlossDance" },
    { "Fonzie Pistol", "Emote_Fonzie_Pistol", nil },
    { "Fresh", "Emote_Fresh", "Emote_Fresh" },
    { "golfclap", "Emote_golfclap", "Emote_golfclap" },
    { "GrooveJam", "Emote_GrooveJam", "Emote_GrooveJam" },
    { "guitar", "Emote_guitar", "Emote_guitar" },
    { "HandSignals", "Emote_HandSignals", nil },
    { "HeelClick", "Emote_HeelClick", nil },
    { "Hillbilly Shuffle", "Emote_Hillbilly_Shuffle_Intro", "Emote_Hillbilly_Shuffle" },
    { "Hiphop 01", "Emote_Hiphop_01", "Emote_Hip_Hop" },
    { "Hotstuff", "Emote_Hotstuff", nil },
    { "Hula", "Emote_Hula_Start", "Emote_Hula" },
    { "IBreakYou", "Emote_IBreakYou", nil },
    { "IHeartYou", "Emote_IHeartYou", nil },
    { "InfiniDab", "Emote_InfiniDab_Intro", "Emote_InfiniDab_Loop" },
    { "Intensity", "Emote_Intensity_Start", "Emote_Intensity_Loop" },
    { "IrishJig", "Emote_IrishJig_Start", "Emote_IrishJig" },
    { "KoreanEagle", "Emote_KoreanEagle", "Emote_KoreanEagle" },
    { "Kpop 02", "Emote_Kpop_02", "Emote_Kpop_02" },
    { "Kung-Fu Salute", "Emote_Kung-Fu_Salute", nil },
    { "Laugh", "Emote_Laugh", "Emote_Laugh_CT" },
    { "LivingLarge", "Emote_LivingLarge", "Emote_LivingLarge" },
    { "Luchador", "Emote_Luchador", nil },
    { "Make It Rain", "Emote_Make_It_Rain", nil },
    { "Maracas", "Emote_Maracas", "Emote_Maracas" },
    { "Mask Off", "Emote_Mask_Off_Intro", "Emote_Mask_Off_Loop" },
    { "NotToday", "Emote_NotToday", nil },
    { "PopLock", "Emote_PopLock", nil },
    { "PopRock", "Emote_PopRock", "Emote_PopRock" },
    { "RobotDance", "Emote_RobotDance", "Emote_RobotDance" },
    { "RockPaperScissor Paper", "Emote_RockPaperScissor_Paper", nil },
    { "RockPaperScissor Rock", "Emote_RockPaperScissor_Rock", nil },
    { "RockPaperScissor Scissor", "Emote_RockPaperScissor_Scissor", nil },
    { "Salt", "Emote_Salt", nil },
    { "Salute", "Emote_Salute", nil },
    { "SmoothDrive", "Emote_SmoothDrive", "Emote_SmoothDrive" },
    { "Snap", "Emote_Snap", nil },
    { "StageBow", "Emote_StageBow", nil },
    { "T-Rex", "Emote_T-Rex", nil },
    { "TechnoZombie", "Emote_TechnoZombie", "Emote_TechnoZombie" },
    { "ThumbsDown", "Emote_ThumbsDown", nil },
    { "ThumbsUp", "Emote_ThumbsUp", nil },
    { "Twist", "Emote_Twist", "Emote_Twist" },
    { "WarehouseDance", "Emote_WarehouseDance_Start", "Emote_WarehouseDance_Loop" },
    { "Wave2", "Emote_Wave2", nil },
    { "Wiggle", "Emote_Wiggle", "Emote_Wiggle" },
    { "Yeet", "Emote_Yeet", nil },
    { "Youre Awesome", "Emote_Youre_Awesome", nil },
    { "Zippy Dance", "Emote_Zippy_Dance", "Emote_Zippy_Dance" },
}

local names = {}
for i, emote in ipairs(EMOTES) do names[i] = emote[1] end

local key       = ui.keybind("emote key", 0x47, 2) -- G, toggle ( hold style works too )
local pick      = ui.combo("emote", names, 1)
local speed     = ui.slider_float("speed", 0.25, 2.0, 1.0, "%.2fx")
local stop_move = ui.checkbox("stop when you move or jump", true)
local third     = ui.checkbox("third person while dancing", true)
-- in your hand: the gun is bone merged to your hand, it dances with it
local gun_mode  = ui.combo("weapon while dancing", { "in your hand", "hidden" }, 1)

local dancing    = false
local emote      = nil   -- EMOTES entry
local sequence   = nil   -- sequence playing now
local elapsed    = 0     -- seconds into it
local length     = nil   -- its length in seconds, from entity:animate ( nil until the model is loaded )
local started_at = 0
local warned     = false
local key_was    = false
local gun, gun_world = -1, -1
local saved_tp   = nil   -- third person settings from before the dance

local function stop()
    if dancing then
        local me = entities.local_player()
        if me then pcall(function() me:animate(nil) end) end -- or it holds one more frame
    end
    if saved_tp then
        config.set("m_third_person", saved_tp[1])
        config.set("m_third_person_key", saved_tp[2], saved_tp[3])
        saved_tp = nil
    end
    dancing, emote, sequence, length = false, nil, nil, nil
    gun, gun_world = -1, -1
end

local function start()
    emote      = EMOTES[(pick:get())] or EMOTES[1]
    sequence   = emote[2]
    elapsed    = 0
    length     = nil
    started_at = globals.realtime()
    dancing    = true

    if third:get() and not saved_tp then
        local bind_key, bind_style = config.get("m_third_person_key")
        saved_tp = { config.get("m_third_person"), bind_key, bind_style }
        config.set("m_third_person", true)
        config.set("m_third_person_key", bind_key, 0) -- 0 = always on
    end
end

-- toggle style: every press flips the dance. hold / always on: dance while active
local function read_key()
    if input.keys_blocked() then return end -- typing: the bind reads inactive, that is no press
    local active, _, style = key:get()
    if active == key_was then return end
    key_was = active

    if style == 2 then
        if dancing then stop() else start() end
    elseif active then
        start()
    else
        stop()
    end
end

callbacks.register("frame_stage", function(stage)
    if stage ~= stages.render_start then return end
    read_key()
    if not dancing then return end

    local me = entities.local_player()
    if not (me and me:is_alive()) then stop() return end

    gun, gun_world = -1, -1
    local weapon = me:weapon()
    if weapon then
        gun = weapon:index()
        local ok, world = pcall(function() return weapon:get_prop("CBaseCombatWeapon->m_hWeaponWorldModel", "handle") end)
        if ok and world then gun_world = world:index() end
    end

    -- time only runs once the length is known ( the model loads at the next frame start )
    if length then
        elapsed = elapsed + globals.frametime() * speed:get()
        if elapsed >= length then
            if not emote[3] then
                stop() -- plays once, like the plugin's OnAnimationDone
                return
            elseif sequence ~= emote[3] then
                elapsed, sequence, length = elapsed - length, emote[3], nil -- intro done, the loop starts
            else
                elapsed = elapsed % length
            end
        end
    end

    -- every frame, before the game sets bones up for drawing
    local cycle = (length and length > 0) and math.min(elapsed / length, 0.9999) or 0
    local seconds = me:animate(MODEL, sequence, cycle)
    if seconds and not length then
        length = seconds
        if seconds > 0 then me:animate(MODEL, sequence, math.min(elapsed / seconds, 0.9999)) end
    elseif not seconds and not warned and globals.realtime() - started_at > 3 then
        warned = true
        print("emote: " .. MODEL .. " is not loading. Copy the plugin's .mdl / .vvd / .dx90.vtx there " ..
              "( see the [lua] model:animate console line ). sv_pure servers need settings > sv_pure bypass.")
    end
end)

callbacks.register("pre_prediction", function(cmd)
    if dancing and stop_move:get() and (cmd.forwardmove ~= 0 or cmd.sidemove ~= 0 or (cmd.buttons & buttons.jump) ~= 0) then
        stop()
    end
end)

callbacks.register("draw_model", function(m)
    if dancing and gun_mode:get() == 2 and m.index > 0 and (m.index == gun or m.index == gun_world) then
        m.hide = true
    end
end)

callbacks.register("level_shutdown", stop)
callbacks.register("unload", stop)
