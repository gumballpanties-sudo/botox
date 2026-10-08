#include "reflections.h"
#include "../../../game/sdk/includes/includes.h"
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
#include <cstdio>
#include <cstring>
#include <d3dx9.h>
#include <string>

static const char k_common[] = R"(
sampler2D s_depth:register(s0);
sampler2D s_normal:register(s1);
sampler2D s_frame:register(s2);
sampler2D s_reflection:register(s3);
samplerCUBE s_cube0:register(s4);
samplerCUBE s_cube1:register(s5);
float4 c0:register(c0);
float4 c1:register(c1);
float4 c2:register(c2);
float4 c3:register(c3);
float4 c4:register(c4);
float4 c5:register(c5);
float4 c6:register(c6);
float lin(float raw){return raw/(1000.0-raw*999.0);}
float depth_at(float2 uv){return lin(tex2Dlod(s_depth,float4(uv,0,0)).r);}
float3 view_pos(float2 uv,float l){return float3((uv-0.5)*c0.xy,1.0)*l*1000.0;}
float3 at(float2 uv){return view_pos(uv,depth_at(uv));}
float4 blur(float2 uv,float4 ck,float4 cm){
	if(c1.w<=0.0)return ck;
	float kk=-2.0/(c1.w*c1.w+1e-3);
	float4 s=0.0;
	float sw=1e-3;
	float ext=floor(c1.w);
	[loop] for(float j=-ext;j<=ext;j++){
		float2 t=uv+c2.xy*(2.0*j-0.5);
		float4 tk=tex2Dlod(s_reflection,float4(t,0,0));
		float4 tm=tex2Dlod(s_normal,float4(t,0,0));
		float dw=saturate(1.0-abs(tm.w-cm.w)*50.0);
		float nw=saturate((dot(tm.xyz,cm.xyz)-0.8660254)*(1.0/(1.0-0.8660254+1e-3)));
		float w=dw*nw*exp(j*j*kk)*tk.w;
		s+=tk*w;
		sw+=w;
	}
	s/=sw;
	return lerp(ck,s,saturate(sw*2.0));
}
)";

static const char k_normals[] = R"(
float4 main(float2 uv:TEXCOORD0):COLOR0{
	float raw=tex2Dlod(s_depth,float4(uv,0,0)).r;
	float l=lin(raw);
	if(raw<0.1)return float4(0,0,1,-1);
	if(l>=c1.x)return float4(0,0,1,l);
	float3 p=view_pos(uv,l);
	float3 x1=at(uv+float2(c0.z,0))-p;
	float3 x2=p-at(uv-float2(c0.z,0));
	float3 y1=at(uv+float2(0,c0.w))-p;
	float3 y2=p-at(uv-float2(0,c0.w));
	x1=(abs(x1.z)>abs(x2.z))?x2:x1;
	y1=(abs(y1.z)>abs(y2.z))?y2:y1;
	return float4(normalize(cross(y1,x1)),l);
}
)";

static const char k_smooth[] = R"(
float4 main(float2 uv:TEXCOORD0):COLOR0{
	float4 c=tex2Dlod(s_normal,float4(uv,0,0));
	if(c.w<0.0||c.w>=c1.x)return c;
	float3 ns=c.xyz;
	float ws=1.0;
	float tol=1.0/(c.w*0.03+1e-6);
	[unroll] for(int j=1;j<=6;j++){
		float2 off=c2.xy*(j*1.2);
		float g=exp(-j*j*0.15);
		float4 a=tex2Dlod(s_normal,float4(uv+off,0,0));
		float4 b=tex2Dlod(s_normal,float4(uv-off,0,0));
		float wa=g*saturate(1.0-abs(a.w-c.w)*tol)*saturate(dot(a.xyz,c.xyz)*4.0-2.8);
		float wb=g*saturate(1.0-abs(b.w-c.w)*tol)*saturate(dot(b.xyz,c.xyz)*4.0-2.8);
		ns+=a.xyz*wa+b.xyz*wb;
		ws+=wa+wb;
	}
	return float4(normalize(ns/ws),c.w);
}
)";

