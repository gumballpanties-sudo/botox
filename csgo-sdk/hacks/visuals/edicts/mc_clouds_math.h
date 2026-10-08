#pragma once

#include "mc_sky.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

/* live minecraft clouds over the baked sky ( CloudRenderer hnv, fancy ): clouds.png cells as 12x4x12 boxes at y 192.33,
   drifting -x by ticks * 0.03 blocks, depth tested so only the first face a ray meets shows. one ps_3_0 pass per sky
   view walks the same cells per pixel ( k_shader == n_mc_sky::cloud_hit ). pure: tools/mc_sky_test.cpp runs it */
namespace n_mc_clouds
{
	inline constexpr double k_units_per_block = 40.0; // csgo player 72 units = mc player 1.8 blocks
	inline constexpr double k_wrap            = 256.0 * 12.0;

	// view origin ( source units ) + game time -> camera in cloud texture blocks. map origin at tick 1000 = the old bake
	inline void offset( const double curtime, const float origin_x, const float origin_y, float out[ 2 ] )
	{
		const double ticks = std::fmod( ( std::max )( curtime, 0.0 ) * 20.0, 256.0 * 400.0 );
		const double x     = n_mc_sky::k_reference_offset[ 0 ] - 30.0 + origin_x / k_units_per_block + ticks * 0.03;
		const double z     = n_mc_sky::k_reference_offset[ 1 ] - origin_y / k_units_per_block;
		const auto wrap    = []( const double v ) {
			const double m = std::fmod( v, k_wrap );
			return static_cast< float >( m < 0.0 ? m + k_wrap : m );
		};
		out[ 0 ] = wrap( x );
		out[ 1 ] = wrap( z );
	}

	// c30.w = GAMMA_LIGHT_SCALE ( hdr tonemap ). the sky faces get it too, so the clouds match them
	inline float gamma_scale( const float w )
	{
		return std::isfinite( w ) && w > 0.01f && w < 64.f ? w : 1.f;
	}

	inline bool invert( const double m[ 4 ][ 4 ], double out[ 4 ][ 4 ] )
	{
		double a[ 4 ][ 8 ]{ };
		for ( int r = 0; r < 4; r++ )
			for ( int c = 0; c < 4; c++ ) {
				a[ r ][ c ]     = m[ r ][ c ];
				a[ r ][ c + 4 ] = r == c ? 1.0 : 0.0;
			}

		for ( int c = 0; c < 4; c++ ) {
			int pivot = c;
			for ( int r = c + 1; r < 4; r++ )
				if ( std::fabs( a[ r ][ c ] ) > std::fabs( a[ pivot ][ c ] ) )
					pivot = r;

			if ( !( std::fabs( a[ pivot ][ c ] ) > 1e-12 ) )
				return false;

			std::swap( a[ c ], a[ pivot ] );
			const double inv = 1.0 / a[ c ][ c ];
			for ( double& v : a[ c ] )
				v *= inv;

			for ( int r = 0; r < 4; r++ ) {
				if ( r == c )
					continue;

				const double f = a[ r ][ c ];
				for ( int k = 0; k < 8; k++ )
					a[ r ][ k ] -= f * a[ c ][ k ];
			}
		}

		for ( int r = 0; r < 4; r++ )
			for ( int c = 0; c < 4; c++ )
				out[ r ][ c ] = a[ r ][ c + 4 ];

		return true;
	}

