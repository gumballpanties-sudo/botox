#include "true_motion_blur.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../entity_cache/entity_cache.h"
#include "../viewmodel_bones.h"
#include "../../../globals/includes/includes.h"
#include "depth_source.h"
#include "fx_compat.h"
#include "frame_copy.h"
#include "gpu_timer.h"
#include "render_queue.h"
#include "serial_render.h"
#include "stream_guard.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3dx9.h>
#include <string>

static constexpr int max_players = 10;

static constexpr int constant_registers = 83 + max_players * 5;

static_assert( constant_registers <= 224, "ps_3_0 has 224 float constant registers" );

static_assert( n_viewmodel_bones::max_proxies == 16, "the bone block is sixteen slots wide in both the shader and the frame" );
static_assert( max_players == 10, "the player block is ten slots wide in the shader, in the frame and in the loop that fills it" );

static const char k_shared_head[] =
	"sampler2D s0:register(s0);"
	"sampler2D s1:register(s1);"
	"sampler2D s2:register(s2);"
	"float4 c0:register(c0);"
	"float4 c1:register(c1);"
	"float4 c2:register(c2);"
	"float4 c3:register(c3);"
	"float4 c4:register(c4);"
	"float4 c5:register(c5);"
	"float4 c6:register(c6);"
	"float4 c7:register(c7);"
	"float4 c8:register(c8);"
	"float4 c9:register(c9);"
	"float4 c10:register(c10);"
	"float4 c11:register(c11);"
	"float4 c12:register(c12);"
	"float4 c13:register(c13);"
	"float4 c15:register(c15);"
	"float linear_z(float d){return (c0.x*c0.y)/(c0.y-d*(c0.y-c0.x));}"
	"float3 view_pos(float2 uv,float z){return float3((uv.x*2.0-1.0)*c0.z,(1.0-uv.y*2.0)*c0.w,1.0)*z;}"
	"float3 reproject(float3 p){return float3(dot(c3.xyz,p)+c3.w,dot(c4.xyz,p)+c4.w,dot(c5.xyz,p)+c5.w);}"
	"float2 prev_uv(float3 q){return float2((q.x/(q.z*c1.x))*0.5+0.5,0.5-(q.y/(q.z*c1.y))*0.5);}";

static const char k_bone_head[] =
	"float4 c14:register(c14);"
	"float4 bone[64]:register(c16);"
	"float4 view_to_world[3]:register(c80);"
	"float4 player[50]:register(c83);";

static const char k_info_body[] =
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float raw=tex2Dlod(s0,float4(uv,0,0)).r;"
	"float2 vel=0.0;"
	"float z;"
	"if(raw>=c1.z){"
	"z=linear_z(raw);"
	"float3 p=view_pos(uv,z);"
	"float3 r0=c3.xyz,r1=c4.xyz,r2=c5.xyz,tw=float3(c3.w,c4.w,c5.w);"
	"if(c14.y>0.5){"
	"float3 w=float3(dot(view_to_world[0].xyz,p)+view_to_world[0].w,dot(view_to_world[1].xyz,p)+view_to_world[1].w,"
	"dot(view_to_world[2].xyz,p)+view_to_world[2].w);"
	"[unroll] for(int k=0;k<10;k++){"
	"float3 hit=step(player[k*5].xyz,w)*step(w,player[k*5+1].xyz);"
	"float ins=hit.x*hit.y*hit.z;"
	"r0=lerp(r0,player[k*5+2].xyz,ins);"
	"r1=lerp(r1,player[k*5+3].xyz,ins);"
	"r2=lerp(r2,player[k*5+4].xyz,ins);"
	"tw=lerp(tw,float3(player[k*5+2].w,player[k*5+3].w,player[k*5+4].w),ins);"
	"}"
	"}"
	"float3 q=float3(dot(r0,p)+tw.x,dot(r1,p)+tw.y,dot(r2,p)+tw.z);"
	"if(q.z>=c0.x)vel=(uv-prev_uv(q))*c6.xy*c1.w;"
	"}else{"
	"float d=raw/c1.z;"
	"z=(c9.x*c9.y)/(c9.y-d*(c9.y-c9.x));"
	"float3 p=float3((uv.x*2.0-1.0)*c9.z,(1.0-uv.y*2.0)*c9.w,1.0)*z;"
	"if(c14.x>0.5){"
	"float3 acc=0.0;"
	"float wsum=0.0;"
	"[unroll] for(int i=0;i<16;i++){"
	"float3 dv=p-bone[i*4].xyz;"
	"dv.z*=3.0;"
	"float dd=dot(dv,dv);"
	"float w=1.0/(dd*dd+0.02);"
	"acc+=w*float3(dot(bone[i*4+1].xyz,p)+bone[i*4+1].w,dot(bone[i*4+2].xyz,p)+bone[i*4+2].w,dot(bone[i*4+3].xyz,p)+bone[i*4+3].w);"
	"wsum+=w;"
	"}"
	"float3 q=acc/max(wsum,1e-15);"
	"if(q.z>=c9.x)vel=(uv-float2((q.x/(q.z*c15.z))*0.5+0.5,0.5-(q.y/(q.z*c15.w))*0.5))*c6.xy*c13.w;"
	"}"
	"z=min(z*0.05,c15.y);"
	"}"
	"float l=length(vel);"
	"float h=l*0.5;"
	"float mx=max(c2.y,1e-3);"
	"float n=(h<0.5*mx)?h:(mx-0.25*mx*mx/max(h,1e-3));"
	"n*=saturate((l-0.5)*2.0);"
	"float2 m=vel*(n/max(1e-15,l));"
	"return float4(n,min(z,60000.0),m/max(1e-15,n));"
	"}";

static const char k_tile_h_body[] =
	"sampler2D s0:register(s0);"
	"float4 c2:register(c2);"
	"float4 c6:register(c6);"
	"float4 c7:register(c7);"
	"float4 c15:register(c15);"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float base=floor(uv.x*c7.x)*c2.z;"
	"float best=0.0;"
	"float2 pick=0.0;"
	"float gbest=0.0;"
	"float2 gpick=0.0;"
	"int k=(int)c2.z;"
	"[loop] for(int i=0;i<k;i++){"
	"float4 info=tex2Dlod(s0,float4((base+0.5+(float)i)*c6.z,uv.y,0,0));"
	"float2 m=info.zw*info.x;"
	"float sq=dot(m,m);"
	"if(sq>best){best=sq;pick=m;}"
	"if(info.y<=c15.y&&sq>gbest){gbest=sq;gpick=m;}"
	"}"
	"return float4(pick,gpick);"
	"}";

static const char k_tile_v_body[] =
	"sampler2D s0:register(s0);"
	"float4 c2:register(c2);"
	"float4 c6:register(c6);"
	"float4 c7:register(c7);"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float base=floor(uv.y*c7.y)*c2.z;"
	"float best=0.0;"
	"float2 pick=0.0;"
	"float gbest=0.0;"
	"float2 gpick=0.0;"
	"int k=(int)c2.z;"
	"[loop] for(int i=0;i<k;i++){"
	"float4 row=tex2Dlod(s0,float4(uv.x,(base+0.5+(float)i)*c6.w,0,0));"
	"float sq=dot(row.xy,row.xy);"
	"if(sq>best){best=sq;pick=row.xy;}"
	"float gsq=dot(row.zw,row.zw);"
	"if(gsq>gbest){gbest=gsq;gpick=row.zw;}"
	"}"
	"return float4(pick,gpick);"
	"}";

static const char k_neighbour_body[] =
	"sampler2D s0:register(s0);"
	"float4 c7:register(c7);"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float best=0.0;"
	"float2 pick=0.0;"
	"float gbest=0.0;"
	"float2 gpick=0.0;"
	"[loop] for(int y=-2;y<=2;y++){"
	"[loop] for(int x=-2;x<=2;x++){"
	"float2 offs=float2((float)x,(float)y);"
	"float4 t=tex2Dlod(s0,float4(uv+offs*c7.zw,0,0));"
	"float diag=abs(offs.x*offs.y);"
	"float2 towards=-offs*rsqrt(max(1e-15,dot(offs,offs)));"
	"float sq=dot(t.xy,t.xy);"
	"if(sq>best){"
	"bool ok=true;"
	"if(diag>0.0){"
	"float a=acos(clamp(dot(towards,t.xy*rsqrt(max(1e-15,sq))),-1.0,1.0));"
	"ok=a<0.7854||a>2.3561;"
	"}"
	"if(ok){best=sq;pick=t.xy;}"
	"}"
	"float gsq=dot(t.zw,t.zw);"
	"if(gsq>gbest){"
	"bool gok=true;"
	"if(diag>0.0){"
	"float a=acos(clamp(dot(towards,t.zw*rsqrt(max(1e-15,gsq))),-1.0,1.0));"
	"gok=a<0.7854||a>2.3561;"
	"}"
	"if(gok){gbest=gsq;gpick=t.zw;}"
	"}"
	"}"
	"}"
	"return float4(pick,gpick);"
	"}";