static const char k_trace[] = R"(
float bayer(float2 v){
	float2 a=fmod(floor(v*0.25),2.0);
	float2 b=fmod(floor(v*0.5),2.0);
	float2 c=fmod(floor(v),2.0);
	float3 x=float3(a.x,b.x,c.x)*2.0;
	float3 r=lerp(x,3.0-x,float3(a.y,b.y,c.y))/3.0;
	return dot(r,float3(4.0,16.0,64.0))/84.0;
}
float4 main(float2 uv:TEXCOORD0,float2 vpos:VPOS):COLOR0{
	float4 nd=tex2Dlod(s_normal,float4(uv,0,0));
	if(nd.w<0.0||nd.w>=c1.x)return 0.0;
	float jitter=bayer(vpos)-0.5;
	float3 e=normalize(view_pos(uv,nd.w));
	float3 d=reflect(e,nd.xyz);
	float st=(0.2+0.0125*jitter)*sqrt(nd.w)/(1e-3+saturate(1.0-dot(d,e)));
	float3 rp=view_pos(uv,nd.w)+d*st;
	float2 t=uv;
	// c6 x steps per run, y growth per step, z refines. 16 / 1.6 / 3 = qUINT
	float j=0.0;
	float k=0.0;
	[loop] for(int it=0;it<80;it++){
		if(j>=c6.x)break;
		j++;
		t=rp.xy/(rp.z*c0.xy)+0.5;
		float ez=depth_at(t)*1000.0-rp.z;
		if(ez<0.0&&ez>-2.5*st){
			j=0.0;
			if(k<c6.z){
				st/=c6.y;
				rp-=d*st;
				st*=c6.y/16.0;
			}else{
				j+=c6.x;
			}
			k++;
		}
		rp+=d*st;
		st*=c6.y;
		float2 edge=saturate(t-t*t);
		if(edge.x<=0.0||edge.y<=0.0)j+=c6.x;
	}
	float nv=saturate(1.0+dot(e,nd.xyz));
	// schlick ( exponent 5 ) is ~0.04 looking straight down: c1.z = falloff, 0 = same strength at every angle
	float schlick=lerp(0.04,1.0,pow(nv,5.0));
	// hits in the outer 1/8 of the screen fade out instead of cutting hard ( the jagged edge )
	float2 border=min(t,1.0-t);
	float hit=k>0?saturate(min(border.x,border.y)*8.0):0.0;
	float3 ssr=tex2Dlod(s_frame,float4(t,0,0)).rgb;
	float3 col;
	float a;
	// saturate: intensity goes to 5, rgb*a past 1 would brighten, not mirror more
	if(c3.w>0.5){
		/* map cubemap under the screen trace: misses, off screen and behind the camera still reflect.
		   world vector straight into texCUBE, as the engine's own envmap shaders do */
		float3 w=d.x*c3.xyz+d.y*c4.xyz+d.z*c5.xyz;
		float3 cube=lerp(texCUBElod(s_cube1,float4(w,0)).rgb,texCUBElod(s_cube0,float4(w,0)).rgb,c4.w);
		col=lerp(cube,ssr,hit);
		a=saturate(c1.y*lerp(1.0,schlick*nv,c1.z));
	}else{
		col=ssr;
		a=saturate(c1.y*lerp(1.0,schlick*saturate(dot(e,d))*nv,c1.z))*hit;
	}
	// distance fade on alpha only ( composite used to do it ): texture then has no hard cutoff for the edge aa
	return float4(col*a,a*saturate(1.0-nd.w/c1.x));
}
)";

static const char k_filter[] = R"(
float4 main(float2 uv:TEXCOORD0):COLOR0{
	float4 ck=tex2Dlod(s_reflection,float4(uv,0,0));
	float4 cm=tex2Dlod(s_normal,float4(uv,0,0));
	if(cm.w<0.0||cm.w>=c1.x)return ck;
	return blur(uv,ck,cm);
}
)";

static const char k_composite[] = R"(
float m(float4 v){return dot(v,float4(0.299,0.587,0.114,0.5));}
float4 main(float2 uv:TEXCOORD0):COLOR0{
	float4 ck=tex2Dlod(s_reflection,float4(uv,0,0));
	float4 cm=tex2Dlod(s_normal,float4(uv,0,0));
	float4 r=ck;
	if(cm.w>=0.0&&cm.w<c1.x)r=blur(uv,ck,cm);
	float2 h=c0.zw*0.5;
	float4 a=tex2Dlod(s_reflection,float4(uv+float2(-h.x,-h.y),0,0));
	float4 b=tex2Dlod(s_reflection,float4(uv+float2(h.x,-h.y),0,0));
	float4 c=tex2Dlod(s_reflection,float4(uv+float2(-h.x,h.y),0,0));
	float4 d=tex2Dlod(s_reflection,float4(uv+float2(h.x,h.y),0,0));
	float ma=m(a),mb=m(b),mc=m(c),md=m(d);
	float hi=max(max(ma,mb),max(mc,md));
	float lo=min(min(ma,mb),min(mc,md));
	float e=saturate((hi-lo)/(hi+0.05)*2.0-0.5);
	return lerp(r,(a+b+c+d)*0.25,e);
}
)";

void n_reflections::impl_t::release_targets( )
{
	const auto drop = []( auto& resource ) {
		if ( resource ) {
			resource->Release( );
			resource = nullptr;
		}
	};

	drop( this->m_resolve_surface );
	drop( this->m_frame_surface );
	drop( this->m_frame_texture );

	for ( int i = 0; i < 2; ++i ) {
		drop( this->m_normal_surface[ i ] );
		drop( this->m_normal_texture[ i ] );
		drop( this->m_reflection_surface[ i ] );
		drop( this->m_reflection_texture[ i ] );
	}

	this->m_width        = 0;
	this->m_height       = 0;
	this->m_trace_width  = 0;
	this->m_trace_height = 0;
	this->m_format       = D3DFMT_UNKNOWN;
	this->m_multi_sample = D3DMULTISAMPLE_NONE;
}

void n_reflections::impl_t::release( )
{
	this->release_targets( );
	this->release_cubes( );

	for ( IDirect3DPixelShader9*& shader : this->m_shaders ) {
		if ( shader ) {
			shader->Release( );
			shader = nullptr;
		}
	}
}

void n_reflections::impl_t::on_device_lost( )
{
	this->release( );
	this->m_active = false;
}

