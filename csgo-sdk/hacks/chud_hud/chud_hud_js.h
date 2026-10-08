#pragma once


static const char* k_hide_fn = R"chud(
var cp = $.GetContextPanel();

function T(fn) { try { fn(); } catch(e) {} }

function nuke(p) {
    if (!p) return;
    T(function() { p.style.opacity = '0.0'; });
    T(function() { p.style.width = '0px'; });
    T(function() { p.style.height = '0px'; });
    T(function() { p.style.position = '-9999px -9999px 0px'; });
}

// exact undo of nuke(): null hands each prop back to the css (never '')
function unnuke(p, keepSize) {
    if (!p) return;
    T(function() { p.style.opacity = null; });
    T(function() { p.style.position = null; });
    if (keepSize) return;
    T(function() { p.style.width = null; });
    T(function() { p.style.height = null; });
}

// every always-on stock hud container. menus and in-world stuff are NOT here.
var CHUD_NUKE = ['HudTopCenter','HudTopLeft','HudTopRight','HudBottomCenter','HudBottomRight',
                 'HudLowerLeft','HudCenter','HudRadio','HudWinPanel','HudChat','HudVote',
                 'HudSpecPlayer','HudSpectator','HudSpectatorVignetting','HudFreezePanel',
                 'HudRetake','EndOfMatch','Versus','MapDraft','TabletPanoLayer',
                 'HudHealthArmor','HudWeaponPanel','HudWeaponSelection','HudDeathNotice',
                 'HudMoney','HudRadar','HudAlerts','HudHintText'];

function chudHideStock() {
    for (var i = 0; i < CHUD_NUKE.length; i++) {
        var p = null;
        try { p = cp.FindChildTraverse(CHUD_NUKE[i]); } catch(e) { p = null; }
        nuke(p);
    }

    // moved off screen but left alive so TimerText keeps ticking for our timer
    var tc = null;
    try { tc = cp.FindChildTraverse('HudTeamCounter'); } catch(e) { tc = null; }
    if (tc) {
        T(function() { tc.style.opacity = '0.0'; });
        T(function() { tc.style.position = '-9999px -9999px 0px'; });
    }
}

function chudUnhideStock() {
    for (var i = 0; i < CHUD_NUKE.length; i++) {
        var p = null;
        try { p = cp.FindChildTraverse(CHUD_NUKE[i]); } catch(e) { p = null; }
        unnuke(p, false);
    }

    var tc = null;
    try { tc = cp.FindChildTraverse('HudTeamCounter'); } catch(e) { tc = null; }
    unnuke(tc, true); // scaleform's height: 100% lives here, keep it
}
)chud";


static const char* k_install = R"chud(
${hideFn}

function P(id) { try { return cp.FindChildTraverse(id); } catch(e) { return null; } }

var FG    = '${fg}';
var BG    = '${bg}';
var SCALE = '${scale}';
var FONT  = '${font}';
var SEC   = '${sec}';
var WATERMARK = ${watermark};
var PAD   = ${pad};
var BLUR  = ${blur};
/* glyph nudge — see NUDGE note on labelStyle */
var NUDGE = '${nudge}';

/* dropshadow, or '' when off. TSH goes on labels, BSH on the hp/ammo bars only —
   never on a pill or a row, see the pillStyle note below */
var TSH = '${textShadow}';
var BSH = '${boxShadow}';

/* text-only scale. SCALE is a ui-scale on the whole tree and grows the boxes with
   the text; this one leaves every pill exactly where it is and only resizes the
   glyphs, which is what "small text on a big background" needs.
   sizes are rounded — a fractional font-size renders blurry */
var FS = ${fontScale} / 100.0;
function fs(px) { return 'font-size: ' + Math.round(px * FS) + 'px;'; }

/* eased, not linear */
var EASE = 'cubic-bezier(0.16, 1.00, 0.30, 1.00)';
var DUR  = '0.28s';

chudHideStock();

/* ---------- our root ----------
   REUSE it, never delete-and-recreate. DeleteAsync is asynchronous, so the old
   root was still alive when we made the new one — P('ChudRoot') then found the
   dying panel and every element got parented to a corpse, which is why changing
   scale made the whole hud vanish */
var root = P('ChudRoot');
if (root) {
    T(function() { root.RemoveAndDeleteChildren(); });
    T(function() { root.style.visibility = 'visible'; }); // k_uninstall collapses it
}
else {
    /* hittest false is NOT optional — this panel covers the whole screen, and
       panorama panels eat mouse input by default. with it on, every click went to
       our invisible root instead of the buy menu / scoreboard underneath */
    $.CreatePanel('Panel', cp, 'ChudRoot', {
        hittest: false,
        style: 'width: 100%; height: 100%; z-index: 100;'
    });
    root = P('ChudRoot');
}

/* root must not eat clicks. nothing under it takes input any more either — the chat
   line is a label fed from c++ — so every panel opts out individually in mk() */
T(function() { root.hittest = false; });
T(function() { root.hittestchildren = true; });

/* everything we create is decoration and must not take mouse input — the chat
   entry is the one exception and opts back in below */
function mk(type, parent, id, style, extra) {
    try {
        var props = extra ? extra : {};
        props.style = style;
        props.hittest = false;
        $.CreatePanel(type, parent, id, props);

        var p = P(id);
        if (p) T(function() { p.hittest = false; });
        return p;
    } catch(e) { return null; }
}

/* WATERMARK mode mirrors misc.cpp draw_watermark: #191919 fill + a #323232 1px
   inner border. panorama's `blur` is NOT applied anywhere any more — it blurs a
   panel's own contents rather than the backdrop, so it only ever made things worse */
/* NOTE: the blur backdrop is an axis-aligned rect and ignores border-radius, so its
   corners sit slightly proud of the rounded panel. rounding stays anyway — user
   asked for it kept. do not "fix" this by squaring the panels. */
/* NO shadow on a pill — the shadow draws outside the panel, which needs `overflow:
   noclip` on every host to not be cut off, and noclip changes what `text-overflow:
   shrink` measures. that moved and resized the weapon-name font. shadows are for
   the CONTENT only (labels, bars). do not put BSH back on pillStyle. */
var pillStyle = WATERMARK
    ? 'background-color: #191919ff; border: 1px solid #323232ff; border-radius: 4px;'
    : 'background-color: ' + BG + '; border-radius: 8px;';

/* NUDGE goes on every label, never on a pill. panorama centres the label's LINE
   BOX — ascent+descent straight out of the ttf — so a font whose metrics are
   lopsided sits off centre in EVERY element at once, and no per-panel align can
   fix that. panorama cannot measure a glyph bbox, so the offset is a manual knob.
   it is a transform, NOT a margin: transforms are visual only, so nothing reflows
   and edge-anchored labels keep their own margins */
var labelStyle = 'color: ' + FG + '; font-family: ' + FONT + '; font-weight: bold;' + NUDGE + TSH;

/* ---------- bottom left: ammo over health ---------- */
var bl = mk('Panel', root, 'ChudBottomLeft',
    'horizontal-align: left; vertical-align: bottom; margin-left: ' + PAD + 'px; margin-bottom: ' + PAD + 'px;' +
    'flow-children: down; ui-scale: ' + SCALE + '% ' + SCALE + '% 100%;');

