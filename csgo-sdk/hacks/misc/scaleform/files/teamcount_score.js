R"(
// Teamcount (only score style)
var hudTeamCounter = contextPanel.FindChildTraverse('HudTeamCounter');
hudTeamCounter.style.height = '100%';

var aliveTextT = hudTeamCounter.FindChildTraverse('AliveTextT');
var aliveTextCT = hudTeamCounter.FindChildTraverse('AliveTextCT');

aliveTextT.style.fontSize = '13px';
aliveTextCT.style.fontSize = '13px';

aliveTextT.style.fontWeight = 'light';
aliveTextCT.style.fontWeight = 'light';

aliveTextCT.style.letterSpacing = '0px';
aliveTextT.style.letterSpacing = '0px';

aliveTextCT.style.marginBottom = '-2px';
aliveTextT.style.marginBottom = '-2px';

aliveTextCT.style.marginTop = '-1px';
aliveTextT.style.marginTop = '-1px';

aliveTextT.style.textShadow = '1px 1px 1px 2.0 #000000';
aliveTextCT.style.textShadow = '1px 1px 1px 2.0 #000000';

aliveTextT.style.fontFamily = 'Stratum2';
aliveTextCT.style.fontFamily = 'Stratum2';

aliveTextT.style.color = '#483a2a';
aliveTextCT.style.color = '#454b51';

aliveTextT.style.opacity = '0.57';
aliveTextCT.style.opacity = '0.57';

aliveTextCT.style.horizontalAlign = 'center';
aliveTextT.style.horizontalAlign = 'center';

var winsT = hudTeamCounter.FindChildTraverse('ScoreT');
var winsCT = hudTeamCounter.FindChildTraverse('ScoreCT');
//winsT.style.fontFamily = 'Stratum2 Bold Monodigit';
//winsCT.style.fontFamily = 'Stratum2 Bold Monodigit';

winsT.style.marginTop = '-1px';
winsCT.style.marginTop = '-1px';

winsT.style.color = '#B99636';
winsCT.style.color = '#6f9ec4';

winsT.style.S2MixBlendMode = 'SRGBadditive';
winsCT.style.S2MixBlendMode = 'SRGBadditive';

var timer = hudTeamCounter.FindChildTraverse('TimerText');
//timer.style.fontFamily = 'Stratum2 Bold Monodigit';
//timer.style.marginLeft = '-2px';
timer.style.marginTop = '-3px';
timer.style.marginBottom = '4px';
timer.style.width = '100%';
timer.style.height = '100%';
timer.style.opacity = '1';
timer.style.fontSize = '28px';
timer.style.fontWeight = 'bold';
timer.style.verticalAlign = 'center';
timer.style.fontFamily = 'Stratum2';
timer.style.letterSpacing = '0px';
timer.style.S2MixBlendMode = 'SRGBadditive';
for (var tAlive of contextPanel.FindChildTraverse('HudTeamCounter').FindChildrenWithClassTraverse('TeamCounter__AliveT')) {
    tAlive.style.backgroundImage = 'url("file://{images}/hud/t.png")';
}
for (var ctAlive of contextPanel.FindChildTraverse('HudTeamCounter').FindChildrenWithClassTraverse('TeamCounter__AliveCT')) {
    ctAlive.style.backgroundImage = 'url("file://{images}/hud/ct.png")';
}
for (var scoreText of contextPanel.FindChildTraverse('HudTeamCounter').FindChildrenWithClassTraverse('TeamCounter__AliveCount')) {
    scoreText.style.color = '#ffffff';
}

for (var sentChat of contextPanel.FindChildrenWithClassTraverse('AlertText')) {
    sentChat.style.backgroundColor = 'gradient( linear, 100% 0%, 0% 0%, from( #00000000 ), color-stop( 0.7, #00000077 ), to( #00000077 ) )';
    sentChat.style.textShadow = '1px 1px 2px #00000055';
    sentChat.style.fontFamily = 'Stratum2';
    sentChat.style.fontWeight = 'normal';
    sentChat.style.fontSize = '21.5px';
    sentChat.style.letterSpacing = '.43px';
    sentChat.style.paddingLeft = '28px';
    sentChat.style.paddingBottom = '0px';
}
)"