#pragma once

#define JS constexpr const char*
#define CSGO_HUD_SCHEMA "panorama/layout/hud/hud.xml"

JS base = R"(
function exist(panel){
if($.GetContextPanel().FindChildTraverse(panel) === null){ return true; }
else{ return false; }
}

var contextPanel = $.GetContextPanel();

var weaponmodule = "https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/ammo.png" //weaponpanel (#AmmoBackground art)
// MoneyBG is afxhidden in the pack - no art exists, so paint it with the hud's own fade-out gradient
var moneyBgImg = 'gradient( linear, 0% 0%, 100% 0%, from( #000000D4 ), color-stop( 0.55, #000000D4 ), to( #00000000 ) )'
var buycartBgImg = "https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/icons/ui/buyzone.png" //buycart
var dashboardLabelImg = "https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/location_corner.png" //radar
var hudTeamCountBgT = "https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/teamcounter/teamcounter_alivebgT.png"; //team icon T
var hudTeamCountBgCT = "https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/teamcounter/teamcounter_alivebgCT.png"; //team icon CT


//weaponmodule
var hudBottomRight = contextPanel.FindChildTraverse('HudWeaponPanel').FindChildTraverse('WeaponPanelBottomBG');
hudBottomRight.style.backgroundImage = `url("${weaponmodule}")`;
hudBottomRight.style.backgroundColor = '#00000000';
hudBottomRight.style.backgroundSize = '100% 100%';
hudBottomRight.style.horizontalAlign = 'right';
hudBottomRight.style.verticalAlign = 'bottom';
hudBottomRight.style.width = '340px';
hudBottomRight.style.height = '59px';
hudBottomRight.style.marginLeft = '37px';
hudBottomRight.style.overflow = 'noclip';
// pack #AmmoBackground background-img-opacity = BackgroundOpacity -> owned by {alpha.js}

var ammo = $.GetContextPanel().FindChildrenWithClassTraverse('Ammo')[0]
ammo.style.fontFamily = 'Stratum2'
ammo.style.S2MixBlendMode = 'SRGBadditive'
ammo.style.margin = '0'
ammo.style.marginRight = '10px'

var ammoblend = $.GetContextPanel().FindChildrenWithClassTraverse('Ammo__Clip')[0]
ammoblend.style.S2MixBlendMode = 'SRGBadditive'
ammoblend.style.opacity = '1.0'
ammoblend.style.margin = '0'
ammoblend.style.x = '2px'
ammoblend.style.y = '0px'
ammoblend.style.fontSize = '45px'

var ammoblend2 = $.GetContextPanel().FindChildrenWithClassTraverse('Ammo__Total')[0]
ammoblend2.style.S2MixBlendMode = 'SRGBadditive'
ammoblend2.style.opacity = '1.0'
ammoblend2.style.margin = '0'
ammoblend2.style.y = '6px'
ammoblend2.style.marginLeft = '6px'
ammoblend.style.fontSize = '45px'

var ammoblend3 = $.GetContextPanel().FindChildrenWithClassTraverse('Ammo__Burst')[0]
ammoblend3.style.S2MixBlendMode = 'SRGBadditive'
ammoblend3.style.opacity = '1.0'
ammoblend3.style.margin = '0'
ammoblend3.style.y = '6px'
ammoblend.style.fontSize = '45px'

var ammoanimpos = $.GetContextPanel().FindChildrenWithClassTraverse('AmmoAnim__BulletIcon--Pos1')[0]
ammoanimpos.style.S2MixBlendMode = 'SRGBadditive'
ammoanimpos.style.opacity = '1.0'
ammoanimpos.style.x = '28px'
ammoanimpos.style.width = '19px'
ammoanimpos.style.y = '22px'

var ammoanimpos1 = $.GetContextPanel().FindChildrenWithClassTraverse('AmmoAnim__BulletIcon--Pos2')[0]
ammoanimpos1.style.S2MixBlendMode = 'SRGBadditive'
ammoanimpos1.style.opacity = '1.0'
ammoanimpos1.style.x = '20px'
ammoanimpos1.style.width = '19px'
ammoanimpos1.style.y = '22px'

var ammoanimpos2 = $.GetContextPanel().FindChildrenWithClassTraverse('AmmoAnim__BulletIcon--Pos3')[0]
ammoanimpos2.style.S2MixBlendMode = 'SRGBadditive'
ammoanimpos2.style.opacity = '1.0'
ammoanimpos2.style.x = '12px'
ammoanimpos2.style.width = '19px'
ammoanimpos2.style.y = '22px'

var ammoanimpos3 = $.GetContextPanel().FindChildrenWithClassTraverse('AmmoAnim__BulletIcon--Pos4')[0]
ammoanimpos3.style.S2MixBlendMode = 'SRGBadditive'
ammoanimpos3.style.opacity = '1.0'
ammoanimpos3.style.x = '4px'
ammoanimpos3.style.width = '19px'
ammoanimpos3.style.y = '22px'

var ammoanimpos4 = $.GetContextPanel().FindChildrenWithClassTraverse('AmmoAnim__BulletIcon--Pos5')[0]
ammoanimpos4.style.S2MixBlendMode = 'SRGBadditive'
ammoanimpos4.style.opacity = '1.0'
ammoanimpos4.style.x = '-4px'
ammoanimpos4.style.width = '19px'
ammoanimpos4.style.y = '22px'


//moneypanel
var hudMoney = contextPanel.FindChildTraverse('HudMoney');
// Background position is 0% by default which doesn't show
// our own buy cart. This is handled in {buyzone.js}
var moneyBg = hudMoney.FindChildTraverse('MoneyBG');
moneyBg.style.width = '256px';
moneyBg.style.height = '41px';
moneyBg.style.horizontalAlign = 'center';
moneyBg.style.verticalAlign = 'center';
moneyBg.style.marginLeft = '0px';
moneyBg.style.backgroundImage = 'none';
moneyBg.style.backgroundColor = moneyBgImg;
moneyBg.style.backgroundSize = '100% 100%';
moneyBg.style.backgroundRepeat = 'no-repeat';
// pack .money-text-bg opacity = BackgroundOpacity -> owned by {alpha.js}
moneyBg.style.x = '-1px';

var moneybgbuy = $.GetContextPanel().FindChildrenWithClassTraverse('buymenu-money')[0]
moneybgbuy.style.width = '246px'
moneybgbuy.style.height = '40px'
moneybgbuy.style.backgroundColor = moneyBgImg
moneybgbuy.style.backgroundImage = 'none';
moneybgbuy.style.margin = '0px'
moneybgbuy.style.x = '-1px'
moneybgbuy.style.y = '-170px'
moneybgbuy.style.backgroundSize = '100%'
moneybgbuy.style.verticalAlign = 'center'


for (var moneyText of hudMoney.FindChildrenWithClassTraverse('money-text')) {
    moneyText.style.horizontalAlign = 'left';
    moneyText.style.flowChildren = 'right';
    moneyText.style.verticalAlign = 'middle';
    moneyText.style.marginLeft = '0px';
}

for (var moneyLabel of hudMoney.FindChildrenWithClassTraverse('money-text-label')) {
	moneyLabel.style.marginTop = '3px';
	moneyLabel.style.marginLeft = '65px';
	moneyLabel.style.transform = 'translateX(0px)';
    moneyLabel.style.S2MixBlendMode = 'SRGBadditive'
}

// Fully remove this, as due to layouting rules we have to replace this with
// a dynamically UV-mapped image
// we are not using this anymore due to newly added buyzone icon <-- remove if you please
for (var buyIcon of hudMoney.FindChildrenWithClassTraverse('money-text-buy-icon')) {
	buyIcon.style.visibility = 'collapse'
}

// Reposition and change the color of animations
var moneyAdd = hudMoney.FindChildTraverse('Money-Add');
moneyAdd.style.color = '#7CC213';
moneyAdd.style.marginLeft = '25px';
var moneyAddFlash = hudMoney.FindChildTraverse('Money-AddFlash');
moneyAddFlash.style.color = '#D3E798';
var moneyRemove = hudMoney.FindChildTraverse('Money-Remove');
moneyRemove.style.marginLeft = '30px';
moneyRemove.style.color = '#FD2424';

// Radar Parent
var hudRadar = contextPanel.FindChildTraverse('HudRadar');
hudRadar.style.margin = '0px';
hudRadar.style.verticalAlign = 'bottom';
hudRadar.style.transform = 'translateY(-1px)'


// Alpha is handled in {alpha.js}
var dashboardLabel = contextPanel.FindChildTraverse('DashboardLabel');
dashboardLabel.style.verticalAlign = 'middle';
dashboardLabel.style.x = '-10px';
dashboardLabel.style.fontFamily = 'Stratum2';
dashboardLabel.style.fontWeight = 'bold';
dashboardLabel.style.fontSize = '24px';
dashboardLabel.style.letterSpacing = '-0.3px';
dashboardLabel.style.S2MixBlendMode = 'SRGBAdditive'
dashboardLabel.style.opacity = '0.92'

// Teamcount (only score style)
var hudTeamCounter = contextPanel.FindChildTraverse('HudTeamCounter');
hudTeamCounter.style.height = '100%';

var winsT = hudTeamCounter.FindChildTraverse('ScoreT');
winsT.style.S2MixBlendMode = 'screen';
winsT.style.fontFamily = 'Stratum2 Bold Monodigit';
winsT.style.fontSize = '22px';