static const char k_gather_body[] =
	"float3 in_color(float3 c){c=saturate(c);c*=c;return c*1.0/(1.5-max(c.r,max(c.g,c.b)));}"
	"float3 out_color(float3 c){return sqrt(saturate((1.5*c)*1.0/(1.0+max(c.r,max(c.g,c.b)))));}"
	"float ign(float2 p){return frac(52.9829189*frac(dot(p+5.588238*c2.w,float2(0.06711056,0.00583715))));}"
	// tile read jittered up to 1/8 tile to hide seams, never diagonally ( halo )
	"float2 tile_offs(float2 p){"
	"float2 s=floor(fmod(p,2.0))*2.0-1.0;"
	"float2 axis=(s.x*s.y<0.0)?float2(1,0):float2(0,1);"
	"return (ign(p+17.0)-0.5)*0.25*c2.z*c6.zw*axis;"
	"}"
	"float3 fringe(float2 uv,float3 c,float edge,float2 wt){"
	"float wl=length(wt);"
	"if(edge<0.004||wl<1.0)return c;"
	"float2 wdir=wt*1.0/(wl);"
	"float wpx=min(wl,max(3.0,c2.y));"
	"float3 sum=0.0;"
	"float w=0.0;"
	"[loop] for(int e=0;e<9;e++){"
	"float2 t=uv+((((float)e+0.5)*(1.0/9.0))-0.5)*2.0*wpx*wdir*c6.zw;"
	"float4 ie=tex2Dlod(s1,float4(t,0,0));"
	"float k=1.0-step(ie.y,c15.y);"
	"sum+=in_color(tex2Dlod(s0,float4(t,0,0)).rgb)*k;"
	"w+=k;"
	"}"
	"return (w<0.5)?c:lerp(c,sum*1.0/(w),edge);"
	"}"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float2 vpos=uv*c6.xy;"
	"float3 cen=in_color(tex2Dlod(s0,float4(uv,0,0)).rgb);"
	"float4 info=tex2Dlod(s1,float4(uv,0,0));"
	"float cen_len=info.x;"
	"float cen_z=info.y;"
	// gun pixels read the gun-only tile ( zw ): gun reaches past itself, the world never reaches in
	"float cen_gun=step(cen_z,c15.y);"
	"float edge=0.0;"
	"if(cen_gun>0.5){"
	"float4 nb=float4(tex2Dlod(s1,float4(uv+float2(c6.z,0.0),0,0)).y,tex2Dlod(s1,float4(uv-float2(c6.z,0.0),0,0)).y,"
	"tex2Dlod(s1,float4(uv+float2(0.0,c6.w),0,0)).y,tex2Dlod(s1,float4(uv-float2(0.0,c6.w),0,0)).y);"
	"edge=saturate((1.0-dot(step(nb,c15.yyyy),0.25))*1.6);"
	"}"
	"float4 tile_pair=tex2Dlod(s2,float4(uv+tile_offs(vpos),0,0));"
	"float2 tile=(cen_gun>0.5)?tile_pair.zw:tile_pair.xy;"
	"float tile_len=length(tile);"
	"if(tile_len<1.0)return float4(out_color(fringe(uv,cen,edge,tile_pair.xy)),1.0);"
	"float2 tile_dir=tile*1.0/(max(1e-15,tile_len));"
	"float agree=(cen_len<1.0)?1.0:abs(dot(info.zw,tile_dir));"
	"float per_px=lerp(2.0,1.0,saturate((agree-0.85)*6.6667));"
	"float half_n=clamp(ceil(tile_len*per_px),3.0,max(3.0,c2.x));"
	"if(fmod(half_n,2.0)<0.5)half_n+=1.0;"
	"float4 tile_main=float4(tile_dir,tile_len,cen_len<1.0?1.0:abs(dot(info.zw,tile_dir)));"
	"float4 cen_main=cen_len<1.0?tile_main:float4(info.zw,cen_len,1.0);"
	// negated on the way back so the two halves don't leave a gap
	"float2 noise=(ign(vpos)-0.5)*float2(1.0,-1.0);"
	"float inv=1.0/(half_n);"
	"float to_step=1.0/(inv*tile_len);"
	"float4 bg=0.0;"
	"float4 fg=0.0;"
	"int n=(int)half_n;"
	"[loop] for(int j=0;j<n;j++){"
	"float4 m=(fmod((float)j,2.0)<0.5)?tile_main:cen_main;"
	"float2 st=(float)j+0.5+noise;"
	"float2 offs=inv*(m.xy*m.z)*c6.zw;"
	"float2 t0=uv-st.x*offs;"
	"float2 t1=uv+st.y*offs;"
	"float4 i0=tex2Dlod(s1,float4(t0,0,0));"
	"float4 i1=tex2Dlod(s1,float4(t1,0,0));"
	"float2 d0=saturate(0.5+c8.y*float2(1.0,-1.0)*(i0.y-cen_z)*1.0/(max(cen_z,1.0)));"
	"float2 d1=saturate(0.5+c8.y*float2(1.0,-1.0)*(i1.y-cen_z)*1.0/(max(cen_z,1.0)));"
	"float2 s0w=saturate(float2(cen_len,i0.x)*to_step-max(0.0,st.x-1.0));"
	"float2 s1w=saturate(float2(cen_len,i1.x)*to_step-max(0.0,st.y-1.0));"
	"float2 w0=d0*s0w*float2(m.w,smoothstep(0.10,0.45,abs(dot(i0.zw,m.xy))));"
	"float2 w1=d1*s1w*float2(m.w,smoothstep(0.10,0.45,abs(dot(i1.zw,m.xy))));"
	// fence fg only: bg term rides the gun's OWN speed = see-through moving edge
	"w0.y*=max(1.0-cen_gun,step(i0.y,c15.y));"
	"w1.y*=max(1.0-cen_gun,step(i1.y,c15.y));"
	"float3 c0v=in_color(tex2Dlod(s0,float4(t0,0,0)).rgb);"
	"float3 c1v=in_color(tex2Dlod(s0,float4(t1,0,0)).rgb);"
	"bool2 mir=bool2(i0.y>i1.y,i1.x>i0.x);"
	"float2 keep=w0;"
	"w0=(mir.x&&mir.y)?w1:keep;"
	"w1=(mir.x||mir.y)?w1:w0;"
	"bg+=float4(c0v,1.0)*w0.x;"
	"fg+=float4(c0v,1.0)*w0.y;"
	"bg+=float4(c1v,1.0)*w1.x;"
	"fg+=float4(c1v,1.0)*w1.y;"
	"}"
	"float total=half_n*2.0+1.0;"
	"float3 fill=(bg.w>1e-4)?bg.rgb*1.0/(bg.w):cen;"
	"bg+=float4(cen,1.0);"
	"float4 acc=(bg+fg)*1.0/(total);"
	"acc.rgb+=saturate(1.0-acc.w)*fill;"
	"return float4(out_color(fringe(uv,acc.rgb,edge,tile_pair.xy)),1.0);"
	"}";

static const char k_accumulate_body[] =
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float4 cur=tex2Dlod(s0,float4(uv,0,0));"
	"float raw=tex2Dlod(s2,float4(uv,0,0)).r;"
	"float2 e=c6.zw*0.5;"
	"float4 around=float4(tex2Dlod(s2,float4(uv+float2(-e.x,-e.y),0,0)).r,tex2Dlod(s2,float4(uv+float2(e.x,-e.y),0,0)).r,"
	"tex2Dlod(s2,float4(uv+float2(-e.x,e.y),0,0)).r,tex2Dlod(s2,float4(uv+float2(e.x,e.y),0,0)).r);"
	"float mask=dot(step(around,c1.zzzz),0.25);"
	"if(raw<c1.z){"
	"if(c8.z<=0.0)return float4(cur.rgb,mask);"
	"float2 g=uv;"
	"if(c13.z>0.5){"
	"float d=raw/c1.z;"
	"float vz=(c9.x*c9.y)/(c9.y-d*(c9.y-c9.x));"
	"float3 p=float3((uv.x*2.0-1.0)*c9.z,(1.0-uv.y*2.0)*c9.w,1.0)*vz;"
	"float3 q=float3(dot(c10.xyz,p)+c10.w,dot(c11.xyz,p)+c11.w,dot(c12.xyz,p)+c12.w);"
	"if(q.z>=c9.x)g=float2((q.x/(q.z*c13.x))*0.5+0.5,0.5-(q.y/(q.z*c13.y))*0.5);"
	"}"
	"if(g.x<0.0||g.x>1.0||g.y<0.0||g.y>1.0)return float4(cur.rgb,mask);"
	"float2 walk=g-uv;"
	"int gn=(int)clamp(ceil(length(walk*c6.xy)*0.5),1.0,8.0);"
	"float4 sum=0.0;"
	"[loop] for(int gi=0;gi<gn;gi++){"
	"float4 hit=tex2Dlod(s1,float4(uv+walk*(((float)gi+0.5)/(float)gn),0,0));"
	"float k=smoothstep(0.35,0.7,hit.a);"
	"sum+=float4(hit.rgb*k,k);"
	"}"
	"sum*=1.0/(float)gn;"
	// sum.a: world history never trails into the gun, and the mask is filtered so the edge ramps
	"return float4(lerp(cur.rgb,sum.rgb*1.0/(max(sum.a,1e-4)),c8.z*sum.a),mask);"
	"}"
	"float4 self=tex2Dlod(s1,float4(uv,0,0));"
	"float gunness=saturate(self.a);"
	"float3 base=lerp(cur.rgb,self.rgb,c8.z*gunness*gunness);"
	"float keep=gunness*c8.z;"
	"float3 q=reproject(view_pos(uv,linear_z(raw)));"
	"if(q.z<c0.x)return float4(base,keep);"
	"float2 prev=prev_uv(q);"
	"if(prev.x<0.0||prev.x>1.0||prev.y<0.0||prev.y>1.0)return float4(base,keep);"
	"float4 history=tex2Dlod(s1,float4(prev,0,0));"
	/* player trail never reads gun history ( a turn would fling the gun over the map ).
	   < 0.5 because the filtered mask smears across the edge. */
	"if(history.a>0.25)return float4(base,keep);"
	"return float4(lerp(base,history.rgb,c8.x),keep);"
	"}";

static const char k_copy_body[] =
	"sampler2D s0:register(s0);"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{return float4(tex2D(s0,uv).rgb,1);}";

static bool create_vector_target( IDirect3DDevice9* device, const unsigned int width, const unsigned int height, IDirect3DTexture9*& texture,
                                  IDirect3DSurface9*& surface )
{
	for ( const D3DFORMAT format : { D3DFMT_A16B16G16R16F, D3DFMT_A32B32G32R32F } ) {
		if ( SUCCEEDED( device->CreateTexture( width, height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &texture, nullptr ) ) &&
		     SUCCEEDED( texture->GetSurfaceLevel( 0, &surface ) ) )
			return true;

		if ( surface ) {
			surface->Release( );
			surface = nullptr;
		}

		if ( texture ) {
			texture->Release( );
			texture = nullptr;
		}
	}

	return false;
}

static void release_target( IDirect3DTexture9*& texture, IDirect3DSurface9*& surface )
{
	if ( surface ) {
		surface->Release( );
		surface = nullptr;
	}

	if ( texture ) {
		texture->Release( );
		texture = nullptr;
	}
}

void n_true_motion_blur::impl_t::release( )
{
	for ( int i = 0; i < 2; ++i )
		release_target( this->m_history_texture[ i ], this->m_history_surface[ i ] );

	release_target( this->m_neighbour_texture, this->m_neighbour_surface );
	release_target( this->m_tile_texture, this->m_tile_surface );
	release_target( this->m_tile_row_texture, this->m_tile_row_surface );
	release_target( this->m_info_texture, this->m_info_surface );
	release_target( this->m_blur_texture, this->m_blur_surface );
	release_target( this->m_stage_texture, this->m_stage_surface );

	if ( this->m_resolve_surface ) {
		this->m_resolve_surface->Release( );
		this->m_resolve_surface = nullptr;
	}

	for ( IDirect3DPixelShader9*& shader : this->m_shaders ) {
		if ( shader ) {
			shader->Release( );
			shader = nullptr;
		}
	}

	this->m_width         = 0;
	this->m_height        = 0;
	this->m_tile_size     = 0;
	this->m_tile_width    = 0;
	this->m_tile_height   = 0;
	this->m_format        = D3DFMT_UNKNOWN;
	this->m_multi_sample  = D3DMULTISAMPLE_NONE;
	this->m_history_index = 0;
	this->m_history_empty = true;
}

void n_true_motion_blur::impl_t::on_device_lost( )
{
	this->release( );

	this->m_have_previous          = false;
	this->m_have_previous_viewmodel = false;
}

IDirect3DPixelShader9* n_true_motion_blur::impl_t::ensure_shader( IDirect3DDevice9* device, const e_shader which )
{
	if ( this->m_shaders[ which ] )
		return this->m_shaders[ which ];

	if ( this->m_compile_failed )
		return nullptr;

	std::string source;

	switch ( which ) {
		case shader_info: source = std::string( k_shared_head ) + k_bone_head + k_info_body; break;
		case shader_tile_h: source = k_tile_h_body; break;
		case shader_tile_v: source = k_tile_v_body; break;
		case shader_neighbour: source = k_neighbour_body; break;
		case shader_gather: source = std::string( k_shared_head ) + k_gather_body; break;
		case shader_accumulate: source = std::string( k_shared_head ) + k_accumulate_body; break;
		default: source = k_copy_body; break;
	}

	ID3DXBuffer* code   = nullptr;
	ID3DXBuffer* errors = nullptr;

	HRESULT result = D3DXCompileShader( source.c_str( ), static_cast< UINT >( source.size( ) ), nullptr, nullptr, "main", "ps_3_0", 0, &code,
	                                    &errors, nullptr );

	if ( FAILED( result ) || !code ) {
		if ( errors ) {
			g_console.print< n_console::log_level::WARNING >( static_cast< const char* >( errors->GetBufferPointer( ) ) );
			errors->Release( );
		}

		if ( code )
			code->Release( );

		this->m_compile_failed = true;
		return nullptr;
	}

	if ( errors )
		errors->Release( );

	result = device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &this->m_shaders[ which ] );
	code->Release( );

	if ( FAILED( result ) || !this->m_shaders[ which ] ) {
		this->m_shaders[ which ] = nullptr;
		this->m_compile_failed   = true;
		return nullptr;
	}

	const int index = static_cast< int >( which );

	g_console.print( std::vformat( "true motion blur: shader {:d} built", std::make_format_args( index ) ).c_str( ) );

	return this->m_shaders[ which ];
}

