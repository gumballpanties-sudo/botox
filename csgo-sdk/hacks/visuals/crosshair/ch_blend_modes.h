#pragma once
// Custom crosshair blend modes. The INDEX is the id: it is saved in the cfg as chBlendMode
// and picks the shader in fx_chblend_ps.h, so append only -- never reorder or delete.
// Categories and names follow Krita's blending-mode menu (KoCompositeOpRegistry.cpp).
// Entries of one category must stay contiguous: the menu starts a new header on change.

struct ChBlendMode { const char* cat; const char* name; };

inline constexpr ChBlendMode kChBlend[] = {
    { "basic",      "normal" },                      //   0  plain alpha, no shader
    { "basic",      "additive" },                    //   1  ROP dest ONE, no shader

    { "arithmetic", "addition" },                    //   2
    { "arithmetic", "subtract" },
    { "arithmetic", "multiply" },
    { "arithmetic", "divide" },
    { "arithmetic", "inverse subtract" },            //   6

    { "binary",     "xor" },                         //   7
    { "binary",     "or" },
    { "binary",     "and" },
    { "binary",     "nand" },
    { "binary",     "nor" },
    { "binary",     "xnor" },
    { "binary",     "implication" },
    { "binary",     "not implication" },
    { "binary",     "converse" },
    { "binary",     "not converse" },                //  16

    { "darken",     "burn" },                        //  17
    { "darken",     "linear burn" },
    { "darken",     "darken" },
    { "darken",     "gamma dark" },
    { "darken",     "darker color" },
    { "darken",     "shade (ifs illusions)" },
    { "darken",     "fog darken (ifs illusions)" },
    { "darken",     "easy burn" },                   //  24

    { "lighten",    "color dodge" },                 //  25
    { "lighten",    "linear dodge" },
    { "lighten",    "lighten" },
    { "lighten",    "linear light" },
    { "lighten",    "screen" },
    { "lighten",    "pin light" },
    { "lighten",    "vivid light" },
    { "lighten",    "flat light" },
    { "lighten",    "hard light" },
    { "lighten",    "soft light (ifs illusions)" },
    { "lighten",    "soft light (pegtop-delphi)" },
    { "lighten",    "soft light (photoshop)" },
    { "lighten",    "soft light (svg)" },
    { "lighten",    "gamma light" },
    { "lighten",    "gamma illumination" },
    { "lighten",    "lighter color" },
    { "lighten",    "p-norm a" },
    { "lighten",    "p-norm b" },
    { "lighten",    "super light" },
    { "lighten",    "tint (ifs illusions)" },
    { "lighten",    "fog lighten (ifs illusions)" },
    { "lighten",    "easy dodge" },                  //  46

    { "modulo",     "modulo" },                      //  47
    { "modulo",     "modulo - continuous" },
    { "modulo",     "divisive modulo" },
    { "modulo",     "divisive modulo - continuous" },
    { "modulo",     "modulo shift" },
    { "modulo",     "modulo shift - continuous" },   //  52

    { "negative",   "difference" },                  //  53
    { "negative",   "equivalence" },
    { "negative",   "additive subtractive" },
    { "negative",   "exclusion" },
    { "negative",   "arcus tangent" },
    { "negative",   "negation" },
    { "negative",   "invert" },                      //  59

    { "mix",        "overlay" },                     //  60
    { "mix",        "lambert lighting (linear)" },
    { "mix",        "lambert lighting (gamma 2.2)" },
    { "mix",        "hard mix" },
    { "mix",        "hard mix (photoshop)" },
    { "mix",        "hard mix softer (photoshop)" },
    { "mix",        "grain merge" },
    { "mix",        "grain extract" },
    { "mix",        "parallel" },
    { "mix",        "allanon" },
    { "mix",        "geometric mean" },
    { "mix",        "hard overlay" },
    { "mix",        "interpolation" },
    { "mix",        "interpolation - 2x" },
    { "mix",        "penumbra a" },
    { "mix",        "penumbra b" },
    { "mix",        "penumbra c" },
    { "mix",        "penumbra d" },                  //  77

    { "misc",       "copy red" },                    //  78
    { "misc",       "copy green" },
    { "misc",       "copy blue" },                   //  80

    { "hsy",        "color" },                       //  81
    { "hsy",        "hue" },
    { "hsy",        "tint" },
    { "hsy",        "saturation" },
    { "hsy",        "luminosity" },
    { "hsy",        "decrease saturation" },
    { "hsy",        "increase saturation" },
    { "hsy",        "decrease luminosity" },
    { "hsy",        "increase luminosity" },         //  89

    { "hsi",        "color hsi" },                   //  90
    { "hsi",        "hue hsi" },
    { "hsi",        "saturation hsi" },
    { "hsi",        "intensity" },
    { "hsi",        "decrease saturation hsi" },
    { "hsi",        "increase saturation hsi" },
    { "hsi",        "decrease intensity" },
    { "hsi",        "increase intensity" },          //  97

    { "hsl",        "color hsl" },                   //  98
    { "hsl",        "hue hsl" },
    { "hsl",        "saturation hsl" },
    { "hsl",        "lightness" },
    { "hsl",        "decrease saturation hsl" },
    { "hsl",        "increase saturation hsl" },
    { "hsl",        "decrease lightness" },
    { "hsl",        "increase lightness" },          // 105

    { "hsv",        "color hsv" },                   // 106
    { "hsv",        "hue hsv" },
    { "hsv",        "saturation hsv" },
    { "hsv",        "value" },
    { "hsv",        "decrease saturation hsv" },
    { "hsv",        "increase saturation hsv" },
    { "hsv",        "decrease value" },
    { "hsv",        "increase value" },              // 113

    { "quadratic",  "reflect" },                     // 114
    { "quadratic",  "glow" },
    { "quadratic",  "freeze" },
    { "quadratic",  "heat" },
    { "quadratic",  "glow-heat" },
    { "quadratic",  "heat-glow" },
    { "quadratic",  "reflect-freeze" },
    { "quadratic",  "freeze-reflect" },              // 121
};

inline constexpr int kChBlendCount = (int)(sizeof(kChBlend) / sizeof(kChBlend[0]));