var winsCT = hudTeamCounter.FindChildTraverse('ScoreCT');
winsCT.style.S2MixBlendMode = 'screen';
winsCT.style.fontFamily = 'Stratum2 Bold Monodigit';
winsCT.style.fontSize = '22px';

var timerbg = hudTeamCounter.FindChildTraverse('Timer');
timerbg.style.width = '83.5px';
timerbg.style.marginLeft = '0.5px';
timerbg.style.height = '50px';
// pack has no image for #Timer, only this flat plate colour
timerbg.style.backgroundColor = '#000000D4';
timerbg.style.backgroundImage = 'none';

/* score plates take the SAME plate colour as #Timer above (pack .GameScore__BGL/BGR).
   those class names are pack-xml only and match nothing in the stock hud - the real
   plate is #ScoreCT/#ScoreT's parent, so walk up from the labels. #Timer does NOT reach
   this row despite its 50px, so these need their own paint or the scores go clear. */
function sfPaintPlate(panel) {
	if (!panel) { return; }
	panel.style.backgroundColor = '#000000D4'; // keep in sync with timerbg above
	panel.style.backgroundImage = 'none';
}
sfPaintPlate(winsCT ? winsCT.GetParent() : null);
sfPaintPlate(winsT ? winsT.GetParent() : null);

var timer = hudTeamCounter.FindChildTraverse('TimerText');
timer.style.fontFamily = 'Stratum2 Bold';
timer.style.marginTop = '-3px';
timer.style.marginBottom = '4px';
timer.style.marginLeft = '2px';
timer.style.fontSize = '29px';
timer.style.letterSpacing = '-0.3px';
timer.style.S2MixBlendMode = 'screen';

// pack .TeamCounter__AliveText = 0.57 (files/teamcount_score.js agrees); 1 was port drift
var zzz = hudTeamCounter.FindChildTraverse('AliveTextT');
zzz.style.color = '#878686';
zzz.style.opacity = '0.57';

var ccc = hudTeamCounter.FindChildTraverse('AliveTextCT');
ccc.style.color = '#878686';
ccc.style.opacity = '0.57';

for (var tAlive of contextPanel.FindChildTraverse('HudTeamCounter').FindChildrenWithClassTraverse('TeamCounter__AliveT')){
    tAlive.style.backgroundImage = `url("${hudTeamCountBgT}")`;
}

for (var ctAlive of contextPanel.FindChildTraverse('HudTeamCounter').FindChildrenWithClassTraverse('TeamCounter__AliveCT'))
{
    ctAlive.style.backgroundImage = `url("${hudTeamCountBgCT}")`;
}

for (var scoreText of contextPanel.FindChildTraverse('HudTeamCounter').FindChildrenWithClassTraverse('TeamCounter__AliveCount'))
{
    scoreText.style.color = '#ffffff';
}

var historyText = contextPanel.FindChildTraverse('HudChat').FindChildTraverse('ChatHistoryText');
historyText.style.fontFamily = 'Stratum2';
historyText.style.fontSize = '18px';
historyText.style.fontWeight = 'lighter';
historyText.style.letterSpacing = '0px';
historyText.style.backgroundColor = '#000000CC';

var textEntry = contextPanel.FindChildTraverse('HudChat').FindChildTraverse('ChatTextEntryBox');
textEntry.style.fontFamily = 'Stratum2';
textEntry.style.fontSize = '18px';
textEntry.style.fontWeight = 'lighter';
textEntry.style.letterSpacing = '0px';

var sendChat = contextPanel.FindChildTraverse('HudChat').FindChildTraverse('ChatSendButton');
sendChat.style.fontFamily = 'Stratum2';
sendChat.style.fontSize = '18px';
sendChat.style.fontWeight = 'lighter';
sendChat.style.letterSpacing = '0px';

var chatbackground = $.GetContextPanel().FindChildrenWithClassTraverse('AlertText');
for(var i=0; i < chatbackground.length; i++)
{
if(chatbackground[i].GetChildCount() === 0){
chatbackground[i].style.fontFamily = 'Stratum2'
chatbackground[i].style.width = '100%'
chatbackground[i].style.maxHeight = '116px'
chatbackground[i].style.textAlign = 'left'
chatbackground[i].style.verticalAlign = 'bottom'
chatbackground[i].style.color = 'white'
chatbackground[i].style.fontSize = '22.5px'
chatbackground[i].style.fontWeight = 'normal'
chatbackground[i].style.letterSpacing = '.43px'
chatbackground[i].style.paddingLeft = '28px'
chatbackground[i].style.paddingBottom = '0px'
chatbackground[i].style.textShadow = '2px 2px 2px #000000FF'
chatbackground[i].style.backgroundColor = 'gradient( linear, 0% 100%, 100% 0%, from( #00000099 ), color-stop(0.65, #00000099 ), color-stop( 0.85, #00000040 ), to( #00000000 ) )'
chatbackground[i].style.flowChildren = 'up'
}
}
)";

#define BUYZONE "${buyZone}"
JS buyzone = R"(

var buyzoneicon1 = $.GetContextPanel().FindChildrenWithClassTraverse('money-text-buy-icon')[0]
buyzoneicon1.style.height = '${buyZone}';
buyzoneicon1.style.S2MixBlendMode = 'SRGBadditive'
buyzoneicon1.style.opacity = '0.8'

if(exist('buyzoneicon2')){
$.CreatePanel('Image', $.GetContextPanel().FindChildrenWithClassTraverse('money-text-bg')[0], 'buyzoneicon2', {src: "https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/icons/ui/buyzone.png", style: "height: ${buyZone}; width: 32px; wash-color-fast: #ceff78; horizontal-align: left; y: -1px; x: 10px; -s2-mix-blend-mode: SRGBadditive; opacity: 0.8;"});
}

$.GetContextPanel().FindChildTraverse('buyzoneicon2').AddClass('money-text-buy-icon')


)";


#define HEALTHAMMO_STYLE "${isShort}"
JS healthammo = R"(
var contextPanel = $.GetContextPanel();
contextPanel.FindChildTraverse('HudHealthArmor').style.marginLeft = '0px';
var hudHa = contextPanel.FindChildTraverse('HudHealthArmor');
/* <---- REMOVALS ----> */
hudHa.FindChildTraverse('HudHealthBG').style.backgroundColor = '#00000000';
hudHa.FindChildTraverse('HudArmorBG').style.backgroundColor = '#00000000';
/* <--------> */
for(var hud_container of hudHa.FindChildrenWithClassTraverse('hud-HA-icon-container'))
{
    hud_container.style.x = '10px';
    hud_container.style.y = '3px';
    hud_container.style.width = '30px';
    hud_container.style.height = '100%';
    hud_container.style.opacity = '1.0';
    hud_container.style.marginTop = '-1px';
}

for(var hudHaArmor of hudHa.FindChildrenWithClassTraverse('hud-HA-armor1'))
{
    hudHaArmor.style.x = '38px';
    hudHaArmor.style.y = '3px';
    hudHaArmor.style.width = '70px';
    hudHaArmor.style.fontSize = '42px';
    hudHaArmor.style.fontWeight = 'bold';
    hudHaArmor.style.color = '#ffffff';
    hudHaArmor.style.letterSpacing = '0px';
    hudHaArmor.style.horizontalAlign = 'left';
    // pack .hud-HA-armor1 has no opacity - the 0.95 lives on its Label below
    hudHaArmor.style.opacity = '1.0';
    hudHaArmor.style.textShadow = '0px 0px 3px 0.0 #000000DD';
    hudHaArmor.style.marginLeft = '5px';
    hudHaArmor.style.textOverflow = 'shrink';
}

for(var hudHaHealth of hudHa.FindChildrenWithClassTraverse('hud-HA-health1'))
{
    hudHaHealth.style.x = '38px';
    hudHaHealth.style.y = '3px';
    hudHaHealth.style.width = '70px';
    hudHaHealth.style.fontSize = '42px';
    hudHaHealth.style.fontWeight = 'bold';
    hudHaHealth.style.color = '#ffffff';
    hudHaHealth.style.letterSpacing = '0px';
    hudHaHealth.style.horizontalAlign = 'center';
    // pack .hud-HA-health1 has no opacity - the 0.95 lives on its Label below
    hudHaHealth.style.opacity = '1.0';
    hudHaHealth.style.textShadow = '0px 0px 3px 0.0 #000000DD';
    hudHaHealth.style.marginLeft = '5px';
    hudHaHealth.style.textOverflow = 'shrink';
}

for(var hudHaText of hudHa.FindChildrenWithClassTraverse('hud-HA-text'))
{
    hudHaText.style.x = '42px';
    hudHaText.style.y = '4px';
}

for(var hudHaBar of hudHa.FindChildrenWithClassTraverse('hud-HA-bar'))
{
    hudHaBar.style.x = '108px';
    hudHaBar.style.y = '34px';
    hudHaBar.style.backgroundColor = '#03030399';
}
for(var hudHaGeneric of hudHa.FindChildrenWithClassTraverse('hud-HA'))
{
        hudHaGeneric.style.width = '465px';
        hudHaGeneric.style.height = '55px';
        hudHaGeneric.style.verticalAlign = 'bottom';
        hudHaGeneric.style.opacity = '0.0';
        hudHaGeneric.style.transitionProperty = 'opacity';
        hudHaGeneric.style.transitionTimingFunction = 'ease-in-out';
        hudHaGeneric.style.transitionDuration = '0.3s';
        hudHaGeneric.style.marginLeft = '0px';

        if(hudHaGeneric.BHasClass('hud-HA--active'))
        {
        for(var hudHaGeneric_active of hudHa.FindChildrenWithClassTraverse('hud-HA--active'))
        {
            hudHaGeneric_active.style.opacity = '1.0';
        }
        }
}

