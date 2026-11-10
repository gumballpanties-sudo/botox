"""Production Botox config codec/registry, with IO redirected to temporary files."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2];S=ROOT/'csgo-sdk'
source=(S/'globals/config/config.cpp').read_text(encoding='utf-8-sig')
def function(signature):
 mask=re.sub(r'//[^\r\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"',lambda m:' '*len(m[0]),source)
 a=source.index(signature);b=mask.index('{',a)+1;depth=1
 while depth:depth+=(mask[b]=='{')-(mask[b]=='}');b+=1
 return source[a:b]
prefix=r'''
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <format>
#include <cstdio>
#include <cstdlib>
#include <crtdbg.h>
#include <cassert>
#include <cstdint>
#include "globals/macros/macros.h"
#include "globals/config/variables.h"
#include "dependencies/json/json.hpp"
struct{void print(const char*){}}g_console;
std::filesystem::path temp_root;
std::filesystem::path test_folder(bool visuals){return visuals?temp_root/"visuals":temp_root;}
'''
functions='\n'.join(function(s) for s in ['static const std::vector< bool >& visual_mask(', 'bool n_config::impl_t::load(', 'bool n_config::impl_t::save(', 'std::size_t n_config::impl_t::get_variable_index('])
functions=functions.replace('this->folder( visuals )','test_folder(visuals)').replace('#include "visual_variables.inc"','#include "globals/config/visual_variables.inc"')
suffix=r'''
int main(int argc,char** argv){
 _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#ifdef _DEBUG
 _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
 assert(argc==2);temp_root=argv[1];
 assert(GET_VARIABLE(g_variables.m_auto_align_type,int)==0);
 GET_VARIABLE(g_variables.m_auto_align_type,int)=1;
 GET_VARIABLE(g_variables.m_auto_align_pixel_jump,bool)=false;
 GET_VARIABLE(g_variables.m_aimbot_general_rcs,float)=37;
 GET_VARIABLE(g_variables.m_aimbot_rifle_override,bool)=true;
 GET_VARIABLE(g_variables.m_aimbot_rifle_rcs,float)=19;
 assert(g_config.save("port.bx"));
 GET_VARIABLE(g_variables.m_auto_align_type,int)=0;GET_VARIABLE(g_variables.m_auto_align_pixel_jump,bool)=true;
 GET_VARIABLE(g_variables.m_aimbot_general_rcs,float)=1;GET_VARIABLE(g_variables.m_aimbot_rifle_rcs,float)=1;
 assert(g_config.load("port.bx"));
 assert(GET_VARIABLE(g_variables.m_auto_align_type,int)==1&&!GET_VARIABLE(g_variables.m_auto_align_pixel_jump,bool));
 assert(GET_VARIABLE(g_variables.m_aimbot_general_rcs,float)==37&&GET_VARIABLE(g_variables.m_aimbot_rifle_rcs,float)==19&&GET_VARIABLE(g_variables.m_aimbot_rifle_override,bool));
 auto entries=nlohmann::json::parse(std::ifstream(temp_root/"port.bx"));
 for(auto it=entries.begin();it!=entries.end();)if((*it)["name-id"]==HASH_BT("m_auto_align_type"))it=entries.erase(it);else ++it;
 std::ofstream(temp_root/"old.bx")<<entries.dump();
 assert(g_config.load("old.bx")&&GET_VARIABLE(g_variables.m_auto_align_type,int)==0);
 // A wrong-type stale record cannot replace std::any's int with a bool.
 entries.push_back({{"name-id",HASH_BT("m_auto_align_type")},{"type-id",HASH_BT("bool")},{"value",true}});
 std::ofstream(temp_root/"wrong.bx")<<entries.dump();GET_VARIABLE(g_variables.m_auto_align_type,int)=1;
 assert(g_config.load("wrong.bx")&&GET_VARIABLE(g_variables.m_auto_align_type,int)==0);
 // Visual-only loads cannot switch movement backends, even for a full .bx.
 std::filesystem::create_directories(temp_root/"visuals");
 std::ofstream(temp_root/"visuals"/"full.bx")<<entries.dump();GET_VARIABLE(g_variables.m_auto_align_type,int)=1;
 assert(g_config.load("full.bx",true)&&GET_VARIABLE(g_variables.m_auto_align_type,int)==1);
 puts("PASS: production config registry/save/load, selector round-trip, default Botox for old/wrong-type files, visual isolation and existing aim overrides/pixeljump option");
}
'''
OUT=Path(tempfile.mkdtemp(prefix='botox-lumia-config-test-'));cpp=OUT/'config.cpp';cpp.write_text(prefix+functions+suffix,encoding='utf-8');exe=OUT/'config.exe'
subprocess.run(['cl','/nologo','/w','/std:c++20','/EHsc','/utf-8','/O2','/MD','/I'+str(S),str(cpp),'/Fo'+str(OUT/'config.obj'),'/Fe'+str(exe)],cwd=OUT,check=True)
subprocess.run([str(exe),str(OUT/'profiles')],cwd=OUT,check=True)
print('Artifacts:',OUT)