bool n_true_motion_blur::impl_t::build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description )
{
	const int width  = static_cast< int >( description.Width );
	const int height = static_cast< int >( description.Height );

	if ( this->m_stage_texture && this->m_width == width && this->m_height == height && this->m_format == description.Format &&
	     this->m_multi_sample == description.MultiSampleType )
		return true;

	IDirect3DPixelShader9* kept[ shader_max ]{ };

	for ( int i = 0; i < shader_max; ++i ) {
		kept[ i ]            = this->m_shaders[ i ];
		this->m_shaders[ i ] = nullptr;
	}

	this->release( );

	for ( int i = 0; i < shader_max; ++i )
		this->m_shaders[ i ] = kept[ i ];

	if ( FAILED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, description.Format, D3DPOOL_DEFAULT,
	                                    &this->m_stage_texture, nullptr ) ) ||
	     FAILED( this->m_stage_texture->GetSurfaceLevel( 0, &this->m_stage_surface ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "true motion blur: stage target creation failed" );
		this->release( );
		return false;
	}

	if ( FAILED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
	                                    &this->m_blur_texture, nullptr ) ) ||
	     FAILED( this->m_blur_texture->GetSurfaceLevel( 0, &this->m_blur_surface ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "true motion blur: gather target creation failed" );
		this->release( );
		return false;
	}

	for ( int i = 0; i < 2; ++i ) {
		for ( const D3DFORMAT format : { D3DFMT_A16B16G16R16F, D3DFMT_A8R8G8B8 } ) {
			if ( SUCCEEDED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT,
			                                       &this->m_history_texture[ i ], nullptr ) ) &&
			     SUCCEEDED( this->m_history_texture[ i ]->GetSurfaceLevel( 0, &this->m_history_surface[ i ] ) ) )
				break;

			release_target( this->m_history_texture[ i ], this->m_history_surface[ i ] );
		}

		if ( !this->m_history_texture[ i ] ) {
			g_console.print< n_console::log_level::WARNING >( "true motion blur: history target creation failed" );
			this->release( );
			return false;
		}
	}

	const int tile = std::max( 8, height / 20 );

	const unsigned int tile_width  = static_cast< unsigned int >( ( width + tile - 1 ) / tile );
	const unsigned int tile_height = static_cast< unsigned int >( ( height + tile - 1 ) / tile );

	if ( !create_vector_target( device, description.Width, description.Height, this->m_info_texture, this->m_info_surface ) ||
	     !create_vector_target( device, tile_width, description.Height, this->m_tile_row_texture, this->m_tile_row_surface ) ||
	     !create_vector_target( device, tile_width, tile_height, this->m_tile_texture, this->m_tile_surface ) ||
	     !create_vector_target( device, tile_width, tile_height, this->m_neighbour_texture, this->m_neighbour_surface ) ) {
		g_console.print< n_console::log_level::WARNING >( "true motion blur: no floating point render target — velocity buffer unavailable" );
		this->release( );
		this->m_compile_failed = true;
		return false;
	}

	if ( description.MultiSampleType != D3DMULTISAMPLE_NONE &&
	     FAILED( device->CreateRenderTarget( description.Width, description.Height, description.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
	                                         &this->m_resolve_surface, nullptr ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "true motion blur: msaa resolve target creation failed" );
		this->release( );
		return false;
	}

	this->m_width         = width;
	this->m_height        = height;
	this->m_tile_size     = tile;
	this->m_tile_width    = static_cast< int >( tile_width );
	this->m_tile_height   = static_cast< int >( tile_height );
	this->m_format        = description.Format;
	this->m_multi_sample  = description.MultiSampleType;
	this->m_history_index = 0;
	this->m_history_empty = true;

	const int samples = static_cast< int >( description.MultiSampleType );

	g_console.print(
		std::vformat( "true motion blur: built {:d}x{:d} msaa {:d} tile {:d}", std::make_format_args( width, height, samples, tile ) ).c_str( ) );

	return true;
}

static void power_transform( float rotation[ 3 ][ 3 ], float translation[ 3 ], const float exponent )
{
	if ( std::abs( exponent - 1.f ) < 0.01f )
		return;

	const float trace = rotation[ 0 ][ 0 ] + rotation[ 1 ][ 1 ] + rotation[ 2 ][ 2 ];

	const float angle = std::acos( std::clamp( ( trace - 1.f ) * 0.5f, -1.f, 1.f ) );

	float axis[ 3 ] = { rotation[ 2 ][ 1 ] - rotation[ 1 ][ 2 ], rotation[ 0 ][ 2 ] - rotation[ 2 ][ 0 ],
		                rotation[ 1 ][ 0 ] - rotation[ 0 ][ 1 ] };

	const float sine = std::sqrt( axis[ 0 ] * axis[ 0 ] + axis[ 1 ] * axis[ 1 ] + axis[ 2 ] * axis[ 2 ] ) * 0.5f;

	if ( sine < 1e-6f || angle < 1e-6f ) {
		for ( int axis_index = 0; axis_index < 3; ++axis_index )
			translation[ axis_index ] *= exponent;

		return;
	}

	for ( float& value : axis )
		value /= sine * 2.f;

	const float turned = angle * exponent;

	const float cosine = std::cos( turned );
	const float sined  = std::sin( turned );

	float scaled[ 3 ][ 3 ]{ };

	for ( int row = 0; row < 3; ++row ) {
		for ( int column = 0; column < 3; ++column )
			scaled[ row ][ column ] = ( row == column ? cosine : 0.f ) + ( 1.f - cosine ) * axis[ row ] * axis[ column ];
	}

	scaled[ 0 ][ 1 ] -= sined * axis[ 2 ];
	scaled[ 0 ][ 2 ] += sined * axis[ 1 ];
	scaled[ 1 ][ 0 ] += sined * axis[ 2 ];
	scaled[ 1 ][ 2 ] -= sined * axis[ 0 ];
	scaled[ 2 ][ 0 ] -= sined * axis[ 1 ];
	scaled[ 2 ][ 1 ] += sined * axis[ 0 ];

	const float along = axis[ 0 ] * translation[ 0 ] + axis[ 1 ] * translation[ 1 ] + axis[ 2 ] * translation[ 2 ];

	const float across[ 3 ] = { translation[ 0 ] - axis[ 0 ] * along, translation[ 1 ] - axis[ 1 ] * along,
		                        translation[ 2 ] - axis[ 2 ] * along };

	bool linear = angle < 0.02f;

	float fixed[ 3 ]{ };

	if ( !linear ) {
		float solve[ 3 ][ 3 ]{ };

		for ( int row = 0; row < 3; ++row ) {
			for ( int column = 0; column < 3; ++column )
				solve[ row ][ column ] = ( row == column ? 1.f : 0.f ) - rotation[ row ][ column ] + axis[ row ] * axis[ column ];
		}

		const float determinant = solve[ 0 ][ 0 ] * ( solve[ 1 ][ 1 ] * solve[ 2 ][ 2 ] - solve[ 1 ][ 2 ] * solve[ 2 ][ 1 ] ) -
		                          solve[ 0 ][ 1 ] * ( solve[ 1 ][ 0 ] * solve[ 2 ][ 2 ] - solve[ 1 ][ 2 ] * solve[ 2 ][ 0 ] ) +
		                          solve[ 0 ][ 2 ] * ( solve[ 1 ][ 0 ] * solve[ 2 ][ 1 ] - solve[ 1 ][ 1 ] * solve[ 2 ][ 0 ] );

		if ( std::abs( determinant ) < 1e-8f ) {
			linear = true;
		}
		else {
			const float inverse = 1.f / determinant;

			for ( int column = 0; column < 3; ++column ) {
				float minor[ 3 ][ 3 ]{ };

				for ( int row = 0; row < 3; ++row ) {
					for ( int inner = 0; inner < 3; ++inner )
						minor[ row ][ inner ] = inner == column ? across[ row ] : solve[ row ][ inner ];
				}

				fixed[ column ] = ( minor[ 0 ][ 0 ] * ( minor[ 1 ][ 1 ] * minor[ 2 ][ 2 ] - minor[ 1 ][ 2 ] * minor[ 2 ][ 1 ] ) -
				                    minor[ 0 ][ 1 ] * ( minor[ 1 ][ 0 ] * minor[ 2 ][ 2 ] - minor[ 1 ][ 2 ] * minor[ 2 ][ 0 ] ) +
				                    minor[ 0 ][ 2 ] * ( minor[ 1 ][ 0 ] * minor[ 2 ][ 1 ] - minor[ 1 ][ 1 ] * minor[ 2 ][ 0 ] ) ) *
				                  inverse;
			}

			if ( fixed[ 0 ] * fixed[ 0 ] + fixed[ 1 ] * fixed[ 1 ] + fixed[ 2 ] * fixed[ 2 ] > 4096.f * 4096.f )
				linear = true;
		}
	}

	for ( int row = 0; row < 3; ++row ) {
		translation[ row ] = linear ? translation[ row ] * exponent
		                            : fixed[ row ] + axis[ row ] * along * exponent -
		                                  ( scaled[ row ][ 0 ] * fixed[ 0 ] + scaled[ row ][ 1 ] * fixed[ 1 ] + scaled[ row ][ 2 ] * fixed[ 2 ] );

		for ( int column = 0; column < 3; ++column )
			rotation[ row ][ column ] = scaled[ row ][ column ];
	}
}

static void power_rows( float rows[ 3 ][ 4 ], const float exponent )
{
	float rotation[ 3 ][ 3 ]{ };
	float translation[ 3 ]{ };

	for ( int row = 0; row < 3; ++row ) {
		for ( int column = 0; column < 3; ++column )
			rotation[ row ][ column ] = rows[ row ][ column ];

		translation[ row ] = rows[ row ][ 3 ];
	}

	power_transform( rotation, translation, exponent );

	for ( int row = 0; row < 3; ++row ) {
		for ( int column = 0; column < 3; ++column )
			rows[ row ][ column ] = rotation[ row ][ column ];

		rows[ row ][ 3 ] = translation[ row ];
	}
}

static constexpr float player_box_margin = 12.f;

static constexpr float player_box_floor = 2.f;

static constexpr float player_teleport = 256.f;