/* opacity is animated when a weapon with ammo comes up — it used to pop in */
/* height and margin animate too, not just opacity. the blur rect is taken from the
   panel's LAYOUT, so a pill that only fades leaves its blur at full strength and
   then pops out the instant visibility collapses. shrinking the box makes the blur
   shrink with it. */
var ammoPill = mk('Panel', bl, 'ChudAmmoPill',
    pillStyle + ' width: 300px; height: 64px; margin-bottom: 8px; flow-children: right;' +
    /* margin is NOT transitionable in panorama — one bad token kills the whole
       declaration, so opacity+height stopped animating too. margin snaps, but
       only after the height collapse finishes (see hide path) */
    'opacity: 0.0; transition-property: opacity, height;' +
    'transition-duration: 0.14s;' +
    'transition-timing-function: ' + EASE + ';');
/* fixed-width slot + text-align, NOT auto width. a shrink-to-fit label anchored
   left makes '7/35' and '100/100' start at the same x but end wherever, so the
   block looks ragged as the count changes. reserving the space left of the bar
   and centring in it keeps the digits balanced no matter how many there are */
mk('Label', ammoPill, 'ChudAmmoText',
    labelStyle + fs(38) + ' width: 162px; text-align: center; vertical-align: center;');
var ammoBar = mk('Panel', ammoPill, 'ChudAmmoBar',
    'width: 120px; height: 10px; border-radius: 2px; background-color: ' + FG.substring(0, 7) + '33;' +
    'horizontal-align: right; vertical-align: center; margin-right: 18px;' + BSH);
mk('Panel', ammoBar, 'ChudAmmoFill',
    'width: 100%; height: 100%; border-radius: 2px; background-color: ' + FG + ';' +
    'horizontal-align: left; transition-property: width; transition-duration: ' + DUR + ';' +
    'transition-timing-function: ' + EASE + ';');

var hpPill = mk('Panel', bl, 'ChudHealthPill',
    pillStyle + ' width: 300px; height: 64px; flow-children: right;');
mk('Label', hpPill, 'ChudHealthText',
    labelStyle + fs(38) + ' width: 162px; text-align: center; vertical-align: center;');
var hpBar = mk('Panel', hpPill, 'ChudHealthBar',
    'width: 120px; height: 10px; border-radius: 2px; background-color: ' + FG.substring(0, 7) + '33;' +
    'horizontal-align: right; vertical-align: center; margin-right: 18px;' + BSH);
mk('Panel', hpBar, 'ChudHealthFill',
    'width: 100%; height: 100%; border-radius: 2px; background-color: ' + FG + ';' +
    'horizontal-align: left; transition-property: width; transition-duration: ' + DUR + ';' +
    'transition-timing-function: ' + EASE + ';');

/* ---------- top center: custom round timer ----------
   fully ours. the stock one could not be made to look right, so it lives off
   screen and we only borrow its text */
var timerPill = mk('Panel', root, 'ChudTimerPill',
    pillStyle + ' horizontal-align: center; vertical-align: top; margin-top: ' + PAD + 'px;' +
    'width: 140px; height: 58px; ui-scale: ' + SCALE + '% ' + SCALE + '% 100%;');
mk('Label', timerPill, 'ChudTimerText',
    labelStyle + fs(36) + ' horizontal-align: center; vertical-align: center;');

/* ---------- top right: killfeed ----------
   host = no ui-scale, so its margin-top is in root px: c++ pushes the watermark's bottom
   there (k_set_killfeed_top) and the feed sits PAD under the box instead of behind it */
var kfHost = mk('Panel', root, 'ChudKillfeedHost',
    'horizontal-align: right; vertical-align: top; flow-children: down;');
mk('Panel', kfHost, 'ChudKillfeed',
    'horizontal-align: right; vertical-align: top; margin-top: ' + PAD + 'px; margin-right: ' + PAD + 'px;' +
    'flow-children: down; ui-scale: ' + SCALE + '% ' + SCALE + '% 100%;');

/* ---------- bottom right: weapon list ---------- */
mk('Panel', root, 'ChudWeapons',
    'horizontal-align: right; vertical-align: bottom; margin-right: ' + PAD + 'px; margin-bottom: ' + PAD + 'px;' +
    'flow-children: down; ui-scale: ' + SCALE + '% ' + SCALE + '% 100%;');

/* ---------- preload ----------
   the headshot icon is fetched over http, so the first kill of the session used to
   pop in late while it downloaded. warm it here with an offscreen instance, and
   keep that instance ALIVE for the whole session so it stays in panorama's cache.
   the url lives on a global the killfeed script reuses. */
CHUD_HS_ICON = 'https://raw.githubusercontent.com/abandonedpools/scaleform/3e4c1f244351844a6236d952356ea087f59ad29e/p_scaleform/materials/panorama/images/hud/deathnotice/icon_headshot.svg';

var preload = mk('Panel', root, 'ChudPreload',
    'width: 1px; height: 1px; opacity: 0.0; position: -9998px -9998px 0px;');

if (preload) {
    T(function() {
        $.CreatePanel('Image', preload, 'ChudPreloadHs', {
            src: CHUD_HS_ICON,
            style: 'width: 24px; height: 24px;'
        });
    });
}

/* ---------- top left: chat ---------- */
var chat = mk('Panel', root, 'ChudChat',
    'horizontal-align: left; vertical-align: top; margin-left: ' + PAD + 'px; margin-top: ' + PAD + 'px;' +
    'width: 620px; flow-children: down; overflow: noclip;' +
    'ui-scale: ' + SCALE + '% ' + SCALE + '% 100%;');

/* noclip: a chat line is a fixed 24px row, so nudging the glyphs pushes them past
   their own box and the row crops them. the nudge is a visual correction, so it must
   not be clipped — the row keeps its 24px for layout, the text just spills */
mk('Panel', chat, 'ChudChatLines', 'width: 100%; flow-children: down; overflow: noclip;');

/* a LABEL, never a TextEntry, and it is never focused. focusing a panorama
   TextEntry puts the ENGINE into text-input mode and there is no js way back out:
   SetFocus takes no argument (SetFocus(false) does nothing) and neither this
   container nor the hud root is focusable, so focus could not be moved off the box
   — after sending a message every key still went into it, escape did nothing and
   only restarting the game freed the keyboard. c++ reads the keys off the window
   proc now (chat_on_key) and just pushes the text here. */
var entry = mk('Label', chat, 'ChudChatEntry',
    pillStyle + ' width: 100%; height: 36px; margin-top: 6px; padding: 5px 10px 5px 10px;' +
    'color: ' + SEC + '; font-family: ' + FONT + '; ' + fs(18) + TSH + ' visibility: collapse;');

)chud";

static const char* k_install_2 = R"chud(
/* ---------- backdrop blur: the game's own ----------
   csgo already does this for the scoreboard and buy menu. the hud layout contains

     <CSGOHudBlurTarget id="HudBlur" class="HudBlur"
                        blurrects="Scoreboard EndOfMatch BuyMenu ...">

   with `blur: fastgaussian( 2, 2, 4 )` in the HudBlur class. it blurs what is
   behind it and composites the result into the rect of every panel named in
   blurrects — and `AddBlurPanel( panel )` adds one at RUNTIME, which is how the
   menus register their own. so our pills can just ask to be in that list.

   this is why the earlier attempts failed and this does not:
     style.blur           blurs a panel's OWN contents
     style.backdropFilter does not exist in this build
     imgui / raw d3d9     draws after panorama, or fights the material system
   NOTE: the blur lands UNDER the panel's own background, so a fully opaque bg
   colour hides it completely — the alpha has to come down to see anything. */
