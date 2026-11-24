-- draws a picture from the lua images folder ( lua\images\ next to the scripts ): png, jpg, bmp or gif ( animated gifs play )
local file     = ui.input("image file", "example.png")
local status   = ui.text("no image yet")
local scale    = ui.slider_float("scale", 0.1, 4, 1, "%.2fx")
local opacity  = ui.slider_int("opacity", 0, 255, 255)
local rounding = ui.slider_int("rounding", 0, 64, 0, "%d px")
local corner   = ui.combo("corner", { "top left", "top right", "bottom left", "bottom right", "centre" }, 2)

local image, loaded

callbacks.register("paint", function()
    -- load once per name, not every frame ( load_image opens the file )
    if file:get() ~= loaded then
        loaded = file:get()
        local err
        image, err = render.load_image(loaded)
        if image then
            local w, h = image:size()
            status:set(string.format("%s: %dx%d", loaded, w, h))
        else
            status:set(err)
        end
    end
    if not image then return end

    local w, h = image:size()
    w, h = w * scale:get(), h * scale:get()

    local sw, sh = render.screen_size()
    local pick   = corner:get()
    local x = (pick == 2 or pick == 4) and sw - w - 20 or 20
    local y = (pick == 3 or pick == 4) and sh - h - 20 or 20
    if pick == 5 then x, y = (sw - w) / 2, (sh - h) / 2 end

    render.image(image, x, y, w, h, color(255, 255, 255, opacity:get()), rounding:get())
end)