void n_true_motion_blur::impl_t::collect_players( frame_t& frame, const float basis[ 3 ][ 3 ], const float origin[ 3 ] )
{
	for ( int row = 0; row < 3; ++row ) {
		frame.m_view_to_world[ row ][ 0 ] = basis[ 0 ][ row ];
		frame.m_view_to_world[ row ][ 1 ] = basis[ 1 ][ row ];
		frame.m_view_to_world[ row ][ 2 ] = basis[ 2 ][ row ];
		frame.m_view_to_world[ row ][ 3 ] = origin[ row ];
	}

	if ( !GET_VARIABLE( g_variables.m_true_motion_blur_players, bool ) || !g_interfaces.m_global_vars_base )
		return;

	struct entry_t {
		float m_distance_squared;
		float m_box[ 2 ][ 4 ];
		float m_rows[ 3 ][ 4 ];
	};

	entry_t entries[ max_player_slots ]{ };
	int total = 0;

	const int this_frame = frame.m_frame;

	g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
		if ( !entity || entity == g_ctx.m_local || entity->is_dormant( ) || !entity->is_alive( ) )
			return;

		const int index = static_cast< int >( entity->get_index( ) );

		if ( index < 1 || index >= max_player_slots )
			return;

		const c_vector where = entity->get_abs_origin( );

		const bool have_previous = this->m_player_frame[ index ] == this_frame - 1;

		const float was[ 3 ] = { this->m_player_pose[ index ][ 0 ], this->m_player_pose[ index ][ 1 ], this->m_player_pose[ index ][ 2 ] };

		this->m_player_frame[ index ]     = this_frame;
		this->m_player_pose[ index ][ 0 ] = where.m_x;
		this->m_player_pose[ index ][ 1 ] = where.m_y;
		this->m_player_pose[ index ][ 2 ] = where.m_z;

		if ( !have_previous || !frame.m_valid || total >= max_player_slots )
			return;

		const float travel[ 3 ] = { where.m_x - was[ 0 ], where.m_y - was[ 1 ], where.m_z - was[ 2 ] };

		if ( travel[ 0 ] * travel[ 0 ] + travel[ 1 ] * travel[ 1 ] + travel[ 2 ] * travel[ 2 ] > player_teleport * player_teleport )
			return;

		entry_t& entry = entries[ total++ ];

		const float shift[ 3 ] = { origin[ 0 ] - this->m_previous_origin[ 0 ] - travel[ 0 ], origin[ 1 ] - this->m_previous_origin[ 1 ] - travel[ 1 ],
			                       origin[ 2 ] - this->m_previous_origin[ 2 ] - travel[ 2 ] };

		for ( int row = 0; row < 3; ++row ) {
			for ( int column = 0; column < 3; ++column )
				entry.m_rows[ row ][ column ] = this->m_previous_basis[ row ][ 0 ] * basis[ column ][ 0 ] +
				                                this->m_previous_basis[ row ][ 1 ] * basis[ column ][ 1 ] +
				                                this->m_previous_basis[ row ][ 2 ] * basis[ column ][ 2 ];

			entry.m_rows[ row ][ 3 ] = this->m_previous_basis[ row ][ 0 ] * shift[ 0 ] + this->m_previous_basis[ row ][ 1 ] * shift[ 1 ] +
			                           this->m_previous_basis[ row ][ 2 ] * shift[ 2 ];
		}

		float mins[ 3 ] = { -16.f, -16.f, 0.f };
		float maxs[ 3 ] = { 16.f, 16.f, 72.f };

		if ( const auto collideable = entity->get_collideable( ) ) {
			const c_vector& hull_mins = collideable->get_obb_mins( );
			const c_vector& hull_maxs = collideable->get_obb_maxs( );

			mins[ 0 ] = hull_mins.m_x;
			mins[ 1 ] = hull_mins.m_y;
			mins[ 2 ] = hull_mins.m_z;

			maxs[ 0 ] = hull_maxs.m_x;
			maxs[ 1 ] = hull_maxs.m_y;
			maxs[ 2 ] = hull_maxs.m_z;
		}

		const float where_at[ 3 ] = { where.m_x, where.m_y, where.m_z };

		for ( int axis = 0; axis < 3; ++axis ) {
			entry.m_box[ 0 ][ axis ] = where_at[ axis ] + mins[ axis ] + ( axis == 2 ? player_box_floor : -player_box_margin );
			entry.m_box[ 1 ][ axis ] = where_at[ axis ] + maxs[ axis ] + player_box_margin;
		}

		const float away[ 3 ] = { where_at[ 0 ] - origin[ 0 ], where_at[ 1 ] - origin[ 1 ], where_at[ 2 ] - origin[ 2 ] };

		entry.m_distance_squared = away[ 0 ] * away[ 0 ] + away[ 1 ] * away[ 1 ] + away[ 2 ] * away[ 2 ];
	} );

	if ( total <= 0 )
		return;

	std::sort( entries, entries + total,
	           []( const entry_t& left, const entry_t& right ) { return left.m_distance_squared < right.m_distance_squared; } );

	const int kept = std::min( total, max_players );

	for ( int slot = 0; slot < kept; ++slot ) {
		const entry_t& entry = entries[ kept - 1 - slot ];

		std::memcpy( frame.m_player_box[ slot ][ 0 ], entry.m_box[ 0 ], sizeof( float ) * 4 );
		std::memcpy( frame.m_player_box[ slot ][ 1 ], entry.m_box[ 1 ], sizeof( float ) * 4 );

		for ( int row = 0; row < 3; ++row )
			std::memcpy( frame.m_player[ slot ][ row ], entry.m_rows[ row ], sizeof( float ) * 4 );
	}

	frame.m_player_count = kept;
}

void n_true_motion_blur::impl_t::sway_sample( const float angles[ 3 ], const float time )
{
	const int frame = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : 0;

	if ( g_interfaces.m_global_vars_base && this->m_sway_filled > 0 && frame == this->m_sway_frame )
		return;

	this->m_sway_frame = frame;

	const int slot = this->m_sway_head;

	this->m_sway_time[ slot ] = time;

	for ( int axis = 0; axis < 3; ++axis )
		this->m_sway_angles[ slot ][ axis ] = angles[ axis ];

	this->m_sway_head = ( slot + 1 ) % sway_samples;

	if ( this->m_sway_filled < sway_samples )
		++this->m_sway_filled;
}

bool n_true_motion_blur::impl_t::sway_angles_at( const float time, float out[ 3 ] ) const
{
	if ( this->m_sway_filled < 2 )
		return false;

	const int newest = ( this->m_sway_head - 1 + sway_samples ) % sway_samples;

	if ( time >= this->m_sway_time[ newest ] ) {
		for ( int axis = 0; axis < 3; ++axis )
			out[ axis ] = this->m_sway_angles[ newest ][ axis ];

		return true;
	}

	int newer = newest;

	for ( int step = 1; step < this->m_sway_filled; ++step ) {
		const int older = ( newest - step + sway_samples ) % sway_samples;

		if ( this->m_sway_time[ older ] > time ) {
			newer = older;
			continue;
		}

		const float span     = this->m_sway_time[ newer ] - this->m_sway_time[ older ];
		const float fraction = span > 1e-6f ? std::clamp( ( time - this->m_sway_time[ older ] ) / span, 0.f, 1.f ) : 0.f;

		for ( int axis = 0; axis < 3; ++axis )
			out[ axis ] = this->m_sway_angles[ older ][ axis ] +
			              g_math.normalize_angle( this->m_sway_angles[ newer ][ axis ] - this->m_sway_angles[ older ][ axis ] ) * fraction;

		return true;
	}

	return false;
}

bool n_true_motion_blur::impl_t::sway_at( const float time, float out[ 3 ] )
{
	out[ 0 ] = out[ 1 ] = out[ 2 ] = 0.f;

	if ( !g_interfaces.m_convar )
		return false;

	static c_cconvar* interp_convar    = g_interfaces.m_convar->find_var( "cl_wpn_sway_interp" );
	static c_cconvar* scale_convar     = g_interfaces.m_convar->find_var( "cl_wpn_sway_scale" );
	static c_cconvar* righthand_convar = g_interfaces.m_convar->find_var( "cl_righthand" );

	if ( !interp_convar || !scale_convar )
		return false;

	const float interp = interp_convar->get_float( );

	if ( interp <= 0.f )
		return false;

	float held[ 3 ]{ };
	float lagged[ 3 ]{ };

	if ( !this->sway_angles_at( time, held ) || !this->sway_angles_at( time - interp, lagged ) )
		return false;

	const c_angle difference( -g_math.normalize_angle( lagged[ 0 ] - held[ 0 ] ), -g_math.normalize_angle( lagged[ 1 ] - held[ 1 ] ),
	                          -g_math.normalize_angle( lagged[ 2 ] - held[ 2 ] ) );

	c_vector lagged_forward{ };
	g_math.angle_vectors( difference, &lagged_forward );

	const float scale = scale_convar->get_float( );

	const float slide[ 3 ] = { ( 1.f - lagged_forward.m_x ) * scale, -lagged_forward.m_y * scale, -lagged_forward.m_z * scale };

	c_vector forward{ }, right{ }, up{ };
	g_math.angle_vectors( c_angle( held[ 0 ], held[ 1 ], held[ 2 ] ), &forward, &right, &up );

	const float fade = std::clamp( std::abs( up.m_z ) - 0.02f, 0.f, 1.f );

	const bool flipped = righthand_convar && !righthand_convar->get_bool( );

	out[ 0 ] = ( flipped ? slide[ 1 ] : -slide[ 1 ] ) * fade;
	out[ 1 ] = slide[ 2 ] * fade;
	out[ 2 ] = slide[ 0 ] * fade;

	return true;
}

bool n_true_motion_blur::impl_t::sway_velocity( const float time, float out[ 3 ] )
{
	out[ 0 ] = out[ 1 ] = out[ 2 ] = 0.f;

	constexpr float window = 0.03f;

	float held[ 3 ]{ };
	float before[ 3 ]{ };

	if ( !this->sway_at( time, held ) || !this->sway_at( time - window, before ) )
		return false;

	for ( int axis = 0; axis < 3; ++axis )
		out[ axis ] = ( held[ axis ] - before[ axis ] ) / window;

	return true;
}