function chudBlur(p) {
    if (!p || !BLUR) return;

    var target = P('HudBlur');
    if (target) T(function() { target.AddBlurPanel(p); });
}

/* ---------- live colour repaint ----------
   colours used to ride the style hash, which meant a full tree rebuild behind a
   0.2s debounce — and the hash only sampled ONE channel per colour, so dragging
   green or blue changed nothing at all. this restyles the existing panels in
   place, so c++ can push it the same frame the picker moves. */
function chudApplyColors(fg, bg, sec, wm) {
    FG = fg; BG = bg; SEC = sec; WATERMARK = wm;

    var fill = wm ? '#191919ff' : bg;

    function paintBg(id) {
        var p = P(id);
        if (!p) return;
        T(function() { p.style.backgroundColor = fill; });
    }

    function paintFg(id) {
        var p = P(id);
        if (p) T(function() { p.style.color = fg; });
    }

    paintBg('ChudAmmoPill');
    paintBg('ChudHealthPill');
    paintBg('ChudTimerPill');
    paintBg('ChudChatEntry');

    paintFg('ChudAmmoText');
    paintFg('ChudHealthText');
    paintFg('ChudTimerText');

    // chat input text follows the secondary colour, like the chat lines
    var ce = P('ChudChatEntry');
    if (ce) T(function() { ce.style.color = sec; });

    // bar tracks are the accent at 20% — alpha is replaced, not appended
    var track = fg.substring(0, 7) + '33';
    var ids = ['ChudAmmoBar', 'ChudHealthBar'];
    for (var t = 0; t < ids.length; t++) {
        (function(p) {
            if (p) T(function() { p.style.backgroundColor = track; });
        })(P(ids[t]));
    }

    var fills = ['ChudAmmoFill', 'ChudHealthFill'];
    for (var f = 0; f < fills.length; f++) {
        (function(p) {
            if (p) T(function() { p.style.backgroundColor = fg; });
        })(P(fills[f]));
    }

    /* rows are created on the fly, so they cannot be reached by id — walk the
       hosts instead. weapon rows: child 0 is the icon row, child 1 the name,
       child 2 the slot number (secondary). */
    var weapons = P('ChudWeapons');
    if (weapons) {
        var wc = 0;
        T(function() { wc = weapons.GetChildCount(); });

        for (var w = 0; w < wc; w++) {
            (function(row) {
                if (!row) return;
                T(function() { row.style.backgroundColor = fill; });

                var rc = 0;
                T(function() { rc = row.GetChildCount(); });

                for (var i = 0; i < rc; i++) {
                    (function(child, idx) {
                        if (!child) return;

                        if (idx === 0) {
                            /* icon row -> one CELL per grenade -> the image inside it,
                               so the tint has to go two levels down, not one */
                            var ic = 0;
                            T(function() { ic = child.GetChildCount(); });

                            for (var j = 0; j < ic; j++) {
                                (function(cell) {
                                    if (!cell) return;
                                    T(function() { cell.style.washColor = fg; });

                                    var cc = 0;
                                    T(function() { cc = cell.GetChildCount(); });
                                    for (var k = 0; k < cc; k++) {
                                        (function(img) {
                                            if (img) T(function() { img.style.washColor = fg; });
                                        })(cell.GetChild(k));
                                    }
                                })(child.GetChild(j));
                            }
                            return;
                        }

                        var col = (idx === 1) ? fg : sec;
                        T(function() { child.style.color = col; });
                    })(row.GetChild(i), i);
                }
            })(weapons.GetChild(w));
        }
    }

    // killfeed rows are all accent — names, weapon icon, headshot icon
    var kf = P('ChudKillfeed');
    if (kf) {
        var kc = 0;
        T(function() { kc = kf.GetChildCount(); });

        for (var k = 0; k < kc; k++) {
            (function(row) {
                if (!row) return;
                T(function() { row.style.backgroundColor = fill; });

                var rc = 0;
                T(function() { rc = row.GetChildCount(); });

                for (var i = 0; i < rc; i++) {
                    (function(child) {
                        if (!child) return;
                        T(function() { child.style.color = fg; });
                        T(function() { child.style.washColor = fg; });

                        // the weapon icon sits one level down, inside its cell
                        var gc = 0;
                        T(function() { gc = child.GetChildCount(); });
                        for (var j = 0; j < gc; j++) {
                            (function(img) {
                                if (img) T(function() { img.style.washColor = fg; });
                            })(child.GetChild(j));
                        }
                    })(row.GetChild(i));
                }
            })(kf.GetChild(k));
        }
    }

    var lines = P('ChudChatLines');
    if (lines) {
        var lc = 0;
        T(function() { lc = lines.GetChildCount(); });

        for (var l = 0; l < lc; l++) {
            (function(line) {
                if (line) T(function() { line.style.color = sec; });
            })(lines.GetChild(l));
        }
    }

    /* spectator rows are two-toned (accent for people watching YOU, secondary for
       everyone else), so they cannot be painted by the loops above — see k_install_3 */
    try { chudPaintSpectators(fg, sec, fill); } catch(e) {}
}

/* register the fixed pills with the game's blur target. rows that are created
   later (killfeed, weapons) call chudBlur themselves as they are made. */
chudBlur(ammoPill);
chudBlur(hpPill);
chudBlur(timerPill);
/* NOT the chat entry. AddBlurPanel is one way — the game has no RemoveBlurPanel —
   so a registered entry stays in the blur list even when collapsed, and it sits
   BELOW ChudChatLines: every new chat line reflows its rect, HudBlur composites
   the stale strip, and you get horizontal smears of the world under the chat.
   the entry keeps its solid pill bg instead. do not put chudBlur(entry) back. */
)chud";

static const char* k_install_3 = R"chud(
/* ---------- centre left: spectator list ----------
   same data and the same animation rules as the imgui list in misc.cpp
   (draw_spectator_list): rows are keyed by the WATCHER so a spec-target switch
   only swaps the text and the row keeps its place, rows slide in from the left
   while they fade and grow, a row that leaves collapses instead of popping, and
   the box width EASES to the widest live row instead of snapping.

   the two colours are the chud scheme, not the misc one: someone watching YOU is
   the accent, everyone else is the secondary. */

/* rebuilt with the tree — the maps must not outlive the panels they key */
CHUD_SPEC_DYING = {};
CHUD_SPEC_LOCAL = {};
CHUD_SPEC_AVATARS = ${specAvatars};

/* EVERY number here is lifted straight from draw_spectator_list in misc.cpp, so
   the two lists lay out identically:

     padding_x 8, bottom_padding 2      window padding
     background_height 25, list_start_y 30    title bar, then a 5px gap
     avatar_size 14, rounded 6          ImGui::AddImageRounded
     ItemSpacing 8,8 (render.cpp:214)   gap after the avatar, and row_height's slack
     row_height = max(avatar, text) + ItemSpacing.y
     text 11px verdana bold             font_name_verdana_bd_11 / the default font
     slide_distance 10, speed 6 -> 0.17s, window fade 2*2 -> 0.25s

   the only thing that is ours is the font FAMILY and the font-scale knob, so a
   row still tracks the hud's own text size rather than being pinned to 11px. */
