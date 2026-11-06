# botox lua api

Lua 5.4.7 scripting for botox. Scripts can draw, read and change user commands, listen to game events, read and write every botox setting, read any netvar, and read/write/call raw memory.

- [Quick start](#quick-start)
- [How scripts run](#how-scripts-run)
- [callbacks](#callbacks) - [ui](#ui) - [render](#render) - [entities](#entities) - [entity](#entity)
- [cmd](#cmd) - [event](#event) - [engine](#engine) - [globals](#globals) - [cvar](#cvar) - [config](#config)
- [trace](#trace) - [input](#input) - [mem](#mem) - [client](#client) - [constants](#constants)
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

Player name for players, class name otherwise.

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
| [speed.lua](lua_examples/speed.lua) | local player velocity, keybind, indicator font |
| [box_esp.lua](lua_examples/box_esp.lua) | entities.players, world_to_screen, mins_maxs, combo |
| [slow_walk.lua](lua_examples/slow_walk.lua) | create_move cmd editing with a keybind |
| [kill_feed.lua](lua_examples/kill_feed.lua) | named game event, event:get_player, client.chat, button, ui.text |
| [raw_access.lua](lua_examples/raw_access.lua) | config, netvars, mem.read, mem.vfunc + mem.call |

---

## Limits

- `render.*` only in `paint`. Drawing is queued on the game thread and drawn by the render thread the next frame, like every botox overlay.
- No vgui / imgui widgets of your own outside the `ui.*` elements.
- `mem.call` passes integer arguments and returns `eax` only.
- `set_prop` / `config.set` / `cvar.set` change your client. Whatever the server owns (health, position) snaps back.
- A script can freeze the game with an endless loop. Botox does not time scripts out.