void n_true_motion_blur::impl_t::on_post_screen_space_effects( c_view_setup* setup )
{
	if ( !setup )
		return;

	const bool enabled = GET_VARIABLE( g_variables.m_true_motion_blur, bool ) && !this->m_compile_failed;

	if ( !enabled && !this->m_active ) {
		g_serial_render.idle( );

		this->m_have_previous = false;
		this->m_sway_filled   = 0;
		this->m_sway_head     = 0;
		return;
	}

	frame_t frame{ };

	frame.m_znear = setup->m_znear;
	frame.m_zfar  = setup->m_zfar;
	frame.m_fov   = setup->m_fov;
	frame.m_frame = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : 0;

	frame.m_viewmodel_znear = setup->m_znear_viewmodel;
	frame.m_viewmodel_zfar  = setup->m_zfar_viewmodel;
	frame.m_viewmodel_fov   = setup->m_fov_viewmodel;

	const float now = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;

	matrix3x4_t basis_matrix{ };
	g_math.angle_matrix( setup->m_angles, basis_matrix );

	const float basis[ 3 ][ 3 ] = {
		{ -basis_matrix[ 0 ][ 1 ], -basis_matrix[ 1 ][ 1 ], -basis_matrix[ 2 ][ 1 ] },
		{ basis_matrix[ 0 ][ 2 ], basis_matrix[ 1 ][ 2 ], basis_matrix[ 2 ][ 2 ] },
		{ basis_matrix[ 0 ][ 0 ], basis_matrix[ 1 ][ 0 ], basis_matrix[ 2 ][ 0 ] }
	};

	const float origin[ 3 ] = { setup->m_origin.m_x, setup->m_origin.m_y, setup->m_origin.m_z };

	float viewmodel_origin[ 3 ]{ };
	float viewmodel_basis[ 3 ][ 3 ]{ };

	bool have_viewmodel = false;

	if ( GET_VARIABLE( g_variables.m_true_motion_blur_viewmodel, bool ) && g_ctx.m_local && g_interfaces.m_client_entity_list ) {
		if ( const auto viewmodel =
		         g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_view_model_handle( ) ) ) {
			const c_vector gun_origin = viewmodel->get_abs_origin( );
			const c_angle gun_angles  = viewmodel->get_abs_angles( );

			matrix3x4_t gun_matrix{ };
			g_math.angle_matrix( gun_angles, gun_matrix );

			for ( int row = 0; row < 3; ++row ) {
				for ( int column = 0; column < 3; ++column )
					viewmodel_basis[ row ][ column ] = gun_matrix[ row ][ column ];
			}

			viewmodel_origin[ 0 ] = gun_origin.m_x;
			viewmodel_origin[ 1 ] = gun_origin.m_y;
			viewmodel_origin[ 2 ] = gun_origin.m_z;

			have_viewmodel = true;
		}
	}

	const float sway_angles[ 3 ] = { setup->m_angles.m_x, setup->m_angles.m_y, setup->m_angles.m_z };

	this->sway_sample( sway_angles, now );

	float sway_speed[ 3 ]{ };

	const bool swaying = this->sway_velocity( now, sway_speed );

	const float elapsed = now - this->m_previous_time;

	const float step[ 3 ] = { origin[ 0 ] - this->m_previous_origin[ 0 ], origin[ 1 ] - this->m_previous_origin[ 1 ],
		                      origin[ 2 ] - this->m_previous_origin[ 2 ] };

	const float travelled = std::sqrt( step[ 0 ] * step[ 0 ] + step[ 1 ] * step[ 1 ] + step[ 2 ] * step[ 2 ] );

	frame.m_valid = this->m_have_previous && elapsed > 0.f && elapsed < ( 1.f / 15.f ) && travelled < 128.f;

	this->collect_players( frame, basis, origin );

	if ( frame.m_valid ) {
		frame.m_prev_fov = this->m_previous_fov;

		for ( int row = 0; row < 3; ++row ) {
			for ( int column = 0; column < 3; ++column )
				frame.m_reprojection[ row ][ column ] = this->m_previous_basis[ row ][ 0 ] * basis[ column ][ 0 ] +
				                                        this->m_previous_basis[ row ][ 1 ] * basis[ column ][ 1 ] +
				                                        this->m_previous_basis[ row ][ 2 ] * basis[ column ][ 2 ];

			frame.m_reprojection[ row ][ 3 ] = this->m_previous_basis[ row ][ 0 ] * step[ 0 ] + this->m_previous_basis[ row ][ 1 ] * step[ 1 ] +
			                                   this->m_previous_basis[ row ][ 2 ] * step[ 2 ];
		}

		const float shutter = std::clamp( GET_VARIABLE( g_variables.m_true_motion_blur_shutter, float ), 0.f, 60.f ) * 0.001f;

		frame.m_exposure = std::max( GET_VARIABLE( g_variables.m_true_motion_blur_camera, float ), 0.f ) * std::min( shutter / elapsed, 8.f );

		const float gun_exposure = std::clamp( shutter / elapsed, 0.f, 8.f );

		const float trail = shutter * std::max( GET_VARIABLE( g_variables.m_true_motion_blur_object, float ), 0.f );

		frame.m_blend = trail > 0.f ? std::clamp( std::exp( -elapsed / trail ), 0.f, 0.9f ) : 0.f;

		const float gun_trail = GET_VARIABLE( g_variables.m_true_motion_blur_animation, bool )
		                          ? shutter * std::max( GET_VARIABLE( g_variables.m_true_motion_blur_animation_trail, float ), 0.f )
		                          : 0.f;

		frame.m_viewmodel_blend = gun_trail > 0.f ? std::clamp( std::exp( -elapsed / gun_trail ), 0.f, 0.9f ) : 0.f;

		frame.m_viewmodel_valid = have_viewmodel && this->m_have_previous_viewmodel && frame.m_viewmodel_znear > 0.f &&
		                          frame.m_viewmodel_zfar > frame.m_viewmodel_znear && frame.m_viewmodel_fov > 1.f;

		if ( frame.m_viewmodel_valid ) {
			const float gun_step[ 3 ] = { viewmodel_origin[ 0 ] - this->m_previous_viewmodel_origin[ 0 ],
				                          viewmodel_origin[ 1 ] - this->m_previous_viewmodel_origin[ 1 ],
				                          viewmodel_origin[ 2 ] - this->m_previous_viewmodel_origin[ 2 ] };

			if ( gun_step[ 0 ] * gun_step[ 0 ] + gun_step[ 1 ] * gun_step[ 1 ] + gun_step[ 2 ] * gun_step[ 2 ] > 32.f * 32.f )
				frame.m_viewmodel_valid = false;
		}

		const float sway_amount = std::max( GET_VARIABLE( g_variables.m_true_motion_blur_viewmodel_sway, float ), 0.f );

		float sway_shift[ 3 ]{ };

		const bool sway = sway_amount > 0.f && swaying && frame.m_viewmodel_valid;

		if ( sway ) {
			const float gain =
				sway_amount / std::max( GET_VARIABLE( g_variables.m_true_motion_blur_viewmodel_strength, float ), 0.05f );

			for ( int axis = 0; axis < 3; ++axis )
				sway_shift[ axis ] = -sway_speed[ axis ] * shutter * gain;

			const float slid = std::sqrt( sway_shift[ 0 ] * sway_shift[ 0 ] + sway_shift[ 1 ] * sway_shift[ 1 ] +
			                              sway_shift[ 2 ] * sway_shift[ 2 ] );

			// clamp, never drop: dropping changes the proxy list's shape ( flicker )
			if ( slid > 4.f ) {
				for ( float& axis : sway_shift )
					axis *= 4.f / slid;
			}
		}

		if ( frame.m_viewmodel_valid ) {
			frame.m_viewmodel_prev_fov = this->m_previous_viewmodel_fov;

			float gun_rotation[ 3 ][ 3 ]{ };

			for ( int row = 0; row < 3; ++row ) {
				for ( int column = 0; column < 3; ++column )
					gun_rotation[ row ][ column ] = this->m_previous_viewmodel_basis[ row ][ 0 ] * viewmodel_basis[ column ][ 0 ] +
					                                this->m_previous_viewmodel_basis[ row ][ 1 ] * viewmodel_basis[ column ][ 1 ] +
					                                this->m_previous_viewmodel_basis[ row ][ 2 ] * viewmodel_basis[ column ][ 2 ];
			}

			float world_to_previous[ 3 ][ 3 ]{ };

			for ( int row = 0; row < 3; ++row ) {
				for ( int column = 0; column < 3; ++column )
					world_to_previous[ row ][ column ] = gun_rotation[ row ][ 0 ] * basis[ column ][ 0 ] +
					                                     gun_rotation[ row ][ 1 ] * basis[ column ][ 1 ] +
					                                     gun_rotation[ row ][ 2 ] * basis[ column ][ 2 ];
			}

			float shift[ 3 ]{ };

			for ( int axis = 0; axis < 3; ++axis )
				shift[ axis ] = ( this->m_previous_viewmodel_origin[ axis ] - this->m_previous_origin[ axis ] ) +
				                gun_rotation[ axis ][ 0 ] * ( origin[ 0 ] - viewmodel_origin[ 0 ] ) +
				                gun_rotation[ axis ][ 1 ] * ( origin[ 1 ] - viewmodel_origin[ 1 ] ) +
				                gun_rotation[ axis ][ 2 ] * ( origin[ 2 ] - viewmodel_origin[ 2 ] );

			for ( int row = 0; row < 3; ++row ) {
				for ( int column = 0; column < 3; ++column )
					frame.m_viewmodel_reprojection[ row ][ column ] = this->m_previous_basis[ row ][ 0 ] * world_to_previous[ 0 ][ column ] +
					                                                 this->m_previous_basis[ row ][ 1 ] * world_to_previous[ 1 ][ column ] +
					                                                 this->m_previous_basis[ row ][ 2 ] * world_to_previous[ 2 ][ column ];

				frame.m_viewmodel_reprojection[ row ][ 3 ] = this->m_previous_basis[ row ][ 0 ] * shift[ 0 ] +
				                                             this->m_previous_basis[ row ][ 1 ] * shift[ 1 ] +
				                                             this->m_previous_basis[ row ][ 2 ] * shift[ 2 ];
			}

		}

		const n_viewmodel_bones::set_t bone_set = g_viewmodel_bones.read( );

		constexpr float bone_flat_turn = 1.5708f;
		constexpr float bone_arc_cap   = 0.7854f;

		float worst_turn = 0.f;
		float worst_step = 0.f;

		int flattened = 0;

		if ( frame.m_viewmodel_valid && bone_set.m_count > 0 && bone_set.m_frame >= frame.m_frame - 1 ) {
			frame.m_bone_count = std::min( bone_set.m_count, 16 );

			for ( int index = 0; index < frame.m_bone_count; ++index ) {
				const n_viewmodel_bones::proxy_t& proxy = bone_set.m_proxy[ index ];

				const float trace = proxy.m_rotation[ 0 ][ 0 ] + proxy.m_rotation[ 1 ][ 1 ] + proxy.m_rotation[ 2 ][ 2 ];
				const float turn  = std::acos( std::clamp( ( trace - 1.f ) * 0.5f, -1.f, 1.f ) );

				const bool impossible = turn > bone_flat_turn;

				float rotation[ 3 ][ 3 ]{ };
				float centre_then[ 3 ]{ };

				if ( impossible ) {
					++flattened;

					for ( int axis = 0; axis < 3; ++axis ) {
						rotation[ axis ][ axis ] = 1.f;
						centre_then[ axis ]      = proxy.m_center[ axis ];
					}
				}
				else {
					for ( int row = 0; row < 3; ++row ) {
						for ( int column = 0; column < 3; ++column )
							rotation[ row ][ column ] = proxy.m_rotation[ row ][ column ];

						centre_then[ row ] = proxy.m_center_then[ row ];
					}
				}

				if ( turn * 57.2957795f > worst_turn ) {
					const float moved_by[ 3 ] = { proxy.m_center_then[ 0 ] - proxy.m_center[ 0 ], proxy.m_center_then[ 1 ] - proxy.m_center[ 1 ],
						                          proxy.m_center_then[ 2 ] - proxy.m_center[ 2 ] };

					worst_turn = turn * 57.2957795f;
					worst_step =
						std::sqrt( moved_by[ 0 ] * moved_by[ 0 ] + moved_by[ 1 ] * moved_by[ 1 ] + moved_by[ 2 ] * moved_by[ 2 ] );
				}

				float world_to_view[ 3 ][ 3 ]{ };

				for ( int row = 0; row < 3; ++row ) {
					for ( int column = 0; column < 3; ++column )
						world_to_view[ row ][ column ] = rotation[ row ][ 0 ] * basis[ column ][ 0 ] + rotation[ row ][ 1 ] * basis[ column ][ 1 ] +
						                                 rotation[ row ][ 2 ] * basis[ column ][ 2 ];
				}

				float shift[ 3 ]{ };

				for ( int axis = 0; axis < 3; ++axis )
					shift[ axis ] = rotation[ axis ][ 0 ] * ( origin[ 0 ] - proxy.m_center[ 0 ] ) +
					                rotation[ axis ][ 1 ] * ( origin[ 1 ] - proxy.m_center[ 1 ] ) +
					                rotation[ axis ][ 2 ] * ( origin[ 2 ] - proxy.m_center[ 2 ] ) + centre_then[ axis ] - origin[ axis ];

				for ( int row = 0; row < 3; ++row ) {
					for ( int column = 0; column < 3; ++column )
						frame.m_bone[ index ][ row ][ column ] = basis[ row ][ 0 ] * world_to_view[ 0 ][ column ] +
						                                         basis[ row ][ 1 ] * world_to_view[ 1 ][ column ] +
						                                         basis[ row ][ 2 ] * world_to_view[ 2 ][ column ];

					frame.m_bone[ index ][ row ][ 3 ] =
						basis[ row ][ 0 ] * shift[ 0 ] + basis[ row ][ 1 ] * shift[ 1 ] + basis[ row ][ 2 ] * shift[ 2 ];
				}

				if ( sway ) {
					for ( int row = 0; row < 3; ++row )
						frame.m_bone[ index ][ row ][ 3 ] += sway_shift[ row ];
				}

				const float spin_trace = frame.m_bone[ index ][ 0 ][ 0 ] + frame.m_bone[ index ][ 1 ][ 1 ] + frame.m_bone[ index ][ 2 ][ 2 ];
				const float spin       = std::acos( std::clamp( ( spin_trace - 1.f ) * 0.5f, -1.f, 1.f ) );

				// proxy already spans the shutter ( viewmodel_bones history ), only the arc is capped
				if ( spin > bone_arc_cap )
					power_rows( frame.m_bone[ index ], bone_arc_cap / spin );

				const float away[ 3 ] = { proxy.m_center[ 0 ] - origin[ 0 ], proxy.m_center[ 1 ] - origin[ 1 ],
					                      proxy.m_center[ 2 ] - origin[ 2 ] };

				for ( int axis = 0; axis < 3; ++axis )
					frame.m_bone_center[ index ][ axis ] =
						basis[ axis ][ 0 ] * away[ 0 ] + basis[ axis ][ 1 ] * away[ 1 ] + basis[ axis ][ 2 ] * away[ 2 ];
			}
		}

		if ( sway && frame.m_viewmodel_valid && frame.m_bone_count <= 0 ) {
			frame.m_bone_count = 1;

			for ( int row = 0; row < 3; ++row ) {
				frame.m_bone[ 0 ][ row ][ row ] = 1.f;
				frame.m_bone[ 0 ][ row ][ 3 ]   = sway_shift[ row ];
			}

			const float away[ 3 ] = { viewmodel_origin[ 0 ] - origin[ 0 ], viewmodel_origin[ 1 ] - origin[ 1 ],
				                      viewmodel_origin[ 2 ] - origin[ 2 ] };

			for ( int axis = 0; axis < 3; ++axis )
				frame.m_bone_center[ 0 ][ axis ] =
					basis[ axis ][ 0 ] * away[ 0 ] + basis[ axis ][ 1 ] * away[ 1 ] + basis[ axis ][ 2 ] * away[ 2 ];
		}

		if ( now - this->m_previous_report > 1.f ) {
			this->m_previous_report = now;

			float streak = 0.f;

			if ( frame.m_viewmodel_valid ) {
				const float probe = 20.f;

				float moved[ 3 ]{ };

				for ( int row = 0; row < 3; ++row )
					moved[ row ] = frame.m_viewmodel_reprojection[ row ][ 2 ] * probe + frame.m_viewmodel_reprojection[ row ][ 3 ];

				const float tan_half = std::tan( std::clamp( frame.m_viewmodel_fov, 1.f, 170.f ) * 0.5f * 3.14159265f / 180.f );

				if ( moved[ 2 ] > frame.m_viewmodel_znear ) {
					const float width  = static_cast< float >( setup->m_width > 0 ? setup->m_width : 1 );
					const float height = static_cast< float >( setup->m_height > 0 ? setup->m_height : 1 );

					const float dx = ( moved[ 0 ] / ( moved[ 2 ] * tan_half ) ) * 0.5f * width;
					const float dy = ( moved[ 1 ] / ( moved[ 2 ] * tan_half / ( width / height ) ) ) * 0.5f * height;

					streak = std::sqrt( dx * dx + dy * dy );
				}
			}

			float bone_streak = 0.f;

			int worst = -1;

			float worst_z = 0.f;

			float probe_streak = 0.f;

			if ( frame.m_bone_count > 0 ) {
				const float tan_half = std::tan( std::clamp( frame.m_viewmodel_fov, 1.f, 170.f ) * 0.5f * 3.14159265f / 180.f );

				const float width  = static_cast< float >( setup->m_width > 0 ? setup->m_width : 1 );
				const float height = static_cast< float >( setup->m_height > 0 ? setup->m_height : 1 );

				const float aspect = tan_half / ( width / height );

				float probe[ 3 ]{ };

				int taken = 0;

				for ( int index = 0; index < frame.m_bone_count; ++index ) {
					const float* const centre = frame.m_bone_center[ index ];

					if ( centre[ 2 ] > 2.f ) {
						for ( int axis = 0; axis < 3; ++axis )
							probe[ axis ] += centre[ axis ];

						++taken;
					}

					if ( centre[ 2 ] <= frame.m_viewmodel_znear )
						continue;

					float moved[ 3 ]{ };

					for ( int row = 0; row < 3; ++row )
						moved[ row ] = frame.m_bone[ index ][ row ][ 0 ] * centre[ 0 ] + frame.m_bone[ index ][ row ][ 1 ] * centre[ 1 ] +
						               frame.m_bone[ index ][ row ][ 2 ] * centre[ 2 ] + frame.m_bone[ index ][ row ][ 3 ];

					if ( moved[ 2 ] <= frame.m_viewmodel_znear )
						continue;

					const float dx = ( centre[ 0 ] / ( centre[ 2 ] * tan_half ) - moved[ 0 ] / ( moved[ 2 ] * tan_half ) ) * 0.5f * width;
					const float dy = ( centre[ 1 ] / ( centre[ 2 ] * aspect ) - moved[ 1 ] / ( moved[ 2 ] * aspect ) ) * 0.5f * height;

					const float length = std::sqrt( dx * dx + dy * dy );

					if ( length > bone_streak ) {
						bone_streak = length;
						worst       = index;
						worst_z     = centre[ 2 ];
					}
				}

				if ( taken > 0 ) {
					for ( float& axis : probe )
						axis /= static_cast< float >( taken );

					float accumulated[ 3 ]{ };
					float weight_sum = 0.f;

					for ( int index = 0; index < frame.m_bone_count; ++index ) {
						const float* const centre = frame.m_bone_center[ index ];

						const float away[ 3 ] = { probe[ 0 ] - centre[ 0 ], probe[ 1 ] - centre[ 1 ], ( probe[ 2 ] - centre[ 2 ] ) * 3.f };

						const float squared = away[ 0 ] * away[ 0 ] + away[ 1 ] * away[ 1 ] + away[ 2 ] * away[ 2 ];
						const float weight  = 1.f / ( squared * squared + 0.02f );

						for ( int row = 0; row < 3; ++row )
							accumulated[ row ] += weight * ( frame.m_bone[ index ][ row ][ 0 ] * probe[ 0 ] +
							                                 frame.m_bone[ index ][ row ][ 1 ] * probe[ 1 ] +
							                                 frame.m_bone[ index ][ row ][ 2 ] * probe[ 2 ] + frame.m_bone[ index ][ row ][ 3 ] );

						weight_sum += weight;
					}

					const float moved[ 3 ] = { accumulated[ 0 ] / std::max( weight_sum, 1e-15f ), accumulated[ 1 ] / std::max( weight_sum, 1e-15f ),
						                       accumulated[ 2 ] / std::max( weight_sum, 1e-15f ) };

					if ( probe[ 2 ] > frame.m_viewmodel_znear && moved[ 2 ] > frame.m_viewmodel_znear ) {
						const float dx = ( probe[ 0 ] / ( probe[ 2 ] * tan_half ) - moved[ 0 ] / ( moved[ 2 ] * tan_half ) ) * 0.5f * width;
						const float dy = ( probe[ 1 ] / ( probe[ 2 ] * aspect ) - moved[ 1 ] / ( moved[ 2 ] * aspect ) ) * 0.5f * height;

						probe_streak = std::sqrt( dx * dx + dy * dy );
					}
				}
			}

			const int found = have_viewmodel ? 1 : 0;
			const int valid = frame.m_viewmodel_valid ? 1 : 0;
			const int bones = frame.m_bone_count;

			const float amount = std::clamp( GET_VARIABLE( g_variables.m_true_motion_blur_viewmodel_strength, float ), 0.f, 64.f );
			const float pushed = std::max( streak, probe_streak ) * amount;

			const int players = frame.m_player_count;

			const int weapon_bones = bone_set.m_weapon;

			const int bone_age = bone_set.m_count > 0 ? frame.m_frame - bone_set.m_frame : -1;

			const int sway_on = sway ? 1 : 0;

			const float span_ms = bone_set.m_span * 1000.f;

			const float sway_step =
				std::sqrt( sway_speed[ 0 ] * sway_speed[ 0 ] + sway_speed[ 1 ] * sway_speed[ 1 ] + sway_speed[ 2 ] * sway_speed[ 2 ] );

			g_console.print( std::vformat( "true motion blur: gun found {:d} valid {:d} bones {:d} gun {:d} age {:d} flat {:d} raw {:.2f}px "
			                               "probe {:.2f}px bone {:.2f}px worst {:d} z {:.2f} ang {:.2f} step {:.3f} x{:.1f} = "
			                               "{:.2f}px span {:.2f}ms frames {:.2f} exposure {:.2f} anim {:.2f} players {:d} sway {:d} slide {:.4f}",
			                               std::make_format_args( found, valid, bones, weapon_bones, bone_age, flattened, streak, probe_streak,
			                                                      bone_streak, worst, worst_z, worst_turn, worst_step, amount, pushed,
			                                                      span_ms, gun_exposure, frame.m_exposure, frame.m_viewmodel_blend, players,
			                                                      sway_on, sway_step ) )
			                     .c_str( ) );
		}
	}

	this->m_have_previous_viewmodel  = have_viewmodel;
	this->m_previous_viewmodel_fov   = setup->m_fov_viewmodel;
	this->m_previous_viewmodel_origin[ 0 ] = viewmodel_origin[ 0 ];
	this->m_previous_viewmodel_origin[ 1 ] = viewmodel_origin[ 1 ];
	this->m_previous_viewmodel_origin[ 2 ] = viewmodel_origin[ 2 ];
	std::memcpy( this->m_previous_viewmodel_basis, viewmodel_basis, sizeof( viewmodel_basis ) );

	this->m_have_previous        = true;
	this->m_previous_time        = now;
	this->m_previous_fov         = setup->m_fov;
	this->m_previous_origin[ 0 ] = origin[ 0 ];
	this->m_previous_origin[ 1 ] = origin[ 1 ];
	this->m_previous_origin[ 2 ] = origin[ 2 ];
	std::memcpy( this->m_previous_basis, basis, sizeof( basis ) );

	// always the queue when there is one: raw d3d from here races the render thread ( see ambient_occlusion )
	g_serial_render.idle( );

	const auto pass = [ this, frame ] { n_fx_compat::run( g_interfaces.m_direct_device, "mblur", [ & ] { this->execute( frame ); } ); };

	if ( n_render_queue::submit( pass ) )
		return;

	pass( );
}

