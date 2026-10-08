R"(
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
	// headshot + penetrate are the only two icons the pack reskins; Suicide keeps the
	// game's own icon_suicide.svg (the old discord link is dead = blank icon)
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
)"