for(var survival of hudHa.FindChildrenWithClassTraverse('survival'))
{
    for(var inner of survival.FindChildrenWithClassTraverse('hud-HA'))
    {
        inner.style.width = '550px';
    }
}

for(var hudHaBg of hudHa.FindChildrenWithClassTraverse('hud-HA-bg'))
{
    hudHaBg.style.flowChildren = 'right';
    hudHaBg.style.width = '100%';
    hudHaBg.style.height = '100%';
    // pack .hud-HA-bg1 opacity = BackgroundOpacity -> owned by {alpha.js}
}

for(var hudHaBgH of hudHa.FindChildrenWithClassTraverse('hud-HA-bg-h'))
{
    hudHaBgH.style.backgroundImage = 'url("https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/hp.png")'; // HP.png
    hudHaBgH.style.backgroundSize = '107% 100%';
    hudHaBgH.style.backgroundImgOpacity = '1.0';
    hudHaBgH.style.width = '200px';
    hudHaBgH.style.height = '100%';
    hudHaBgH.style.horizontalAlign = 'left';
}

for(var hudHaBgA of hudHa.FindChildrenWithClassTraverse('hud-HA-bg-a'))
{
    hudHaBgA.style.backgroundImage = 'url("https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/armor.png")'; // Armor.png
    hudHaBgA.style.backgroundSize = '100% 100%';
    hudHaBgA.style.backgroundImgOpacity = '1.0';
    hudHaBgA.style.width = '210px'; // value from files/health_and_ammo.js
    hudHaBgA.style.height = '100%';
    hudHaBgA.style.opacity = '1';
    hudHaBgA.style.horizontalAlign = 'left';
    hudHaBgA.style.overflow =  'noclip';
}

    var healthbgh = $.GetContextPanel().FindChildTraverse('HudHealthArmorBG')
    healthbgh.style.width =  ${isShort} ? '550px' : '310px';
    healthbgh.style.height = '54px'
    // HudHealthArmorBG is afxhidden in the pack - stays clear, hp.png + armor.png are the bar
    healthbgh.style.backgroundImage = 'none'
    healthbgh.style.backgroundColor = '#00000000'
    healthbgh.style.horizontalAlign = 'left'
    healthbgh.style.x = '-1px'
    healthbgh.style.y = '1px'

for(var onDmg of hudHa.FindChildrenWithClassTraverse('hud-HA--on-damage'))
{
    for(var dmg of onDmg.FindChildrenWithClassTraverse('hud-HA-text'))
    {
        dmg.style.washColor = '#ff0000';
    }
}

for(var oncrit of hudHa.FindChildrenWithClassTraverse('hud-HA--critical'))
{
    for(var hudHaBg of oncrit.FindChildTraverse('HudHealthArmor').FindChildrenWithClassTraverse('hud-HA-bg-h'))
    {
        hudHaBg.style.backgroundImage = 'url("https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/hpred.png")';
        hudHaBg.style.backgroundImgOpacity = '0.92';
    }
}


if(exist('hud-HA-icon-Healthsf')){
$.CreatePanel('Image', $.GetContextPanel().FindChildrenWithClassTraverse('hud-HA')[0], 'hud-HA-icon-Healthsf', {src: "https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/healtharmor/hp.png", style: "height: 20px; width: 20px; y: 21px; x: 16px; opacity: 0.85; -s2-mix-blend-mode: Additive; z-index: 31;"});
}


if(exist('hud-HA-icon-Armorsf')){
$.CreatePanel('Image', $.GetContextPanel().FindChildrenWithClassTraverse('hud-HA')[0], 'hud-HA-icon-Armorsf', {src: "https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/healtharmor/armor.png", style: "height: 20px; width: 20px; y: 21px; x: 226px; opacity: 0.85; -s2-mix-blend-mode: Additive; z-index: 31;"});
}

if(exist('hud-HA-icon-Helmetsf')){
$.CreatePanel('Image', $.GetContextPanel().FindChildrenWithClassTraverse('hud-HA')[0], 'hud-HA-icon-Helmetsf', {src: "https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/healtharmor/helmet.png", style: "height: 20px; width: 20px; y: 21px; x: 226px; opacity: 0.85; -s2-mix-blend-mode: Additive; z-index: 31;"});
}

	if(exist('hudhabarborder')){
$.CreatePanel('Image', $.GetContextPanel().FindChildrenWithClassTraverse('hud-HA')[0], 'hudhabarborder', {src: "https://media.discordapp.net/attachments/1051901947545931857/1051902080207556608/hudhabar.png", style: "height: 14px; width: 84px; y: 29px; x: 114px; opacity: 0.70;"});
}

if(exist('hudhabarborder2')){
$.CreatePanel('Image', $.GetContextPanel().FindChildrenWithClassTraverse('hud-HA')[0], 'hudhabarborder2', {src: "https://media.discordapp.net/attachments/1051901947545931857/1051902080207556608/hudhabar.png", style: "height: 14px; width: 84px; y: 29px; x: 321px; opacity: 0.70;"});
}

var hudhabar1 = $.GetContextPanel().FindChildrenWithClassTraverse('hud-HA-bar')[0]
hudhabar1.style.S2MixBlendMode = 'SRGBadditive'
hudhabar1.style.border = '1px solid #55555500'
hudhabar1.style.boxShadow = '0px 0px 0px 0px #00000000'
hudhabar1.style.backgroundColor = '#00000000'
hudhabar1.style.opacity = '0.70' // pack ProgressBar
hudhabar1.style.y = '35px'
hudhabar1.style.x = '110px'

var hudarmorbarblend = $.GetContextPanel().FindChildTraverse('ArmorBar')
hudarmorbarblend.style.S2MixBlendMode = 'SRGBadditive'
hudarmorbarblend.style.border = '1px solid #55555500'
hudarmorbarblend.style.boxShadow = '0px 0px 0px 0px #00000000'
hudarmorbarblend.style.backgroundColor = '#00000000'
hudarmorbarblend.style.opacity = '0.70' // pack ProgressBar
hudarmorbarblend.style.y = '35px'
hudarmorbarblend.style.x = '110px'

var hudhahealthicon = $.GetContextPanel().FindChildrenWithClassTraverse('hud-HA-icon')[0]
hudhahealthicon.style.S2MixBlendMode = 'SRGBadditive'
hudhahealthicon.style.visibility = 'collapse'

var hudhahealthicon = $.GetContextPanel().FindChildrenWithClassTraverse('hud-HA-icon')[1]
hudhahealthicon.style.S2MixBlendMode = 'SRGBadditive'
hudhahealthicon.style.visibility = 'collapse'

var hudhaarmoricon = $.GetContextPanel().FindChildrenWithClassTraverse('hud-HA-icon')[2]
hudhaarmoricon.style.S2MixBlendMode = 'SRGBadditive'
hudhaarmoricon.style.visibility = 'collapse'

var hudhahelmeticon = $.GetContextPanel().FindChildrenWithClassTraverse('hud-HA-icon')[3]
hudhahelmeticon.style.S2MixBlendMode = 'SRGBadditive'
hudhahelmeticon.style.visibility = 'collapse'


$.GetContextPanel().FindChildTraverse('hud-HA-icon-Healthsf').AddClass('cl-hud-wash-color')


$.GetContextPanel().FindChildTraverse('hud-HA-icon-Armorsf').AddClass('cl-hud-wash-color')

$.GetContextPanel().FindChildTraverse('hud-HA-icon-Helmetsf').AddClass('cl-hud-wash-color')

$.GetContextPanel().FindChildTraverse('hud-HA-icon-Helmetsf').AddClass('hud-HA-icon--helmet')

$.GetContextPanel().FindChildTraverse('hud-HA-icon-Helmetsf').style.S2MixBlendMode = 'SRGBadditive'

var hudhatext1 = $.GetContextPanel().FindChildrenWithClassTraverse('hud-HA-health-label')[0]
hudhatext1.style.S2MixBlendMode = 'SRGBadditive'
hudhatext1.style.fontFamily = 'Stratum2'
hudhatext1.style.fontSize = '44px'
hudhatext1.style.opacity = '0.95' // pack .hud-HA-health1 Label
hudhatext1.style.textOverflow = 'shrink'


var hudhatext2= $.GetContextPanel().FindChildrenWithClassTraverse('hud-HA-armor-label')[0]
hudhatext2.style.S2MixBlendMode = 'SRGBadditive'
hudhatext2.style.fontFamily = 'Stratum2'
hudhatext2.style.fontSize = '44px'
hudhatext2.style.opacity = '0.95' // pack .hud-HA-armor1 Label
hudhatext2.style.textOverflow = 'shrink'


)";

JS deathnotices = R"(
var contextPanel = $.GetContextPanel();

for (var content of contextPanel.FindChildrenWithClassTraverse('DeathNoticeContent')) {
	content.style.S2MixBlendMode = 'SRGBadditive'
	content.style.fontFamily = 'Stratum2';
}