bool read_game_file( const char* path, std::vector< unsigned char >& out, const unsigned int offset, unsigned int length )
{
	static void* const file_system = g_modules[ FILESYSTEM_DLL ].find_interface( "VBaseFileSystem011" );

	if ( !file_system )
		return false;

	void* const handle = g_virtual.call< void* >( file_system, 2, path, "rb", "GAME" );

	if ( !handle )
		return false;

	g_virtual.call< void >( file_system, 4, handle, 0, SEEK_END );
	const unsigned int size = g_virtual.call< unsigned int >( file_system, 5, handle );

	if ( !length && size > offset )
		length = size - offset;

	bool ok = length > 0 && offset + length <= size;

	if ( ok ) {
		g_virtual.call< void >( file_system, 4, handle, static_cast< int >( offset ), SEEK_SET );
		out.resize( length );
		ok = g_virtual.call< int >( file_system, 0, out.data( ), static_cast< int >( length ), handle ) == static_cast< int >( length );
	}

	g_virtual.call< void >( file_system, 3, handle );
	return ok;
}

static float half_to_float( const unsigned short half )
{
	const int exponent = ( half >> 10 ) & 31;
	const int mantissa = half & 1023;

	float value = exponent == 0 ? std::ldexp( static_cast< float >( mantissa ), -24 )
	                            : ( exponent == 31 ? 65504.f : std::ldexp( static_cast< float >( mantissa | 1024 ), exponent - 25 ) );

	return ( half & 0x8000 ) ? -value : value;
}

static std::shared_ptr< const n_reflections::cube_data_t > parse_cube_vtf( const std::vector< unsigned char >& file )
{
	const auto u16 = [ & ]( const size_t at ) { return static_cast< unsigned int >( file[ at ] | ( file[ at + 1 ] << 8 ) ); };
	const auto u32 = [ & ]( const size_t at ) { return static_cast< unsigned int >( u16( at ) | ( u16( at + 2 ) << 16 ) ); };

	if ( file.size( ) < 80 || std::memcmp( file.data( ), "VTF", 4 ) != 0 || u32( 4 ) != 7 )
		return nullptr;

	const unsigned int minor  = u32( 8 );
	const unsigned int header = u32( 12 );
	const int size            = static_cast< int >( u16( 16 ) );
	const int frames          = std::max( static_cast< int >( u16( 24 ) ), 1 );
	const int format          = static_cast< int >( u32( 52 ) );
	const int mips            = std::max( static_cast< int >( file[ 56 ] ), 1 );

	if ( size < 4 || u16( 18 ) != static_cast< unsigned int >( size ) || !( u32( 20 ) & 0x4000  ) )
		return nullptr;

	const auto mip_bytes = [ format ]( const int width ) -> size_t {
		const size_t blocks = static_cast< size_t >( std::max( ( width + 3 ) / 4, 1 ) );

		switch ( format ) {
			case 13: return blocks * blocks * 8;
			case 14:
			case 15: return blocks * blocks * 16;
			case 24: return static_cast< size_t >( width ) * width * 8;
			case 0:
			case 12:
			case 16: return static_cast< size_t >( width ) * width * 4;
			default: return 0;
		}
	};

	if ( !mip_bytes( size ) )
		return nullptr;

	size_t image = 0;

	if ( minor >= 3 ) {
		const unsigned int count = std::min( u32( 68 ), 32u );

		for ( unsigned int i = 0; i < count && 88 + i * 8 <= file.size( ); ++i ) {
			if ( ( u32( 80 + i * 8 ) & 0x00ffffff ) == 0x30 )
				image = u32( 84 + i * 8 );
		}
	} else {
		const int low_format = static_cast< int >( u32( 57 ) );
		const size_t low_w   = static_cast< size_t >( std::max( ( file[ 61 ] + 3 ) / 4, 1 ) );
		const size_t low_h   = static_cast< size_t >( std::max( ( file[ 62 ] + 3 ) / 4, 1 ) );

		image = header + ( low_format == 13 ? low_w * low_h * 8 : 0 );
	}

	if ( !image || image >= file.size( ) )
		return nullptr;

	size_t face_chain = 0;
	size_t below_top  = 0;

	for ( int mip = 0; mip < mips; ++mip ) {
		const size_t bytes = mip_bytes( std::max( size >> mip, 1 ) );

		face_chain += bytes;
		below_top += mip ? bytes : 0;
	}

	const size_t faces = ( file.size( ) - image ) / ( face_chain * frames );

	if ( faces < 6 )
		return nullptr;

	const size_t top   = mip_bytes( size );
	const size_t first = image + below_top * frames * faces;

	auto data    = std::make_shared< n_reflections::cube_data_t >( );
	data->m_size = size;

	const bool compressed = format == 13 || format == 14 || format == 15;

	data->m_format = format == 13 ? D3DFMT_DXT1 : format == 14 ? D3DFMT_DXT3 : format == 15 ? D3DFMT_DXT5 : D3DFMT_X8R8G8B8;
	data->m_pitch  = compressed ? static_cast< int >( top ) / std::max( ( size + 3 ) / 4, 1 ) : size * 4;
	data->m_rows   = compressed ? std::max( ( size + 3 ) / 4, 1 ) : size;

	for ( int face = 0; face < 6; ++face ) {
		const unsigned char* source = file.data( ) + first + face * top;
		std::vector< unsigned char >& out = data->m_faces[ face ];

		if ( compressed || format == 12 || format == 16 ) {
			out.assign( source, source + top );

			if ( compressed && face == 5 ) {
				size_t sum = 0;

				for ( const std::vector< unsigned char >& any : data->m_faces )
					for ( const unsigned char byte : any )
						sum += byte;

				if ( sum < top * 6 )
					return nullptr;
			}

			continue;
		}

		const size_t pixels = static_cast< size_t >( size ) * size;
		out.resize( pixels * 4 );

		for ( size_t p = 0; p < pixels; ++p ) {
			unsigned char rgb[ 3 ];

			for ( int c = 0; c < 3; ++c ) {
				if ( format == 24 ) {
					const float linear = half_to_float( static_cast< unsigned short >( source[ p * 8 + c * 2 ] | ( source[ p * 8 + c * 2 + 1 ] << 8 ) ) );
					rgb[ c ]           = static_cast< unsigned char >( std::pow( std::clamp( linear, 0.f, 1.f ), 1.f / 2.2f ) * 255.f + 0.5f );
				} else
					rgb[ c ] = source[ p * 4 + c ];
			}

			out[ p * 4 + 0 ] = rgb[ 2 ];
			out[ p * 4 + 1 ] = rgb[ 1 ];
			out[ p * 4 + 2 ] = rgb[ 0 ];
			out[ p * 4 + 3 ] = 255;
		}
	}

	return data;
}