namespace
{
	constexpr int k_leak_stages   = 16;
	constexpr int k_leak_samplers = 14;
	constexpr int k_leak_states   = 210;

	struct device_snapshot_t {
		DWORD m_render[ k_leak_states ];
		DWORD m_sampler[ k_leak_stages ][ k_leak_samplers ];
		void* m_texture[ k_leak_stages ];
		float m_constant[ constant_registers ][ 4 ];
		void* m_pixel_shader;
		void* m_vertex_shader;
		void* m_declaration;
		void* m_target;
		void* m_depth;
		void* m_stream;
		UINT m_offset, m_stride;
		DWORD m_fvf;
		D3DVIEWPORT9 m_viewport;
	};

	template < class T >
	void* peek( T* object )
	{
		if ( object )
			object->Release( );

		return object;
	}

	void take_snapshot( IDirect3DDevice9* device, device_snapshot_t& out )
	{
		std::memset( &out, 0, sizeof( out ) );

		for ( int state = 0; state < k_leak_states; ++state )
			device->GetRenderState( static_cast< D3DRENDERSTATETYPE >( state ), &out.m_render[ state ] );

		for ( int stage = 0; stage < k_leak_stages; ++stage ) {
			for ( int sampler = 1; sampler < k_leak_samplers; ++sampler )
				device->GetSamplerState( stage, static_cast< D3DSAMPLERSTATETYPE >( sampler ), &out.m_sampler[ stage ][ sampler ] );

			IDirect3DBaseTexture9* texture = nullptr;
			device->GetTexture( stage, &texture );
			out.m_texture[ stage ] = peek( texture );
		}

		device->GetPixelShaderConstantF( 0, out.m_constant[ 0 ], constant_registers );

		IDirect3DPixelShader9* pixel_shader = nullptr;
		IDirect3DVertexShader9* vertex_shader = nullptr;
		IDirect3DVertexDeclaration9* declaration = nullptr;
		IDirect3DSurface9* target = nullptr;
		IDirect3DSurface9* depth = nullptr;
		IDirect3DVertexBuffer9* stream = nullptr;

		device->GetPixelShader( &pixel_shader );
		device->GetVertexShader( &vertex_shader );
		device->GetVertexDeclaration( &declaration );
		device->GetRenderTarget( 0, &target );
		device->GetDepthStencilSurface( &depth );
		device->GetStreamSource( 0, &stream, &out.m_offset, &out.m_stride );
		device->GetFVF( &out.m_fvf );
		device->GetViewport( &out.m_viewport );

		out.m_pixel_shader  = peek( pixel_shader );
		out.m_vertex_shader = peek( vertex_shader );
		out.m_declaration   = peek( declaration );
		out.m_target        = peek( target );
		out.m_depth         = peek( depth );
		out.m_stream        = peek( stream );
	}