CHUD_SPEC_PADX    = 8;
CHUD_SPEC_BOT     = 2;
CHUD_SPEC_GAP     = 5;
CHUD_SPEC_SPACING = 8;
CHUD_SPEC_SLIDE   = 10;

/* px cap on a row's TEXT (menu slider). valve's own idiom (navbar-btn in
   code.pbin): max-width + text-overflow: ellipsis + white-space: nowrap, so an
   overlong name ends in "..." instead of the old hard cut at 24 chars. the probe
   carries the same cap, so the measured box width can never exceed it either. */
CHUD_SPEC_MAXW = ${specMaxW};

/* the text is the WEAPON LIST's text — 17px, bold, same family and nudge — so the
   list reads as part of the same hud. every box below is derived from it, which is
   what keeps the imgui proportions while the font scale still moves everything:

     imgui  text 11 -> avatar 14, title bar 25
     here   text 17 -> avatar 18, title bar 31   (same ratios, one source number) */
CHUD_SPEC_TEXTH  = Math.round(17 * FS);
CHUD_SPEC_AVA    = Math.round(CHUD_SPEC_TEXTH * 14 / 11);
/* title bar swallows the old 5px row gap too (14 + GAP), and the rows now sit flush
   under it — a centred title inside one taller bar splits that space EVENLY, so the
   gap above the title matches the gap below it. two separate insets could not. */
CHUD_SPEC_TITLEH = CHUD_SPEC_TEXTH + 14 + CHUD_SPEC_GAP;

/* imgui spaces rows by the FONT SIZE plus ItemSpacing.y, not by the line box —
   that is what ImGui::GetTextLineHeight returns */
CHUD_SPEC_ROWH = Math.max(CHUD_SPEC_AVATARS ? CHUD_SPEC_AVA : 0, CHUD_SPEC_TEXTH) + CHUD_SPEC_SPACING;

// longest live row text, fed to the probe so the width has an unclamped reference
CHUD_SPEC_LONGEST = '';

var specHost = mk('Panel', root, 'ChudSpectators',
    'horizontal-align: left; vertical-align: center; margin-left: ' + PAD + 'px;' +
    'flow-children: down; overflow: noclip; opacity: 0.0;' +
    // 0.25s = the imgui window alpha (speed 2 * scaling 2 -> 4/sec)
    'transition-property: opacity; transition-duration: 0.25s;' +
    'transition-timing-function: ' + EASE + ';' +
    'ui-scale: ' + SCALE + '% ' + SCALE + '% 100%;');

/* EXPLICIT width, driven by chudSpecMeasure — `fit-children` sizes correctly but
   snaps, and panorama cannot transition an implicit width. the measured value is
   assigned to a real px width, which the transition below then eases into, so the
   box grows and shrinks with the names exactly like the imgui window lerp. */
var specPill = mk('Panel', specHost, 'ChudSpecPill',
    pillStyle + ' width: 0px;' +
    // top padding is 0 — the 25px title bar IS the top inset, exactly like imgui
    'padding: 0px ' + CHUD_SPEC_PADX + 'px ' + CHUD_SPEC_BOT + 'px ' + CHUD_SPEC_PADX + 'px;' +
    'flow-children: down; overflow: noclip;' +
    // 0.18s ~ the imgui width lerp (DeltaTime * 12)
    'transition-property: width; transition-duration: 0.18s;' +
    'transition-timing-function: ' + EASE + ';');

/* fixed-height bar, title centred inside it — the imgui list draws its title into
   a 25px band and starts the rows at y 30, so the gap is the 5px below.

   the bar is pulled OUT of the pill's horizontal padding: a child starts at the
   padding edge, so a bar that only spans the CONTENT box centres its title over
   the content box, which sits PADX right of the pill's real centre. the negative
   left margin puts the bar back on the pill's left edge and chudSpecMeasure gives
   it the pill's OUTER width in px, so the title centres over the whole panel.
   width is set in px, never %, because % vs padding is the ambiguity being fixed. */
var specTitleBar = mk('Panel', specPill, 'ChudSpecTitleBar',
    'width: 100%; margin-left: -' + CHUD_SPEC_PADX + 'px;' +
    'height: ' + CHUD_SPEC_TITLEH + 'px; overflow: noclip;' +
    // same lerp as the pill, so title and box glide to a new width together
    'transition-property: width; transition-duration: 0.18s;' +
    'transition-timing-function: ' + EASE + ';');

/* full-width label + text-align, NOT horizontal-align: center — panorama does not
   re-run a child's align when the pill's width EASES to a new value, so the title
   froze at its old centre when the max-width slider moved. a 100% label re-lays its
   text every frame the width changes, so the glyphs track the centre for free. */
mk('Label', specTitleBar, 'ChudSpecTitle',
    labelStyle + fs(17) + ' width: 100%; text-align: center; vertical-align: center;',
    { text: 'spectators' });

mk('Panel', specPill, 'ChudSpecRows',
    // no margin-top — the gap lives inside CHUD_SPEC_TITLEH so it splits both sides
    'width: fit-children;' +
    'flow-children: down; overflow: noclip;');

/* offscreen probe in the SAME font, size and weight as a row, parented to the ROOT
   so nothing can constrain it, and left at its natural size. it holds the longest
   row text as an UNCLAMPED width reference, so the box can still grow even if a
   parent ever squeezes the row host we measure first. */
mk('Label', root, 'ChudSpecProbe',
    'color: #00000000; font-family: ' + FONT + '; font-weight: bold;' + fs(17) +
    // same cap as a real row label, so the probe never reports a width past it
    ' max-width: ' + CHUD_SPEC_MAXW + 'px; white-space: nowrap;' +
    ' position: -9997px -9997px 0px;',
    { text: 'Wg' });

/* the title is width: 100% now (see the note on it), so its layout width IS the
   pill's — measuring it would feed the pill its own width back and the box could
   never shrink. this probe carries the title text at its natural size instead. */
mk('Label', root, 'ChudSpecTitleProbe',
    'color: #00000000; font-family: ' + FONT + '; font-weight: bold;' + fs(17) +
    ' position: -9997px -9967px 0px;',
    { text: 'spectators' });

chudBlur(specPill);

function chudSpecColor(local) { return local ? FG : SEC; }

/* valve's own idiom (code.pbin, _SetWidth): actuallayoutwidth is post-layout and
   already scaled, so it has to be divided by the panel's ui scale before it can go
   back out as a style width. */
function chudSpecPx(p, vertical) {
    var out = 0;

    T(function() {
        var s = vertical ? p.actualuiscale_y : p.actualuiscale_x;
        if (!s || s <= 0) s = 1;

        out = (vertical ? p.actuallayoutheight : p.actuallayoutwidth) / s;
    });

    return out;
}

/* width follows the widest LIVE row, title included, and is re-read a frame after
   any change — panorama has no synchronous measure, the layout pass has to run
   first. snap is for the frame the list opens: easing from 0 is the "grows out of
   nothing" pop the imgui list avoids with its snap_width latch. */
