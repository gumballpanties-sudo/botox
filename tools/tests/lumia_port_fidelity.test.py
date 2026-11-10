"""Audit the port against pinned Lumia algorithms and Botox feature wrappers."""
from pathlib import Path
import subprocess,re,json,hashlib
ROOT=Path(__file__).resolve().parents[2];S=ROOT/'csgo-sdk'
ref=json.loads((Path(__file__).parent/'lumia_port_reference.json').read_text())
def old(path):return subprocess.check_output(['git','show',ref['botox_base']+':csgo-sdk/'+path],cwd=ROOT).decode('utf-8-sig')
def current(path):return (S/path).read_text(encoding='utf-8-sig')
def function(text,signature):
 mask=re.sub(r'//[^\r\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"',lambda m:' '*len(m[0]),text)
 a=text.index(signature);b=mask.index('{',a)+1;depth=1
 while depth:depth+=(mask[b]=='{')-(mask[b]=='}');b+=1
 return text[a:b]
def canonical(text):return re.sub(r'\s+','',re.sub(r'//[^\r\n]*|/\*[\s\S]*?\*/','',text))
def sha(text):return hashlib.sha256(canonical(text).encode()).hexdigest()
math=current('hacks/aimbot/lumia_aim_logic.h')
smooth=function(math,'inline c_angle smooth_angle(');assert sha(smooth[smooth.index('{'):])==ref['lumia_smooth_body']
rcs=function(math,'inline c_angle apply_rcs(');assert sha(rcs[rcs.index('const c_angle punch_component'):])==ref['lumia_rcs_body']
align=function(current('hacks/movement/auto_align_lumia.cpp'),'void n_movement::impl_t::auto_align_lumia(')
search=align[align.index('const float max_radias'):]
search=search.replace('wall_detected=true;','').replace('if(auto_align_lumia_pixel_jump(cmd,trace,start_pos,mins,maxs))return;','')
assert sha(search)==ref['lumia_align_search_body'],'Lumia candidate order/search changed'
botox_align=function(current('hacks/movement/auto_align.cpp'),'void n_movement::impl_t::auto_align( c_user_cmd* cmd )')
botox_align=botox_align.replace('    if(GET_VARIABLE(g_variables.m_auto_align_type,int)==1){wall_detected=false;auto_align_lumia(cmd);return;}','')
assert canonical(botox_align)==canonical(function(old('hacks/movement/auto_align.cpp'),'void n_movement::impl_t::auto_align( c_user_cmd* cmd )'))
before=old('hacks/aimbot/aimbot.cpp');after=current('hacks/aimbot/aimbot.cpp')
wrappers=['const class_vars_t* weapon_class_vars(', 'float scale_fov_with_scope(', 'aim_settings_t weapon_settings(',
 'hitbox_list_t enabled_hitboxes(', 'c_base_entity* firing_weapon(', 'float n_aimbot::impl_t::active_fov(',
 'void n_aimbot::impl_t::run_backtrack(', 'void n_aimbot::impl_t::run_zeusbug(', 'void n_aimbot::impl_t::rcs_track(',
 'float n_aimbot::impl_t::rcs_strength(', 'void n_aimbot::impl_t::run_nospread(', 'void n_aimbot::impl_t::nospread_pre_move(',
 'void n_aimbot::impl_t::nospread_mark_shot(', 'void n_aimbot::impl_t::nospread_frame_stage(']
for name in wrappers:assert canonical(function(before,name))==canonical(function(after,name)),name
for path in ['hacks/menu/menu.cpp','hooks/functions/create_move_proxy.cpp','hacks/aimbot/grenade_aim.cpp','hacks/lagcomp/lagcomp.cpp']:
 assert canonical(current(path))==canonical(old(path)),path
variables=current('globals/config/variables.h')
variables=variables.replace('        ADD_VARIABLE( int, m_auto_align_type, 0 ); // 0 Botox, 1 Lumia.\n','')
assert canonical(variables)==canonical(old('globals/config/variables.h')),'existing settings/registry order changed'
menu=current('hacks/menu/menu_tab_movement.cpp');assert '"botox\\0lumia\\0"' in menu
assert '"pixel jumps over pixelsurfs"' in menu
print('PASS: pinned Lumia smoothing/RCS/search, original Botox alignment,14 existing aim wrappers, aim layout, command hooks, grenade/backtrack and existing settings preserved')
