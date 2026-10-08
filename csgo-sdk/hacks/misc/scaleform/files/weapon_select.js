R"(
var weapon_selection_bg = $.GetContextPanel().FindChildrenWithClassTraverse('weapon-row-background');
var weapon_selection = $.GetContextPanel().FindChildrenWithClassTraverse('weapon-row');
var weapon_selection_number = $.GetContextPanel().FindChildrenWithClassTraverse('weapon-row-number');
for (var j = 0; j < 9; j++) { 
    for (rarity of $.GetContextPanel().FindChildrenWithClassTraverse("weapon-selection-item--rarity-" + (j == 8 ? 99 : j))) { 
        if (j == 0)
            rarity.Children()[0].Children()[0].style.imgShadow = '0px 0px 0px 0 transparent';
        for (name of rarity.GetParent().GetParent().FindChildrenWithClassTraverse('weapon-selection-item-name-text')) {
            name.style.textShadowFast = '0px 0px transparent';
            name.style.fontWeight = 'bold';
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
for (var i = 0; i < 8; i++) { 
    if (weapon_selection[i] != undefined && weapon_selection[i].IsSizeValid()) { 
        weapon_selection_bg[i].style.backgroundColor = 'transparent';
        weapon_selection_bg[i].style.backgroundImage = 'url("https://media.discordapp.net/attachments/963223061337886780/990683924957110322/weapon.png")';
        weapon_selection_bg[i].style.backgroundSize = '100% 100%';
        weapon_selection_bg[i].style.width = '320px';
        weapon_selection_bg[i].style.horizontalAlign = 'right';
        weapon_selection[i].style.height = '80px';
        weapon_selection_number[i].style.marginTop = '-28px';
        weapon_selection_number[i].style.textShadowFast = '0px 0px transparent';
        weapon_selection_bg[i].style.opacity = '0.55'; //'{1}';
    }
}
)"