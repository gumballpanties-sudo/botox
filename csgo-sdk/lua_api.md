# botox lua api

Lua 5.4.7 scripting for botox. Scripts can draw, read and change user commands, listen to game events, read and write every botox setting, read any netvar, and read/write/call raw memory.

- [Quick start](#quick-start)
- [How scripts run](#how-scripts-run)
- [callbacks](#callbacks) - [ui](#ui) - [render](#render) - [image](#image) - [entities](#entities) - [entity](#entity)
- [cmd](#cmd) - [event](#event) - [model](#model) - [engine](#engine) - [globals](#globals) - [cvar](#cvar) - [config](#config)
- [trace](#trace) - [fx](#fx) - [input](#input) - [mem](#mem) - [client](#client) - [constants](#constants)
- [Examples](#examples) - [Limits](#limits)

---

## Quick start

1. Put a `.lua` file in `C:\botox\lua\` (the folder is created on inject; settings > lua > **open folder** opens it).
2. Open the menu, **settings > lua**.
3. Left box: pick the script, press **load**. Its settings show up in the right box, one group per loaded script.
4. Edit the file, press **reload**. Errors show in red under the buttons and in the game console.

```lua
local enabled = ui.checkbox("hello text", true)

callbacks.register("paint", function()
    if enabled:get() then
        render.text(20, 200, "hello from lua", color(255, 255, 255))
    end
end)
```

Files and folders:

| path | what |
| --- | --- |
| `C:\botox\lua\*.lua` | scripts, listed in the menu (top level only) |
| `C:\botox\lua\lib\*.lua` | modules for `require( "name" )`, never listed as scripts |
| `C:\botox\lua\images\` | pictures for [`render.load_image`](#renderload_imagefile) (png, jpg, gif, bmp), created on inject |
| `C:\botox\lua\settings\<script>.json` | saved ui values, keyed by element label |
| `C:\botox\lua\autoload.txt` | scripts that were loaded last time, loaded again on inject |

---

## How scripts run

- **One Lua state per script.** Scripts cannot see each other's globals. Unload = the state is closed, all its memory is gone.
- **Every callback runs on the game's main thread.** No locking needed in Lua.
- **The whole Lua standard library is open** (`io`, `os`, `string`, `table`, `math`, `utf8`, `coroutine`, `debug`, `package`).
- **`print(...)`** writes to the game console as `[script] text`.
- **Errors:** a script that errors (at load, in any callback, in a button) is unloaded on the spot. Its `unload` callback still runs. The error with a traceback shows in the menu and the game console.
- **Settings save themselves.** Every ui element is saved by its label to `settings\<script>.json` (one second after a change, and on unload). On the next load, creating an element with the same label gives back the saved value. Keep labels unique inside a script.
- **Reload** = unload + load. Script top level code runs again.
- **Eject** runs every script's `unload` callback, then closes all states. Undo whatever your script changed (cvars, config values) in `unload`.
- **Colors** are packed integers made by [`color(r, g, b, a)`](#colorr-g-b-a).
- **Vectors** are three return values: `local x, y, z = player:origin()`.
- **Addresses** are integers.

---

## callbacks

#### `callbacks.register(name, fn)`

Adds `fn` to the callback `name`. One name can have many functions, they run in register order.

#### `callbacks.unregister(name, fn)`

Removes `fn` from `name`. Pass the same function value you registered.

| name | args | when |
| --- | --- | --- |
| `paint` | none | every frame, also in the main menu. The only place `render.*` works. |
| `pre_prediction` | `cmd` | every user cmd, before engine prediction and before botox aimbot / movement post features. Only while alive and not spectating. |
| `create_move` | `cmd` | every user cmd, after every botox feature, right before the cmd is normalized and sent. Runs dead or alive. |
| `frame_stage` | `stage` | every FrameStageNotify stage, before the game runs it. Compare with [`stages`](#stages). |
| `game_event` | `event` | every game event the client receives. |
| any game event name, e.g. `player_death` | `event` | only that event. |
| `level_init` | none | after a map loaded (LevelInitPostEntity). |
| `level_shutdown` | none | map unloading. |
| `draw_model` | `model` | every DrawModelExecute call, every pass: players, your viewmodel, weapons in hands, ragdolls. Props and dropped weapons the game batch-draws (DrawModelArray) never come here. Hide, tint, swap or animate it, see [`model`](#model). |
| `unload` | none | the script is being unloaded (button, error, `client.unload`, eject). |

```lua
callbacks.register("player_hurt", function(e)
    print("hurt for", e:get_int("dmg_health"))
end)
```

---

## ui

Elements live in the right box of settings > lua, in a group named after the script, in creation order. Create them at the top level of the script, once.

All constructors return an [element](#element).

#### `ui.checkbox(label, default)`

`default` = bool, default `false`. `get()` returns bool.

#### `ui.slider_int(label, min, max, default, format)`

`default` = `min` when left out. `format` = printf string, default `"%d"`. `get()` returns integer.

#### `ui.slider_float(label, min, max, default, format)`

`format` default `"%.2f"`. `get()` returns number.

#### `ui.combo(label, items, default)`

`items` = table of strings. `default` = 1-based index, default `1`. `get()` returns `index, item_name`.

#### `ui.color(label, r, g, b, a)`

Channels 0-255, each defaults to `255`. `get()` returns `r, g, b, a`, so `color(el:get())` gives a draw color.

#### `ui.keybind(label, key, style)`

`key` = Windows virtual key code (`0x12` alt, `0x05` mouse 4, see [Microsoft's list](https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes)), default `0` (none). `style`: `0` always on, `1` hold (default), `2` toggle. The user can change both in the menu. `get()` returns `active, key, style`. Not active while typing in chat / console.

#### `ui.button(label, fn)`

`fn` runs on the game thread the frame after the click.

#### `ui.text(label)`

A text line. `get()` returns the text, `set(text)` changes it.

#### `ui.input(label, default)`

Text input, max 255 chars. `get()` returns the string.

### element

#### `element:get()`

Current value, see each constructor above.

#### `element:set(...)`

Same shape `get()` returns: `checkbox:set(true)`, `combo:set(2)`, `color:set(255, 0, 0, 255)`, `keybind:set(key, style)`, `text:set("hi")`. Values are clamped to the element's range. Counts as a change (saved).

#### `element:visible(show)`

Hide or show the element. Without an argument just returns the current state. Hidden elements keep their value.

#### `element:label()`

The label string.

```lua
local glow  = ui.checkbox("glow", false)
local color_picker = ui.color("glow color", 255, 0, 0)

callbacks.register("paint", function()
    color_picker:visible(glow:get())
end)
```

---

## render

Only inside the `paint` callback (anything else raises an error). Coordinates are screen pixels, (0, 0) = top left. Drawn under the botox menu, over the game. Every `color` argument defaults to white.

#### `color(r, g, b, a)`

Global. Packs a color, channels 0-255, `a` defaults to `255`. Works anywhere.

#### `render.screen_size()`

Returns `width, height`. Works anywhere.

#### `render.text(x, y, text, color, font, flags, size)`

`font` = [`fonts`](#fonts) value, default `fonts.verdana`. `flags` = [`text_flags`](#text_flags), default `text_flags.shadow`. `size` = font size in px, `0` / left out = the font's own size.

#### `render.text_size(text, font, size)`

Returns `width, height` in px.

#### `render.line(x1, y1, x2, y2, color, thickness)`

`thickness` default `1`.

#### `render.rect(x1, y1, x2, y2, color, thickness, rounding)`

Outline. `thickness` default `1`, `rounding` default `0`.

#### `render.rect_filled(x1, y1, x2, y2, color, rounding)`

#### `render.gradient(x1, y1, x2, y2, top_color, bottom_color, rounding)`

Vertical gradient.

#### `render.circle(x, y, radius, color, segments, thickness)`

`segments` default `32`.

#### `render.circle_filled(x, y, radius, color, segments)`

#### `render.triangle(x1, y1, x2, y2, x3, y3, color, filled, thickness)`

`filled` default `true`; `false` draws the outline with `thickness`.

#### `render.poly(points, fill_color, outline_color, thickness)`

`points` = flat table `{ x1, y1, x2, y2, x3, y3, ... }`, 3 to 32 points, convex. `outline_color` left out = no outline.

#### `render.world_to_screen(x, y, z)`

Returns `screen_x, screen_y`, or `nil` when the point is behind the camera.

#### `render.blur(x1, y1, x2, y2, strength, color, rounding)`

Blurs what is on screen under the rect: the game, its hud, the botox overlays drawn before lua, and everything drawn before this call (this script's earlier draws, scripts loaded before it). Draw your panel on top right after it = frosted glass panel. `strength` = blur radius, default `1` = 34 px (the menu background blur at 100), `0` to `3`, `0` = no blur. `color` tints and fades the blur: default white = as is, `color(255, 255, 255, 128)` = half blurred, `color(170, 200, 255)` = bluish. `rounding` = corner radius, default `0`; give it your panel's. Each call blurs the whole frame once at about 270 rows: cheap, but one call per panel, not per element.

```lua
callbacks.register("paint", function()
    render.blur(100, 100, 360, 220, 1, color(255, 255, 255), 8)       -- blur first
    render.rect_filled(100, 100, 360, 220, color(20, 20, 20, 120), 8) -- see-through panel over it
end)
```

#### `render.load_image(file)`

Works anywhere. `file` = a png, jpg, gif or bmp in `C:\botox\lua\images\` (`"logo.png"`, subfolders too: `"icons\\skull.gif"`), or a full path (`"D:\\pics\\cat.png"`). Returns an [image](#image), or `nil, error` (no such file, other format, bigger than 4096 px a side, file over 32 MB). Plain ASCII file names.

Only the file header is read here. The picture itself is decoded the first time it is drawn (a big one can hitch that frame) and freed 10 s after its last draw. A file with a good header but broken pixels (cut short, still being written) draws nothing or a cut picture. Load once (top level, or when the name changes), not every frame. Changed the file on disk? Call `load_image` again (or reload the script) to see the new one.

#### `render.image(image, x, y, w, h, color, rounding)`

Draws `image` with its top left corner at `x, y`. `w, h` left out = the file's own size. Only `w` (or `h = 0`) = the other one keeps the aspect ratio, same for only `h` (`w = 0`). `color` tints and fades: default white = as is, `color(255, 255, 255, 128)` = half see-through. `rounding` = corner radius, default `0`. Animated gifs play on their own.

### image

#### `image:size()`

Returns `width, height` in px, the file's own size. Works anywhere.

```lua
local logo = render.load_image("logo.png")

callbacks.register("paint", function()
    if logo then
        render.image(logo, 20, 20, 128)   -- 128 px wide, height keeps the aspect
    end
end)
```

---

## entities

#### `entities.local_player()`

Your player, or `nil` when not in game.

#### `entities.get(index)`

Entity at `index`, or `nil`. 0 = world, 1-64 = players.

#### `entities.from_handle(handle)`

Entity from an EHANDLE integer (what `*_handle` netvars hold), or `nil`.

#### `entities.highest_index()`

Highest entity index in use.

#### `entities.players(include_dead, include_dormant)`

Array of player entities. Both flags default `false` (only alive, non dormant). Includes you.

#### `entities.by_class(network_name)`

Array of entities whose client class is `network_name`, e.g. `"CPlantedC4"`, `"CCSPlayer"`, `"CSmokeGrenadeProjectile"`.

### entity

An entity value remembers only its index. Every method looks the entity up again, so a kept entity never dangles: if it is gone the call raises `entity N is gone` (use `is_valid` first when unsure). `==` compares indexes, `tostring` gives `entity(N)`.

#### `entity:index()`
#### `entity:ptr()`

Address of the C++ entity, for `mem.*`.

#### `entity:is_valid()`

`false` when the index no longer has an entity. Never errors.

#### `entity:is_player()`
#### `entity:is_alive()`
#### `entity:is_dormant()`
#### `entity:is_enemy()`

Against your player, follows game mode / `mp_teammates_are_enemies`. `false` for yourself.

#### `entity:team()`

`2` T, `3` CT, `1` spectator.

#### `entity:health()`
#### `entity:armor()`

`0` when the entity's class has no armor.

#### `entity:flags()`

Bit field, test with [`flags`](#flags): `me:flags() & flags.onground ~= 0`.

#### `entity:move_type()`

`2` walk, `8` noclip, `9` ladder.

#### `entity:origin()`

`x, y, z` (abs origin).

#### `entity:velocity()`

`x, y, z` units per second.

#### `entity:eye_position()`

`x, y, z`. Not a player = its origin.

#### `entity:mins_maxs()`

Collision box relative to origin: `min_x, min_y, min_z, max_x, max_y, max_z`. Nothing when the entity has no collideable.

#### `entity:class_name()`

Client class network name, e.g. `"CCSPlayer"`.

#### `entity:class_id()`

Client class id number.

#### `entity:name()`

Player name for players, `class_name()` otherwise (also for a player whose info has not arrived yet, mid join).

#### `entity:weapon()`

Active weapon entity, or `nil` (also for entities that cannot hold one).

#### `entity:weapon_id()`

Item definition index (on a weapon entity): `7` ak47, `9` awp, `16` m4a4, `61` usp-s ... `-1` when the entity's class has none.

#### `entity:bone_position(bone)`

`x, y, z` from the cached bone matrix. Nothing back when the entity is not a studio model or the bone is not in its cache.

#### `entity:hitbox_position(hitbox)`

`x, y, z` centre of a hitbox (0 = head). Nothing back when the entity is not a studio model.

#### `entity:get_prop(netvar, type, extra_offset)`

Any networked variable by `"Class->m_name"`, the same names botox uses in [entity.h](game/sdk/classes/entity.h): `"CBasePlayer->m_iHealth"`, `"CCSPlayer->m_bIsScoped"`, `"CBaseEntity->m_vecOrigin"`, `"CBaseCombatWeapon->m_iClip1"`. Use the class that declares the prop or any class below it (`"CCSPlayer->m_iHealth"` works too). `type` (default `"int"`):

| type | returns |
| --- | --- |
| `int` | integer (4 bytes) |
| `float` | number |
| `bool` | bool (1 byte) |
| `short` | integer (2 bytes) |
| `byte` | integer (1 byte) |
| `vector` | `x, y, z` |
| `string` | string (char array prop) |
| `handle` | entity or `nil` |

`extra_offset` is added to the address (array elements: `index * 4`).

#### `entity:set_prop(netvar, type, value...)`

Writes it. `vector` takes three numbers. `string` is read only. Local only: the server keeps its own copy.

#### `entity:get_datafield(name, type)`

Prediction / data map field (not networked), e.g. `"m_MoveType"`, `"m_flStamina"`, `"m_nButtons"`, `"m_vecNetworkOrigin"`. Same types as `get_prop`.

#### `entity:set_datafield(name, type, value...)`

#### `entity:animate(path, sequence, cycle)`

Poses this player with another model's animation, on your client only: the game's own bone merge (`EF_BONEMERGE`, what server emote plugins use) done to the player's real bones. `path`'s skeleton plays `sequence` (name like `"Emote_Dance_Pump"`, or 0-based index) at `cycle` (0 to 1: a looping sequence wraps, a one shot holds its last frame) where the player stands. Bones with the same name follow it, bones the animation model lacks keep their own motion relative to their parent. Because the real bones move, everything built from them follows: the model in every pass (shadows, glow, chams), the weapon in hand (bone merged to the hand), the holstered gear the game hangs on attachments (pistol, knife, grenades, c4, defuser), effects at attachments. The server and movement don't change.

Call it every frame (`frame_stage` with `stages.render_start` is the place: right before the game sets bones up to draw). It holds for that frame and the next, so a script that stops calling ends it a frame later; `entity:animate(nil)` ends it at once. Returns the sequence length in seconds (add `frametime / length` to `cycle` each frame for its own speed), or `nil` while the model loads (at the next frame start), when it can't load (one console line says why) or has no such sequence. Works on your own player only (others: `model:animate` on their draws, so botox's aimbot and backtrack never read a posed bone) and only in third person (first person draws no body, and the camera height reads the head bone). Same animation model rules as [`model:animate`](#modelanimatepath-sequence-cycle).

```lua
local EMOTES = "models/player/custom_player/kodua/fortnite_emotes_v2.mdl"
local start = globals.realtime()

callbacks.register("frame_stage", function(stage)
    local me = entities.local_player()
    if stage == stages.render_start and me and me:is_alive() then
        me:animate(EMOTES, "Emote_Dance_Pump", (globals.realtime() - start) / 3.53)
    end
end)
```

#### `entity:set_model_index(index)`

Gives the entity another model on your client: the game's own `C_BaseEntity::SetModelIndex`. Get `index` from [`engine.precache_model`](#engineprecache_modelpath). Unlike `set_prop("...->m_nModelIndex")`, which only writes the number, it also moves the model the entity draws with and resets its bones, attachments and animation state for it. A bare `m_nModelIndex` write leaves the entity drawing data built for the old model.

The server keeps sending its own model whenever it changes, so a one-off call lasts until then. For your viewmodel use [`engine.swap_viewmodel`](#engineswap_viewmodelfrom_index-to_index-map), which keeps the swap on and renumbers the animations. Use this for anything else, such as a weapon's world model (`CBaseCombatWeapon->m_hWeaponWorldModel`), from `frame_stage` whenever its model index isn't yours. Errors when `index` has no model or when called inside `draw_model`.

---

## cmd

The user command in `create_move` / `pre_prediction`. Read and write fields directly: `cmd.buttons = cmd.buttons | buttons.duck`. Only valid inside its callback (a kept cmd raises an error later).

| field | type | |
| --- | --- | --- |
| `cmd.command_number` | int | |
| `cmd.tick_count` | int | |
| `cmd.pitch` | number | view angle x, sent to the server (silent aim = change this, not engine view angles) |
| `cmd.yaw` | number | view angle y |
| `cmd.roll` | number | view angle z |
| `cmd.forwardmove` | number | -450 .. 450 |
| `cmd.sidemove` | number | -450 .. 450, + = right |
| `cmd.upmove` | number | |
| `cmd.buttons` | int | bit field, see [`buttons`](#buttons) |
| `cmd.impulse` | int | |
| `cmd.weaponselect` | int | |
| `cmd.weaponsubtype` | int | |
| `cmd.random_seed` | int | |
| `cmd.mousedx` | int | |
| `cmd.mousedy` | int | |
| `cmd.has_been_predicted` | bool | read only |
| `cmd.send_packet` | bool | `false` holds this cmd back (choke) |
| `cmd.ptr` | int | read only, address of the CUserCmd |

Angles are normalized and clamped after `create_move`. Changing `yaw` without fixing `forwardmove` / `sidemove` changes where you walk.

---

## event

Passed to `game_event` and named event callbacks. Only valid inside its callback. Field names are in the game's `resource/modevents.res` (e.g. `player_death`: `userid`, `attacker`, `assister`, `weapon`, `headshot`).

#### `event:name()`
#### `event:get_int(key, default)`
#### `event:get_float(key, default)`
#### `event:get_bool(key, default)`
#### `event:get_string(key, default)`
#### `event:get_uint64(key)`
#### `event:get_player(key)`

Reads a user id field (default key `"userid"`) and returns that player's entity, or `nil`.

```lua
callbacks.register("player_death", function(e)
    local victim, attacker = e:get_player("userid"), e:get_player("attacker")
    if attacker and attacker == entities.local_player() then
        print("you killed", victim and victim:name() or "?", "with", e:get_string("weapon"))
    end
end)
```

---

## model

Passed to `draw_model`, once per model draw (hundreds a frame: keep the callback short). Only valid inside its callback.

| field | type | |
| --- | --- | --- |
| `model.name` | string | read only, model path, e.g. `"models/weapons/v_knife_default_ct.mdl"` |
| `model.index` | int | read only, entity index. Your viewmodel draws under its own index |
| `model.entity` | entity | read only, `nil` for draws with no entity |
| `model.flags` | int | read only, `STUDIO_*` render flags. Shadow / depth passes have `0x40000000` / `0x08000000` set |
| `model.x` | number | read only, render origin |
| `model.y` | number | |
| `model.z` | number | |
| `model.hide` | bool | `true` = this draw is skipped (shadow passes too, they come here as well) |
| `model.color` | int | tint for this draw, made by [`color(r, g, b, a)`](#colorr-g-b-a). Alpha multiplies the game's own. `nil` = no tint |
| `model.model` | string | draw another `.mdl` in its place, e.g. a custom gun. Other paths are ignored. Reads back `nil` until the swap is live |
| `model.ptr` | int | read only, address of the ModelRenderInfo_t |

**Swaps:** the path loads at the start of the next frame (never inside a draw), so the first frame draws the original. It loads again by itself after every map change. Every new path stays loaded for the session and botox keeps at most 64 of them, so set final paths (e.g. from a button), not text that is still being typed.

The new model is drawn with the original's skeleton: rig it to the same bones, and weight it only to bones the stock mesh uses too (the game skips the others when it builds the pose, they hold old data). A swap is refused, with one console line saying why, when the new model has more bones than the drawn one, has flexes (faces) or is a static prop model. A file that can't be loaded prints once too.

Botox features that pick models by file name (enemy chams look for `player`, viewmodel motion blur / sheen look for `weapons/v_`) see the new file's name. Keep the stock folder names (`models/weapons/v_...`) to keep them working.

**Custom files on servers:** loose `.mdl` / `.vtx` / `.vvd` / `.vmt` / `.vtf` files under `csgo\` are rejected by sv_pure servers (the engine then hands back the error model). Turn on settings > sv_pure bypass, or pack them in a vpk. No precache is needed: the swap never goes through the server's precache table.

**Order:** scripts see the draw first. A tint or swap is then drawn through botox chams / ragdoll removal / motion blur as usual, so enemy chams still win over a tint. Glow outlines and the rear view mirror show the real model.

```lua
callbacks.register("draw_model", function(m)
    if m.name:find("v_knife") then
        m.model = "models/weapons/v_knife_custom.mdl"
    elseif m.entity and m.entity:is_player() and m.entity:is_enemy() then
        m.color = color(255, 80, 80, 255)
    end
end)
```

#### `model:animate(path, sequence, cycle)`

Poses this draw with another model's animation, the way the game's bone merge (`EF_BONEMERGE`) does: `path`'s skeleton plays `sequence` (name like `"Emote_Dance_Pump"`, or 0-based index) at `cycle` (0 to 1: a looping sequence wraps, a one shot holds its last frame) where this model stands, and every bone with the same name follows it. Bones the animation model lacks (pouches, jiggle bones, finger tips) keep their rest offset from their parent. Works on mesh-less animation models (animsets, emote skeletons). Combine with `m.model` to animate the swapped mesh. `m:animate(nil)` cancels it for this draw.

Returns the sequence length in seconds (add `frametime / length` to `cycle` each frame for its own speed), or `nil` while the model loads, when it can't load (one console line says why) or has no such sequence.

Loads like a swap: at the start of the next frame, so the first frame draws the normal pose. At most 16 animation models per session. Only the model's own sequences, not ones it pulls in with `$includemodel`; a blend sequence plays its first animation. Only this draw moves: the entity's bones, attachments and what hangs on them keep the real pose (the holstered gear csgo draws on its fast path never comes through `draw_model`). To pose a player and everything attached to it, use [`entity:animate`](#entityanimatepath-sequence-cycle). `m:animate` is for draws without an entity of their own, or a pose on one pass only. Glow outlines and the rear view mirror show the real pose. Call it on every pass of the entity (shadows come here too) so they match.

```lua
local EMOTES = "models/player/custom_player/kodua/fortnite_emotes_v2.mdl"
local start = globals.realtime()

callbacks.register("draw_model", function(m)
    -- enemy bodies dance in the picture only ( their guns, gear and hitboxes stay put )
    if m.entity and m.entity:is_player() and m.entity:is_enemy() then
        m:animate(EMOTES, "Emote_Dance_Pump", (globals.realtime() - start) / 3.53)
    end
end)
```

---

## engine

#### `engine.exec(command)`

Runs a console command, unrestricted (`"say hi"`, `"+jump"`, `"bind f5 ..."`).

#### `engine.in_game()`
#### `engine.connected()`
#### `engine.local_index()`
#### `engine.max_clients()`
#### `engine.map_name()`

Short name, `"de_mirage"`.

#### `engine.level_name()`

Full path, `"maps/de_mirage.bsp"`.

#### `engine.view_angles()`

Your camera angles `pitch, yaw, roll`.

#### `engine.set_view_angles(pitch, yaw, roll)`

Moves your camera (normalized + clamped first).

#### `engine.player_info(index)`

Table `{ name, user_id, steam_id, xuid, friends_id, bot, hltv }`, or `nil`.

#### `engine.player_for_user_id(user_id)`

Entity index, `0` when unknown.

#### `engine.playing_demo()`
#### `engine.latency()`

Returns `outgoing, incoming` latency in seconds (`0, 0` when not connected).

#### `engine.precache_model(path)`

Model index for a `.mdl`, loading it and adding it to the model precache table if the map didn't. `nil` when not in game or when the model can't be drawn. Like a `draw_model` swap it needs the `.mdl` plus its `.vvd` and `.dx90.vtx` from the same compile. A missing or mismatched file (or a loose file on an sv_pure server) prints one console line and returns `nil`. Without that check you'd get an index whose model is never drawn. Not inside `draw_model`. Botox's skin changer uses the same precache. The index only exists on your client, and a server that precaches more models later can take its slot, so look it up again after `level_init`.

#### `engine.model_sequences(index)`

The model's sequences, numbered the way `m_nSequence` numbers them (sequences pulled in with `$includemodel` included, like every stock knife's): `{ { name = "awp_idle", activity = "ACT_VM_IDLE" }, ... }`. `[1]` is sequence 0, and `activity` is `""` when the sequence has none. `nil` when there's no model at `index`. Not inside `draw_model`. Use it to check or hand-write a `swap_viewmodel` map.

#### `engine.swap_viewmodel(from_index, to_index, map)`

Your viewmodel shows `to_index` wherever the game picks `from_index` (both from [`engine.precache_model`](#engineprecache_modelpath), e.g. the stock `models/weapons/v_snip_awp.mdl` and your AWP). It's applied the way botox's knife changer does it. The incoming model is rewritten as it arrives from the server. Each server update writes it again before prediction saves its copy, so a swap made while the weapon is in your hands sticks too. Botox also switches it back to yours at render start whenever the game resets the viewmodel itself (a fresh viewmodel, weapon deploy). You don't need code that runs every frame. With botox's knife changer on, `from_index` is the knife the changer shows: read it off the viewmodel (`CBaseViewModel->m_nModelIndex`) while holding it, not the stock knife path.

The server still numbers the animations for the stock model, so each incoming sequence is renumbered: the sequence of `to_index` with the same activity (`ACT_VM_IDLE`, `ACT_VM_PRIMARYATTACK`, ...), else the same name, else its idle. `map` (optional) overrides entries: `{ [from_sequence] = to_sequence }`, both 0-based, numbers as in [`engine.model_sequences`](#enginemodel_sequencesindex).

This is the way for a model rigged differently from the stock one (other bone names, its own animations, arms built in): `model.model` in `draw_model` only fits meshes rigged to the stock skeleton. Built-in arms come with their own hands, so hide the game's arms in `draw_model` (names with `v_models/arms/`). Call it again with another `to_index` to change it. A call with bad arguments errors and keeps the old swap. `engine.swap_viewmodel(from_index, nil)` ends it, and so does unloading the script: the game's model comes back with the next server update. On eject the stock model comes back with the full update botox requests on online servers, and on a local server at the next weapon switch. Swaps end on map change, because indexes are per map, so set them again from `level_init` or `frame_stage`. Your client only. Full flow: [viewmodel_swap.lua](lua_examples/viewmodel_swap.lua).

---

## globals

Engine time values (CGlobalVarsBase).

#### `globals.realtime()`
#### `globals.curtime()`
#### `globals.frametime()`
#### `globals.absframetime()`
#### `globals.tickcount()`
#### `globals.tickinterval()`

`1 / tickrate`.

#### `globals.framecount()`
#### `globals.maxclients()`

---

## cvar

#### `cvar.exists(name)`
#### `cvar.get_int(name)`

`nil` when the cvar does not exist (same for `get_float` / `get_string`).

#### `cvar.get_float(name)`
#### `cvar.get_string(name)`
#### `cvar.set(name, value)`

`value` = integer, number, bool or string. Raises an error when the cvar does not exist. Respects the cvar's min / max. Put it back in your `unload` callback.

---

## config

Every botox setting, by its name in [variables.h](globals/config/variables.h) (the `m_*` names, e.g. `m_debug_log`, `m_menu_key`, `m_accent`; `config.list()` prints them all). Reads and writes the live value: a `set` acts exactly like changing it in the menu. Not saved to the `.bx` until you save the config in the menu.

#### `config.get(name)`

| type | returns |
| --- | --- |
| bool / int / float / string | the value |
| color | `r, g, b, a` (0-255) |
| keybind | `key, style` |
| font | `name, size` |
| bool[] / int[] / float[] / string[] | table (1-based) |
| color[] | table of `{ r, g, b, a }` |

Raises an error for an unknown name.

#### `config.set(name, value...)`

Same shapes as `get`: `config.set("m_accent", 255, 0, 0, 255)`, `config.set("m_menu_key", 0x2D, 1)`. Arrays: pass a table, missing / nil entries stay. Setting a font reloads fonts.

#### `config.type(name)`

`"bool"`, `"int"`, `"float"`, `"string"`, `"color"`, `"keybind"`, `"font"`, `"bool[]"`, `"int[]"`, `"float[]"`, `"string[]"`, `"color[]"`.

#### `config.reset(name)`

Back to the default value.

#### `config.list()`

Table `name -> type` of every variable (about 1580).

```lua
for name, kind in pairs(config.list()) do
    if name:find("glow") then print(name, kind, config.get(name)) end
end
```

---

## trace

#### `trace.line(sx, sy, sz, ex, ey, ez, mask, skip)`

Ray from start to end. `mask` default [`masks.shot`](#masks). `skip` = entity to ignore, default your player. Returns a table:

| field | |
| --- | --- |
| `fraction` | 0..1, 1 = nothing hit |
| `hit` | bool |
| `x`, `y`, `z` | end position |
| `normal_x`, `normal_y`, `normal_z` | surface normal |
| `entity` | entity hit or `nil` (world = entity 0) |
| `hitgroup`, `hitbox` | on players |
| `contents` | contents bits |
| `surface` | surface name |
| `start_solid`, `all_solid` | bool |

#### `trace.hull(sx, sy, sz, ex, ey, ez, min_x, min_y, min_z, max_x, max_y, max_z, mask, skip)`

Same with a box (player standing = `-16, -16, 0, 16, 16, 72`).

#### `trace.point_contents(x, y, z, mask)`

Contents bits at a point. `mask` default `masks.all`.

---

## fx

Real game effects in the world. Only you see them (client side). Call from any callback, they fire once and the game animates them.

#### `fx.particle(name, x, y, z, pitch, yaw, roll)`

Spawns the particle system `name` at the point, angles default `0`. Returns `false` when not in game or the map never precached that system, so try a few names. Names botox death particles use: `weapon_confetti_balloons`, `weapon_confetti_omni`, `weapon_confetti`, `explosion_hegrenade_interior`, `explosion_hegrenade_dirt`, `explosion_basic`, `blood_impact_heavy` (pitch `-90` = sprays up).

#### `fx.sparks(x, y, z, magnitude, trail)`

Metal sparks, always available. `magnitude` and `trail` `1` to `16`, default `1`.

---

## input

#### `input.is_key_down(vk)`

Windows virtual key code held right now (botox's own key state, works with the game focused).

#### `input.mouse_pos()`

Cursor `x, y` in game window pixels.

#### `input.menu_open()`

Botox menu open.

#### `input.keys_blocked()`

`true` while typing in chat / console / a menu text box.

---

## mem

Raw memory. Every read and write is crash guarded: a bad address raises a Lua error instead of crashing the game. Writes to read only / code pages unprotect, write and restore on their own.

#### `mem.module(name)`

Base address of a loaded dll (`"client.dll"`), or `nil`.

#### `mem.find_pattern(module, pattern)`

IDA style pattern in the module's code: `"55 8B EC 83 E4 F8 ?? ??"`. Address or `nil`.

#### `mem.interface(module, name)`

`CreateInterface` lookup: `mem.interface("engine.dll", "VEngineClient014")`.

#### `mem.export(module, name)`

`GetProcAddress`.

#### `mem.interfaces()`

Table of every interface botox already holds: `base_client`, `engine_client`, `client_entity_list`, `input_system`, `material_system`, `engine_trace`, `debug_overlay`, `convar`, `model_info`, `game_movement`, `prediction`, `model_render`, `string_tables`, `model_cache`, `engine_sound`, `physics_surface_props`, `surface`, `game_types`, `localize`, `panel`, `physics_collision`, `studio_render`, `render_view`, `game_event_manager`, `key_values_system`, `global_vars`, `client_mode`, `weapon_system`, `client_state`, `input`, `move_helper`, `d3d_device`, `panorama`, `view_render_beams`, `file_system`, `hud_chat`.

#### `mem.netvar(name)`

Netvar offset by `"Class->m_name"`, `0` when unknown.

#### `mem.read_i8(address)`
#### `mem.read_u8(address)`
#### `mem.read_i16(address)`
#### `mem.read_u16(address)`
#### `mem.read_i32(address)`
#### `mem.read_u32(address)`
#### `mem.read_i64(address)`
#### `mem.read_float(address)`
#### `mem.read_double(address)`
#### `mem.read_ptr(address)`

Pointer sized (4 bytes in the x86 build).

#### `mem.read_string(address, max)`

Up to the first zero byte, at most `max` (default 256) bytes.

#### `mem.read_bytes(address, count)`

Lua string of raw bytes (`string.byte` / `string.unpack` to decode).

#### `mem.write_i8(address, value)`
#### `mem.write_u8(address, value)`
#### `mem.write_i16(address, value)`
#### `mem.write_u16(address, value)`
#### `mem.write_i32(address, value)`
#### `mem.write_u32(address, value)`
#### `mem.write_i64(address, value)`
#### `mem.write_float(address, value)`
#### `mem.write_double(address, value)`
#### `mem.write_ptr(address, value)`
#### `mem.write_string(address, text)`

Writes the text plus a zero byte.

#### `mem.write_bytes(address, bytes)`

Writes a Lua string of raw bytes (`"\x90\x90"`, or build with `string.pack`).

#### `mem.vfunc(object, index)`

Address of virtual function `index` of `object` (reads its vtable).

#### `mem.call(address, convention, args...)`

Calls native code. `convention` = `"cdecl"`, `"stdcall"`, `"thiscall"` (first arg = `this`), `"fastcall"` (first two args = ecx, edx). Up to 12 args after those. Args: integers / addresses, bools (0/1), nil (0), strings (pointer to the Lua string, valid during the call). Returns `eax` as an integer. Float args: pass `mem.f2i(x)`. Float returns (x87) are not readable. A crash inside the call raises a Lua error, but the game may be left in a bad state.

```lua
-- IVEngineClient::GetLocalPlayer, vtable slot 12
local engine_client = mem.interfaces().engine_client
print(mem.call(mem.vfunc(engine_client, 12), "thiscall", engine_client))
```

#### `mem.alloc(size)`

Zeroed memory block, address back. Freed on unload if you do not free it.

#### `mem.free(address)`

Only blocks from `mem.alloc`.

#### `mem.protect(address, size, protection)`

`VirtualProtect`, returns the old protection or `nil`. (`0x40` = PAGE_EXECUTE_READWRITE.)

#### `mem.f2i(number)`

Float bits as an integer.

#### `mem.i2f(integer)`

Integer bits as a float.

---

## client

#### `client.log(text)`

Botox on-screen log line, prefixed with the script name.

#### `client.chat(text)`

Local chat line (only you see it). Color bytes (names as in [chat_hud.h](hacks/chat_hud.h)): `\x01` default `\x02` red `\x03` light purple `\x04` green `\x05` lime `\x06` light green `\x07` light red `\x08` gray `\x09` light olive `\x0A` light steel blue `\x0B` light blue `\x0C` blue `\x0D` purple `\x0E` pink `\x0F` light red 2 `\x10` olive. Start the text with a space or colors are ignored.

#### `client.name()`

This script's name (file name without `.lua`).

#### `client.folder()`

`C:\botox\lua`.

#### `client.unload()`

Unloads this script at the next frame (its `unload` callback runs).

---

## constants

### buttons

For `cmd.buttons`.

| name | name | name |
| --- | --- | --- |
| `buttons.attack` | `buttons.jump` | `buttons.duck` |
| `buttons.forward` | `buttons.back` | `buttons.use` |
| `buttons.left` | `buttons.right` | `buttons.moveleft` |
| `buttons.moveright` | `buttons.attack2` | `buttons.reload` |
| `buttons.score` | `buttons.speed` | `buttons.walk` |
| `buttons.zoom` | `buttons.bullrush` | `buttons.lookatweapon` |

### flags

For `entity:flags()`.

| name | name | name |
| --- | --- | --- |
| `flags.onground` | `flags.ducking` | `flags.animducking` |
| `flags.waterjump` | `flags.frozen` | `flags.inwater` |
| `flags.fakeclient` | `flags.godmode` | `flags.partialground` |

### stages

For the `frame_stage` callback.

| name | |
| --- | --- |
| `stages.start` | |
| `stages.net_update_start` | |
| `stages.net_update_postdataupdate_start` | after new network data, before it is applied: skin / model changes go here |
| `stages.net_update_postdataupdate_end` | |
| `stages.net_update_end` | |
| `stages.render_start` | before the frame renders |
| `stages.render_end` | |

### fonts

For `render.text` / `render.text_size`. Any botox font slot number also works.

| name | |
| --- | --- |
| `fonts.verdana` | 11 px, default |
| `fonts.verdana_bold` | 11 px bold |
| `fonts.tahoma` | 12 px |
| `fonts.tahoma_bold` | 12 px bold |
| `fonts.icons` | botox icon font |
| `fonts.indicator` | big indicator font |
| `fonts.weapon_icons` | csgo weapon icons |
| `fonts.custom_indicator` | the font picked in fonts > indicators |
| `fonts.custom_esp` | the font picked in fonts > esp |
| `fonts.custom_net_graph` | the font picked in fonts > net graph |

### text_flags

| name | |
| --- | --- |
| `text_flags.none` | |
| `text_flags.shadow` | drop shadow, default |
| `text_flags.outline` | 1 px outline |

### masks

For `trace.*`.

| name | |
| --- | --- |
| `masks.all` | everything |
| `masks.solid` | world + props + players |
| `masks.playersolid` | what blocks player movement (adds player clips) |
| `masks.shot` | what bullets hit, default for traces |

---

## Examples

Full scripts in [lua_examples/](lua_examples/), copy them into `C:\botox\lua\`:

| file | shows |
| --- | --- |
| [watermark.lua](lua_examples/watermark.lua) | paint, text_size, ui color / slider, latency |
| [image.lua](lua_examples/image.lua) | render.load_image + render.image: a picture from `lua\images\` in a corner, scale / opacity / rounding, ui.input file name, ui.text status |
| [blur_panel.lua](lua_examples/blur_panel.lua) | render.blur: frosted glass panel, strength / tint / rounding / panel color options, drag it with the mouse while the menu is open |
| [speed.lua](lua_examples/speed.lua) | local player velocity, keybind, indicator font |
| [box_esp.lua](lua_examples/box_esp.lua) | entities.players, world_to_screen, mins_maxs, combo |
| [slow_walk.lua](lua_examples/slow_walk.lua) | create_move cmd editing with a keybind |
| [kill_feed.lua](lua_examples/kill_feed.lua) | named game event, event:get_player, client.chat, button, ui.text |
| [raw_access.lua](lua_examples/raw_access.lua) | config, netvars, mem.read, mem.vfunc + mem.call |
| [edgebug_fx.lua](lua_examples/edgebug_fx.lua) | edgebug detect in pre_prediction, own world particles (spark sim, shockwave rings, pillar) via world_to_screen + poly, engine.exec sound |
| [custom_models.lua](lua_examples/custom_models.lua) | draw_model: custom knife / gun model swap applied by a button, enemy tint, hide arms, ui read once a frame in paint |
| [viewmodel_swap.lua](lua_examples/viewmodel_swap.lua) | AWP viewmodel + world model swapped to a differently rigged model (own bones, own anims, arms built in): engine.precache_model, engine.swap_viewmodel (animations renumbered by activity), entity:set_model_index on the world model, stock arms hidden in draw_model |
| [fortnite_emotes_local.lua](lua_examples/fortnite_emotes_local.lua) | `entity:animate`: 84 Fortnite emotes on your own player off the plugin's mesh-less emote skeleton ( gun in hand + holstered gear dance along ), intro then loop, keybind toggle / hold, forced third person restored after, weapon in hand or hidden, stop on move |
| [custom_edgebug.lua](lua_examples/custom_edgebug.lua) | full edgebug from scratch, hotwheels style: own csgo air movement sim on trace.hull, cmd replay in pre_prediction + create_move, mouse lock / strafe camera via frame_stage + set_view_angles. Gate: `lua tools/custom_edgebug_check.lua` |

---

## Limits

- `render.*` only in `paint`. Drawing is queued on the game thread and drawn by the render thread the next frame, like every botox overlay.
- No vgui / imgui widgets of your own outside the `ui.*` elements.
- `mem.call` passes integer arguments and returns `eax` only.
- `set_prop` / `config.set` / `cvar.set` change your client. Whatever the server owns (health, position) snaps back.
- A script can freeze the game with an endless loop. Botox does not time scripts out.
- `draw_model` runs per draw: a slow callback costs fps directly. A swap needs a model rigged to the drawn model's bones. `model:animate` matches bones by name, so the animation's skeleton must use the drawn model's bone names.
