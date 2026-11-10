"""Compile the actual ported target scanner/aim path with controlled native IO.
Checks behavior and Lumia math parity; does not launch or inject the game.
Run under an MSVC x86 developer environment.
"""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2];S=ROOT/'csgo-sdk'
def function(text,signature):
 mask=re.sub(r'//[^\r\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"',lambda m:' '*len(m[0]),text)
 a=text.index(signature);b=mask.index('{',a)+1;depth=1
 while depth:depth+=(mask[b]=='{')-(mask[b]=='}');b+=1
 return text[a:b]
source=(S/'hacks/aimbot/aimbot.cpp').read_text(encoding='utf-8-sig')
header=(S/'hacks/aimbot/aimbot.h').read_text(encoding='utf-8-sig')
header=re.sub(r'^#(?:include|pragma).*\n','',header,flags=re.M)
prefix=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <crtdbg.h>
#include <cstdint>
#include <vector>
#define GET_VARIABLE(v,t) (v)
#define HASH_BT(v) 0
constexpr float pi=3.14159265358979323846f;
float deg2rad(float x){return x*pi/180;}float rad2deg(float x){return x*180/pi;}
struct c_vector {
 float m_x=0,m_y=0,m_z=0;
 c_vector()=default;c_vector(float x,float y,float z):m_x(x),m_y(y),m_z(z){}
 c_vector operator+(c_vector b)const{return {m_x+b.m_x,m_y+b.m_y,m_z+b.m_z};}
 c_vector operator-(c_vector b)const{return {m_x-b.m_x,m_y-b.m_y,m_z-b.m_z};}
 c_vector operator*(float k)const{return {m_x*k,m_y*k,m_z*k};}
 c_vector operator/(float k)const{return *this*(1/k);}
 float dot_product(c_vector b)const{return m_x*b.m_x+m_y*b.m_y+m_z*b.m_z;}
 float length_squared()const{return dot_product(*this);}float length()const{return std::sqrt(length_squared());}
 float length_2d()const{return std::hypot(m_x,m_y);}float dist_to_squared(c_vector b)const{return (*this-b).length_squared();}
 bool is_zero()const{return m_x==0&&m_y==0&&m_z==0;}bool is_valid()const{return std::isfinite(m_x)&&std::isfinite(m_y)&&std::isfinite(m_z);}
 c_vector normalized()const{return *this/std::max(length(),.00001f);}
};
struct c_angle {
 float m_x=0,m_y=0,m_z=0;
 c_angle()=default;c_angle(float x,float y,float z):m_x(x),m_y(y),m_z(z){}
 c_angle operator+(c_angle b)const{return {m_x+b.m_x,m_y+b.m_y,m_z+b.m_z};}
 c_angle operator-(c_angle b)const{return {m_x-b.m_x,m_y-b.m_y,m_z-b.m_z};}
 c_angle operator*(float k)const{return {m_x*k,m_y*k,m_z*k};}
 c_angle& operator-=(c_angle b){return *this=*this-b;}c_angle& operator+=(c_angle b){return *this=*this+b;}
 void normalize(){m_x=std::remainder(m_x,360.f);m_y=std::remainder(m_y,360.f);m_z=0;}
 void clamp(){m_x=std::clamp(m_x,-89.f,89.f);m_y=std::clamp(m_y,-180.f,180.f);m_z=0;}
 bool is_zero()const{return m_x==0&&m_y==0&&m_z==0;}
};
struct c_user_cmd{int m_command_number=1,m_tick_count=1,m_buttons=1;c_angle m_view_point{};float m_forward_move=21,m_side_move=37;};
struct matrix3x4_t{c_vector point;};
struct box_t{int m_bone=0,m_group=1;};
struct set_t{int m_hit_boxes=19;std::array<box_t,19> boxes;box_t* get_hitbox(int i){return i>=0&&i<m_hit_boxes?&boxes[i]:nullptr;}};
struct model_t{int n_bones=19;set_t set;};
struct c_base_entity {
 int index=0,handle=0;bool alive=true,immune=false,allowed=true,bones_ok=true,fire_ready=true;
 int bone_calls=0,ammo=30; c_vector origin{},offset{0,0,64};c_angle punch{};
 model_t model;std::array<c_vector,19> points{};
 bool is_alive(){return alive;}bool is_valid_aim_target(){return alive&&index>1;}bool has_immunity(){return immune;}
 int get_index(){return index;}unsigned get_ref_ehandle(){return handle;}
 int get_active_weapon_handle(){return 99;}short get_item_definition_index(){return 7;}int get_ammo(){return ammo;}
 bool can_shoot(c_base_entity*){return fire_ready;}bool is_scoped(){return true;}
 c_vector get_origin(){return origin;}c_vector get_view_offset(){return offset;}c_vector get_eye_position(bool){return origin+offset;}
 c_angle get_punch(){return punch;}model_t* get_model(){return &model;}
 bool setup_bones(matrix3x4_t* out,int count,int mask,float time);
};
struct ray_t{c_vector start,end;ray_t(c_vector a,c_vector b):start(a),end(b){}};
struct trace_t{c_base_entity* m_hit_entity=nullptr;int m_hitbox=-1;};
struct fire_bullet_data_t{int m_penetrate_count=4;trace_t m_enter_trace;};
enum{in_attack=1,hitbox_head=0,hitbox_stomach=3,hitbox_chest=5,hitbox_pelvis=2,mask_shot=1};
namespace e_client_frame_stage{enum{render_start=5};}
struct key_bind_t{int m_key=1,m_key_style=1;};
struct{
 bool m_aimbot_enable=true,m_aimbot_camlock=false,m_aimbot_silent=false,m_aimbot_no_switch=false;
 bool m_aimbot_autowall=false,m_aimbot_aim_at_backtrack=false,m_backtrack_enable=false,m_aimbot_on_key=false;
 bool m_aimbot_rcs_only_with_aimbot=false,m_edgebug_mouse_lock=false;
 int m_aimbot_rcs_start=1,m_aimbot_min_damage=1;
 float m_aimbot_rcs_pitch=100,m_aimbot_rcs_yaw=100,m_aimbot_silent_step=0;key_bind_t m_aimbot_key;
}g_variables;
struct c_cconvar{float value=2;float get_float(){return value;}}recoil;
struct{c_cconvar* operator[](int){return &recoil;}}g_convars;
c_base_entity local,weapon,enemy,other;c_user_cmd cmd;
namespace n_lagcomp {enum class e_shot{none};struct impl_t{
 struct record_t{matrix3x4_t m_matrix[128];float m_sim_time=0;c_vector m_vec_origin;};
 record_t* m_records[65]{};int commits=0;record_t* committed=nullptr;
 bool live_accepted(){return true;}bool is_hosting(){return true;}bool usable(c_base_entity*,float t){return t>0;}
 void commit_shot(record_t* r){++commits;committed=r;}
};}n_lagcomp::impl_t g_lagcomp;
struct{c_base_entity* m_local=&local;c_user_cmd* m_cmd=&cmd;c_angle old_view_point{};int m_max_allocations=2;n_lagcomp::impl_t::record_t* m_record=nullptr;}g_ctx;
struct Entities{c_base_entity* slots[65]{};template<class T>T* get(int i){return i==99?&weapon:i>0&&i<65?slots[i]:nullptr;}}entities;
struct Globals{float m_interval_per_tick=1.f/64,m_current_time=1,m_real_time=1,m_frame_time=1.f/240;int m_max_clients=3;}globals;
struct ModelInfo{model_t* get_studio_model(model_t* m){return m;}}model_info;
struct Engine{c_angle view{};void get_view_angles(c_angle& a){a=view;}void set_view_angles(c_angle a){view=a;}}engine;
bool hit_selected=false,release_during_scan=false,physical=true;
struct Trace{void clip_ray_to_entity(const ray_t&,int,c_base_entity* e,trace_t* t){if(hit_selected){t->m_hit_entity=e;t->m_hitbox=0;}}}traces;
struct{Entities* m_client_entity_list=&entities;Globals* m_global_vars_base=&globals;ModelInfo* m_model_info=&model_info;Engine* m_engine_client=&engine;Trace* m_engine_trace=&traces;}g_interfaces;
bool c_base_entity::setup_bones(matrix3x4_t* out,int count,int mask,float time){
 ++bone_calls;assert(count==128&&mask==0x7FF00&&time==globals.m_current_time);
 if(release_during_scan)physical=false;
 for(int i=0;i<19;++i)out[i].point=points[i];return bones_ok;
}
struct{void angle_vectors(c_angle a,c_vector* v){const float p=deg2rad(a.m_x),y=deg2rad(a.m_y);*v={std::cos(p)*std::cos(y),std::cos(p)*std::sin(y),-std::sin(p)};}
 c_angle calculate_angle(c_vector a,c_vector b){auto d=b-a;return {rad2deg(std::atan2(-d.m_z,d.length_2d())),rad2deg(std::atan2(d.m_y,d.m_x)),0};}
}g_math;
#include "hacks/aimbot/lumia_aim_logic.h"
short GetAsyncKeyState(int){return physical?short(0x8000):0;}
struct{bool check_input(key_bind_t*){return true;}}g_input;
struct{bool m_found=false;}g_edgebug;
struct{float lock_amount(){return 0;}}g_edge_skip;
void botox_dbg_log(const char*,...){}
void dbg_gate(const char*){}
int g_dbg_players=0,g_dbg_valid=0,g_dbg_immune=0,g_dbg_nobones=0,g_dbg_gated=0,g_dbg_void=0;
float g_dbg_best=0,g_dbg_hc=0,g_dbg_cone=0;
bool player_list_aim_allowed(int index){return entities.slots[index]->allowed;}
c_base_entity* candidate_owner=nullptr;bool obstruction=false,penetration=false;
struct hitbox_resolver_t{
 set_t* m_set=nullptr;c_base_entity* entity=nullptr;
 bool setup(c_base_entity* e){entity=e;m_set=&e->model.set;return true;}
 c_vector position(int hitbox,matrix3x4_t* matrix){candidate_owner=entity;return matrix[m_set->boxes[hitbox].m_bone].point;}
};
struct{float get_damage(c_vector,fire_bullet_data_t* data){data->m_enter_trace.m_hit_entity=obstruction?&local:candidate_owner;data->m_penetrate_count=penetration?3:4;return 100;}}g_auto_wall;
bool passes_gate(c_base_entity*,c_vector,c_vector,bool,int){return !obstruction;}
float recoil_scale(){return recoil.value;}
struct aim_settings_t{float m_fov=30,m_smooth=1,m_rcs=100;}settings;
struct class_vars_t{bool m_sniper=false;}class_vars;
const class_vars_t* weapon_class_vars(short){return &class_vars;}
aim_settings_t weapon_settings(const class_vars_t*){return settings;}
struct hitbox_list_t{std::vector<int> items;void add(int i){items.push_back(i);}auto begin()const{return items.begin();}auto end()const{return items.end();}};
std::vector<int> selected_boxes{0,5};
hitbox_list_t enabled_hitboxes(const class_vars_t*){return {selected_boxes};}
c_base_entity* firing_weapon(){return (cmd.m_buttons&in_attack)?&weapon:nullptr;}
struct fov_scorer_t{c_angle m_view;void setup(c_angle view,c_angle){m_view=view;}};
struct cone_t{float m_inaccuracy=0,m_spread=0;};
struct target_t{c_base_entity* m_entity=nullptr;n_lagcomp::impl_t::record_t* m_record=nullptr;int m_hitbox=0;c_vector m_position;float m_fov=180;};
struct scan_t{fov_scorer_t m_scorer;hitbox_list_t m_hitboxes;cone_t m_cone;float m_fov=180;bool m_live=true,m_records=false;int m_only=0,m_prefer_entity=0,m_prefer_hitbox=-1;bool m_sticky=false;};
struct smoothing_state_t{unsigned target=0,weapon=0;int command=-1,hitbox=-1;c_angle input{},aim{};bool silent=false,settled=false;}s_lumia_smoothing;
#define private public
@HEADER@
#undef private
@FUNCTIONS@
void position(c_base_entity& e,float yaw){
 e.origin={100,100*std::tan(deg2rad(yaw)),0};
 for(int i=0;i<19;++i){e.points[i]=e.origin+c_vector(0,0,i==0?64:55);e.model.set.boxes[i].m_bone=i;}
}
void reset(){
 local={};local.index=1;local.handle=1;weapon={};weapon.handle=99;
 enemy={};enemy.index=2;enemy.handle=102;position(enemy,8);
 other={};other.index=3;other.handle=103;other.alive=false;position(other,12);
 entities={};entities.slots[2]=&enemy;entities.slots[3]=&other;
 g_aimbot={};s_lumia_smoothing={};g_variables={};settings={};engine={};cmd={};g_ctx.old_view_point={};
 recoil.value=2;physical=true;hit_selected=release_during_scan=obstruction=penetration=false;g_lagcomp={};globals={};selected_boxes={0,5};
 g_aimbot.m_rcs_shots=1;
}
scan_t scan(){scan_t s;s.m_hitboxes={{0,5}};s.m_fov=30;return s;}
int main(){
 _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#ifdef _DEBUG
 _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
 // Independent time composition, yaw wrapping, snap endpoints and recoil axes.
 for(int percent:{0,20,50,100}){
  c_angle from{0,179,0},to{0,-179,0};auto half=n_lumia_aim::smooth_angle(from,to,percent,1.f/128);
  auto twice=n_lumia_aim::smooth_angle(half,to,percent,1.f/128),once=n_lumia_aim::smooth_angle(from,to,percent,1.f/64);
  assert(std::fabs(std::remainder(twice.m_y-once.m_y,360.f))<.0001f);
 }
 reset();local.punch={2,3,0};auto correction=n_lumia_aim::apply_rcs({10,20,0},50,25);
 assert(correction.m_x==8&&correction.m_y==18.5f);
 assert(n_lumia_aim::calculate_fov({},{})==0);
 assert(n_lumia_aim::smoothing_percent(1)==0&&n_lumia_aim::smoothing_percent(30)==100);
 // Fresh pose, nearest angle, stale/out-of-body/invalid bones and target filters.
 reset();other.alive=true;position(other,4);auto s=scan();auto picked=scan_targets(s);
 assert(picked.m_entity==&other&&enemy.bone_calls==1&&other.bone_calls==1);
 other.immune=true;assert(scan_targets(s).m_entity==&enemy);other.immune=false;other.allowed=false;assert(scan_targets(s).m_entity==&enemy);
 other.allowed=true;other.bones_ok=false;assert(scan_targets(s).m_entity==&enemy);
 enemy.points[0]={9000,9000,9000};enemy.points[5]=enemy.points[0];assert(!scan_targets(s).m_entity);
 reset();obstruction=true;assert(!scan_targets(scan()).m_entity);obstruction=false;penetration=true;assert(!scan_targets(scan()).m_entity);
 g_variables.m_aimbot_autowall=true;assert(scan_targets(scan()).m_entity==&enemy);
 // No first-shot smoothing bypass, and silent progress carries between ticks.
 reset();settings.m_smooth=30;g_aimbot.on_create_move_post();assert(g_aimbot.m_shot_is_aim&&g_aimbot.m_shot_yaw>0&&g_aimbot.m_shot_yaw<7);
 reset();g_variables.m_aimbot_camlock=true;settings.m_smooth=20;g_aimbot.on_create_move_post();
 const float before_frame=engine.view.m_y;g_aimbot.camlock_frame(e_client_frame_stage::render_start);assert(engine.view.m_y>before_frame);
 const float before_pause=engine.view.m_y;globals.m_frame_time=0;g_aimbot.camlock_frame(e_client_frame_stage::render_start);assert(engine.view.m_y==before_pause);
 reset();g_variables.m_aimbot_silent=true;settings.m_smooth=12;g_variables.m_aimbot_silent_step=.5f;
 float previous=0;for(int i=0;i<25;++i){g_aimbot.on_create_move_post();assert(g_aimbot.m_shot_yaw>previous&&g_aimbot.m_shot_yaw<=previous+.5001f);previous=g_aimbot.m_shot_yaw;++cmd.m_command_number;}
 assert(previous>5&&engine.view.is_zero());
 // Silent RCS respects percentages/start bullet, with no invented attack input.
 for(bool silent:{false,true})for(float strength:{0.f,25.f,100.f}){
  reset();selected_boxes={0};local.punch={2,0,0};settings.m_rcs=strength;g_variables.m_aimbot_silent=silent;
  const int buttons=cmd.m_buttons;g_aimbot.on_create_move_post();assert(std::fabs(g_aimbot.m_shot_pitch+4*strength/100)<.0001f&&cmd.m_buttons==buttons);
 }
 reset();selected_boxes={0};local.punch={2,0,0};g_variables.m_aimbot_rcs_start=2;g_aimbot.on_create_move_post();assert(g_aimbot.m_shot_pitch==0);
 // No-switch holds an unavailable player; slot reuse cannot retain that lock.
 reset();g_variables.m_aimbot_no_switch=true;g_aimbot.on_create_move_post();position(enemy,80);other.alive=true;position(other,3);++cmd.m_command_number;
 g_aimbot.on_create_move_post();assert(!g_aimbot.m_shot_is_aim);enemy.handle=202;++cmd.m_command_number;
 g_aimbot.on_create_move_post();assert(g_aimbot.m_shot_is_aim&&g_aimbot.m_lock_handle==other.handle);
 // Standalone RCS and its existing only-with-aimbot gate stay available.
 reset();g_variables.m_aimbot_enable=false;local.punch={2,0,0};g_variables.m_aimbot_rcs_only_with_aimbot=true;g_aimbot.on_create_move_post();assert(g_aimbot.m_shot_pitch==0);
 g_variables.m_aimbot_rcs_only_with_aimbot=false;g_aimbot.on_create_move_post();assert(g_aimbot.m_shot_pitch==-4);
 // Real physical hold release during fresh-bone work must drop aim ownership.
 reset();g_variables.m_aimbot_on_key=true;release_during_scan=true;g_aimbot.on_create_move_post();assert(!g_aimbot.m_shot_is_aim&&engine.view.is_zero());
 // Preserve the native historical-record route, including usability validation.
 reset();n_lagcomp::impl_t::record_t history[2]{};history[0].m_sim_time=1;history[0].m_vec_origin=enemy.origin;
 for(int i=0;i<19;++i)history[0].m_matrix[i].point=enemy.points[i];g_lagcomp.m_records[2]=history;
 s=scan();s.m_live=false;s.m_records=true;assert(scan_targets(s).m_record==&history[0]);history[0].m_sim_time=0;assert(!scan_targets(s).m_entity);
 puts("PASS: production aim/scanner; fresh poses/FOV, silent progress and step, first-shot smoothing, RCS controls/standalone, no-switch/handles, physical release and backtrack IO");
}
'''
functions='\n'.join(function(source,s) for s in [
 '    bool lumia_live_bones(', '    c_angle rcs_offset(', '\tfloat rcs_unwind(', '    target_t scan_targets(',
 'bool n_aimbot::impl_t::can_aimbot(', 'float n_aimbot::impl_t::rcs_strength(', 'void n_aimbot::impl_t::camlock_frame(', 'void n_aimbot::impl_t::on_create_move_post( )'])
code=prefix.replace('@HEADER@',header).replace('@FUNCTIONS@',functions)
OUT=Path(tempfile.mkdtemp(prefix='botox-lumia-aim-test-'));cpp=OUT/'aim.cpp';cpp.write_text(code,encoding='utf-8')
for name,flags in [('debug',['/Od','/RTC1','/MDd']),('release',['/O2','/fp:fast','/arch:SSE2','/MD'])]:
 exe=OUT/(name+'.exe')
 subprocess.run(['cl','/nologo','/w','/std:c++20','/EHsc',*flags,'/I'+str(S),str(cpp),'/Fo'+str(OUT/(name+'.obj')),'/Fe'+str(exe)],cwd=OUT,check=True)
 subprocess.run([str(exe)],cwd=OUT,check=True)
print('Artifacts:',OUT)