function chudSpecMeasure(snap) {
    var pill  = P('ChudSpecPill');
    var rows  = P('ChudSpecRows');
    // the probe, NOT the label — the label is 100% wide and would echo the pill
    var title = P('ChudSpecTitleProbe');
    if (!pill || !rows) return;

    var rowsW = chudSpecPx(rows, false);

    /* the probe is carrying the longest row text (set by the push) and lives outside
       the pill, so it measures the same glyphs with nothing able to squeeze them.
       an avatar is not text, so its column is added back on here. */
    var probe = P('ChudSpecProbe');
    if (probe && CHUD_SPEC_LONGEST) {
        var pw = chudSpecPx(probe, false);
        if (CHUD_SPEC_AVATARS) pw += CHUD_SPEC_AVA + CHUD_SPEC_SPACING;

        rowsW = Math.max(rowsW, pw);
    }

    /* imgui: max( title + 40, widest_row + padding_x * 2 ) — the title gets its own
       slack so a short list still reads as a titled box */
    var want = Math.ceil(rowsW) + CHUD_SPEC_PADX * 2;
    if (title) want = Math.max(want, Math.ceil(chudSpecPx(title, false)) + 40);

    if (!want || want < 1) return;

    /* the title bar carries the pill's OUTER width (see its creation note) — it is
       margin-pulled to the pill's left edge, so this width makes `text-align:
       center` centre the title over the whole panel and not over the padding box */
    var bar = P('ChudSpecTitleBar');

    if (snap) {
        /* kill the transition for exactly one assignment, then hand it back — a
           transition-duration of 0 would otherwise stick and every later resize
           would snap too */
        T(function() { pill.style.transitionDuration = '0s'; });
        T(function() { pill.style.width = want + 'px'; });

        if (bar) {
            T(function() { bar.style.transitionDuration = '0s'; });
            T(function() { bar.style.width = want + 'px'; });
        }

        $.Schedule(0.02, function() {
            try { pill.style.transitionDuration = '0.18s'; } catch(err) {}
            try { if (bar) bar.style.transitionDuration = '0.18s'; } catch(err) {}
        });

        return;
    }

    T(function() { pill.style.width = want + 'px'; });
    if (bar) T(function() { bar.style.width = want + 'px'; });
}
)chud";

static const char* k_install_4 = R"chud(
/* list is [{ i: watcher index, t: 'watcher -> target', l: watching us,
             s: '<steamid64>' ('0' for bots), d: 'CT' | 'TERRORIST' }] */
function chudSetSpectators(list) {
    var host = P('ChudSpecRows');
    var box  = P('ChudSpectators');
    if (!host || !box) return;

    var had = 0;
    T(function() { had = host.GetChildCount(); });

    /* longest by character count picks the CANDIDATE, the probe then measures the
       real glyphs — so the width still comes from a measurement, never from a
       characters-times-pixels guess */
    var longest = '';
    for (var q = 0; q < list.length; q++)
        if (list[q].t.length > longest.length) longest = list[q].t;

    if (longest !== CHUD_SPEC_LONGEST) {
        CHUD_SPEC_LONGEST = longest;

        var probeLbl = P('ChudSpecProbe');
        if (probeLbl && longest) T(function() { probeLbl.text = longest; });
    }

    var seen = {};

    for (var n = 0; n < list.length; n++) {
        (function(e) {
            var id  = 'ChudSpec' + e.i;
            var tid = 'ChudSpecTxt' + e.i;

            seen[id] = true;
            CHUD_SPEC_LOCAL[id] = e.l;

            var row = P(id);

            if (row) {
                /* a watcher that comes back mid-fade REVIVES its own row rather than
                   racing a pending delete with a new panel of the same id — this is
                   the js version of the imgui progress simply reversing */
                if (CHUD_SPEC_DYING[id]) {
                    CHUD_SPEC_DYING[id] = false;

                    T(function() {
                        row.style.transform = 'translateX(0px)';
                        row.style.opacity   = '1.0';
                        row.style.height    = CHUD_SPEC_ROWH + 'px';
                    });
                }

                var lbl = P(tid);
                if (lbl) {
                    T(function() { lbl.text = e.t; });
                    T(function() { lbl.style.color = chudSpecColor(e.l); });
                }

                return;
            }

            /* the row is a flow container even with avatars off — a bare label would
               have to be swapped for a panel the moment they are turned on */
            row = $.CreatePanel('Panel', host, id, {
                hittest: false,
                style: 'flow-children: right; height: 0px; margin-top: 0px; opacity: 0.0;' +
                       'overflow: noclip; transform: translateX(-' + CHUD_SPEC_SLIDE + 'px);' +
                       'transition-property: transform, opacity, height;' +
                       // 0.17s = imgui animation_speed 6 (progress 0 -> 1)
                       'transition-duration: 0.17s;' +
                       'transition-timing-function: ' + EASE + ';'
            });

            if (CHUD_SPEC_AVATARS) {
                /* CSGOAvatarImage is the game's own steam avatar panel (the scoreboard
                   and team counter use it): assigning .steamid fetches the real avatar,
                   and bots have no valid xuid so they get valve's team placeholder,
                   which is exactly what the stock code does.

                   IT IS AN IMAGE PANEL, not a plain one — valve always gives it a
                   `scaling` (hud_team_equipment: scaling + textureheight="32"). without
                   one it lays out at the RAW TEXTURE size, so a 184px steam avatar was
                   drawn cropped to our box, and every player's texture being a different
                   size is what made the name column start at a different x per row.

                   the fixed-size CELL is the second half of that fix: the same pattern
                   the killfeed icons use here. whatever the image does inside it, the
                   cell is what the flow measures, so the text can never be pushed. */
                var avaCell = $.CreatePanel('Panel', row, 'ChudSpecAvaCell' + e.i, {
                    hittest: false,
                    style: 'width: ' + CHUD_SPEC_AVA + 'px; height: ' + CHUD_SPEC_AVA + 'px;' +
                           'margin-right: ' + CHUD_SPEC_SPACING + 'px;' +
                           'vertical-align: center; overflow: noclip;'
                });

                var ava = $.CreatePanel('CSGOAvatarImage', avaCell, 'ChudSpecAva' + e.i, {
                    hittest: false,
                    scaling: 'stretch-to-fit-preserve-aspect',
                    style: 'width: 100%; height: 100%; border-radius: 6px;' +
                           'horizontal-align: center; vertical-align: center;'
                });

                if (ava) {
                    if (e.s && e.s !== '0')
                        T(function() { ava.steamid = e.s; });
                    else
                        T(function() { ava.SetDefaultImage('file://{images}/icons/scoreboard/avatar-' + e.d + '.png'); });
                }
            }

            /* natural height + vertical-align: center. pinning a height here would
               centre the BOX and leave the glyphs riding low inside it, since a line
               box is always taller than the font size */
            $.CreatePanel('Label', row, tid, {
                text: e.t,
                hittest: false,
                style: 'color: ' + chudSpecColor(e.l) + '; font-family: ' + FONT + '; font-weight: bold;' +
                       fs(17) + TSH + NUDGE + ' vertical-align: center; overflow: noclip;' +
                       'max-width: ' + CHUD_SPEC_MAXW + 'px; text-overflow: ellipsis; white-space: nowrap;'
            });

            $.Schedule(0.01, function() {
                try {
                    row.style.transform = 'translateX(0px)';
                    row.style.opacity   = '1.0';
                    row.style.height    = CHUD_SPEC_ROWH + 'px';
                } catch(err) {}
            });
        })(list[n]);
    }

    // anything missing from this push is leaving — collapse it, then delete
    var count = 0;
    T(function() { count = host.GetChildCount(); });

    for (var c = 0; c < count; c++) {
        (function(row) {
            if (!row) return;

            var id = '';
            T(function() { id = row.id; });
            if (seen[id] || CHUD_SPEC_DYING[id]) return;

            CHUD_SPEC_DYING[id] = true;

            T(function() {
                row.style.opacity   = '0.0';
                row.style.height    = '0px';
                row.style.transform = 'translateX(-14px)';
            });

            // guarded: a revived row cleared the flag and must survive
            $.Schedule(0.22, function() {
                try {
                    if (!CHUD_SPEC_DYING[id]) return;

                    CHUD_SPEC_DYING[id] = false;
                    CHUD_SPEC_LOCAL[id] = false;
                    row.DeleteAsync(0.0);
                } catch(err) {}
            });
        })(host.GetChild(c));
    }

    // the whole box fades with the list, exactly like the imgui window alpha
    T(function() { box.style.opacity = list.length ? '1.0' : '0.0'; });

    /* measure AFTER the layout pass this push causes, never during it. twice: the
       first catches the new text, the second catches a row whose height was still
       animating (and, on the very first push, the probe). snap only when the list
       was empty, so the box opens at its own width. */
    var snap = (had === 0 && list.length > 0);

    $.Schedule(0.03, function() {
        try { chudSpecMeasure(snap); } catch(err) {}
    });
    $.Schedule(0.24, function() {
        try { chudSpecMeasure(false); } catch(err) {}
    });
}