	/* rows = vs c8..c11 ( cViewProj, clip = rows * world ). world ray through the 4 draw_up quad corners
	   ( viewport px -0.5 / size - 0.5, strip order tl tr bl br ). eye = the point clip maps to w 0 */
	inline bool corner_rays( const float rows[ 4 ][ 4 ], const float width, const float height, float out[ 4 ][ 3 ] )
	{
		if ( !( width > 0.f ) || !( height > 0.f ) )
			return false;

		double m[ 4 ][ 4 ], inv[ 4 ][ 4 ];
		for ( int r = 0; r < 4; r++ )
			for ( int c = 0; c < 4; c++ )
				m[ r ][ c ] = rows[ r ][ c ];

		if ( !invert( m, inv ) )
			return false;

		const auto unproject = [ & ]( const double x, const double y, const double z, const double w, double p[ 3 ] ) {
			double h[ 4 ];
			for ( int r = 0; r < 4; r++ )
				h[ r ] = inv[ r ][ 0 ] * x + inv[ r ][ 1 ] * y + inv[ r ][ 2 ] * z + inv[ r ][ 3 ] * w;

			if ( !( std::fabs( h[ 3 ] ) > 1e-12 ) )
				return false;

			for ( int i = 0; i < 3; i++ )
				p[ i ] = h[ i ] / h[ 3 ];

			return true;
		};

		double eye[ 3 ];
		if ( !unproject( 0.0, 0.0, 1.0, 0.0, eye ) )
			return false;

		for ( int i = 0; i < 4; i++ ) {
			const double px = ( i & 1 ) ? width - 0.5 : -0.5, py = ( i & 2 ) ? height - 0.5 : -0.5;
			double p[ 3 ];
			if ( !unproject( 2.0 * px / width - 1.0, 1.0 - 2.0 * py / height, 0.5, 1.0, p ) )
				return false;

			double len = 0.0;
			for ( int k = 0; k < 3; k++ ) {
				const double d = p[ k ] - eye[ k ];
				out[ i ][ k ]  = static_cast< float >( d );
				len += d * d;
			}

			if ( !std::isfinite( len ) || !( len > 0.0 ) )
				return false;
		}

		return true;
	}

	// a = cell has cloud, row y = cell z
	inline void cells_argb( std::uint32_t* out, const int pitch_texels )
	{
		for ( int z = 0; z < 256; z++ )
			for ( int x = 0; x < 256; x++ )
				out[ z * pitch_texels + x ] = n_mc_sky::cloud_cell( x, z ) ? 0xffffffffu : 0x00ffffffu;
	}

	// TEXCOORD0 xyz = source ray, TEXCOORD1 = cloud offset x z, gamma scale. s0 = cells_argb, point + wrap.
	// never discard: texkill after the [loop] break killed every pixel on hal ( mc_sky_test gpu check )
	inline constexpr char k_shader[] = R"(
sampler2D s_cells:register(s0);
float4 main(float4 ray:TEXCOORD0,float4 info:TEXCOORD1):COLOR0{
	float3 n=normalize(ray.xyz);
	float3 d=float3(n.x,n.z,-n.y);
	if(d.y<=0.0)return float4(0,0,0,0);
	float h=192.33-65.62;
	float t=h/d.y;
	float top=(h+4.0)/d.y;
	if(t>=2048.0)return float4(0,0,0,0);
	float2 p=info.xy+d.xz*t;
	float2 c=floor(p/12.0);
	float2 home=floor(info.xy/12.0);
	float2 st=float2(d.x>0.0?1.0:-1.0,d.z>0.0?1.0:-1.0);
	float2 dt=float2(d.x!=0.0?12.0/abs(d.x):1e30,d.z!=0.0?12.0/abs(d.z):1e30);
	float2 nx=float2(d.x!=0.0?t+((c.x+(st.x>0.0?1.0:0.0))*12.0-p.x)/d.x:1e30,d.z!=0.0?t+((c.y+(st.y>0.0?1.0:0.0))*12.0-p.y)/d.z:1e30);
	float face=0.7;
	bool hit=false;
	[loop]for(int i=0;i<32;i++){
		if(t>=top||t>=2048.0)break;
		float2 r=c-home;
		if(dot(r,r)<=29241.0&&tex2Dlod(s_cells,float4((c+0.5)/256.0,0,0)).a>0.5){hit=true;break;}
		if(nx.x<nx.y){t=nx.x;nx.x+=dt.x;c.x+=st.x;face=0.9;}else{t=nx.y;nx.y+=dt.y;c.y+=st.y;face=0.8;}
	}
	if(!hit)return float4(0,0,0,0);
	return float4(face*info.zzz,0.8*(1.0-min(t/2048.0,1.0)));
}
)";
}