for (var dnBackground of contextPanel.FindChildrenWithClassTraverse('DeathNoticeBGBorder'))
{

	dnBackground.style.border = '0px solid #00000000';
	dnBackground.style.boxShadow = 'inset #00000000 0px 0px 0px 0px;';
	dnBackground.style.borderRadius = '5px';
	dnBackground.style.width = '105%';
	dnBackground.style.paddingTop = '0px';
	dnBackground.style.height = '36px';
}

for (var dnBackgroundg of contextPanel.FindChildrenWithClassTraverse('DeathNoticeBG')) {
	// lobo paints its own bar image here; host dead -> panel stays clear, gradient is the bar
	dnBackgroundg.style.backgroundImage = 'none';
	dnBackgroundg.style.backgroundSize = '100% 100%';
	dnBackgroundg.style.backgroundColor = '#00000000';
	dnBackgroundg.style.boxShadow = 'inset #00000000 0px 0px 0px;';
	dnBackgroundg.style.border = '0px solid #00000000';
	dnBackgroundg.style.opacity = '0.7';
}

/* the game's own radialgradient.png = the blurred centre blob. no inset shadow and 95%
   height: a rim + full-height stretch filled the 5px-radius border panel = black pill. */
for (var dnGradient of contextPanel.FindChildrenWithClassTraverse('DeathNoticeBGGradient')) {
	dnGradient.style.visibility = 'visible';
	dnGradient.style.width = '100%';
	dnGradient.style.height = '95%';
	dnGradient.style.verticalAlign = 'center';
	dnGradient.style.opacity = '0.82';
	dnGradient.style.boxShadow = 'none';
}

for (var dnIcon of $.GetContextPanel().FindChildrenWithClassTraverse('DeathNoticeIcon')) {
	dnIcon.style.boxShadow = 'inset #e1000000 0px 0px 0px;';
	dnIcon.style.backgroundColor = '#00000000'
	if (dnIcon.id === "Weapon") {
		// lobo: fixed 24px row height + 70% scale, 5px gutters either side
		dnIcon.style.height = '24px';
		dnIcon.style.verticalAlign = 'top';
		dnIcon.style.uiScale = '70%';
		dnIcon.style.transform = 'translateY(2px)';
		dnIcon.style.margin = '0px 5px 0px 5px';
	} else {
		dnIcon.style.height = '22px';
		dnIcon.style.verticalAlign = 'top';
		dnIcon.style.uiScale = '80%';
		dnIcon.style.margin = '0px 5px 0px 5px';
	}
}

for (var dnT of $.GetContextPanel().FindChildrenWithClassTraverse('DeathNoticeTColor')) {
	dnT.style.color = '#f5ca68';
	dnT.style.textShadow = '0px 0px 0px #00000000';
	dnT.style.fontFamily = 'Stratum2, "Arial Unicode MS"';
	dnT.style.fontSize = '21px';
	dnT.style.fontWeight = 'bold';
	dnT.style.letterSpacing = '0.20px';
	dnT.style.verticalAlign = 'bottom';
	dnT.style.textOverflow = 'ellipsis';
	dnT.style.transform = 'scaleX(0.99) scaleY(1.03) translateY(1.9px)';
	dnT.style.S2MixBlendMode = 'SRGBadditive';
}
for (var dnCT of $.GetContextPanel().FindChildrenWithClassTraverse('DeathNoticeCTColor')) {
	dnCT.style.color = '#98abd5';
	dnCT.style.textShadow = '0px 0px 0px #00000000';
	dnCT.style.fontFamily = 'Stratum2, "Arial Unicode MS"';
	dnCT.style.fontSize = '21px';
	dnCT.style.fontWeight = 'bold';
	dnCT.style.letterSpacing = '0.20px';
	dnCT.style.verticalAlign = 'bottom';
	dnCT.style.textOverflow = 'ellipsis';
	dnCT.style.transform = 'scaleX(0.99) scaleY(1.03) translateY(1.9px)';
	dnCT.style.S2MixBlendMode = 'SRGBadditive';
}


// null-safe child lookup — a missing panel throws and kills the REST of the script
function dnChild(row, id) { try { return row.FindChildTraverse(id); } catch (e) { return null; } }

for (var deathnotice of contextPanel.FindChildrenWithClassTraverse('DeathNotice'))
{
	// ---------------- ICON REMOVALS ----------------
	var dnNoScope = dnChild(deathnotice, 'NoScopeIcon');
	if (dnNoScope) dnNoScope.style.visibility = 'collapse';
	var dnSmoke = dnChild(deathnotice, 'ThroughSmokeIcon');
	if (dnSmoke) dnSmoke.style.visibility = 'collapse';
	var dnBlind = dnChild(deathnotice, 'AttackerBlindIcon');
	if (dnBlind) dnBlind.style.visibility = 'collapse';
	var dnDom = dnChild(deathnotice, 'Domination');
	if (dnDom) dnDom.style.visibility = 'collapse';
	var dnSuicide = dnChild(deathnotice, 'Suicide');
	if (dnSuicide) {
		dnSuicide.style.backgroundColor = '#00000000';
		dnSuicide.style.boxShadow = 'inset #e1000000 0px 0px 0px;';
	}
	// headshot + penetrate are the only two icons the pack actually reskins; Suicide
	// keeps the game's own icon_suicide.svg (the old discord link is dead = blank icon)
	var dnHs = dnChild(deathnotice, 'HeadShot');
	if (dnHs) dnHs.SetImage('https://raw.githubusercontent.com/abandonedpools/scaleform/3e4c1f244351844a6236d952356ea087f59ad29e/p_scaleform/materials/panorama/images/hud/deathnotice/icon_headshot.svg')
	var dnPen = dnChild(deathnotice, 'Penetrate');
	if (dnPen) dnPen.SetImage('https://raw.githubusercontent.com/abandonedpools/scaleform/3e4c1f244351844a6236d952356ea087f59ad29e/p_scaleform/materials/panorama/images/hud/deathnotice/penetrate.svg')
    deathnotice.style.margin = '0px -8px 1px 18px'
	// ---------------- REMOVALS ----------------
	if(deathnotice.BHasClass('DeathNotice_Killer'))
	{
		for (var killer of contextPanel.FindChildTraverse('HudDeathNotice').FindChildrenWithClassTraverse('DeathNotice_Killer'))
		{
			// pack css collapses the gradient here — killer BG paints its own black,
			// leaving both on = two black layers = opaque row
			for(var grad of killer.FindChildrenWithClassTraverse('DeathNoticeBGGradient'))
				grad.style.visibility = 'collapse';
			// lobo: red border on the BG panel itself
			for(var content of killer.FindChildrenWithClassTraverse('DeathNoticeBG'))
			{
				content.style.backgroundImage = 'none';
				content.style.border = '1.5px solid #b3070d;';
				content.style.borderRadius = '3px';
				content.style.backgroundColor = 'black';
                content.style.opacity = '0.8';
			}
		}
	}
	if(deathnotice.BHasClass('DeathNotice_Victim'))
	{
	    for (var victim of contextPanel.FindChildTraverse('HudDeathNotice').FindChildrenWithClassTraverse('DeathNotice_Victim'))
		{
		    for(var content of victim.FindChildrenWithClassTraverse('DeathNoticeBG'))
            {
				content.style.backgroundImage = 'none';

                content.style.backgroundColor = '#a81313';
		    	content.style.borderRadius = '3px';
                content.style.boxShadow = 'inset #e10000e6 0px 0px 1px;';
                content.style.opacity = '1';
		    }
	    }
    }
}
)";

JS weapon_select = R"(

var weaponRowBG = $.GetContextPanel().FindChildrenWithClassTraverse('weapon-row-background');{
		for(var i=0; i < weaponRowBG.length; i++)
		if(weaponRowBG[i].GetChildCount() === 0)
{
	weaponRowBG[i].style.width = '280px'
	weaponRowBG[i].style.height = '80px'
	weaponRowBG[i].style.horizontalAlign = 'right'

	$.CreatePanel('Image', weaponRowBG[i], 'weaponrowoutline-Center', {src: "https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/weapon.png", style: "height: 80px; width: 280px; horizontal-align: right; vertical-align: center; x: 0px; opacity: 0.58;"});
	var weaponrowcenter = $.GetContextPanel().FindChildTraverse('weaponrowoutline-Center')
				}
			}

    var WeaponselectIcon = $.GetContextPanel().FindChildrenWithClassTraverse('weapon-selection-item-icon-main');
	for(var i=0; i < WeaponselectIcon.length; i++) {
	if(WeaponselectIcon[i].GetChildCount() === 0){
	WeaponselectIcon[i].style.uiScale = '110% 110% 100%'
	WeaponselectIcon[i].style.y = '-1px'
	WeaponselectIcon[i].style.marginRight = '8px';
    WeaponselectIcon[i].style.marginLeft = '12px';
    WeaponselectIcon[i].style.padding = '5px';
	WeaponselectIcon[i].style.opacity = '0.92';
	WeaponselectIcon[i].style.transform = 'scaleX(-1)';
				}
	}

