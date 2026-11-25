-- frosted glass panel: render.blur under a see-through panel.
-- open the menu and drag the panel with mouse 1 to watch it blur whatever is behind it
local enabled  = ui.checkbox("show panel", true)
local strength = ui.slider_float("blur strength", 0, 3, 1, "%.2f")
local tint     = ui.color("blur tint", 255, 255, 255, 255)
local fill     = ui.color("panel color", 20, 20, 20, 110)
local rounding = ui.slider_int("rounding", 0, 30, 8, "%d px")
local width    = ui.slider_int("width", 80, 800, 280, "%d px")
local height   = ui.slider_int("height", 40, 500, 140, "%d px")

local x, y = 120, 120
local grab_x, grab_y   -- cursor offset inside the panel while dragging
local was_down = false

callbacks.register("paint", function()
    if not enabled:get() then return end
    local w, h = width:get(), height:get()

    local mx, my = input.mouse_pos()
    local down   = input.menu_open() and input.is_key_down(0x01)
    if down and not was_down and mx >= x and mx <= x + w and my >= y and my <= y + h then
        grab_x, grab_y = mx - x, my - y   -- a press on the panel starts the drag
    elseif not down then
        grab_x = nil
    end
    if grab_x then x, y = mx - grab_x, my - grab_y end
    was_down = down

    local r = rounding:get()
    render.blur(x, y, x + w, y + h, strength:get(), color(tint:get()), r)   -- blur first ...
    render.rect_filled(x, y, x + w, y + h, color(fill:get()), r)           -- ... see-through panel over it
    render.rect(x, y, x + w, y + h, color(255, 255, 255, 40), 1, r)
    render.text(x + 12, y + 10, string.format("blur %.2f", strength:get()), color(255, 255, 255))
end)