void n_reflections::impl_t::load_cube_samples( )
{
	this->m_cube_samples.clear( );
	this->m_cube_data.clear( );
	this->m_cube_tried.clear( );
	this->m_cube_recent.clear( );
	this->m_cube_current  = -1;
	this->m_cube_previous = -1;

	std::vector< unsigned char > header;

	if ( !read_game_file( this->m_level.c_str( ), header, 0, 8 + 64 * 16 ) || std::memcmp( header.data( ), "VBSP", 4 ) != 0 ) {
		g_console.print< n_console::log_level::WARNING >( std::format( "reflections: can't read {:s}, no cubemaps", this->m_level ).c_str( ) );
		return;
	}

	unsigned int lump[ 2 ]{ };
	std::memcpy( lump, header.data( ) + 8 + 42 * 16, sizeof( lump ) );

	std::vector< unsigned char > samples;

	if ( lump[ 1 ] < 16 || !read_game_file( this->m_level.c_str( ), samples, lump[ 0 ], lump[ 1 ] - lump[ 1 ] % 16 ) )
		return;

	for ( size_t i = 0; i + 16 <= samples.size( ); i += 16 ) {
		cube_sample_t sample{ };
		std::memcpy( sample.m_origin, samples.data( ) + i, sizeof( sample.m_origin ) );
		this->m_cube_samples.push_back( sample );
	}

	this->m_cube_data.resize( this->m_cube_samples.size( ) );
	this->m_cube_tried.resize( this->m_cube_samples.size( ), false );

	const size_t count = this->m_cube_samples.size( );
	g_console.print( std::format( "reflections: {:d} cubemaps in {:s}", count, this->m_level ).c_str( ) );
}

std::shared_ptr< const n_reflections::cube_data_t > n_reflections::impl_t::load_cube( const int index )
{
	if ( index < 0 || index >= static_cast< int >( this->m_cube_samples.size( ) ) )
		return nullptr;

	if ( this->m_cube_tried[ index ] )
		return this->m_cube_data[ index ];

	this->m_cube_tried[ index ] = true;

	const std::string folder = "materials/" + this->m_level.substr( 0, this->m_level.size( ) - 4 ) + "/";
	const int* origin        = this->m_cube_samples[ index ].m_origin;
	const std::string name   = std::format( "c{:d}_{:d}_{:d}", origin[ 0 ], origin[ 1 ], origin[ 2 ] );

	for ( const std::string& file : { name + ".hdr.vtf", name + ".vtf", std::string( "cubemapdefault.hdr.vtf" ), std::string( "cubemapdefault.vtf" ) } ) {
		std::vector< unsigned char > bytes;

		if ( !read_game_file( ( folder + file ).c_str( ), bytes ) )
			continue;

		if ( auto data = parse_cube_vtf( bytes ) ) {
			this->m_cube_data[ index ] = data;
			return data;
		}
	}

	g_console.print< n_console::log_level::WARNING >( std::format( "reflections: no usable cubemap for {:s}{:s}", folder, name ).c_str( ) );
	return nullptr;
}