var weaponselecttext = $.GetContextPanel().FindChildrenWithClassTraverse('weapon-selection-item-name-text');{
	for(var i=0; i < weaponselecttext.length; i++)
{
	if(weaponselecttext[i].GetChildCount() === 0){
	weaponselecttext[i].style.color = 'white'
	weaponselecttext[i].style.fontSize = '19px'
    weaponselecttext[i].style.fontFamily = 'Stratum2'
    weaponselecttext[i].style.fontWeight = 'bold'
	weaponselecttext[i].style.textShadow = 'none'
	weaponselecttext[i].style.opacity = '0.95' // pack .weapon-selection-item-name-text
	weaponselecttext[i].style.y = '-2px'
				}
			}
		}

    var TextGenerator2 = $.GetContextPanel().FindChildrenWithClassTraverse('weapon-selection-item-name-text-owned');{
	for(var i=0; i < TextGenerator2.length; i++)
{
	if(TextGenerator2[i].GetChildCount() === 0){
	TextGenerator2[i].style.color = 'grey'
	TextGenerator2[i].style.fontSize = '19px'
    TextGenerator2[i].style.fontFamily = 'Stratum2'
    TextGenerator2[i].style.fontWeight = 'bold'
	TextGenerator2[i].style.textShadow = 'none'
	TextGenerator2[i].style.y = '-2px'
				}
			}

    var TextGenerator3 = $.GetContextPanel().FindChildrenWithClassTraverse('weapon-row-number');{
	for(var i=0; i < TextGenerator3.length; i++)
{
	if(TextGenerator3[i].GetChildCount() === 0){
	TextGenerator3[i].style.color = 'white'
	TextGenerator3[i].style.fontSize = '19px'
    TextGenerator3[i].style.fontFamily = 'Stratum2'
    TextGenerator3[i].style.fontWeight = 'bold'
	TextGenerator3[i].style.textShadow = 'none'
	TextGenerator3[i].style.opacity = '0.92' // pack .weapon-row-number
	TextGenerator3[i].style.transform = 'translateY(-7px)';
				}
			}
		}
	}

	var TextGenerator4 = $.GetContextPanel().FindChildrenWithClassTraverse('weapon-selection-item-count');{
	for(var i=0; i < TextGenerator4.length; i++)
{
	if(TextGenerator4[i].GetChildCount() === 0){
	TextGenerator4[i].style.color = 'white'
	TextGenerator4[i].style.fontSize = '19px'
    TextGenerator4[i].style.fontFamily = 'Stratum2'
    TextGenerator4[i].style.fontWeight = 'bold'
	TextGenerator4[i].style.textShadow = 'none'
			}
		}
	}

	var hudweaponselect = $.GetContextPanel().FindChildTraverse('HudWeaponSelection')
	hudweaponselect.style.marginBottom = '15px'

for (var j = 0; j < 9; j++) {
    for (rarity of $.GetContextPanel().FindChildrenWithClassTraverse("weapon-selection-item--rarity-" + (j == 8 ? 99 : j))) {
        if (j == 0)
            rarity.Children()[0].Children()[0].style.imgShadow = '0px 0px 0px 0 transparent';
        for (name of rarity.GetParent().GetParent().FindChildrenWithClassTraverse('weapon-selection-item-name-text')) {
            name.style.textShadowFast = '0px 0px transparent';
            if (name.text == "Zeus x27")
                name.style.color = '#FFFFFF';
            else
                name.style.color = j == 0 ? '#FFFFFF' :
                    j == 1 ? '#b0c3d9' :
                        j == 2 ? '#5e98d9' :
                            j == 3 ? '#4b69ff' :
                                j == 4 ? '#8847ff' :
                                    j == 5 ? '#d32ce6' :
                                        j == 6 ? '#eb4b4b' :
                                            j == 7 ? '#e4ae39' :
                                                '#ffd700';
        }
    }
}


)";

JS teamcount_avatar = R"(
function runner() {
    var hudTeamCounter = $.GetContextPanel().FindChildTraverse('HudTeamCounter');

    for (var avatarBackground of hudTeamCounter.FindChildrenWithClassTraverse('AvatarL__Default--CT')) {
        avatarBackground.style.backgroundSize = '92% 86%';
        avatarBackground.style.backgroundPosition = '50% 25%';
        avatarBackground.style.backgroundRepeat = 'no-repeat';
    }
    for (var avatarBackground of hudTeamCounter.FindChildrenWithClassTraverse('AvatarL__Default--T')) {
        avatarBackground.style.backgroundSize = '92% 86%';
        avatarBackground.style.backgroundPosition = '50% 25%';
        avatarBackground.style.backgroundRepeat = 'no-repeat';

    }

    for (var avatarClr of hudTeamCounter.FindChildrenWithClassTraverse('AvatarL__PlayerColor')) {
        avatarClr.style.y = '-2px';
        avatarClr.style.width = '24px';
        avatarClr.style.height = '12px';
        avatarClr.style.marginLeft = '0px';
        avatarClr.style.horizontalAlign = 'center';
        avatarClr.style.imgShadow = '0px 0px 1px 1.0 #000000CC';
    }

    for (avatarL of hudTeamCounter.FindChildrenWithClassTraverse('AvatarL__Internal')) {
        avatarL.style.height = '59px';
        avatarL.style.width = '85%';
        avatarL.style.horizontalAlign = 'center';
        avatarL.style.verticalAlign = 'top';

        // pack .AvatarL__Internal is clear - only .AvatarS__Internal takes the #7f7f7f plate
        avatarL.style.backgroundColor = '#00000000';
    }

    for (var avatarL of hudTeamCounter.FindChildrenWithClassTraverse('AvatarL_HealthBarBG')) {
        avatarL.style.verticalAlign = 'bottom';
        avatarL.style.marginBottom = '1.5px';
        avatarL.style.marginRight = '1.5px';
        avatarL.style.marginLeft = '1.5px';
        avatarL.style.height = '4.55px';
        avatarL.style.backgroundColor = '#00000040';
        avatarL.style.position = 'absolute';
    }

    for (var avbg of hudTeamCounter.FindChildrenWithClassTraverse('AvatarL__ImageBG')) {
        avbg.style.width = '95%';
        avbg.style.height = '59px';
    }

    for (var avatarL of hudTeamCounter.FindChildrenWithClassTraverse('AvatarL')) {
        avatarL.style.width = '58px';
        avatarL.style.height = '100px';
        avatarL.style.overflow = 'noclip';
    }

    for (var avatarS of hudTeamCounter.FindChildrenWithClassTraverse('AvatarS')) {
        avatarS.style.width = '40px';
    }

    for (var avatarS of hudTeamCounter.FindChildrenWithClassTraverse('AvatarS__Internal')) {
        avatarS.style.opacity = '1';
        avatarS.style.width = '25px';
        avatarS.style.height = '28.6px';
        avatarS.style.backgroundColor = '#7f7f7f';
        avatarS.style.verticalAlign = 'top';
    }

    for (var avatarS of hudTeamCounter.FindChildrenWithClassTraverse('AvatarS__PlayerImage')) {
        avatarS.style.backgroundColor = 'black';
        avatarS.style.width = '100%';
        avatarS.style.height = '100%';
        avatarS.style.verticalAlign = 'middle';
    }


    for (var avatarS of hudTeamCounter.FindChildrenWithClassTraverse('AvatarS__HealthBar')) {
        avatarS.style.height = '3px';
        avatarS.style.verticalAlign = 'bottom';
    }

    for (var avatarS of hudTeamCounter.FindChildrenWithClassTraverse('AvatarS__Default--CT')) {
        avatarS.style.backgroundSize = '100% 180%';
        avatarS.style.backgroundPosition = 'center';
        avatarS.style.height = '100%'
    }

    for (var avatarS of hudTeamCounter.FindChildrenWithClassTraverse('AvatarS__Default--T')) {
        avatarS.style.backgroundSize = '100% 180%';
        avatarS.style.backgroundPosition = 'center';
        avatarS.style.height = '100%';
    }


    for (var internalColor of hudTeamCounter.FindChildrenWithClassTraverse('AvatarImageT__Color')) {
        internalColor.style.backgroundColor = '#987344E0';
        internalColor.style.border = '2px solid #8a6a41';
    }

    for (var internalColor of hudTeamCounter.FindChildrenWithClassTraverse('AvatarImageT__InternalColor')) {
        internalColor.style.backgroundColor = '#FFFFFF';
    }


    for (var internalColor of hudTeamCounter.FindChildrenWithClassTraverse('AvatarImageCT__Color')) {
        internalColor.style.backgroundColor = '#72859dDD';
        internalColor.style.border = '2px solid #FFFFFF';
    }

    for (var internalColor of hudTeamCounter.FindChildrenWithClassTraverse('AvatarImageCT__InternalColor')) {
        internalColor.style.backgroundColor = '#FFFFFF';
    }

    for (var noOutline of hudTeamCounter.FindChildrenWithClassTraverse('Avatar__PlayerOutline--None')) {
        noOutline.style.border = '0px solid #0000000';
    }

    for (var noOutline of hudTeamCounter.FindChildrenWithClassTraverse('Avatar__PlayerOutline--Spectate')) {
        noOutline.style.border = '2px solid #D8D8D8D8';
    }

    for (var avatarOutline of hudTeamCounter.FindChildrenWithClassTraverse('Avatar__PlayerOutline--White')) {
        avatarOutline.style.border = '2px solid white';
    }

    for (var ct of hudTeamCounter.FindChildrenWithClassTraverse('Avatar__PlayerBorder--CT')) {
        ct.style.border = '1px solid #454b51';
    }

    for (var t of hudTeamCounter.FindChildrenWithClassTraverse('Avatar__PlayerBorder--T')) {
        t.style.border = '1px solid #483a2a';
    }


    for (var avatarOutline of hudTeamCounter.FindChildrenWithClassTraverse('Avatar__PlayerOutline--T')) {
        avatarOutline.style.backgroundColor = '#756337';
        avatarOutline.style.border = '2px solid #87663e';
    }

    for (var avatarOutline of hudTeamCounter.FindChildrenWithClassTraverse('Avatar__PlayerOutline--CT')) {
        avatarOutline.style.backgroundColor = '#7C858F';
        avatarOutline.style.border = '2px solid #4F5D6D';
    }

    for (var av of hudTeamCounter.FindChildrenWithClassTraverse('Avatar__HealthBar--Normal')) {
        av.style.backgroundColor = '#99ae91';
    }
    for (var ab of hudTeamCounter.FindChildrenWithClassTraverse('Avatar__HealthBar--Red')) {
        ab.style.backgroundColor = '#99ae91';
    }
}