/* the row colour depends on WHO is being watched, which only c++ knows, so the
   live repaint replays the flag from the last push instead of guessing */
function chudPaintSpectators(fg, sec, fill) {
    var pill = P('ChudSpecPill');
    if (pill) T(function() { pill.style.backgroundColor = fill; });

    var title = P('ChudSpecTitle');
    if (title) T(function() { title.style.color = fg; });

    var host = P('ChudSpecRows');
    if (!host) return;

    var count = 0;
    T(function() { count = host.GetChildCount(); });

    for (var i = 0; i < count; i++) {
        (function(row) {
            if (!row) return;

            var id = '';
            T(function() { id = row.id; });

            var col = CHUD_SPEC_LOCAL[id] ? fg : sec;

            // the text is a child now (the avatar is its sibling), so walk one down
            var rc = 0;
            T(function() { rc = row.GetChildCount(); });

            for (var j = 0; j < rc; j++) {
                (function(child) {
                    if (child) T(function() { child.style.color = col; });
                })(row.GetChild(j));
            }
        })(host.GetChild(i));
    }
}
)chud";

static const char* k_set_spectators = R"chud(
try { chudSetSpectators(${list}); } catch(e) {}
)chud";

static const char* k_set_colors = R"chud(
try { chudApplyColors('${fg}', '${bg}', '${sec}', ${watermark}); } catch(e) {}
)chud";

static const char* k_hide_run = R"chud(
${hideFn}
chudHideStock();
)chud";

static const char* k_uninstall = R"chud(
${hideFn}
var root = null;
try { root = cp.FindChildTraverse('ChudRoot'); } catch(e) { root = null; }
if (root) {
    T(function() { root.RemoveAndDeleteChildren(); });
    T(function() { root.style.visibility = 'collapse'; });
}
chudUnhideStock();
)chud";


static const char* k_set_health = R"chud(
try {
    var t = $.GetContextPanel().FindChildTraverse('ChudHealthText');
    var f = $.GetContextPanel().FindChildTraverse('ChudHealthFill');
    if (t) t.text = '${hp}/100';
    if (f) f.style.width = '${hpFrac}%';
} catch(e) {}
)chud";

static const char* k_set_ammo = R"chud(
try {
    var cp = $.GetContextPanel();
    var pill = cp.FindChildTraverse('ChudAmmoPill');
    var t = cp.FindChildTraverse('ChudAmmoText');
    var f = cp.FindChildTraverse('ChudAmmoFill');

    /* fade instead of pop. visible is set first so the opacity transition has
       something to animate, and the collapse is deferred until the fade ends */
    /* the deferred collapse is generation-guarded. switching gun -> knife -> gun
       quickly left the knife's pending collapse in flight, and it fired while the
       gun was already up, hiding the ammo until the next change */
    if (pill) {
        var want = ${hasAmmo} ? 'show' : 'hide';
        try { pill.SetAttributeString('chudwant', want); } catch(e) {}

        if (${hasAmmo}) {
            pill.style.visibility = 'visible';
            $.Schedule(0.01, function() {
                try {
                    if (pill.GetAttributeString('chudwant', '') !== 'show') return;
                    pill.style.opacity = '1.0';
                    pill.style.height = '64px';
                    pill.style.marginBottom = '8px';
                } catch(e) {}
            });
        }
        else {
            /* collapse the BOX as well as the alpha — the blur backdrop tracks the
               panel's rect and cannot fade on its own */
            pill.style.opacity = '0.0';
            pill.style.height = '0px';

            $.Schedule(0.16, function() {
                try {
                    if (pill.GetAttributeString('chudwant', '') !== 'hide') return;
                    /* margin cannot ease, so zero it once the box is already gone */
                    pill.style.marginBottom = '0px';
                    pill.style.visibility = 'collapse';
                } catch(e) {}
            });
        }
    }

    if (t) t.text = '${ammo}';
    if (f) f.style.width = '${ammoFrac}%';
} catch(e) {}
)chud";

static const char* k_set_timer = R"chud(
try {
    var cp = $.GetContextPanel();
    var dst = cp.FindChildTraverse('ChudTimerText');
    var pill = cp.FindChildTraverse('ChudTimerPill');
    if (dst) {
        var text = '';
        if (${bombOverride}) {
            text = '${bombText}';
            dst.style.color = '${bombColor}';
        }
        else {
            var src = cp.FindChildTraverse('TimerText');
            if (src) text = src.text || '';
            if (!text) {
                var warmup = false;
                try { warmup = FriendsListAPI.IsGameInWarmup(); } catch(e) {}
                if (warmup) text = 'warmup';
            }
            dst.style.color = '${fg}';
        }
        dst.text = text;
        if (pill) pill.style.visibility = text ? 'visible' : 'collapse';
    }
} catch(e) {}
)chud";