void n_reflections::impl_t::pick_cubemap( const c_view_setup* setup, frame_t& frame )
{
	const char* level = g_interfaces.m_engine_client->is_in_game( ) ? g_interfaces.m_engine_client->get_level_name( ) : nullptr;

	if ( !level || std::strlen( level ) < 5 ) {
		if ( !this->m_level.empty( ) ) {
			this->m_level.clear( );
			this->m_cube_samples.clear( );
			this->m_cube_data.clear( );
			this->m_cube_tried.clear( );
			this->m_cube_recent.clear( );
			++this->m_generation;
		}

		return;
	}

	if ( this->m_level != level ) {
		this->m_level = level;
		++this->m_generation;
		this->load_cube_samples( );
	}

	frame.m_generation = this->m_generation;

	int nearest   = -1;
	float closest = 0.f;

	for ( int i = 0; i < static_cast< int >( this->m_cube_samples.size( ) ); ++i ) {
		const int* origin = this->m_cube_samples[ i ].m_origin;
		const c_vector delta( static_cast< float >( origin[ 0 ] ) - setup->m_origin.m_x, static_cast< float >( origin[ 1 ] ) - setup->m_origin.m_y,
		                      static_cast< float >( origin[ 2 ] ) - setup->m_origin.m_z );
		const float distance = delta.dot_product( delta );

		if ( nearest < 0 || distance < closest ) {
			nearest = i;
			closest = distance;
		}
	}

	if ( nearest < 0 )
		return;

	const float now = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;

	if ( nearest != this->m_cube_current ) {
		this->m_cube_previous = this->m_cube_current;
		this->m_cube_current  = nearest;
		this->m_cube_switch   = now;

		constexpr std::size_t k_cube_keep = 8;

		std::erase( this->m_cube_recent, nearest );
		this->m_cube_recent.insert( this->m_cube_recent.begin( ), nearest );

		while ( this->m_cube_recent.size( ) > k_cube_keep ) {
			const int old = this->m_cube_recent.back( );
			this->m_cube_recent.pop_back( );
			this->m_cube_data[ old ].reset( );
			this->m_cube_tried[ old ] = false;
		}
	}

	frame.m_cube[ 0 ]  = this->load_cube( this->m_cube_current );
	frame.m_cube_blend = std::clamp( ( now - this->m_cube_switch ) / 0.5f, 0.f, 1.f );

	if ( frame.m_cube_blend < 1.f )
		frame.m_cube[ 1 ] = this->load_cube( this->m_cube_previous );

	if ( !frame.m_cube[ 1 ] )
		frame.m_cube_blend = 1.f;
}

IDirect3DCubeTexture9* n_reflections::impl_t::cube_texture( IDirect3DDevice9* device, const std::shared_ptr< const cube_data_t >& data )
{
	if ( !data )
		return nullptr;

	for ( const cube_texture_t& entry : this->m_cube_textures ) {
		if ( entry.m_data == data )
			return entry.m_texture;
	}

	/* use_count 1 = main thread dropped the data and no queued frame holds it: nobody can ask for this cube again */
	std::erase_if( this->m_cube_textures, [ ]( const cube_texture_t& entry ) {
		if ( entry.m_data.use_count( ) != 1 )
			return false;

		if ( entry.m_texture )
			entry.m_texture->Release( );

		return true;
	} );

	IDirect3DCubeTexture9* staging = nullptr;
	IDirect3DCubeTexture9* texture = nullptr;

	if ( SUCCEEDED( device->CreateCubeTexture( data->m_size, 1, 0, data->m_format, D3DPOOL_SYSTEMMEM, &staging, nullptr ) ) ) {
		bool filled = true;

		for ( int face = 0; face < 6 && filled; ++face ) {
			D3DLOCKED_RECT locked{ };

			if ( FAILED( staging->LockRect( static_cast< D3DCUBEMAP_FACES >( face ), 0, &locked, nullptr, 0 ) ) ) {
				filled = false;
				break;
			}

			for ( int row = 0; row < data->m_rows; ++row )
				std::memcpy( static_cast< unsigned char* >( locked.pBits ) + row * locked.Pitch, data->m_faces[ face ].data( ) + row * data->m_pitch,
				             static_cast< size_t >( data->m_pitch ) );

			staging->UnlockRect( static_cast< D3DCUBEMAP_FACES >( face ), 0 );
		}

		if ( filled && SUCCEEDED( device->CreateCubeTexture( data->m_size, 1, 0, data->m_format, D3DPOOL_DEFAULT, &texture, nullptr ) ) &&
		     FAILED( device->UpdateTexture( staging, texture ) ) ) {
			texture->Release( );
			texture = nullptr;
		}

		staging->Release( );
	}

	if ( !texture )
		g_console.print< n_console::log_level::WARNING >( "reflections: cubemap upload failed" );

	this->m_cube_textures.push_back( { data, texture } );
	return texture;
}

void n_reflections::impl_t::release_cubes( )
{
	for ( cube_texture_t& entry : this->m_cube_textures ) {
		if ( entry.m_texture )
			entry.m_texture->Release( );
	}

	this->m_cube_textures.clear( );
}

IDirect3DPixelShader9* n_reflections::impl_t::ensure_shader( IDirect3DDevice9* device, const e_shader which )
{
	if ( this->m_shaders[ which ] )
		return this->m_shaders[ which ];

	if ( this->m_compile_failed )
		return nullptr;

	static const char* const sources[ shader_max ] = { k_normals, k_smooth, k_trace, k_filter, k_composite };

	const std::string source = std::string( k_common ) + sources[ which ];

	ID3DXBuffer* code   = nullptr;
	ID3DXBuffer* errors = nullptr;

	HRESULT result = D3DXCompileShader( source.c_str( ), static_cast< UINT >( source.size( ) ), nullptr, nullptr, "main", "ps_3_0", 0, &code, &errors,
	                                    nullptr );

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

	g_console.print( std::vformat( "reflections: shader {:d} built", std::make_format_args( index ) ).c_str( ) );

	return this->m_shaders[ which ];
}