$.Schedule(0.3, runner);
//runner();
)";

JS alerts = R"alert(
var contextPanel = $.GetContextPanel();
var joinPanelCTBG = "https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/fade.png";
var joinPanelTBG = "https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/fade.png";
var joinPanelBotBG = "https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/fade.png";

for(var hudHint of $.GetContextPanel().FindChildrenWithClassTraverse('hud-hint'))
{
    hudHint.style.width = '400px';
    hudHint.style.height = '103px';
    hudHint.style.overflow = 'noclip';
    hudHint.style.flowChildren = 'none';
    hudHint.style.textAlign = 'left';
    hudHint.style.horizontalAlign = 'center'
	hudHint.style.position = '0px 80px 0px'
	hudHint.style.backgroundColor = 'gradient( linear, 0% 0%, 100% 0%, from( #000000CC ), color-stop( 0.55, #000000CC ), to( #00000000 ) )';
}

for(var hrtop of $.GetContextPanel().FindChildrenWithClassTraverse('hrTop'))
{
	hrtop.style.backgroundColor = "#00000000";
}

for(var hrbot of $.GetContextPanel().FindChildrenWithClassTraverse('hrBot'))
{
	hrbot.style.backgroundColor = "#00000000";
}

for(var hudHintIcon of $.GetContextPanel().FindChildrenWithClassTraverse('hud-hint_icon'))
{
    hudHintIcon.style.visibility = 'collapse'; // removal
}
for(var hint of $.GetContextPanel().FindChildrenWithClassTraverse('hud-hint__icon'))
{
    hint.style.visibility = 'collapse'; // removal
}


if(exist('hud-hint__icon2')){
$.CreatePanel('Image', $.GetContextPanel().FindChildrenWithClassTraverse('hud-hint')[0], 'hud-hint__icon2', {src: "https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/icons/ui/alert.png", style: "height: 110px; width: 110px; y: -4px; x: -40px; z-index: 21;"});
}

for(var hudHintText of $.GetContextPanel().FindChildrenWithClassTraverse('hud-hint__text'))
{
    hudHintText.style.width = '100%';
    hudHintText.style.horizontalAlign = 'left';
    hudHintText.style.verticalAlign = 'bottom';
	hudHintText.style.textAlign = 'left';
	hudHintText.style.margin = '0px'
	hudHintText.style.zIndex = '1000';
	hudHintText.style.y = '30px'
	hudHintText.style.marginLeft = '73px'
}


for(var hudHintPriorityLabel of $.GetContextPanel().FindChildrenWithClassTraverse('hud-hint__priority-label'))
{
    hudHintPriorityLabel.style.horizontalAlign = 'left';
    hudHintPriorityLabel.style.verticalAlign = 'bottom';
    hudHintPriorityLabel.style.fontSize = '24px';
    hudHintPriorityLabel.style.fontFamily = 'Stratum2';
    hudHintPriorityLabel.style.fontWeight = 'bold';
    hudHintPriorityLabel.style.color = '#c93121';
    hudHintPriorityLabel.style.letterSpacing = '0px';
    hudHintPriorityLabel.style.S2MixBlendMode = 'SRGBadditive';
    hudHintPriorityLabel.style.visibility = 'visible';
	hudHintPriorityLabel.style.marginRight = '200px';
	hudHintPriorityLabel.style.textAlign = 'left';
	hudHintPriorityLabel.style.overflow = 'noclip';
	hudHintPriorityLabel.style.textOverflow = 'noclip';
}

for(var hudHintLabel of $.GetContextPanel().FindChildrenWithClassTraverse('hud-hint__label'))
{
	hudHintLabel.style.height = '90px';
	hudHintLabel.style.width = '250px';
    hudHintLabel.style.horizontalAlign = 'left';
    hudHintLabel.style.verticalAlign = 'bottom';
    hudHintLabel.style.textAlign = 'left';
    hudHintLabel.style.fontSize = '22px';
    hudHintLabel.style.fontWeight = 'bold';
    hudHintLabel.style.letterSpacing = '0px';
	hudHintLabel.style.fontFamily = 'Stratum2';
	hudHintLabel.style.S2MixBlendMode = 'SRGBadditive';
	hudHintLabel.style.lineHeight = '21px';
}

var hudTeamCounter = contextPanel.FindChildTraverse('HudTeamCounter');
for (var hrTop of hudTeamCounter.FindChildrenWithClassTraverse('hrTop')) {
    hrTop.style.visibility = 'collapse'; // removal
}


var joinPanelCT = hudTeamCounter.FindChildTraverse('JoinPanelCT');
joinPanelCT.style.width = '630px';
joinPanelCT.style.height = '56px';
joinPanelCT.style.horizontalAlign = 'center';
joinPanelCT.style.flowChildren = 'none';
joinPanelCT.style.backgroundImage = `url("${joinPanelCTBG}")`;
joinPanelCT.style.backgroundSize = '100% 100%';

var joinPanelT = hudTeamCounter.FindChildTraverse('JoinPanelT');
joinPanelT.style.width = '630px';
joinPanelT.style.height = '56px';
joinPanelT.style.horizontalAlign = 'center';
joinPanelT.style.flowChildren = 'none';
joinPanelT.style.backgroundImage = `url("${joinPanelTBG}")`;
joinPanelT.style.backgroundSize = '100% 100%';

var joinPanelBot = hudTeamCounter.FindChildTraverse('JoinPanelBot');
joinPanelBot.style.width = '630px';
joinPanelBot.style.height = '56px';
joinPanelBot.style.horizontalAlign = 'center';
joinPanelBot.style.flowChildren = 'none';
joinPanelBot.style.backgroundImage = `url("${joinPanelBotBG}")`;
joinPanelBot.style.backgroundSize = '100% 100%';

var entryTextCt = hudTeamCounter.FindChildTraverse('JoinTextCT');
entryTextCt.style.fontSize = '30px';
entryTextCt.style.fontWeight = 'black';
entryTextCt.style.horizontalAlign = 'center';
entryTextCt.style.verticalAlign = 'top';
entryTextCt.style.letterSpacing = '0px';
entryTextCt.style.marginTop = '12px';
entryTextCt.style.opacity = '0.85';
entryTextCt.style.backgroundColor = 'none';
entryTextCt.style.backgroundImage = 'none';
entryTextCt.style.backgroundSize = '100% 100%';
entryTextCt.style.overflow = 'noclip';
entryTextCt.style.backgroundRepeat = 'no-repeat';
entryTextCt.style.backgroundPosition = '40px -5px';
entryTextCt.style.marginLeft = '0%';
entryTextCt.style.S2MixBlendMode = 'SRGBAdditive';

var entryTextT = hudTeamCounter.FindChildTraverse('JoinTextT');
entryTextT.style.fontSize = '30px';
entryTextT.style.fontWeight = 'black';
entryTextT.style.horizontalAlign = 'center';
entryTextT.style.verticalAlign = 'top';
entryTextT.style.letterSpacing = '0px';
entryTextT.style.marginTop = '12px';
entryTextT.style.opacity = '0.85';
entryTextT.style.backgroundColor = 'none';
entryTextT.style.backgroundImage = 'none';
entryTextT.style.backgroundSize = '100% 100%';
entryTextT.style.overflow = 'noclip';
entryTextT.style.backgroundRepeat = 'no-repeat';
entryTextT.style.backgroundPosition = '40px -5px';
entryTextT.style.S2MixBlendMode = 'SRGBAdditive';

var entryTextBot = hudTeamCounter.FindChildTraverse('JoinTextBot');
entryTextBot.style.fontSize = '30px';
entryTextBot.style.fontWeight = 'black';
entryTextBot.style.horizontalAlign = 'center';
entryTextBot.style.verticalAlign = 'top';
entryTextBot.style.letterSpacing = '0px';
entryTextBot.style.marginTop = '12px';
entryTextBot.style.opacity = '0.85';
entryTextBot.style.backgroundColor = 'none';
entryTextBot.style.backgroundImage = 'none';
entryTextBot.style.backgroundSize = '100% 100%';
entryTextBot.style.overflow = 'noclip';
entryTextBot.style.backgroundRepeat = 'no-repeat';
entryTextBot.style.backgroundPosition = '40px -5px';
entryTextBot.style.marginLeft = '0%';
entryTextBot.style.S2MixBlendMode = 'SRGBAdditive';

// Top alerts (warmup, start etc)

var hal = contextPanel.FindChildTraverse('HudAlerts');
hal.style.width = '883px';
hal.style.height = '44px';
hal.style.marginTop = '-40px';

for (var hrbot of hal.FindChildrenWithClassTraverse('hrBot')) {hrbot.style.visibility = 'collapse';}
for (var hrtop of hal.FindChildrenWithClassTraverse('hrTop')) {hrtop.style.visibility = 'collapse';}