	// one line a second naming every state the pass left different from how it found it
	void report_leaks( const device_snapshot_t& before, const device_snapshot_t& after )
	{
		static float last = -10.f;

		const float now = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;

		std::string line;
		int count = 0;

		const auto add = [ & ]( const std::string& what ) {
			if ( ++count <= 24 )
				line += " " + what;
		};

		for ( int state = 0; state < k_leak_states; ++state ) {
			if ( before.m_render[ state ] != after.m_render[ state ] )
				add( std::vformat( "rs{:d}={:#x}->{:#x}", std::make_format_args( state, before.m_render[ state ], after.m_render[ state ] ) ) );
		}

		for ( int stage = 0; stage < k_leak_stages; ++stage ) {
			for ( int sampler = 1; sampler < k_leak_samplers; ++sampler ) {
				if ( before.m_sampler[ stage ][ sampler ] != after.m_sampler[ stage ][ sampler ] )
					add( std::vformat( "ss{:d}.{:d}={:#x}->{:#x}",
					                   std::make_format_args( stage, sampler, before.m_sampler[ stage ][ sampler ], after.m_sampler[ stage ][ sampler ] ) ) );
			}

			if ( before.m_texture[ stage ] != after.m_texture[ stage ] )
				add( std::vformat( "tex{:d}", std::make_format_args( stage ) ) );
		}

		for ( int reg = 0; reg < constant_registers; ++reg ) {
			if ( std::memcmp( before.m_constant[ reg ], after.m_constant[ reg ], sizeof( before.m_constant[ reg ] ) ) )
				add( std::vformat( "c{:d}", std::make_format_args( reg ) ) );
		}

		if ( before.m_pixel_shader != after.m_pixel_shader )
			add( "ps" );
		if ( before.m_vertex_shader != after.m_vertex_shader )
			add( "vs" );
		if ( before.m_declaration != after.m_declaration )
			add( "decl" );
		if ( before.m_fvf != after.m_fvf )
			add( "fvf" );
		if ( before.m_target != after.m_target )
			add( "rt0" );
		if ( before.m_depth != after.m_depth )
			add( "ds" );
		if ( before.m_stream != after.m_stream || before.m_offset != after.m_offset || before.m_stride != after.m_stride )
			add( "stream0" );
		if ( std::memcmp( &before.m_viewport, &after.m_viewport, sizeof( before.m_viewport ) ) )
			add( "viewport" );

		if ( now - last < 1.f )
			return;

		last = now;

		g_console.print( std::vformat( "true motion blur: leak {:d}{:s}", std::make_format_args( count, line ) ).c_str( ) );
	}
}

