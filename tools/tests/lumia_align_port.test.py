"""Actual Lumia Auto Align and private native-state adapter under controlled IO."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2];S=ROOT/'csgo-sdk'
def stripped(path):return re.sub(r'^#(?:include|pragma).*\n','',path.read_text(encoding='utf-8-sig'),flags=re.M)
fixture=r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <crtdbg.h>
#include <cstring>
#include <cstddef>
#include <string>
#include <vector>
#include "game/sdk/enums/e_field_types.h"
#include "game/sdk/structs/data_map_t.h"
#define GET_VARIABLE(v,t) (v)
constexpr float pi=3.14159265358979323846f;
float deg2rad(float x){return x*pi/180;}float rad2deg(float x){return x*180/pi;}
struct c_angle{float m_x=0,m_y=0,m_z=0;
 c_angle()=default;c_angle(float x,float y,float z):m_x(x),m_y(y),m_z(z){}
 c_angle operator-(c_angle b)const{return {m_x-b.m_x,m_y-b.m_y,m_z-b.m_z};}
 void normalize(){m_x=std::remainder(m_x,360.f);m_y=std::remainder(m_y,360.f);m_z=0;}
};
struct c_vector{float m_x=0,m_y=0,m_z=0;
 c_vector()=default;c_vector(float x,float y,float z):m_x(x),m_y(y),m_z(z){}
 c_vector operator+(c_vector b)const{return {m_x+b.m_x,m_y+b.m_y,m_z+b.m_z};}
 c_vector operator*(float k)const{return {m_x*k,m_y*k,m_z*k};}
 float length_2d()const{return std::hypot(m_x,m_y);}
 c_angle to_angle2()const{return {rad2deg(std::atan2(-m_z,length_2d())),rad2deg(std::atan2(m_y,m_x)),0};}
};
struct c_user_cmd{float m_forward_move=0,m_side_move=0;c_angle m_view_point;int m_buttons=0,m_command_number=1,m_tick_count=1;};
enum{in_forward=8,in_back=16,in_moveleft=512,in_moveright=1024,in_duck=4,in_jump=2,fl_onground=1,mask_playersolid=1};
namespace e_move_types{enum{move_type_walk=2,move_type_ladder=9,move_type_noclip=8};}
data_map_t prediction_map{},ordinary_map{};bool maps_valid=true;
struct collideable_t{c_vector mins{-16,-16,0},maxs{16,16,72};const c_vector& get_obb_mins(){return mins;}const c_vector& get_obb_maxs(){return maxs;}};
struct c_base_entity{
 int padding=0;c_vector origin{16.02f,0,100},absolute{16.02f,0,100},velocity{};collideable_t col;
 c_user_cmd last{};c_user_cmd* current=nullptr;int tick=100,flags=0,move_type=e_move_types::move_type_walk;
 float stamina=12,ordinary=27;bool alive=true;char extra[66000]{};float high_field=83;
 bool is_alive(){return alive;}int& get_flags(){return flags;}int get_move_type(){return move_type;}
 collideable_t* get_collideable(){return &col;}c_vector& get_origin(){return origin;}c_vector& get_velocity(){return velocity;}
 c_vector get_abs_origin(){return absolute;}void set_abs_origin(c_vector p){absolute=p;}
 c_user_cmd& get_last_command(){return last;}c_user_cmd** get_current_command(){return &current;}int& get_tick_base(){return tick;}
 data_map_t* get_prediction_desc_map(){return maps_valid?&prediction_map:nullptr;}data_map_t* get_data_desc_map(){return &ordinary_map;}
}player,baseline;
struct{c_base_entity* m_local=&player;}g_ctx;
struct key_t{int id=0;};
struct{bool m_auto_align=true,m_wall_climb=false,m_air_stuck=false;key_t m_wall_climb_key,m_air_stuck_key,m_pixel_surf_key{1};}g_variables;
bool texturebug=false,ps_held=false,geometry=true,pj_override=false;int pin_mode=0,native_calls=0,trace_calls=0;
struct{bool check_input(key_t* key){return key->id==1&&ps_held;}}g_input;
bool tb_key(){return texturebug;}
struct trace_t{float m_fraction=1;struct{c_vector m_normal;float m_distance=0;}m_plane;c_vector m_end;};
struct c_trace_filter_world{};
struct ray_t{c_vector start,end,mins,maxs;ray_t(c_vector a,c_vector b,c_vector c,c_vector d):start(a),end(b),mins(c),maxs(d){}};
struct Trace{
 void trace_ray(ray_t ray,int,c_trace_filter_world*,trace_t* hit){
  ++trace_calls;*hit={};if(!geometry)return;
  const float support=-ray.mins.m_x,a=ray.start.m_x-support,b=ray.end.m_x-support;
  if(a>=-.0001f&&b<=0&&a>b){hit->m_fraction=std::clamp(a/(a-b),0.f,1.f);hit->m_plane.m_normal={1,0,0};hit->m_end=ray.start+(ray.end+ray.start*-1.f)*hit->m_fraction;}
 }
}engine_trace;
struct Globals{float m_interval_per_tick=1.f/64;}globals;
struct Prediction{int m_commands_predicted=1;c_angle view{7,12,0};void get_local_view_angles(c_angle& out){out=view;}void set_local_view_angles(c_angle& in){view=in;}}prediction;
struct Convar{float get_float(){return 800;}}gravity;
struct Convars{Convar* find_var(const char*){return &gravity;}}convars;
struct{Globals* m_global_vars_base=&globals;Prediction* m_prediction=&prediction;Trace* m_engine_trace=&engine_trace;Convars* m_convar=&convars;void* m_move_helper=&prediction;}g_interfaces;
struct PredictionAdapter{
 float m_bounds_max_speed=1,m_last_max_speed=2,m_real_max_speed=3,m_last_face_dz=4,m_pin_face_dz=5;
 float get_engine_target_predict_z_velocity(){return -400*globals.m_interval_per_tick;}
 void restore_entity_to_predicted_frame(int frame){assert(frame==0);player=baseline;}
 void begin(c_base_entity* p,c_user_cmd* c,bool real,bool lean){
  assert(!real&&!lean);++native_calls;const float dt=globals.m_interval_per_tick,a=deg2rad(c->m_view_point.m_y);
  p->velocity.m_x+=(std::cos(a)*c->m_forward_move+std::sin(a)*c->m_side_move)*12*dt;
  p->velocity.m_y+=(std::sin(a)*c->m_forward_move-std::cos(a)*c->m_side_move)*12*dt;
  p->origin=p->origin+p->velocity*dt;p->origin.m_x=std::max(16.f,p->origin.m_x);p->absolute=p->origin;
  bool pin=pin_mode==1||(pin_mode==2&&(c->m_buttons&in_jump)&&!(c->m_buttons&in_duck))||(pin_mode==3&&(c->m_buttons&in_jump)&&(c->m_buttons&in_duck));
  p->velocity.m_z=pin?get_engine_target_predict_z_velocity():-100;
  p->stamina+=1;p->high_field+=1;p->ordinary+=1;++p->tick;p->last=*c;p->current=c;
  prediction.view=c->m_view_point;m_bounds_max_speed=11;m_last_max_speed=12;m_real_max_speed=13;m_last_face_dz=14;m_pin_face_dz=15;
 }
 void end(c_base_entity* p){p->current=nullptr;}
}g_prediction;
bool wall_detected=false;
bool auto_align_lumia_pixel_jump(c_user_cmd* cmd,const trace_t&,const c_vector&,const c_vector&,const c_vector&){if(pj_override)cmd->m_forward_move=123;return pj_override;}
namespace n_movement{struct impl_t{
 struct{bool is_ladder=false;}m_fireman_data;struct{bool should_pixel_surf=false;}m_pixelsurf_data;
 void auto_align_lumia(c_user_cmd*);
};}
@STATE@
@NATIVE@
@PORT@
void setup(float dt=1.f/64,float gap=.02f){
 player={};player.origin.m_x=player.absolute.m_x=16+gap;baseline=player;
 maps_valid=true;geometry=true;texturebug=ps_held=pj_override=false;pin_mode=0;native_calls=trace_calls=0;
 globals.m_interval_per_tick=dt;g_variables={};g_prediction={};prediction={};wall_detected=false;
 using field=n_lumia_state::layout_t::field_t;static field predicted[2]{},ordinary[1]{};
 predicted[0]={};predicted[0].type=field_float;predicted[0].name="stamina";predicted[0].offset=offsetof(c_base_entity,stamina);predicted[0].count=1;
 predicted[1]=predicted[0];predicted[1].name="high";predicted[1].offset=offsetof(c_base_entity,high_field);
 ordinary[0]=predicted[0];ordinary[0].name="ordinary";ordinary[0].offset=offsetof(c_base_entity,ordinary);
 prediction_map.m_data_desc=reinterpret_cast<type_description_t*>(predicted);prediction_map.m_data_fields=2;
 ordinary_map.m_data_desc=reinterpret_cast<type_description_t*>(ordinary);ordinary_map.m_data_fields=1;
}
void check_restored(const c_base_entity& caller,const PredictionAdapter& adapter,c_angle view){
 assert(std::memcmp(&player,&caller,sizeof(player))==0&&std::memcmp(&g_prediction,&adapter,sizeof(adapter))==0);
 assert(prediction.view.m_x==view.m_x&&prediction.view.m_y==view.m_y);
}
int main(){
 _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#ifdef _DEBUG
 _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
 int pushes=0;
 for(float dt:{1.f/64,1.f/128})for(float yaw:{0.f,90.f,179.f})for(float gap:{.02f,.08f}){
  setup(dt,gap);n_movement::impl_t movement;c_user_cmd c;c.m_view_point.m_y=yaw;
  const auto caller=player;const auto adapter=g_prediction;const auto view=prediction.view;
  movement.auto_align_lumia(&c);check_restored(caller,adapter,view);
  const float a=deg2rad(yaw),x=std::cos(a)*c.m_forward_move+std::sin(a)*c.m_side_move;
  if(!(wall_detected&&x<0&&native_calls>0&&c.m_buttons==0)){printf("push miss dt%f yaw%f gap%f x%f calls%d wall%d buttons%d\n",dt,yaw,gap,x,native_calls,wall_detected,c.m_buttons);fflush(stdout);}
  assert(wall_detected&&x<0&&native_calls>0&&c.m_buttons==0);++pushes;
 }
 // Original freelook and separate standing/crouched jump candidates.
 for(int mode:{1,2,3}){
  setup(1.f/64,0);ps_held=true;pin_mode=mode;n_movement::impl_t movement;c_user_cmd c;c.m_view_point.m_y=90;
  const auto caller=player;const auto adapter=g_prediction;const auto view=prediction.view;
  movement.auto_align_lumia(&c);check_restored(caller,adapter,view);
  assert(native_calls>0&&(std::fabs(c.m_forward_move)+std::fabs(c.m_side_move))>0);
  if(mode==2)assert((c.m_buttons&in_jump)&&!(c.m_buttons&in_duck));
  if(mode==3)assert((c.m_buttons&in_jump)&&(c.m_buttons&in_duck));
 }
 // All guards/no-wall and invalid state imports leave inputs/player unchanged.
 for(int gate=0;gate<7;++gate){
  setup();n_movement::impl_t movement;c_user_cmd c;c.m_forward_move=31;
  if(gate==0)geometry=false;if(gate==1)texturebug=true;if(gate==2)player.flags=fl_onground;
  if(gate==3)player.alive=false;if(gate==4)g_variables.m_auto_align=false;if(gate==5)maps_valid=false;
  if(gate==6)movement.m_fireman_data.is_ladder=true;
  const auto caller=player;const auto adapter=g_prediction;const auto view=prediction.view;
  movement.auto_align_lumia(&c);check_restored(caller,adapter,view);assert(c.m_forward_move==31&&native_calls==0);
 }
 setup();pj_override=true;n_movement::impl_t movement;c_user_cmd c;movement.auto_align_lumia(&c);assert(c.m_forward_move==123&&native_calls==0);
 printf("PASS: %d production close-wall pushes,64/128/views, freelook and standing/crouched jump search, private/full high-offset field restoration, guards and pixeljump-priority routing\n",pushes);
}
'''
port=stripped(S/'hacks/movement/auto_align_lumia.cpp')
fixture=fixture.replace('@STATE@',stripped(S/'hacks/movement/lumia_prediction_state.h')).replace('@NATIVE@',stripped(S/'hacks/prediction/custom/lumia_native_fields.h')).replace('@PORT@',port)
OUT=Path(tempfile.mkdtemp(prefix='botox-lumia-align-test-'));cpp=OUT/'align.cpp';cpp.write_text(fixture,encoding='utf-8')
for name,flags in [('debug',['/Od','/RTC1','/MDd']),('release',['/O2','/fp:fast','/arch:SSE2','/MD'])]:
 exe=OUT/(name+'.exe')
 subprocess.run(['cl','/nologo','/w','/std:c++20','/EHsc',*flags,'/I'+str(S),str(cpp),'/Fo'+str(OUT/(name+'.obj')),'/Fe'+str(exe)],cwd=OUT,check=True)
 subprocess.run([str(exe)],cwd=OUT,check=True)
print('Artifacts:',OUT)