var hal = contextPanel.FindChildTraverse('HudAlerts');
hal.style.width = '883px';
hal.style.height = '44px';
hal.style.marginTop = '-40px';
for (var hrbot of hal.FindChildrenWithClassTraverse('hrBot')) {hrbot.style.visibility = 'collapse';}
for (var hrtop of hal.FindChildrenWithClassTraverse('hrTop')) {hrtop.style.visibility = 'collapse';}

var alertText = contextPanel.FindChildTraverse('AlertText');
alertText.style.opacity = '1';
alertText.style.fontSize = '30px';
alertText.style.fontWeight = 'bold';
alertText.style.verticalAlign = 'center';
alertText.style.fontFamily = 'Stratum2';
alertText.style.letterSpacing = '0px';
alertText.style.marginTop = '5px';

var alertBg = contextPanel.FindChildTraverse('AlertBg');
alertBg.style.opacity = '1';
alertBg.style.height = '44px';
// pack #AlertBg = horizontal fade gradient, no image
alertBg.style.backgroundImage = 'none';
alertBg.style.backgroundColor = 'gradient( linear, 0% 0%, 100% 0%, from( #77777700 ), color-stop( 0.20, #00000099 ), color-stop( 0.40, #00000099 ), color-stop( 0.60, #00000099 ), color-stop( 0.80, #00000099 ), to( #00000000 ) )';
)alert";

#define IS_CT "${isCt}"
#define IS_T "${isT}"
#define PENDING_MVP "${pendingMvp}"
#define IS_2013 "${is2013}"
JS winpanel = R"(
var contextPanel = $.GetContextPanel();
var textColor = ${isCt} ? '#B4BBBE' : '#D6C8B5';
var winPanelFg = contextPanel.FindChildrenWithClassTraverse('WinPanelTeam')[0].FindChildrenWithClassTraverse('TeamFG')[0];
winPanelFg.backgroundColor = '#ffffff00';
var mvpStar = 'https://raw.githubusercontent.com/abandonedpools/scaleform/3e4c1f244351844a6236d952356ea087f59ad29e/p_scaleform/materials/panorama/images/hud/star.svg';
var winpanelBackground = ${isCt} ? 'https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/mvp.png' : ${isT} ? 'https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/mvp2.png' : 'https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/mvp.png';


for (var mvp of contextPanel.FindChildTraverse('HudWinPanel').FindChildrenWithClassTraverse('MVP'))
{
    if (${pendingMvp})
    {
        mvp.style.visibility = 'visible';
        mvp.style.width = '815px';
    } else
    {
        mvp.style.visibility = 'collapse';
    }
}
for (var winPanelBg of contextPanel.FindChildTraverse('HudWinPanel').FindChildrenWithClassTraverse('TeamBG__MainBG')) {
    winPanelBg.style.marginTop = '-42px';
    winPanelBg.style.width = '815px';
    winPanelBg.style.height = '155px';
    winPanelFg.style.marginTop = '-42px';
    winPanelFg.style.width = '815px';
    winPanelFg.style.height = '155px';
	winPanelFg.style.marginLeft = '105px';
    winPanelBg.style.backgroundImage = `url("${winpanelBackground}")`;
    winPanelBg.style.backgroundSize = 'cover';
    winPanelBg.style.marginLeft = '105px';
    winPanelBg.style.transitionProperty = 'brightness';
}
var win_icon = $.GetContextPanel().FindChildrenWithClassTraverse('TeamBG__DefaultLogo')[0];

			win_icon.style.opacity = '0.15';
			win_icon.style.verticalAlign = 'bottom';
			win_icon.style.horizontalAlign = 'right';
			win_icon.style.width = '65px';
			win_icon.style.height = '65px';
			win_icon.style.marginTop = '-4px';
			win_icon.style.x = '-120px';
			win_icon.style.marginBottom = '0px';
			win_icon.style.overflow = 'noclip'
for (var teamTitle of contextPanel.FindChildTraverse('HudWinPanel').FindChildrenWithClassTraverse('TeamFG__Title'))
{
   // we comment this out due to removal of 2013 panels
   //teamTitle.style.visibility = ${is2013} ? 'collapse' : 'visible';
     teamTitle.style.visibility = 'visible';
     teamTitle.style.horizontalAlign = 'center'
     teamTitle.style.verticalAlign = 'top'
	 teamTitle.style.marginTop = '0px';
	 teamTitle.style.color = '#999999'
	 teamTitle.style.fontSize = '38px';
	 teamTitle.style.x = '5px'
     teamTitle.style.fontFamily = 'Stratum2'
	 teamTitle.style.fontWeight = 'bold'
	 teamTitle.style.textShadow = 'none'
	 teamTitle.style.S2MixBlendMode = 'SRGBadditive'
	 teamTitle.style.overflow = 'noclip'
}
for (var star of contextPanel.FindChildTraverse('HudWinPanel').FindChildrenWithClassTraverse('MVP__WinnerStar')) {
    if (${pendingMvp})
    {
        star.style.visibility = ${is2013} ?'collapse' : 'visible';
        star.style.marginTop = "25%";
        star.style.marginLeft = "-2px";
        star.SetImage(mvpStar);
        star.style.transform = 'translateY(5px)';
    } else
    {
        star.style.visibility = 'collapse';
    }
}
for (var playerName of contextPanel.FindChildTraverse('HudWinPanel').FindChildrenWithClassTraverse('MVP__WinnerName')){
    if (${pendingMvp})
    {
        playerName.style.visibility = 'visible';
        playerName.style.marginTop = ${is2013} ?"15px" : "20px";
        playerName.style.color = ${isCt} ?"#717377" : "#8A7E6C";
        playerName.style.marginLeft = ${is2013} ?"-4px" : "2px";
        playerName.style.fontSize = ${is2013} ?"22px" : "17px";
        playerName.style.fontWeight = ${is2013} ?'light' : 'bold';
        playerName.style.transform = 'translateY(5px)';
    } else
    {
        playerName.style.visibility = 'collapse';
    }
}
for (var profileImg of contextPanel.FindChildTraverse('HudWinPanel').FindChildrenWithClassTraverse('MVP__Avatar'))
{
    if (${pendingMvp})
    {
        if (${is2013})
        {
            profileImg.style.visibility = 'visible';
            profileImg.style.zIndex = "500";
            profileImg.style.height = "59px";
            profileImg.style.width = "59px";
            profileImg.style.marginTop = "19px";
            profileImg.style.padding = '0px';
            profileImg.style.paddingTop = '2px';
            profileImg.style.backgroundSize = '50%';
            profileImg.style.marginLeft = "25px";
            profileImg.style.border = '1px solid lightgrey'
            profileImg.style.backgroundColor = 'gradient( linear, 0% 0%, 0% 100%, from( #7d7d7d ), to( #202020) )';
            for (var imgPadding of contextPanel.FindChildTraverse('HudWinPanel').FindChildrenWithClassTraverse('AvatarImage')) {
                imgPadding.style.padding = '0px';
            }
        } else
        {
            profileImg.style.visibility = 'visible';
            profileImg.style.zIndex = "1000";
            profileImg.style.height = "60px";
            profileImg.style.width = "60px";
            profileImg.style.backgroundColor = "#00000000";
            profileImg.style.marginTop = "19px";
            profileImg.style.paddingLeft = "0px";
            profileImg.style.marginLeft = "25px";
        }
    } else
    {
        profileImg.style.visibility = 'collapse';
    }
}
var count = 60
var start = 5 + count
var funFact = contextPanel.FindChildTraverse('FunFactText');
if (!${is2013})
{
    funFact.style.width = '813px';
    funFact.style.height = '35px';
    funFact.style.x = '107px';
    funFact.style.marginTop = `${start}px`;
    funFact.style.backgroundColor = '#000000CC';
    funFact.style.verticalAlign = 'center';
    funFact.style.textAlign = 'center';
    funFact.style.paddingTop = '10px';
    funFact.style.paddingLeft = '0px';
    funFact.style.fontWeight = 'normal';
    funFact.style.fontSize = '16px';
    funFact.style.fontFamily = 'Stratum2'
	funFact.style.fontWeight = 'bold'
    funFact.style.textShadow = 'none'
}
else
{
    funFact.style.width = '813px';
    funFact.style.height = '40px';
    funFact.style.x = '107px';
    funFact.style.marginTop = `${start}px`;
    funFact.style.backgroundColor = '#000000CC';
    funFact.style.textAlign = 'left';
    funFact.style.fontSize = '19px';
    funFact.style.fontWeight = 'light';
    funFact.style.verticalAlign = 'center';
    funFact.style.paddingTop = '10px';
    funFact.style.paddingLeft = '25px';
    funFact.style.fontFamily = 'Stratum2'
	funFact.style.fontWeight = 'bold'
    funFact.style.textShadow = 'none'
}
// is this horrible? yes.
// does it work? yes.
// you win some,
// you lose some.
var time = 0.20
var funfactTime = 1.0
$.Schedule(funfactTime, function() {
	for (var z = 0; z <= 60; ++z)
	{
		$.Schedule(time * (z/60), function() {
			if (count >= 0) {
				var e = 5 + count
				funFact.style.marginTop = `${e}px`
				count -= 1
			}
		})
	}
})
/* <--------- REMOVALS ---------->*/
for (var h of contextPanel.FindChildTraverse('HudWinPanel').FindChildrenWithClassTraverse('TeamBG__hrTop')) {h.style.x = '0';h.style.y = '0';h.style.width = '0';h.style.height = '0';}
for (var h of contextPanel.FindChildTraverse('HudWinPanel').FindChildrenWithClassTraverse('TeamBG__hrMid')) {h.style.x = '0';h.style.y = '0';h.style.width = '0';h.style.height = '0';}
for (var h of contextPanel.FindChildTraverse('HudWinPanel').FindChildrenWithClassTraverse('TeamBG__hrBot')) {h.style.x = '0';h.style.y = '0';h.style.width = '0';h.style.height = '0';}
for (var hideTeamIcon of contextPanel.FindChildTraverse('HudWinPanel').FindChildrenWithClassTraverse('TeamBG__DefaultLogo')){hideTeamIcon.style.visibility ='collapse'; hideTeamIcon.style.opacity = '0';}