static const char* k_set_weapons = R"chud(
try {
    var cp = $.GetContextPanel();
    var host = cp.FindChildTraverse('ChudWeapons');
    if (host) {
        var rows = ${rows};

        /* reconcile rather than RemoveAndDeleteChildren — wiping the list made
           picking up one weapon re-animate every row. keyed by slot. */
        var wanted = {};
        for (var w = 0; w < rows.length; w++) wanted['ChudWeapSlot' + rows[w].slot] = true;

        for (var c = host.GetChildCount() - 1; c >= 0; c--) {
            var child = host.GetChild(c);
            if (!child) continue;
            if (!wanted[child.id]) {
                (function(dying) {
                    try {
                        dying.style.opacity = '0.0';
                        dying.style.height = '0px';
                        /* margin snaps — do it after the collapse, row is gone by then */
                        $.Schedule(0.24, function() { try { dying.style.marginTop = '0px'; } catch(e) {} });
                        dying.DeleteAsync(0.28);
                    } catch(e) {}
                })(child);
            }
        }

        for (var i = 0; i < rows.length; i++) {
            var rowId = 'ChudWeapSlot' + rows[i].slot;
            var rowKey = rows[i].icons.join('|') + '#' + rows[i].name;
            var icons = rows[i].icons;

            var row = cp.FindChildTraverse(rowId);
            var isNew = false;

            if (!row) {
                isNew = true;
                $.CreatePanel('Panel', host, rowId, {
                    hittest: false,
                    style: '${panelBg}' +
                           'width: 240px; height: 0px; margin-top: 0px;' +
                           'horizontal-align: right; opacity: 0.0;' +
                           'transition-property: opacity, height;' +
                           'transition-duration: 0.24s;' +
                           'transition-timing-function: cubic-bezier(0.16, 1.00, 0.30, 1.00);'
                });
                row = cp.FindChildTraverse(rowId);
                try { chudBlur(row); } catch(e) {}
            }

            if (!row) continue;

            // contents rebuild only when they actually changed (grenade counts)
            var prevKey = '';
            try { prevKey = row.GetAttributeString('chudkey', ''); } catch(e) {}

            if (!isNew && prevKey === rowKey) {
                try { row.style.opacity = rows[i].active ? '1.0' : '0.38'; } catch(e) {}
                continue;
            }

            try { row.SetAttributeString('chudkey', rowKey); } catch(e) {}
            try { row.RemoveAndDeleteChildren(); } catch(e) {}

            /* grenades share ONE row, icons side by side.

               EVERY icon needs its own fixed-width CELL. an Image that sets only a
               height carries no width into the flow, so all the grenades stacked at
               the same x and read as one blown-up blob. the killfeed has always done
               it this way (iconCell + image inside) — same pattern here.

               budget is 216px: the 240px row minus the name/slot gutters. the row
               width is computed from the count rather than fit-children, so
               horizontal-align can actually centre it, and it never grows past the
               row and off the right edge of the screen. */
            var nIcons = icons.length;
            var cellW  = nIcons > 1 ? Math.max(18, Math.min(48, Math.floor(216 / nIcons))) : 48;
            var iconH  = nIcons > 1 ? Math.max(16, Math.min(34, cellW - 6)) : 42;
            var rowW   = Math.min(216, nIcons * cellW);

            var iconRow = $.CreatePanel('Panel', row, '', {
                hittest: false,
                style: 'width: ' + rowW + 'px; height: 44px;' +
                       'flow-children: right; overflow: noclip;' +
                       'horizontal-align: center; vertical-align: center;'
            });

            for (var g = 0; g < icons.length; g++) {
                /* the cell owns the WIDTH, the image owns the HEIGHT. noclip lets a
                   wide svg spill past its cell while the cell still holds the
                   spacing — forcing width on the image itself squashes it. */
                var iconCell = $.CreatePanel('Panel', iconRow, '', {
                    hittest: false,
                    style: 'width: ' + cellW + 'px; height: 44px;' +
                           'vertical-align: center; overflow: noclip;'
                });

                /* tall/narrow svgs (c4, defuser) read tiny at the same height a wide
                   rifle uses — the aspect eats all the width. give them more height
                   so they read at the same visual weight. noclip absorbs the spill. */
                var tall = (icons[g] === 'c4' || icons[g] === 'defuser');
                var thisH = tall ? Math.round(iconH * 1.35) : iconH;

                $.CreatePanel('Image', iconCell, '', {
                    src: 'file://{images}/icons/equipment/' + icons[g] + '.svg',
                    scaling: 'stretch-to-fit-y-preserve-aspect',
                    hittest: false,
                    // scaleX(-1) faces them left, per the mockup
                    style: 'height: ' + thisH + 'px;' +
                           'vertical-align: center; horizontal-align: center;' +
                           'wash-color: ${fg}; transform: scaleX(-1);' + '${imgShadow}'
                });
            }

            $.CreatePanel('Label', row, '', {
                text: rows[i].name,
                hittest: false,
                style: 'color: ${fg}; font-family: ${font}; font-weight: bold; font-size: ${fs17}px;' +
                       'horizontal-align: left; vertical-align: bottom;' +
                       'margin-left: 12px; margin-bottom: 6px; text-overflow: shrink;' +
                       '${nudge}${textShadow}'
            });

            $.CreatePanel('Label', row, '', {
                text: rows[i].slot,
                hittest: false,
                style: 'color: ${sec}; font-family: ${font}; font-size: ${fs17}px;' +
                       'horizontal-align: right; vertical-align: bottom;' +
                       'margin-right: 12px; margin-bottom: 6px;' +
                       '${nudge}${textShadow}'
            });

            (function(r, isActive, animate) {
                if (!animate) {
                    try { r.style.opacity = isActive ? '1.0' : '0.38'; } catch(e) {}
                    return;
                }
                $.Schedule(0.01, function() {
                    try {
                        r.style.height = '76px';
                        r.style.marginTop = '6px';
                        r.style.opacity = isActive ? '1.0' : '0.38';
                    } catch(e) {}
                });
            })(row, rows[i].active, isNew);
        }

        /* reconcile APPENDS, so a freshly picked up awp landed at the bottom.
           re-sort into slot order after the pass */
        for (var o = 1; o < rows.length; o++) {
            var prev = cp.FindChildTraverse('ChudWeapSlot' + rows[o - 1].slot);
            var curr = cp.FindChildTraverse('ChudWeapSlot' + rows[o].slot);
            if (!prev || !curr) continue;
            try { host.MoveChildAfter(curr, prev); } catch(e) {}
        }
    }
} catch(e) {}
)chud";

static const char* k_set_active_weapon = R"chud(
try {
    var cp = $.GetContextPanel();
    var host = cp.FindChildTraverse('ChudWeapons');
    if (host) {
        var wantId = 'ChudWeapSlot${activeRow}';
        for (var i = 0; i < host.GetChildCount(); i++) {
            var row = host.GetChild(i);
            if (!row) continue;
            row.style.opacity = (row.id === wantId) ? '1.0' : '0.38';
        }
    }
} catch(e) {}
)chud";

static const char* k_set_killfeed_top = R"chud(
try {
    var cp = $.GetContextPanel();
    var h = cp.FindChildTraverse('ChudKillfeedHost');
    if (h) {
        var s = 0;
        try { s = h.actualuiscale_y; } catch(e) { s = 0; }
        if (!s || s <= 0) s = ${screenH} / 1080.0;
        h.style.marginTop = Math.round(${wmBottom} / s) + 'px';
    }
} catch(e) {}
)chud";