bool n_reflections::impl_t::build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description )
{
	const int width  = static_cast< int >( description.Width );
	const int height = static_cast< int >( description.Height );

	const float scale      = std::clamp( GET_VARIABLE( g_variables.m_reflections_scale, float ), 0.25f, 1.f );
	const int trace_width  = std::max( static_cast< int >( width * scale + 0.5f ), 16 );
	const int trace_height = std::max( static_cast< int >( height * scale + 0.5f ), 16 );

	if ( this->m_frame_texture && this->m_width == width && this->m_height == height && this->m_format == description.Format &&
	     this->m_multi_sample == description.MultiSampleType && this->m_trace_width == trace_width && this->m_trace_height == trace_height )
		return true;

	this->release_targets( );

	const auto fail = [ this ]( const char* what ) {
		g_console.print< n_console::log_level::WARNING >( what );
		this->release_targets( );
		return false;
	};

	if ( FAILED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, description.Format, D3DPOOL_DEFAULT,
	                                    &this->m_frame_texture, nullptr ) ) ||
	     FAILED( this->m_frame_texture->GetSurfaceLevel( 0, &this->m_frame_surface ) ) )
		return fail( "reflections: frame copy creation failed" );

	if ( description.MultiSampleType != D3DMULTISAMPLE_NONE &&
	     FAILED( device->CreateRenderTarget( description.Width, description.Height, description.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
	                                         &this->m_resolve_surface, nullptr ) ) )
		return fail( "reflections: msaa resolve target creation failed" );

	for ( int i = 0; i < 2; ++i ) {
		if ( FAILED( device->CreateTexture( trace_width, trace_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT,
		                                    &this->m_normal_texture[ i ], nullptr ) ) ||
		     FAILED( this->m_normal_texture[ i ]->GetSurfaceLevel( 0, &this->m_normal_surface[ i ] ) ) )
			return fail( "reflections: normal target creation failed ( fp16 )" );

		if ( FAILED( device->CreateTexture( trace_width, trace_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
		                                    &this->m_reflection_texture[ i ], nullptr ) ) ||
		     FAILED( this->m_reflection_texture[ i ]->GetSurfaceLevel( 0, &this->m_reflection_surface[ i ] ) ) )
			return fail( "reflections: reflection target creation failed" );
	}

	this->m_width        = width;
	this->m_height       = height;
	this->m_trace_width  = trace_width;
	this->m_trace_height = trace_height;
	this->m_format       = description.Format;
	this->m_multi_sample = description.MultiSampleType;

	g_console.print(
		std::vformat( "reflections: built {:d}x{:d}, trace {:d}x{:d}", std::make_format_args( width, height, trace_width, trace_height ) ).c_str( ) );

	return true;
}

void n_reflections::impl_t::on_post_screen_space_effects( c_view_setup* setup )
{
	if ( !setup )
		return;

	const bool enabled = GET_VARIABLE( g_variables.m_reflections, bool ) && !this->m_compile_failed;

	if ( !enabled && !this->m_active ) {
		g_serial_render.idle( );
		return;
	}

	frame_t frame{ };

	frame.m_fov   = setup->m_fov;
	frame.m_frame = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : 0;

	if ( enabled ) {
		c_vector forward{ }, right{ }, up{ };
		g_math.angle_vectors( setup->m_angles, &forward, &right, &up );

		for ( int i = 0; i < 3; ++i ) {
			frame.m_right[ i ]   = right[ i ];
			frame.m_down[ i ]    = -up[ i ];
			frame.m_forward[ i ] = forward[ i ];
		}

		// file reads + entity-free, but the level name and globals belong to this thread
		this->pick_cubemap( setup, frame );
	}

	// always the queue when there is one: raw d3d from here races the render thread ( see ambient_occlusion )
	g_serial_render.idle( );

	const auto pass = [ this, frame ] { n_fx_compat::run( g_interfaces.m_direct_device, "ssr", [ & ] { this->execute( frame ); } ); };

	if ( n_render_queue::submit( pass ) )
		return;

	pass( );
}