/* <--------- FLASHING ----------> */

function firstFlash() {
    for (var star of contextPanel.FindChildTraverse('HudWinPanel').FindChildrenWithClassTraverse('MVP__WinnerStar'))    {
    	star.style.transition = 'wash-color 0.15s linear 0.0s';
    	star.style.washColor = '#FFFFFF';
	}

	winPanelFg.style.transition = 'background-color 0.15s linear 0.0s';
	winPanelFg.style.backgroundColor = '#FFFFFF';
}

function secondFlash() {
    for (var star of contextPanel.FindChildTraverse('HudWinPanel').FindChildrenWithClassTraverse('MVP__WinnerStar'))    {
    	star.style.transition = 'wash-color 0.15s linear 0.0s';
    	star.style.washColor = '#FFD856';
	}

	winPanelFg.style.transition = 'background-color 0.15s linear 0.0s';
	winPanelFg.style.backgroundColor = 'transparent';
}

$.Schedule(0.2, firstFlash);
$.Schedule(0.35, secondFlash);
)";

#define RADAR_COLOR "${radarColor}"
#define DASHBOARD_LABEL_COLOR "${dashboardLabelColor}"
JS color = R"(
var contextPanel = $.GetContextPanel();
var hudRadar = contextPanel.FindChildTraverse('HudRadar');

var roundBorder = hudRadar.FindChildTraverse('Radar__Round--Border');
roundBorder.style.borderRadius = '50% / 50%';
roundBorder.style.borderWidth = '2px';
roundBorder.style.borderStyle = 'solid';
roundBorder.style.borderColor = `${radarColor}`;
roundBorder.style.opacity = '0.70';

var squareBorder = hudRadar.FindChildTraverse('Radar__Square--Border');
squareBorder.style.borderRadius = '0';
squareBorder.style.borderWidth = '2px';
squareBorder.style.borderStyle = 'solid';
squareBorder.style.borderColor = `${radarColor}`;
squareBorder.style.opacity = '0.70';

//using a different panel for radar because it used to be connected to text
var dashboardbg = $.GetContextPanel().FindChildrenWithClassTraverse('hud-radar')[0].GetChild(0)
dashboardbg.style.width = '350px'
dashboardbg.style.height = '46px'
dashboardbg.style.verticalAlign = 'top';
dashboardbg.style.horizontalAlign = 'left';
dashboardbg.style.backgroundImage = 'url(\'https://raw.githubusercontent.com/abandonedpools/scaleform/refs/heads/master/p_scaleform/materials/panorama/images/hud/location_corner.png\')'//radar
dashboardbg.style.backgroundSize = '100% 100%'
dashboardbg.style.transform = 'translateX(-1px)'

)";

JS spectating = R"(
var contextPanel = $.GetContextPanel();

for(var spec_parent of contextPanel.FindChildrenWithClassTraverse('HudSpecplayerParentContainer'))
{
    spec_parent.style.height = '260px';
    spec_parent.style.width = '642px';
}
for(var spec_bg of contextPanel.FindChildrenWithClassTraverse('HudSpecplayerBG'))
{
   spec_bg.style.backgroundColor = '#0000004D';
   spec_bg.style.height = '100px';
   spec_bg.style.verticalAlign = 'top';
   spec_bg.style.overflow = 'noclip'
}

for(var spec_bgb of contextPanel.FindChildrenWithClassTraverse('HudSpecplayerBG__Border'))
{
    spec_bgb.style.backgroundColor = '#0000004D';
}

for(var spec_def_logo of contextPanel.FindChildrenWithClassTraverse('HudSpecplayerBG__DefaultLogo'))
{
    spec_def_logo.style.visibility = 'collapse';
}

for(var spec_top_strip of contextPanel.FindChildrenWithClassTraverse('HudSpecplayerBG__TopStrip'))
{
    spec_top_strip.style.visibility = 'collapse';
}

for(var spec_bottom_strip of contextPanel.FindChildrenWithClassTraverse('HudSpecplayerBG__BottomStrip'))
{
    spec_bottom_strip.style.backgroundColor = '#0000004D';
    spec_bottom_strip.style.height = '24px';
    spec_bottom_strip.style.transform = 'translateY(0px)';
}

for(var fix of contextPanel.FindChildrenWithClassTraverse('HudSpecplayer__ItemContainer'))
{
    fix.style.overflow = 'noclip'
}

function updateText()
{
	for (var row of contextPanel.FindChildrenWithClassTraverse('weapon-row')) {
    	if (row.BHasClass('weapon-row--selected')) {
        	for(var x of row.FindChildrenWithClassTraverse('weapon-selection-item-name-text'))
        	{
            	for(var txt of contextPanel.FindChildrenWithClassTraverse('HudSpecplayer__ItemName'))
            	{
                	txt.text = x.text;
                	txt.style.visibility = 'visible';
                	txt.style.height = 'fit-children';
                	txt.style.verticalAlign = 'bottom';
                	txt.style.backgroundColor = '#00000000';
                	txt.style.transform = 'translateY(13px) translateX(-5px)';
                	txt.style.fontSize = '16px';
                	txt.style.fontWeight = 'light'
            	}
        	}
    	}
	}
}

for(var spec_player_name of contextPanel.FindChildrenWithClassTraverse('HudSpecplayer__player-name'))
{
    spec_player_name.style.verticalAlign = 'center';
    spec_player_name.style.transform = 'translateY(0px)'
    spec_player_name.style.fontSize = '36px';
    spec_player_name.style.fontWeight = 'bold';
    spec_player_name.style.fontFamily = 'Stratum2';
    spec_player_name.style.letterSpacing = '-1px';
    spec_player_name.style.textShadow = '0px 0px 0px 0.0 #000000';
}

for(var spec_root_avatar of contextPanel.FindChildrenWithClassTraverse('HudSpecplayerRoot__avatar'))
{
    spec_root_avatar.style.height = '76px';
    spec_root_avatar.style.width = '76px';
    spec_root_avatar.style.marginTop = '-3px';
    spec_root_avatar.style.border = '0px solid #ff0000';
    spec_root_avatar.style.backgroundColor = 'gradient( linear, 0% 0%, 0% 100%, from( #838383 ), to( #303030 ) )';
    spec_root_avatar.style.padding = '3px'
}
for(var spec_k_hints of contextPanel.FindChildrenWithClassTraverse('HudSpecplayer__key-hints-text'))
{
    spec_k_hints.style.opacity = '1';
    spec_k_hints.style.verticalAlign = 'bottom';
    spec_k_hints.style.transform = 'translateY(63%) translateX(-10px)';
    spec_k_hints.style.fontSize = '17px';
    spec_k_hints.style.fontWeight = 'light';
    spec_k_hints.style.horizontalAlign = 'left';
    spec_k_hints.style.backgroundColor = '#00000000';
}
for(var spec_root_t of contextPanel.FindChildrenWithClassTraverse('HudSpecplayerRoot--TeamT'))
{
    for(var names of spec_root_t.FindChildrenWithClassTraverse('HudSpecplayer__player-name'))
    {
        names.style.color = '#F7F4CB'
    }
}
for(var spec_root_ct of contextPanel.FindChildrenWithClassTraverse('HudSpecplayerRoot--TeamCT'))
{
    for(var names of spec_root_ct.FindChildrenWithClassTraverse('HudSpecplayer__player-name'))
    {
        names.style.color = '#E5F8FF'
    }
}

$.Schedule(0.05, updateText);
)";

#define ALPHA "${alpha}"
JS alpha = R"(
// pack csgostyles.css @define BackgroundOpacity feeds exactly these four hud
// backgrounds, so they all take cl_hud_background_alpha - never hardcode one
var contextPanel = $.GetContextPanel();

// .hud-topleft (radar/location plate)
var dashboardbg = contextPanel.FindChildrenWithClassTraverse('hud-radar')[0].GetChild(0)
dashboardbg.style.backgroundImgOpacity = '${alpha}';

// #AmmoBackground
contextPanel.FindChildTraverse('HudWeaponPanel').FindChildTraverse('WeaponPanelBottomBG').style.backgroundImgOpacity = '${alpha}';

// .money-text-bg (gradient plate, so panel opacity not img opacity)
contextPanel.FindChildTraverse('HudMoney').FindChildTraverse('MoneyBG').style.opacity = '${alpha}';

// .hud-HA-bg1
for (var haBg of contextPanel.FindChildrenWithClassTraverse('hud-HA-bg')) {
	haBg.style.opacity = '${alpha}';
}
)";