void n_true_motion_blur::impl_t::execute( const frame_t frame )
{
	IDirect3DDevice9* device = g_interfaces.m_direct_device;

	if ( !device )
		return;

	const n_gpu_timer::scope_t gpu_timer( device, n_gpu_timer::pass_motion_blur );

	if ( !GET_VARIABLE( g_variables.m_true_motion_blur, bool ) || this->m_compile_failed ) {
		if ( this->m_stage_texture )
			this->release( );

		this->m_active = false;
		return;
	}

	IDirect3DSurface9* target = nullptr;

	if ( FAILED( device->GetRenderTarget( 0, &target ) ) || !target )
		return;

	D3DSURFACE_DESC description{ };

	if ( FAILED( target->GetDesc( &description ) ) ) {
		target->Release( );
		return;
	}

	if ( !this->build( device, description ) ) {
		target->Release( );
		return;
	}

	this->m_active = true;

	if ( !frame.m_valid )
		this->m_history_empty = true;

	const bool blur_gun_velocity = frame.m_valid && frame.m_viewmodel_valid && GET_VARIABLE( g_variables.m_true_motion_blur_viewmodel, bool ) &&
	                               GET_VARIABLE( g_variables.m_true_motion_blur_viewmodel_strength, float ) > 0.f;

	const bool blur_camera = frame.m_valid && ( frame.m_exposure > 0.f || blur_gun_velocity );
	const bool blur_object = frame.m_valid && frame.m_blend > 0.01f && !this->m_history_empty;
	const bool blur_gun    = frame.m_valid && frame.m_viewmodel_blend > 0.01f && !this->m_history_empty;

	const bool keep_history = frame.m_blend > 0.01f || frame.m_viewmodel_blend > 0.01f;

	if ( !keep_history )
		this->m_history_empty = true;

	if ( !blur_camera && !keep_history ) {
		target->Release( );
		return;
	}

	IDirect3DPixelShader9* info       = blur_camera ? this->ensure_shader( device, shader_info ) : nullptr;
	IDirect3DPixelShader9* tile_h     = blur_camera ? this->ensure_shader( device, shader_tile_h ) : nullptr;
	IDirect3DPixelShader9* tile_v     = blur_camera ? this->ensure_shader( device, shader_tile_v ) : nullptr;
	IDirect3DPixelShader9* neighbour  = blur_camera ? this->ensure_shader( device, shader_neighbour ) : nullptr;
	IDirect3DPixelShader9* gather     = blur_camera ? this->ensure_shader( device, shader_gather ) : nullptr;
	IDirect3DPixelShader9* accumulate = keep_history ? this->ensure_shader( device, shader_accumulate ) : nullptr;
	IDirect3DPixelShader9* copy       = this->ensure_shader( device, shader_copy );

	if ( ( blur_camera && ( !info || !tile_h || !tile_v || !neighbour || !gather ) ) || ( keep_history && !accumulate ) || !copy ) {
		target->Release( );
		return;
	}

	IDirect3DTexture9* depth = g_depth_source.ensure( device, description ) ? g_depth_source.texture( ) : nullptr;

	if ( !depth ) {
		static bool logged_depth = false;

		if ( !logged_depth ) {
			logged_depth = true;
			g_console.print( "true motion blur: no depth target yet — load a map ( or change resolution ) once with this on" );
		}

		target->Release( );
		return;
	}

	// full frame sized, or we are sampling something that was never the scene's depth
	D3DSURFACE_DESC depth_description{ };

	if ( FAILED( depth->GetLevelDesc( 0, &depth_description ) ) || depth_description.Width != description.Width ||
	     depth_description.Height != description.Height ) {
		static bool logged_size = false;

		if ( !logged_size ) {
			logged_size = true;
			g_console.print< n_console::log_level::WARNING >( "true motion blur: depth target size does not match the frame" );
		}

		target->Release( );
		return;
	}

	const long blit_result = n_frame_copy::copy( device, target, this->m_resolve_surface, this->m_stage_surface );

	if ( FAILED( blit_result ) ) {
		static bool logged_blit = false;

		if ( !logged_blit ) {
			logged_blit = true;

			const unsigned long code = static_cast< unsigned long >( blit_result );

			g_console.print< n_console::log_level::WARNING >(
				std::vformat( "true motion blur: frame blit failed {:#x}", std::make_format_args( code ) ).c_str( ) );
		}

		target->Release( );
		return;
	}

	static device_snapshot_t leak_before{ };
	static device_snapshot_t leak_after{ };

	take_snapshot( device, leak_before );

	IDirect3DSurface9* old_depth_stencil                = nullptr;
	IDirect3DPixelShader9* old_pixel_shader             = nullptr;
	IDirect3DVertexShader9* old_vertex_shader           = nullptr;
	IDirect3DBaseTexture9* old_texture[ 3 ]{ };
	IDirect3DVertexDeclaration9* old_vertex_declaration = nullptr;
	D3DVIEWPORT9 old_viewport{ };

	DWORD z_enable, z_write, alpha_blend, alpha_test, cull, scissor, srgb_write, fvf;
	DWORD stencil, colour_write, clip_planes, fill_mode, point_size, source_blend, dest_blend, blend_operation;

	DWORD address_u[ 3 ], address_v[ 3 ], min_filter[ 3 ], mag_filter[ 3 ], mip_filter[ 3 ], srgb_texture[ 3 ];

	float old_constants[ constant_registers ][ 4 ]{ };

	if ( FAILED( device->GetDepthStencilSurface( &old_depth_stencil ) ) )
		old_depth_stencil = nullptr;

	device->GetPixelShader( &old_pixel_shader );
	device->GetVertexShader( &old_vertex_shader );
	device->GetViewport( &old_viewport );
	device->GetRenderState( D3DRS_STENCILENABLE, &stencil );
	device->GetRenderState( D3DRS_COLORWRITEENABLE, &colour_write );
	device->GetRenderState( D3DRS_CLIPPLANEENABLE, &clip_planes );
	device->GetRenderState( D3DRS_FILLMODE, &fill_mode );
	device->GetRenderState( D3DRS_ZENABLE, &z_enable );
	device->GetRenderState( D3DRS_ZWRITEENABLE, &z_write );
	device->GetRenderState( D3DRS_ALPHABLENDENABLE, &alpha_blend );
	device->GetRenderState( D3DRS_ALPHATESTENABLE, &alpha_test );
	device->GetRenderState( D3DRS_SRCBLEND, &source_blend );
	device->GetRenderState( D3DRS_DESTBLEND, &dest_blend );
	device->GetRenderState( D3DRS_BLENDOP, &blend_operation );
	device->GetRenderState( D3DRS_CULLMODE, &cull );
	device->GetRenderState( D3DRS_SCISSORTESTENABLE, &scissor );
	device->GetRenderState( D3DRS_SRGBWRITEENABLE, &srgb_write );
	device->GetRenderState( D3DRS_POINTSIZE, &point_size );

	for ( unsigned long stage = 0; stage < 3; ++stage ) {
		device->GetTexture( stage, &old_texture[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_ADDRESSU, &address_u[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_ADDRESSV, &address_v[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_MINFILTER, &min_filter[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_MAGFILTER, &mag_filter[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_MIPFILTER, &mip_filter[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_SRGBTEXTURE, &srgb_texture[ stage ] );
	}

	device->GetFVF( &fvf );
	device->GetVertexDeclaration( &old_vertex_declaration );
	device->GetPixelShaderConstantF( 0, old_constants[ 0 ], constant_registers );

	n_stream_guard::state_t streams{ };
	streams.capture( device );

	g_depth_source.resolve( device, old_depth_stencil, frame.m_frame );

	device->SetDepthStencilSurface( nullptr );
	device->SetVertexShader( nullptr );
	device->SetVertexDeclaration( nullptr );
	device->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX1 );
	device->SetRenderState( D3DRS_STENCILENABLE, FALSE );
	device->SetRenderState( D3DRS_CLIPPLANEENABLE, 0 );
	device->SetRenderState( D3DRS_FILLMODE, D3DFILL_SOLID );
	device->SetRenderState( D3DRS_ZENABLE, FALSE );
	device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
	device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
	device->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
	device->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
	device->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );
	device->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, 0x0f );

	for ( unsigned long stage = 0; stage < 3; ++stage ) {
		device->SetSamplerState( stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
		device->SetSamplerState( stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
		device->SetSamplerState( stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
		device->SetSamplerState( stage, D3DSAMP_SRGBTEXTURE, 0 );
		device->SetSamplerState( stage, D3DSAMP_MINFILTER, D3DTEXF_POINT );
		device->SetSamplerState( stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT );
	}

	const float width  = static_cast< float >( this->m_width );
	const float height = static_cast< float >( this->m_height );

	const float aspect     = height > 0.f ? width / height : 1.777778f;
	const float tan_half_x = std::tan( std::clamp( frame.m_fov, 1.f, 170.f ) * 0.5f * 3.14159265f / 180.f );
	const float tan_half_y = tan_half_x / aspect;

	const float prev_tan_x = std::tan( std::clamp( frame.m_prev_fov, 1.f, 170.f ) * 0.5f * 3.14159265f / 180.f );
	const float prev_tan_y = prev_tan_x / aspect;

	const float viewmodel_cutoff = 0.1f;

	const float streak_ceiling = static_cast< float >( this->m_tile_size * 2 );

	const int taps = std::clamp( GET_VARIABLE( g_variables.m_true_motion_blur_samples, int ), 4, 64 );

	const float max_length =
		std::clamp( GET_VARIABLE( g_variables.m_true_motion_blur_max, float ) * height, 4.f, std::min( streak_ceiling, static_cast< float >( taps ) ) );

	float block[ constant_registers ][ 4 ]{ };

	block[ 0 ][ 0 ] = frame.m_znear;
	block[ 0 ][ 1 ] = frame.m_zfar;
	block[ 0 ][ 2 ] = tan_half_x;
	block[ 0 ][ 3 ] = tan_half_y;

	block[ 1 ][ 0 ] = prev_tan_x;
	block[ 1 ][ 1 ] = prev_tan_y;
	block[ 1 ][ 2 ] = viewmodel_cutoff;
	block[ 1 ][ 3 ] = frame.m_exposure;

	block[ 2 ][ 0 ] = static_cast< float >( taps );
	block[ 2 ][ 1 ] = max_length;
	block[ 2 ][ 2 ] = static_cast< float >( this->m_tile_size );
	// fixed dither seed: a per-frame seed crawls as grain on every blurred pixel
	block[ 2 ][ 3 ] = 0.f;

	for ( int row = 0; row < 3; ++row )
		std::memcpy( block[ 3 + row ], frame.m_reprojection[ row ], sizeof( float ) * 4 );

	block[ 6 ][ 0 ] = width;
	block[ 6 ][ 1 ] = height;
	block[ 6 ][ 2 ] = 1.f / std::max( width, 1.f );
	block[ 6 ][ 3 ] = 1.f / std::max( height, 1.f );

	block[ 7 ][ 0 ] = static_cast< float >( this->m_tile_width );
	block[ 7 ][ 1 ] = static_cast< float >( this->m_tile_height );
	block[ 7 ][ 2 ] = 1.f / std::max( static_cast< float >( this->m_tile_width ), 1.f );
	block[ 7 ][ 3 ] = 1.f / std::max( static_cast< float >( this->m_tile_height ), 1.f );

	block[ 8 ][ 0 ] = blur_object ? frame.m_blend : 0.f;
	block[ 8 ][ 1 ] = 12.f;
	block[ 8 ][ 2 ] = blur_gun ? frame.m_viewmodel_blend : 0.f;

	const float vm_tan_half_x = std::tan( std::clamp( frame.m_viewmodel_fov, 1.f, 170.f ) * 0.5f * 3.14159265f / 180.f );
	const float vm_prev_tan_x = std::tan( std::clamp( frame.m_viewmodel_prev_fov, 1.f, 170.f ) * 0.5f * 3.14159265f / 180.f );

	block[ 9 ][ 0 ] = std::max( frame.m_viewmodel_znear, 0.01f );
	block[ 9 ][ 1 ] = std::max( frame.m_viewmodel_zfar, block[ 9 ][ 0 ] + 1.f );
	block[ 9 ][ 2 ] = vm_tan_half_x;
	block[ 9 ][ 3 ] = vm_tan_half_x / aspect;

	for ( int row = 0; row < 3; ++row )
		std::memcpy( block[ 10 + row ], frame.m_viewmodel_reprojection[ row ], sizeof( float ) * 4 );

	block[ 13 ][ 0 ] = vm_prev_tan_x;
	block[ 13 ][ 1 ] = vm_prev_tan_x / aspect;
	block[ 13 ][ 2 ] = frame.m_viewmodel_valid ? 1.f : 0.f;
	block[ 13 ][ 3 ] = std::clamp( GET_VARIABLE( g_variables.m_true_motion_blur_viewmodel_strength, float ), 0.f, 64.f );

	block[ 15 ][ 1 ] = std::max( frame.m_znear, 0.01f ) * 0.5f;
	block[ 15 ][ 2 ] = vm_tan_half_x;
	block[ 15 ][ 3 ] = vm_tan_half_x / aspect;

	block[ 14 ][ 0 ] = static_cast< float >( frame.m_bone_count );

	for ( int index = 0; index < n_viewmodel_bones::max_proxies; ++index ) {
		const int base = 16 + index * 4;

		if ( index >= frame.m_bone_count ) {
			block[ base ][ 0 ] = 1e6f;
			block[ base ][ 1 ] = 1e6f;
			block[ base ][ 2 ] = 1e6f;
			continue;
		}

		std::memcpy( block[ base ], frame.m_bone_center[ index ], sizeof( float ) * 4 );

		for ( int row = 0; row < 3; ++row )
			std::memcpy( block[ base + 1 + row ], frame.m_bone[ index ][ row ], sizeof( float ) * 4 );
	}

	// c14.y = player switch + count, c80-c82 view to world. empty slots parked INSIDE OUT ( mins > maxs, never matches )
	block[ 14 ][ 1 ] = static_cast< float >( frame.m_player_count );

	for ( int row = 0; row < 3; ++row )
		std::memcpy( block[ 80 + row ], frame.m_view_to_world[ row ], sizeof( float ) * 4 );

	for ( int index = 0; index < max_players; ++index ) {
		const int base = 83 + index * 5;

		if ( index >= frame.m_player_count ) {
			for ( int axis = 0; axis < 3; ++axis ) {
				block[ base ][ axis ]     = 1e9f;
				block[ base + 1 ][ axis ] = -1e9f;
			}

			continue;
		}

		std::memcpy( block[ base ], frame.m_player_box[ index ][ 0 ], sizeof( float ) * 4 );
		std::memcpy( block[ base + 1 ], frame.m_player_box[ index ][ 1 ], sizeof( float ) * 4 );

		for ( int row = 0; row < 3; ++row )
			std::memcpy( block[ base + 2 + row ], frame.m_player[ index ][ row ], sizeof( float ) * 4 );
	}

	static bool logged_viewmodel = false;

	if ( frame.m_viewmodel_valid && !logged_viewmodel ) {
		logged_viewmodel = true;

		g_console.print( std::vformat( "true motion blur: viewmodel near {:.2f} far {:.1f} fov {:.1f}",
		                               std::make_format_args( frame.m_viewmodel_znear, frame.m_viewmodel_zfar, frame.m_viewmodel_fov ) )
		                     .c_str( ) );
	}

	struct vertex_t {
		float m_x, m_y, m_z, m_rhw, m_u, m_v;
	};

	const auto pass = [ & ]( IDirect3DSurface9* surface, const int pass_width, const int pass_height, IDirect3DPixelShader9* shader ) {
		const float right  = static_cast< float >( pass_width ) - 0.5f;
		const float bottom = static_cast< float >( pass_height ) - 0.5f;

		const vertex_t quad[ 4 ] = {
			{ -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f },
			{ right, -0.5f, 0.f, 1.f, 1.f, 0.f },
			{ -0.5f, bottom, 0.f, 1.f, 0.f, 1.f },
			{ right, bottom, 0.f, 1.f, 1.f, 1.f },
		};

		D3DVIEWPORT9 viewport{ 0, 0, static_cast< DWORD >( pass_width ), static_cast< DWORD >( pass_height ), 0.f, 1.f };

		device->SetRenderTarget( 0, surface );
		device->SetViewport( &viewport );
		device->SetPixelShaderConstantF( 0, block[ 0 ], constant_registers );
		device->SetPixelShader( shader );
		n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof( vertex_t ) );
	};

	IDirect3DTexture9* current = this->m_stage_texture;

	if ( blur_camera ) {
		device->SetTexture( 0, depth );
		pass( this->m_info_surface, this->m_width, this->m_height, info );

		device->SetTexture( 0, this->m_info_texture );
		pass( this->m_tile_row_surface, this->m_tile_width, this->m_height, tile_h );

		device->SetTexture( 0, this->m_tile_row_texture );
		pass( this->m_tile_surface, this->m_tile_width, this->m_tile_height, tile_v );

		device->SetTexture( 0, this->m_tile_texture );
		pass( this->m_neighbour_surface, this->m_tile_width, this->m_tile_height, neighbour );

		// pass five: reconstruction. colour filtered, velocity + tiles never
		device->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
		device->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );

		device->SetTexture( 0, this->m_stage_texture );
		device->SetTexture( 1, this->m_info_texture );
		device->SetTexture( 2, this->m_neighbour_texture );

		// no history after it: gather straight into the frame, same bits as gather -> 8 bit blur target -> copy
		if ( keep_history ) {
			pass( this->m_blur_surface, this->m_width, this->m_height, gather );
			current = this->m_blur_texture;
		} else {
			device->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE );
			pass( target, this->m_width, this->m_height, gather );
			current = nullptr;
		}
	}

	const int next = 1 - this->m_history_index;

	if ( keep_history ) {
		device->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
		device->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
		device->SetSamplerState( 1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
		device->SetSamplerState( 1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );

		device->SetTexture( 0, current );
		device->SetTexture( 1, this->m_history_texture[ this->m_history_index ] );
		device->SetTexture( 2, depth );
		pass( this->m_history_surface[ next ], this->m_width, this->m_height, accumulate );

		device->SetSamplerState( 1, D3DSAMP_MINFILTER, D3DTEXF_POINT );
		device->SetSamplerState( 1, D3DSAMP_MAGFILTER, D3DTEXF_POINT );

		current = this->m_history_texture[ next ];

		this->m_history_index = next;
		this->m_history_empty = false;
	}

	if ( current ) {
		device->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
		device->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
		device->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE );
		device->SetTexture( 0, current );
		pass( target, this->m_width, this->m_height, copy );
	}

	device->SetPixelShaderConstantF( 0, old_constants[ 0 ], constant_registers );
	device->SetPixelShader( old_pixel_shader );
	device->SetVertexShader( old_vertex_shader );

	if ( fvf )
		device->SetFVF( fvf );

	device->SetVertexDeclaration( old_vertex_declaration );

	streams.restore( device );

	for ( unsigned long stage = 0; stage < 3; ++stage ) {
		device->SetTexture( stage, old_texture[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_ADDRESSU, address_u[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_ADDRESSV, address_v[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_MINFILTER, min_filter[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_MAGFILTER, mag_filter[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_MIPFILTER, mip_filter[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_SRGBTEXTURE, srgb_texture[ stage ] );
	}

	device->SetRenderState( D3DRS_POINTSIZE, point_size );
	device->SetRenderState( D3DRS_SRGBWRITEENABLE, srgb_write );
	device->SetRenderState( D3DRS_SCISSORTESTENABLE, scissor );
	device->SetRenderState( D3DRS_CULLMODE, cull );
	device->SetRenderState( D3DRS_BLENDOP, blend_operation );
	device->SetRenderState( D3DRS_DESTBLEND, dest_blend );
	device->SetRenderState( D3DRS_SRCBLEND, source_blend );
	device->SetRenderState( D3DRS_ALPHATESTENABLE, alpha_test );
	device->SetRenderState( D3DRS_ALPHABLENDENABLE, alpha_blend );
	device->SetRenderState( D3DRS_ZWRITEENABLE, z_write );
	device->SetRenderState( D3DRS_ZENABLE, z_enable );
	device->SetRenderState( D3DRS_FILLMODE, fill_mode );
	device->SetRenderState( D3DRS_CLIPPLANEENABLE, clip_planes );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, colour_write );
	device->SetRenderState( D3DRS_STENCILENABLE, stencil );
	device->SetViewport( &old_viewport );
	device->SetDepthStencilSurface( old_depth_stencil );

	if ( old_depth_stencil )
		old_depth_stencil->Release( );

	if ( old_vertex_declaration )
		old_vertex_declaration->Release( );

	for ( IDirect3DBaseTexture9* texture : old_texture ) {
		if ( texture )
			texture->Release( );
	}

	if ( old_vertex_shader )
		old_vertex_shader->Release( );

	if ( old_pixel_shader )
		old_pixel_shader->Release( );

	take_snapshot( device, leak_after );
	report_leaks( leak_before, leak_after );

	target->Release( );
}