void n_reflections::impl_t::execute( const frame_t frame )
{
	IDirect3DDevice9* device = g_interfaces.m_direct_device;

	if ( !device )
		return;

	const n_gpu_timer::scope_t gpu_timer( device, n_gpu_timer::pass_reflections );

	if ( !GET_VARIABLE( g_variables.m_reflections, bool ) || this->m_compile_failed ) {
		if ( this->m_frame_texture )
			this->release_targets( );

		this->release_cubes( );
		this->m_active = false;
		return;
	}

	if ( frame.m_generation != this->m_cube_generation ) {
		this->release_cubes( );
		this->m_cube_generation = frame.m_generation;
	}

	IDirect3DSurface9* target = nullptr;

	if ( FAILED( device->GetRenderTarget( 0, &target ) ) || !target )
		return;

	D3DSURFACE_DESC description{ };

	if ( FAILED( target->GetDesc( &description ) ) || !this->build( device, description ) ) {
		target->Release( );
		return;
	}

	this->m_active = true;

	IDirect3DPixelShader9* shaders[ shader_max ]{ };

	for ( int i = 0; i < shader_max; ++i ) {
		shaders[ i ] = this->ensure_shader( device, static_cast< e_shader >( i ) );

		if ( !shaders[ i ] ) {
			target->Release( );
			return;
		}
	}

	IDirect3DTexture9* depth = g_depth_source.ensure( device, description ) ? g_depth_source.texture( ) : nullptr;

	D3DSURFACE_DESC depth_description{ };

	// must be frame sized, else it's not the scene's depth
	if ( !depth || FAILED( depth->GetLevelDesc( 0, &depth_description ) ) || depth_description.Width != description.Width ||
	     depth_description.Height != description.Height ) {
		static bool logged_depth = false;

		if ( !logged_depth ) {
			logged_depth = true;
			g_console.print( "reflections: no frame sized depth yet — load a map ( or change resolution ) once with this on" );
		}

		target->Release( );
		return;
	}

	static bool logged_blit = false;

	const long blit_result = n_frame_copy::copy( device, target, this->m_resolve_surface, this->m_frame_surface );

	if ( FAILED( blit_result ) ) {
		if ( !logged_blit ) {
			logged_blit = true;

			const unsigned long code = static_cast< unsigned long >( blit_result );

			g_console.print< n_console::log_level::WARNING >(
				std::vformat( "reflections: frame copy failed {:#x}", std::make_format_args( code ) ).c_str( ) );
		}

		target->Release( );
		return;
	}

	IDirect3DSurface9* old_depth_stencil                = nullptr;
	IDirect3DPixelShader9* old_pixel_shader             = nullptr;
	IDirect3DVertexShader9* old_vertex_shader           = nullptr;
	constexpr unsigned long k_stages = 6;

	IDirect3DBaseTexture9* old_texture[ k_stages ]{ };
	IDirect3DVertexDeclaration9* old_vertex_declaration = nullptr;
	D3DVIEWPORT9 old_viewport{ };

	DWORD z_enable, z_write, alpha_blend, alpha_test, cull, scissor, srgb_write, fvf;
	DWORD stencil, colour_write, clip_planes, fill_mode, point_size, source_blend, dest_blend, blend_operation;

	DWORD address_u[ k_stages ], address_v[ k_stages ], address_w[ k_stages ], min_filter[ k_stages ], mag_filter[ k_stages ],
		mip_filter[ k_stages ], srgb_texture[ k_stages ];

	float old_constants[ 7 ][ 4 ]{ };

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

	for ( unsigned long stage = 0; stage < k_stages; ++stage ) {
		device->GetTexture( stage, &old_texture[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_ADDRESSU, &address_u[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_ADDRESSV, &address_v[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_ADDRESSW, &address_w[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_MINFILTER, &min_filter[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_MAGFILTER, &mag_filter[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_MIPFILTER, &mip_filter[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_SRGBTEXTURE, &srgb_texture[ stage ] );
	}

	device->GetFVF( &fvf );
	device->GetVertexDeclaration( &old_vertex_declaration );
	device->GetPixelShaderConstantF( 0, old_constants[ 0 ], 7 );

	n_stream_guard::state_t streams{ };
	streams.capture( device );

	g_depth_source.resolve( device, old_depth_stencil, frame.m_frame );

	struct vertex_t {
		float m_x, m_y, m_z, m_rhw, m_u, m_v;
	};

	const auto make_quad = []( const float width, const float height, vertex_t( &quad )[ 4 ] ) {
		quad[ 0 ] = { -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f };
		quad[ 1 ] = { width - 0.5f, -0.5f, 0.f, 1.f, 1.f, 0.f };
		quad[ 2 ] = { -0.5f, height - 0.5f, 0.f, 1.f, 0.f, 1.f };
		quad[ 3 ] = { width - 0.5f, height - 0.5f, 0.f, 1.f, 1.f, 1.f };
	};

	const float width  = static_cast< float >( this->m_width );
	const float height = static_cast< float >( this->m_height );

	vertex_t full_quad[ 4 ], trace_quad[ 4 ];

	make_quad( width, height, full_quad );
	make_quad( static_cast< float >( this->m_trace_width ), static_cast< float >( this->m_trace_height ), trace_quad );

	D3DVIEWPORT9 full_viewport{ 0, 0, static_cast< DWORD >( this->m_width ), static_cast< DWORD >( this->m_height ), 0.f, 1.f };
	D3DVIEWPORT9 trace_viewport{ 0, 0, static_cast< DWORD >( this->m_trace_width ), static_cast< DWORD >( this->m_trace_height ), 0.f, 1.f };

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

	for ( unsigned long stage = 0; stage < k_stages; ++stage ) {
		const DWORD filter = stage >= 2 ? D3DTEXF_LINEAR : D3DTEXF_POINT;

		device->SetSamplerState( stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
		device->SetSamplerState( stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
		device->SetSamplerState( stage, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP );
		device->SetSamplerState( stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
		device->SetSamplerState( stage, D3DSAMP_SRGBTEXTURE, 0 );
		device->SetSamplerState( stage, D3DSAMP_MINFILTER, filter );
		device->SetSamplerState( stage, D3DSAMP_MAGFILTER, filter );
	}

	const float aspect     = height > 0.f ? width / height : 1.777778f;
	const float tan_half_x = std::tan( std::clamp( frame.m_fov, 1.f, 170.f ) * 0.5f * 3.14159265f / 180.f );
	const float tan_half_y = tan_half_x / aspect;

	const float constant_0[ 4 ] = { 2.f * tan_half_x, 2.f * tan_half_y, 1.f / static_cast< float >( this->m_trace_width ),
		                            1.f / static_cast< float >( this->m_trace_height ) };
	const float constant_1[ 4 ] = { std::clamp( GET_VARIABLE( g_variables.m_reflections_distance, float ), 0.001f, 1.f ),
		                            std::clamp( GET_VARIABLE( g_variables.m_reflections_intensity, float ), 0.f, 1.f ),
		                            std::clamp( GET_VARIABLE( g_variables.m_reflections_falloff, float ), 0.f, 1.f ),
		                            std::clamp( GET_VARIABLE( g_variables.m_reflections_blur, float ), 0.f, 5.f ) };

	device->SetPixelShaderConstantF( 0, constant_0, 1 );
	device->SetPixelShaderConstantF( 1, constant_1, 1 );

	constexpr float k_march[ 3 ][ 4 ] = { { 6.f, 3.505f, 1.f, 0.f }, { 10.f, 2.123f, 2.f, 0.f }, { 16.f, 1.6f, 3.f, 0.f } };

	device->SetPixelShaderConstantF( 6, k_march[ std::clamp( GET_VARIABLE( g_variables.m_reflections_quality, int ), 0, 2 ) ], 1 );

	const float axis_x[ 4 ] = { 1.f / width, 0.f, 0.f, 0.f };
	const float axis_y[ 4 ] = { 0.f, 1.f / height, 0.f, 0.f };

	const auto pass = [ & ]( IDirect3DPixelShader9* shader, IDirect3DSurface9* surface, const float* axis ) {
		if ( axis )
			device->SetPixelShaderConstantF( 2, axis, 1 );

		device->SetRenderTarget( 0, surface );
		device->SetViewport( &trace_viewport );
		device->SetPixelShader( shader );
		n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, trace_quad, sizeof( vertex_t ) );
	};

	/* cubemaps: c3 right ( w = have one ), c4 down ( w = crossfade ), c5 forward. no cube = plain ssr,
	   stages bound null ( samples black, branch never reads them ) */
	IDirect3DCubeTexture9* cube_new = this->cube_texture( device, frame.m_cube[ 0 ] );
	IDirect3DCubeTexture9* cube_old = this->cube_texture( device, frame.m_cube[ 1 ] );

	const float cube_block[ 3 ][ 4 ] = { { frame.m_right[ 0 ], frame.m_right[ 1 ], frame.m_right[ 2 ], cube_new ? 1.f : 0.f },
		                                 { frame.m_down[ 0 ], frame.m_down[ 1 ], frame.m_down[ 2 ], cube_old ? frame.m_cube_blend : 1.f },
		                                 { frame.m_forward[ 0 ], frame.m_forward[ 1 ], frame.m_forward[ 2 ], 0.f } };

	device->SetPixelShaderConstantF( 3, cube_block[ 0 ], 3 );
	device->SetTexture( 4, cube_new );
	device->SetTexture( 5, cube_old ? cube_old : cube_new );

	device->SetTexture( 0, depth );
	device->SetTexture( 2, this->m_frame_texture );

	pass( shaders[ shader_normals ], this->m_normal_surface[ 0 ], nullptr );

	if ( GET_VARIABLE( g_variables.m_reflections_smooth_normals, bool ) ) {
		device->SetTexture( 1, this->m_normal_texture[ 0 ] );
		pass( shaders[ shader_smooth ], this->m_normal_surface[ 1 ], axis_x );

		device->SetTexture( 1, this->m_normal_texture[ 1 ] );
		pass( shaders[ shader_smooth ], this->m_normal_surface[ 0 ], axis_y );
	}

	device->SetTexture( 1, this->m_normal_texture[ 0 ] );
	pass( shaders[ shader_trace ], this->m_reflection_surface[ 0 ], nullptr );

	device->SetTexture( 3, this->m_reflection_texture[ 0 ] );
	pass( shaders[ shader_filter ], this->m_reflection_surface[ 1 ], axis_y );

	device->SetTexture( 3, this->m_reflection_texture[ 1 ] );
	device->SetPixelShaderConstantF( 2, axis_x, 1 );
	device->SetRenderTarget( 0, target );
	device->SetViewport( &full_viewport );
	device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
	device->SetRenderState( D3DRS_BLENDOP, D3DBLENDOP_ADD );
	device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
	device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE );
	device->SetPixelShader( shaders[ shader_composite ] );
	n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, full_quad, sizeof( vertex_t ) );

	device->SetPixelShaderConstantF( 0, old_constants[ 0 ], 7 );
	device->SetPixelShader( old_pixel_shader );
	device->SetVertexShader( old_vertex_shader );

	if ( fvf )
		device->SetFVF( fvf );

	device->SetVertexDeclaration( old_vertex_declaration );

	streams.restore( device );

	for ( unsigned long stage = 0; stage < k_stages; ++stage ) {
		device->SetTexture( stage, old_texture[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_ADDRESSU, address_u[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_ADDRESSV, address_v[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_ADDRESSW, address_w[ stage ] );
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

	target->Release( );
}