static const char* k_add_kill = R"chud(
try {
    var cp = $.GetContextPanel();
    var host = cp.FindChildTraverse('ChudKillfeed');
    if (host) {
        /* spawns at FULL height and slides in from the right — only the removal
           animates height, so surviving rows glide up instead of snapping */
        var row = $.CreatePanel('Panel', host, 'ChudKill${rowId}', {
            hittest: false,
            style: '${panelBg}' +
                   'margin-top: 5px; height: 42px; padding: 0px 14px 0px 14px;' +
                   'flow-children: right; opacity: 0.0; horizontal-align: right;' +
                   'transform: translateX(420px);' +
                   'transition-property: transform, opacity, height;' +
                   'transition-duration: 0.28s;' +
                   'transition-timing-function: cubic-bezier(0.16, 1.00, 0.30, 1.00);' +
                   '${outline}'
        });

        try { chudBlur(row); } catch(e) {}

        // suicides / world deaths have no attacker — one name, no icon
        if (${hasAttacker}) {
            $.CreatePanel('Label', row, '', {
                text: '${attacker}',
                style: 'color: ${fg}; font-family: ${font}; font-weight: bold;' +
                       'font-size: ${fs18}px; vertical-align: center;' +
                       '${nudge}${textShadow}'
            });
        }

        /* column width is per weapon CLASS — a deagle should not take the same room
           as an awp. still a fixed width per class rather than raw aspect, so rows
           stay aligned instead of jittering with every individual svg */
        /* overflow noclip — the cell reserves a per-class width, but a knife svg is
           wider than its box and was getting its blade cut off. noclip lets the
           image spill past the cell while the cell still controls the spacing */
        var iconCell = $.CreatePanel('Panel', row, '', {
            hittest: false,
            style: 'width: ${iconWidth}px; height: 28px; margin: 0px 16px 0px 16px;' +
                   'vertical-align: center; overflow: noclip;'
        });
        $.CreatePanel('Image', iconCell, '', {
            src: 'file://{images}/icons/equipment/${weapon}.svg',
            scaling: 'stretch-to-fit-y-preserve-aspect',
            style: 'height: 26px; horizontal-align: center; vertical-align: center;' +
                   'wash-color: ${fg};' + '${imgShadow}'
        });

        if (${headshot}) {
            /* same url string the preload warmed at install — panorama caches per
               url, so this resolves instantly instead of hitting the network */
            var hsIcon = (typeof CHUD_HS_ICON !== 'undefined') ? CHUD_HS_ICON :
                'https://raw.githubusercontent.com/abandonedpools/scaleform/3e4c1f244351844a6236d952356ea087f59ad29e/p_scaleform/materials/panorama/images/hud/deathnotice/icon_headshot.svg';

            $.CreatePanel('Image', row, '', {
                src: hsIcon,
                scaling: 'stretch-to-fit-y-preserve-aspect',
                style: 'height: 22px; width: 22px; margin-right: 12px;' +
                       'vertical-align: center; wash-color: ${fg};' + '${imgShadow}'
            });
        }
        $.CreatePanel('Label', row, '', {
            text: '${victim}',
            style: 'color: ${fg}; font-family: ${font}; font-weight: bold;' +
                   'font-size: ${fs18}px; vertical-align: center;' +
                   '${nudge}${textShadow}'
        });

        $.Schedule(0.01, function() {
            try {
                row.style.transform = 'translateX(0px)';
                row.style.opacity = '1.0';
            } catch(e) {}
        });
    }
} catch(e) {}
)chud";

static const char* k_add_chat = R"chud(
try {
    var cp = $.GetContextPanel();
    var host = cp.FindChildTraverse('ChudChatLines');
    if (host) {
        /* NO measure pass and no white-space property — one of those two was
           throwing at creation time and nothing appeared in chat at all. natural
           height handles wrapping on its own; only opacity and slide animate, so
           a new line still reflows instantly but never vanishes */
        var line = $.CreatePanel('Label', host, 'ChudChatLine${rowId}', {
            text: '${text}',
            hittest: false,
            style: 'color: ${sec}; font-family: ${font}; font-size: ${fs18}px; width: 100%;' +
                   'overflow: noclip;' +
                   /* chat used to hardcode a 1px black shadow — it is the user's
                      dropshadow now, so it is off unless they turn it on */
                   '${textShadow}' + 'opacity: 0.0;' +
                   'height: 0px; margin-top: 0px;' +
                   /* chat is the one label that cannot take the nudge declaration —
                      it already owns transform for its slide-in, and the second
                      declaration would just replace the first. the nudge is baked
                      into the slide's end state instead (see Schedule below) */
                   'transform: translateX(-260px);' +
                   'transition-property: transform, opacity, height;' +
                   'transition-duration: 0.28s;' +
                   'transition-timing-function: cubic-bezier(0.16, 1.00, 0.30, 1.00);'
        });

        /* height animates too, so existing lines glide up instead of snapping when
           a new one arrives. fixed row height: a wrapped message clips, which is the
           tradeoff for smooth reflow until per-line measuring is reliable.
           NOT tied to font scale — font scale is text only, boxes never move. big
           text in a 24px row clips at the top, which is the user's call to make */
        $.Schedule(0.01, function() {
            try {
                line.style.transform = 'translate3d(${nudgeX}px, ${nudgeY}px, 0px)';
                line.style.opacity = '1.0';
                line.style.height = '24px';
                line.style.marginTop = '2px';
            } catch(e) {}
        });
    }
} catch(e) {}
)chud";

static const char* k_expire_row = R"chud(
try {
    var p = $.GetContextPanel().FindChildTraverse('${rowName}');
    if (p) {
        p.style.opacity = '0.0';
        p.style.height = '0px';
        /* margin cannot transition — zero it after the height collapse */
        $.Schedule(0.28, function() { try { p.style.marginTop = '0px'; } catch(e) {} });
        p.DeleteAsync(0.32);
    }
} catch(e) {}
)chud";

static const char* k_chat_set = R"chud(
try {
    var e = $.GetContextPanel().FindChildTraverse('ChudChatEntry');
    if (e) { e.text = '${text}'; e.style.visibility = 'visible'; }
} catch(e) {}
)chud";

static const char* k_close_chat = R"chud(
try {
    var e = $.GetContextPanel().FindChildTraverse('ChudChatEntry');
    if (e) { e.text = ''; e.style.visibility = 'collapse'; }
} catch(e) {}
)chud";

static const char* k_force_drop_focus = R"chud(
try {
    var cp = $.GetContextPanel();
    var e  = cp.FindChildTraverse('ChudChatEntry');
    if (e) {
        try { e.text = ''; } catch(err) {}
        try { e.style.visibility = 'collapse'; } catch(err) {}
    }
    /* native HudChat is only hidden, not deleted — if a messagemode bind opened it
       behind our overlay it can hold input too. blank its activation as well. */
    try {
        var nc = cp.FindChildTraverse('ChatInput');
        if (nc) { try { nc.activationenabled = false; } catch(e2) {} try { nc.SetFocus(false); } catch(e2) {} }
    } catch(err) {}
} catch(e) {}
)chud";

static const char* k_release_teamselect_focus = R"chud(
try {
    var cp = $.GetContextPanel();
    var tsm = cp.FindChildTraverse('TeamSelectMenu');

    // only when the game has already taken it off screen — never fight a live menu
    if (tsm && tsm.visible === false) {
        try { $.DispatchEvent('DropInputFocus', tsm); } catch(e) {}
        try { cp.SetFocus(); } catch(e) {}
    }
} catch(e) {}
)chud";
