#include "misc.h"
#include "mc_xp_orb.h"
#include "../visuals/screen/reflections.h"
#include "../visuals/screen/render_queue.h"
#include "../visuals/screen/stream_guard.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../utilities/perf/perf_watch.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern void botox_dbg_log( const char* fmt, ... );

void on_healthshot( const int trigger )
{
	if ( !GET_VARIABLE( g_variables.m_healthshot_effect, bool ) || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;

	const auto& on = GET_VARIABLE( g_variables.m_healthshot_triggers, std::vector< bool > );
	if ( trigger < 0 || trigger >= static_cast< int >( on.size( ) ) || !on[ trigger ] )
		return;

	const float duration                  = std::clamp( GET_VARIABLE( g_variables.m_healthshot_duration, float ), 0.1f, 5.f );
	g_ctx.m_local->get_health_shot_expire( ) = g_interfaces.m_global_vars_base->m_current_time + duration;
}

struct effect_data_t {
	c_vector m_origin, m_start, m_normal;
	c_angle m_angles;
	int m_flags;
	unsigned int m_entity;
	unsigned char m_pad[ 0x58 - 0x38 ];
	int m_hit_box;
	unsigned char m_tail[ 0x20 ];
};
static_assert( offsetof( effect_data_t, m_flags ) == 0x30 && offsetof( effect_data_t, m_hit_box ) == 0x58, "CEffectData layout" );

static bool dispatch_particle( const char* name, const c_vector& origin, const c_angle& angles )
{
	using callback_t     = void( __cdecl* )( const effect_data_t& );
	static auto callback = reinterpret_cast< callback_t >(
		g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 8B 0D ? ? ? ? 8B 01 FF 50 34 8B 4D 08 83 CA FF 5D E9" ) );
	if ( !callback || !g_interfaces.m_string_tables )
		return false;

	const auto table = g_interfaces.m_string_tables->find_table( "ParticleEffectNames" );
	const int index  = table ? table->find_string_index( name ) : 0;
	if ( index <= 0 || index >= 0xffff )
		return false;

	effect_data_t data{ };
	data.m_origin  = origin;
	data.m_start   = origin;
	data.m_angles  = angles;
	data.m_entity  = 0xffffffff;
	data.m_hit_box = index;
	callback( data );
	return true;
}

static void* effects( )
{
	static void* iface = g_modules[ CLIENT_DLL ].find_interface( "IEffects001" );
	return iface;
}

/* smoke / soul / bouncy balls: own particles, main thread only ( spawned from fire_event, stepped + queued in paint_traverse ) */
namespace
{
	struct kfx_particle_t {
		c_vector m_pos{ }, m_vel{ };
		c_color m_color{ };
		float m_age = 0.f, m_life = 1.f;
		float m_size  = 1.f;
		float m_seed  = 0.f;
		int m_style   = 0;
		bool m_resting = false;
		// minecraft: ticks at 20 hz, m_vel = u / tick
		c_vector m_prev{ };
		float m_tick_acc = 0.f;
		float m_tint[ 3 ]{ };
		int m_tick = 0, m_lifetime = 0, m_icon = 0;
	};

	constexpr std::size_t k_kfx_cap = 4096;
	constexpr int k_soul_hitboxes   = 19;
	constexpr float k_ball_gravity  = 640.f;
	constexpr int k_smoke_style     = 4;
	constexpr int k_mc_style        = 5;
	constexpr int k_mc_xp_style     = 6;

	std::vector< kfx_particle_t > g_kfx{ };
	std::mt19937 g_kfx_rng{ 0x6b667821u };
	float g_kfx_last_real = -1.f;
	float g_kfx_last_cur  = -1.f;

	float rand01( )
	{
		return std::uniform_real_distribution< float >( 0.f, 1.f )( g_kfx_rng );
	}

	float rand_range( const float lo, const float hi )
	{
		return lo + ( hi - lo ) * rand01( );
	}

	c_color particle_color( )
	{
		const c_color& picked = GET_VARIABLE( g_variables.m_death_particles_color, c_color );
		if ( GET_VARIABLE( g_variables.m_death_particles_multicolor, bool ) )
			return c_color::from_hsb( rand01( ), 0.75f, 1.f, picked.base< e_color_type::color_type_a >( ) );

		return picked;
	}

	void push( const kfx_particle_t& particle )
	{
		if ( g_kfx.size( ) < k_kfx_cap )
			g_kfx.push_back( particle );
	}

	void spawn_soul( c_base_entity* victim )
	{
		const int style  = std::clamp( GET_VARIABLE( g_variables.m_death_particles_soul_style, int ), 0, 3 );
		const int amount = std::clamp( GET_VARIABLE( g_variables.m_death_particles_amount, int ), 1, 40 );
		const float size = std::clamp( GET_VARIABLE( g_variables.m_death_particles_size, float ), 0.25f, 4.f );
		const float life = std::clamp( GET_VARIABLE( g_variables.m_death_particles_lifetime, float ), 0.5f, 10.f );

		std::vector< c_vector > points{ };
		points.reserve( k_soul_hitboxes );

		if ( auto& bones = victim->get_cached_bone_data( ); bones.count( ) > 0 && bones.base( ) ) {
			for ( int hitbox = 0; hitbox < k_soul_hitboxes && hitbox < e_hitboxes::hitbox_max; hitbox++ ) {
				const c_vector point = victim->get_hitbox_position( hitbox, bones.base( ) );
				if ( !point.is_zero( ) )
					points.push_back( point );
			}
		}

		if ( points.empty( ) ) {
			const c_vector origin = victim->get_abs_origin( );
			for ( int i = 0; i < 8; i++ )
				points.push_back( origin + c_vector( 0.f, 0.f, 8.f + 8.f * static_cast< float >( i ) ) );
		}

		const c_vector core = points.front( ) + c_vector( 0.f, 0.f, 40.f );

		for ( const c_vector& point : points ) {
			for ( int i = 0; i < amount; i++ ) {
				kfx_particle_t particle{ };
				particle.m_pos   = point + c_vector( rand_range( -5.f, 5.f ), rand_range( -5.f, 5.f ), rand_range( -5.f, 5.f ) );
				particle.m_style = style;

				const float angle = rand_range( 0.f, 6.2831853f );

				switch ( style ) {
				case 0: {
					const float speed = rand_range( 8.f, 30.f );
					particle.m_vel    = c_vector( std::cos( angle ) * speed, std::sin( angle ) * speed, rand_range( 4.f, 15.f ) );
					break;
				}
				case 1: particle.m_vel = c_vector( rand_range( -2.5f, 2.5f ), rand_range( -2.5f, 2.5f ), rand_range( 12.f, 45.f ) ); break;
				case 2: {
					const float speed = rand_range( 15.f, 50.f );
					particle.m_vel    = c_vector( std::cos( angle ) * speed, std::sin( angle ) * speed, rand_range( -10.f, 10.f ) );
					break;
				}
				default: {
					c_vector to_core   = core - particle.m_pos;
					const float length = to_core.length( );
					if ( length > 0.1f )
						to_core = to_core * ( 1.f / length );
					particle.m_vel = to_core * rand_range( 20.f, 40.f );
					break;
				}
				}

				particle.m_size  = rand_range( 1.f, 2.f ) * size;
				particle.m_life  = rand_range( 0.7f, 1.f ) * life;
				particle.m_color = particle_color( );
				push( particle );
			}
		}
	}

	void spawn_smoke( c_base_entity* victim )
	{
		const int amount = std::clamp( GET_VARIABLE( g_variables.m_death_particles_amount, int ), 1, 40 );
		const float size = std::clamp( GET_VARIABLE( g_variables.m_death_particles_size, float ), 0.25f, 3.f );
		const float life = std::clamp( GET_VARIABLE( g_variables.m_death_particles_lifetime, float ), 0.5f, 10.f );

		const auto random_dir = [ ]( ) {
			const float z = rand_range( -1.f, 1.f ), angle = rand_range( 0.f, 6.2831853f ), r = std::sqrt( 1.f - z * z );
			return c_vector( std::cos( angle ) * r, std::sin( angle ) * r, z );
		};

		const auto emit = [ & ]( const c_vector& at, const c_vector& dir, const float radius ) {
			kfx_particle_t puff{ };
			puff.m_pos   = at;
			puff.m_vel   = dir * rand_range( 2.f, 9.f ) + c_vector( 0.f, 0.f, rand_range( 2.f, 8.f ) );
			puff.m_style = k_smoke_style;
			puff.m_size  = radius * rand_range( 0.75f, 1.15f ) * size;
			puff.m_life  = rand_range( 0.6f, 1.f ) * life;
			puff.m_seed  = rand01( );
			const int shade = static_cast< int >( rand_range( 150.f, 205.f ) );
			puff.m_color    = c_color( shade, shade, shade, 255 );
			push( puff );
		};

		hitbox_resolver_t resolver{ };
		auto& bones   = victim->get_cached_bone_data( );
		bool any_bone = false;

		if ( bones.count( ) > 0 && bones.base( ) && resolver.setup( victim ) ) {
			for ( int hitbox = 0; hitbox < resolver.m_set->m_hit_boxes; hitbox++ ) {
				const mstudiobbox_t* box = resolver.m_set->get_hitbox( hitbox );
				if ( !box || box->m_bone < 0 || box->m_bone >= bones.count( ) )
					continue;

				const matrix3x4_t& bone = bones.base( )[ box->m_bone ];
				const c_vector a        = g_math.vector_transform( box->m_bb_min, bone );
				const c_vector b        = g_math.vector_transform( box->m_bb_max, bone );
				const float radius      = box->m_radius > 0.f ? box->m_radius : 3.f;
				any_bone                = true;

				for ( int i = 0; i < amount; i++ ) {
					const c_vector dir = random_dir( );
					emit( a + ( b - a ) * rand01( ) + dir * ( radius * rand_range( 0.f, 0.6f ) ), dir, radius );
				}
			}
		}

		if ( !any_bone ) {
			const c_vector origin = victim->get_abs_origin( );
			for ( int i = 0; i < amount * 8; i++ ) {
				const c_vector dir = random_dir( );
				emit( origin + c_vector( 0.f, 0.f, rand_range( 6.f, 66.f ) ) + dir * rand_range( 0.f, 6.f ), dir, 6.f );
			}
		}
	}

	void step_smoke( kfx_particle_t& puff, const float dt )
	{
		puff.m_vel = puff.m_vel * ( std::max )( 0.f, 1.f - 2.5f * dt );
		puff.m_vel.m_z += 6.f * dt;
		puff.m_pos = puff.m_pos + puff.m_vel * dt;
	}

	/* minecraft 1.21.11 crit hit, decompiled: ClientboundAnimatePacket( target, 4 ) -> TrackingEmitter( CRIT, 3 ticks ):
	   per tick 16 tries d e f in unit ball, pos = bb * ( d / 4, 0.5 + e / 4, f / 4 ), vel ( d, e + 0.2, f ) -> CritParticle.
	   1 block = 40u ( 1.8 block player = 72u ), particle m_tick_acc < 0 = emitter tick not reached yet */
	constexpr float k_mc_block = 40.f;

	// textures/particle/critical_hit.png rgb, 0 = clear ( tools/mc_crit_check.py )
	constexpr std::uint32_t k_mc_crit[ 8 ][ 8 ] = {
		{ 0xffffff, 0, 0, 0, 0, 0, 0xffffff, 0 },
		{ 0, 0xffffff, 0xcecece, 0, 0xcecece, 0xffffff, 0, 0 },
		{ 0, 0xcecece, 0xffffff, 0xcecece, 0xffffff, 0xcecece, 0, 0 },
		{ 0, 0, 0xcecece, 0xffffff, 0xcecece, 0, 0, 0 },
		{ 0, 0xcecece, 0xffffff, 0xcecece, 0xffffff, 0xcecece, 0, 0 },
		{ 0, 0xffffff, 0xcecece, 0, 0xcecece, 0xffffff, 0, 0 },
		{ 0xffffff, 0, 0, 0, 0, 0, 0xffffff, 0 },
		{ 0, 0, 0, 0, 0, 0, 0, 0 },
	};

	void mc_tick( kfx_particle_t& crit )
	{
		crit.m_prev = crit.m_pos;
		if ( crit.m_tick++ >= crit.m_lifetime ) {
			crit.m_age = crit.m_life;
			return;
		}
		crit.m_vel.m_z -= 0.04f * 0.5f * k_mc_block;
		crit.m_pos = crit.m_pos + crit.m_vel;
		crit.m_vel = crit.m_vel * 0.7f;
		crit.m_tint[ 1 ] *= 0.96f;
		crit.m_tint[ 2 ] *= 0.9f;
	}

	/* ExperienceOrb cgz, decompiled: bb 0.5 block, gravity 0.03, drag 0.98, ground 0.6 * 0.98, lands below -0.03 bounce 0.4.
	   player pull / pickup / merge skipped */
	void mc_xp_tick( kfx_particle_t& orb )
	{
		orb.m_prev = orb.m_pos;
		orb.m_vel.m_z -= 0.03f * k_mc_block;
		const float fall = orb.m_vel.m_z;

		c_trace_filter filter( g_ctx.m_local );
		c_vector move = orb.m_vel;
		bool ground   = false;
		for ( int i = 0; i < 3 && !move.is_zero( ); i++ ) {
			trace_t trace{ };
			g_interfaces.m_engine_trace->trace_ray( ray_t( orb.m_pos, orb.m_pos + move, c_vector( -10.f, -10.f, 0.f ), c_vector( 10.f, 10.f, 20.f ) ),
			                                        e_mask::mask_solid_brushonly, &filter, &trace );
			if ( trace.m_start_solid )
				break;
			orb.m_pos = trace.m_end;
			if ( trace.m_fraction >= 1.f )
				break;

			const c_vector normal = trace.m_plane.m_normal;
			ground                = ground || ( normal.m_z > 0.7f && move.m_z < 0.f );
			orb.m_vel             = orb.m_vel - normal * orb.m_vel.dot_product( normal );
			move                  = move * ( 1.f - trace.m_fraction );
			move                  = move - normal * move.dot_product( normal );
		}

		orb.m_vel = orb.m_vel * ( ground ? 0.6f * 0.98f : 0.98f );
		if ( ground && fall < -0.03f * k_mc_block )
			orb.m_vel.m_z = -fall * 0.4f;
	}

	// ExperienceOrb.getExperienceValue split + getIcon
	int mc_xp_value( const int xp )
	{
		for ( const int step : { 2477, 1237, 617, 307, 149, 73, 37, 17, 7, 3 } )
			if ( xp >= step )
				return step;
		return 1;
	}

	int mc_xp_icon( const int value )
	{
		int icon = 10;
		for ( const int step : { 2477, 1237, 617, 307, 149, 73, 37, 17, 7, 3 } ) {
			if ( value >= step )
				return icon;
			icon--;
		}
		return 0;
	}

	void spawn_mc_xp( c_base_entity* victim, const float size )
	{
		const float life = std::clamp( GET_VARIABLE( g_variables.m_death_particles_mc_xp_time, float ), 1.f, 60.f );
		const int count  = std::uniform_int_distribution< int >( 4, 10 )( g_kfx_rng );

		for ( int i = 0; i < count; i++ ) {
			kfx_particle_t orb{ };
			orb.m_style = k_mc_xp_style;
			orb.m_life  = life;
			orb.m_pos   = orb.m_prev = victim->get_abs_origin( );
			orb.m_vel   = c_vector( ( rand01( ) * 0.2f - 0.1f ) * 2.f, ( rand01( ) * 0.2f - 0.1f ) * 2.f, rand01( ) * 0.2f * 2.f ) * k_mc_block;
			orb.m_size  = size;
			orb.m_icon  = mc_xp_icon( mc_xp_value( std::uniform_int_distribution< int >( 1, 40 )( g_kfx_rng ) ) );
			push( orb );
		}
	}

	void spawn_mc( c_base_entity* victim )
	{
		const float size = std::clamp( GET_VARIABLE( g_variables.m_death_particles_size, float ), 0.25f, 3.f );
		const int mode   = std::clamp( GET_VARIABLE( g_variables.m_death_particles_mc_mode, int ), 0, 2 );

		if ( mode != 0 )
			spawn_mc_xp( victim, size );
		if ( mode == 2 )
			return;

		c_vector mins( -16.f, -16.f, 0.f ), maxs( 16.f, 16.f, 72.f );
		if ( const auto renderable = victim->get_client_renderable( ) )
			if ( const auto unknown = renderable->get_client_unknown( ) )
				if ( const auto collideable = unknown->get_collideable( ) ) {
					mins = collideable->get_obb_mins( );
					maxs = collideable->get_obb_maxs( );
				}
		const c_vector feet = victim->get_abs_origin( );
		const float width   = maxs.m_x - mins.m_x;
		const float height  = maxs.m_z - mins.m_z;

		for ( int burst = 0; burst < 3; burst++ )
			for ( int i = 0; i < 16; i++ ) {
				const float d = rand_range( -1.f, 1.f ), e = rand_range( -1.f, 1.f ), f = rand_range( -1.f, 1.f );
				if ( d * d + e * e + f * f > 1.f )
					continue;

				kfx_particle_t crit{ };
				crit.m_style    = k_mc_style;
				crit.m_life     = 10.f;
				crit.m_tick_acc = -static_cast< float >( burst );
				crit.m_pos      = feet + c_vector( width * d / 4.f, width * f / 4.f, mins.m_z + height * ( 0.5f + e / 4.f ) );

				c_vector base( rand_range( -1.f, 1.f ), rand_range( -1.f, 1.f ), rand_range( -1.f, 1.f ) );
				const float length = base.length( );
				base = length > 1e-4f ? base * ( ( rand01( ) + rand01( ) + 1.f ) * 0.15f * 0.4f / length ) : c_vector( 0.f, 0.f, 0.f );
				base.m_z += 0.1f;
				crit.m_vel = ( base * 0.1f + c_vector( d, f, e + 0.2f ) * 0.4f ) * k_mc_block;

				const float shade = rand01( ) * 0.3f + 0.6f;
				crit.m_tint[ 0 ] = crit.m_tint[ 1 ] = crit.m_tint[ 2 ] = shade;
				crit.m_size     = 0.1f * ( rand01( ) * 0.5f + 0.5f ) * 2.f * 0.75f * k_mc_block * size;
				crit.m_lifetime = ( std::max )( static_cast< int >( 6.f / ( rand01( ) * 0.8f + 0.6f ) ), 1 );

				mc_tick( crit ); // CritParticle ctor ticks once
				push( crit );
			}
	}

	void spawn_balls( c_base_entity* victim )
	{
		const int amount = std::clamp( GET_VARIABLE( g_variables.m_death_particles_amount, int ), 5, 100 );
		const float size = std::clamp( GET_VARIABLE( g_variables.m_death_particles_size, float ), 0.5f, 4.f );
		const float life = std::clamp( GET_VARIABLE( g_variables.m_death_particles_lifetime, float ), 1.f, 8.f );

		const c_vector origin = victim->get_abs_origin( ) + c_vector( 0.f, 0.f, 40.f );

		for ( int i = 0; i < amount; i++ ) {
			kfx_particle_t ball{ };
			ball.m_pos = origin + c_vector( rand_range( -8.f, 8.f ), rand_range( -8.f, 8.f ), rand_range( -4.f, 6.f ) );

			const float angle = rand_range( 0.f, 6.2831853f );
			const float speed = rand_range( 80.f, 260.f );
			ball.m_vel        = c_vector( std::cos( angle ) * speed, std::sin( angle ) * speed, rand_range( 180.f, 420.f ) );
			ball.m_style      = -1;
			ball.m_size       = rand_range( 1.6f, 3.2f ) * size;
			ball.m_life       = rand_range( 0.8f, 1.2f ) * life;
			ball.m_color      = particle_color( );
			push( ball );
		}
	}

	void step_ball( kfx_particle_t& ball, const float dt )
	{
		if ( ball.m_resting )
			return;

		ball.m_vel.m_z -= k_ball_gravity * dt;

		c_vector move = ball.m_vel * dt;

		// one trace per 24u of travel, max 4: fast balls don't tunnel through thin brushes
		const int steps   = std::clamp( static_cast< int >( std::ceil( move.length( ) / 24.f ) ), 1, 4 );
		const float slice = 1.f / static_cast< float >( steps );

		c_trace_filter filter( g_ctx.m_local );

		for ( int i = 0; i < steps; i++ ) {
			const c_vector target = ball.m_pos + move * slice;

			trace_t trace{ };
			g_interfaces.m_engine_trace->trace_ray( ray_t( ball.m_pos, target ), e_mask::mask_solid, &filter, &trace );

			if ( trace.m_start_solid ) {
				ball.m_resting = true;
				return;
			}

			if ( trace.m_fraction >= 1.f ) {
				ball.m_pos = target;
				continue;
			}

			const c_vector normal = trace.m_plane.m_normal;
			ball.m_pos            = trace.m_end + normal * 0.25f;

			const float into   = ball.m_vel.dot_product( normal );
			const c_vector tan = ball.m_vel - normal * into;
			ball.m_vel         = tan * 0.85f - normal * ( into * 0.62f );
			move               = ball.m_vel * ( dt * ( 1.f - trace.m_fraction ) );

			if ( normal.m_z > 0.7f && ball.m_vel.length( ) < 24.f ) {
				ball.m_vel     = c_vector( 0.f, 0.f, 0.f );
				ball.m_resting = true;
				return;
			}
		}
	}

	void step_soul( kfx_particle_t& particle, const float dt )
	{
		particle.m_vel.m_z += ( particle.m_style == 3 ? 6.f : 14.f ) * dt;
		particle.m_vel = particle.m_vel * ( std::max )( 0.f, 1.f - 0.6f * dt );

		if ( particle.m_style == 1 || particle.m_style == 2 ) {
			const float jitter = particle.m_style == 1 ? 1.2f : 0.6f;
			particle.m_pos.m_x += rand_range( -jitter, jitter );
			particle.m_pos.m_y += rand_range( -jitter, jitter );
		}

		particle.m_pos = particle.m_pos + particle.m_vel * dt;
	}

	/* world billboard corner = center + ( right * m_ox + up * m_oy ) * m_radius, expanded in k_kfx_vs with the view's axes.
	   m_su / m_sv run -1..1 across the quad ( shape space for the pixel shader ) */
	struct kfx_vertex_t {
		float m_x, m_y, m_z;
		unsigned int m_color;
		float m_ox, m_oy, m_radius, m_kind;
		float m_su, m_sv;
	};

	enum e_kfx_kind : int { kfx_square, kfx_disc, kfx_glow, kfx_smoke };

	// rebuilt every paint, drawn in kill_effects_world ( main thread both )
	std::vector< kfx_vertex_t > g_kfx_quads{ };

	// spin = 0..1 shader param packed into frac( kind ), smoke only
	void kfx_quad( const c_vector& at, const float radius, const float x0, const float y0, const float x1, const float y1, const e_kfx_kind kind,
	               const ImU32 color, const float spin = 0.f )
	{
		// imgui packs abgr, d3d diffuse = argb
		const unsigned int argb = ( color & 0xff00ff00u ) | ( ( color & 0xffu ) << 16 ) | ( ( color >> 16 ) & 0xffu );
		const auto corner       = [ & ]( const float x, const float y, const float su, const float sv ) {
			return kfx_vertex_t{ at.m_x, at.m_y, at.m_z, argb, x, y, radius, static_cast< float >( kind ) + spin, su, sv };
		};
		const kfx_vertex_t a = corner( x0, y0, -1.f, -1.f ), b = corner( x1, y0, 1.f, -1.f ), c = corner( x1, y1, 1.f, 1.f ), d = corner( x0, y1, -1.f, 1.f );
		g_kfx_quads.insert( g_kfx_quads.end( ), { a, b, c, a, c, d } );
	}

	/* pixel art spanning -1..1, row 0 on top: one square per texel, texel( x, y ) == 0 = clear */
	template < typename T >
	void kfx_sprite( const c_vector& at, const float radius, const int texels, T&& texel )
	{
		const float step = 2.f / static_cast< float >( texels );
		for ( int y = 0; y < texels; y++ )
			for ( int x = 0; x < texels; x++ )
				if ( const ImU32 color = texel( x, y ) )
					kfx_quad( at, radius, -1.f + step * x, 1.f - step * ( y + 1 ), -1.f + step * ( x + 1 ), 1.f - step * y, kfx_square, color );
	}

	/* ---- melt sim: pure cpu, built offline by tools/melt_bench ( rerun it after any edit up to "melt sim end" ) ---- */
	constexpr float k_melt_cell        = 4.f;
	constexpr int k_melt_grid          = 96;
	constexpr float k_melt_climb       = 6.f;
	constexpr float k_melt_sink_time   = 1.6f;
	constexpr float k_melt_sink_ease   = 1.4f;
	constexpr float k_melt_squash      = 0.45f;
	constexpr float k_melt_flare       = 5.f;
	constexpr float k_melt_flare_band  = 14.f;
	constexpr float k_melt_wet_band    = 12.f;
	constexpr float k_melt_drip        = 12.f;
	constexpr float k_melt_drip_cell   = 4.f;
	constexpr float k_melt_soften      = 0.55f;
	constexpr int k_melt_soften_passes = 3;
	constexpr float k_melt_bury        = 2.f;
	constexpr float k_melt_weld        = 64.f;
	constexpr float k_melt_body_volume = 5000.f;
	constexpr float k_melt_pool_cell   = 2.f;
	constexpr int k_melt_pool_grid     = 192;
	constexpr float k_melt_flow        = 60.f;
	constexpr float k_melt_stop        = 0.55f;
	constexpr float k_melt_film_cap    = 2.5f;
	constexpr float k_melt_edge        = 0.15f;
	constexpr float k_melt_lift        = 0.1f;
	constexpr float k_melt_max_dt      = 1.f / 120.f;
	constexpr int k_melt_max_sub       = 64;
	constexpr float k_melt_facing      = 0.3f;
	constexpr float k_melt_seen_slack  = 0.75f;
	constexpr int k_melt_zbuf          = 384;
	constexpr float k_melt_still       = 0.6f;
	constexpr int k_melt_probe_budget  = 24;
	constexpr int k_melt_from_above    = -0x7fffffff;
	constexpr int k_melt_vvd_id        = ( 'V' << 24 ) + ( 'S' << 16 ) + ( 'D' << 8 ) + 'I';
	static_assert( k_melt_pool_cell * 2.f == k_melt_cell && k_melt_pool_grid == k_melt_grid * 2 );

	enum e_melt_cell : unsigned char { melt_cell_unknown, melt_cell_open, melt_cell_blocked, melt_cell_asked };

	struct melt_cell_t {
		float m_floor         = 0.f;
		unsigned char m_state = melt_cell_unknown;
	};

	/* m_from_x == k_melt_from_above: straight down from m_z, else walked in from that open cell */
	struct melt_probe_t {
		int m_x, m_y, m_from_x, m_from_y;
		float m_z;
	};

	/* csgo vertexFileHeader_t / mstudiovertex_t / mstudiobodyparts_t / mstudiomodel_t ( x86 ) */
	struct melt_vvd_header_t {
		int m_id, m_version, m_checksum, m_lods, m_lod_vertices[ 8 ], m_fixups, m_fixup_start, m_vertex_start, m_tangent_start;
	};
	struct melt_vvd_vertex_t {
		float m_weight[ 3 ];
		unsigned char m_bone[ 3 ], m_bones;
		float m_pos[ 3 ], m_normal[ 3 ], m_uv[ 2 ];
	};
	struct melt_body_part_t {
		int m_name, m_models, m_base, m_model_index;
	};
	struct melt_studio_model_t {
		char m_name[ 64 ];
		int m_type;
		float m_radius;
		int m_meshes, m_mesh_index, m_vertices, m_vertex_index;
		unsigned char m_pad[ 60 ];
	};
	static_assert( sizeof( melt_vvd_header_t ) == 64 && sizeof( melt_vvd_vertex_t ) == 48 && sizeof( melt_body_part_t ) == 16 &&
	               sizeof( melt_studio_model_t ) == 148 );

	struct melt_raw_vert_t {
		c_vector m_pos{ }, m_normal{ };
		float m_weight[ 3 ]{ };
		unsigned char m_bone[ 3 ]{ };
		int m_count = 0;
	};

	/* hitbox in bone space */
	struct melt_capsule_t {
		c_vector m_a{ }, m_b{ };
		float m_radius = 0.f;
		int m_bone     = 0;
	};

	/* immutable once finished: shared by the main thread and the worker */
	struct melt_template_t {
		std::vector< melt_raw_vert_t > m_verts{ };
		std::vector< unsigned int > m_tris{ };
		std::vector< int > m_adj_start{ }, m_adj{ };
		std::vector< float > m_share{ };
		std::vector< matrix3x4_t > m_pose{ };
		float m_volume  = 0.f;
		int m_raw_verts = 0;
	};

	struct melt_vertex_t {
		float m_x, m_y, m_z, m_nx, m_ny, m_nz;
		unsigned int m_color;
		float m_u, m_v, m_tex, m_wet;
	};

	struct melt_mesh_t {
		std::vector< melt_vertex_t > m_vertices{ };
		std::vector< unsigned int > m_indices{ };
		int m_body_vertices = 0;
	};

	struct melt_stats_t {
		int m_live = 0, m_sub = 0, m_wet_cells = 0, m_trusted = 0, m_split = 0;
		float m_body_ms = 0.f, m_pool_ms = 0.f, m_mesh_ms = 0.f, m_pool_volume = 0.f, m_lost = 0.f, m_change = 1e9f;
	};

	/* worker owns it while a job runs ( melt_t::m_sim null ), main thread otherwise */
	struct melt_sim_t {
		std::shared_ptr< const melt_template_t > m_skin{ };
		std::vector< matrix3x4_t > m_bones{ };
		float m_age = 0.f, m_top = 1e9f, m_volume = 0.f, m_base_floor = 0.f;
		int m_ox = 0, m_oy = 0;
		std::vector< melt_cell_t > m_cells{ };
		std::vector< melt_probe_t > m_probes{ };
		std::vector< c_vector > m_at_capture{ };
		std::vector< unsigned int > m_samples{ };
		std::vector< float > m_rgb{ }, m_uv{ }, m_tex{ }, m_drip{ };
		std::vector< unsigned char > m_gone{ };
		/* m_paint = rgb per pool cell ( not times depth ) */
		std::vector< float > m_h{ }, m_paint{ }, m_floor{ };
		std::vector< unsigned char > m_open{ };
		melt_stats_t m_stats{ };
		float m_view[ 4 ][ 4 ]{ }, m_crop[ 4 ]{ }, m_screen[ 2 ]{ };
		bool m_textured = false, m_colors_ready = false, m_melted = false;
	};

	c_vector melt_transform( const matrix3x4_t& m, const c_vector& p )
	{
		return c_vector( m.data[ 0 ][ 0 ] * p.m_x + m.data[ 0 ][ 1 ] * p.m_y + m.data[ 0 ][ 2 ] * p.m_z + m.data[ 0 ][ 3 ],
		                 m.data[ 1 ][ 0 ] * p.m_x + m.data[ 1 ][ 1 ] * p.m_y + m.data[ 1 ][ 2 ] * p.m_z + m.data[ 1 ][ 3 ],
		                 m.data[ 2 ][ 0 ] * p.m_x + m.data[ 2 ][ 1 ] * p.m_y + m.data[ 2 ][ 2 ] * p.m_z + m.data[ 2 ][ 3 ] );
	}

	c_vector melt_rotate( const matrix3x4_t& m, const c_vector& p )
	{
		return c_vector( m.data[ 0 ][ 0 ] * p.m_x + m.data[ 0 ][ 1 ] * p.m_y + m.data[ 0 ][ 2 ] * p.m_z,
		                 m.data[ 1 ][ 0 ] * p.m_x + m.data[ 1 ][ 1 ] * p.m_y + m.data[ 1 ][ 2 ] * p.m_z,
		                 m.data[ 2 ][ 0 ] * p.m_x + m.data[ 2 ][ 1 ] * p.m_y + m.data[ 2 ][ 2 ] * p.m_z );
	}

	matrix3x4_t melt_identity( )
	{
		matrix3x4_t m;
		for ( int i = 0; i < 3; i++ )
			for ( int j = 0; j < 4; j++ )
				m.data[ i ][ j ] = i == j ? 1.f : 0.f;
		return m;
	}

	void melt_mats( const melt_sim_t& sim, std::vector< matrix3x4_t >& mats )
	{
		const auto& pose = sim.m_skin->m_pose;
		mats.resize( ( std::min )( sim.m_bones.size( ), pose.size( ) ) );
		for ( std::size_t b = 0; b < mats.size( ); b++ ) {
			const matrix3x4_t &a = sim.m_bones[ b ], &p = pose[ b ];
			for ( int i = 0; i < 3; i++ )
				for ( int j = 0; j < 4; j++ )
					mats[ b ].data[ i ][ j ] = a.data[ i ][ 0 ] * p.data[ 0 ][ j ] + a.data[ i ][ 1 ] * p.data[ 1 ][ j ] + a.data[ i ][ 2 ] * p.data[ 2 ][ j ] +
					                           ( j == 3 ? a.data[ i ][ 3 ] : 0.f );
		}
	}

	c_vector melt_skin( const melt_raw_vert_t& vert, const std::vector< matrix3x4_t >& mats, const bool normal )
	{
		const c_vector& p = normal ? vert.m_normal : vert.m_pos;
		c_vector sum{ };
		float total = 0.f;
		for ( int k = 0; k < vert.m_count; k++ ) {
			if ( vert.m_bone[ k ] >= mats.size( ) || vert.m_weight[ k ] <= 0.f )
				continue;
			sum += ( normal ? melt_rotate( mats[ vert.m_bone[ k ] ], p ) : melt_transform( mats[ vert.m_bone[ k ] ], p ) ) * vert.m_weight[ k ];
			total += vert.m_weight[ k ];
		}
		return total > 1e-6f ? sum * ( 1.f / total ) : p;
	}

	int melt_i32( const unsigned char* base, const long long offset )
	{
		int value = 0;
		std::memcpy( &value, base + offset, sizeof( value ) );
		return value;
	}

	int melt_u16( const unsigned char* base, const long long offset )
	{
		unsigned short value = 0;
		std::memcpy( &value, base + offset, sizeof( value ) );
		return value;
	}

	/* hdr = studiohdr_t image ( .mdl bytes ), vvd = vertexFileHeader_t, vtx = .dx90.vtx file; LOD 0 of the chosen bodygroups. null = ok, else why not */
	const char* melt_parse( const unsigned char* hdr, const melt_vvd_header_t* vvd, const unsigned char* vtx, const std::size_t vtx_size, const int body,
	                        melt_template_t& out )
	{
		const int checksum = melt_i32( hdr, 8 );
		if ( vvd->m_id != k_melt_vvd_id || vvd->m_checksum != checksum )
			return "bad vvd";
		if ( vvd->m_fixups )
			return "fixups";
		if ( vtx_size < 36 || melt_i32( vtx, 16 ) != checksum )
			return "bad vtx";

		const int bones = melt_i32( hdr, 156 ), bone_index = melt_i32( hdr, 160 ), parts = melt_i32( hdr, 232 ), part_index = melt_i32( hdr, 236 );
		if ( bones <= 0 || bones > 256 || parts <= 0 || parts > 64 )
			return "header";
		out.m_pose.resize( static_cast< std::size_t >( bones ) );
		for ( int b = 0; b < bones; b++ )
			std::memcpy( &out.m_pose[ b ], hdr + bone_index + 216ll * b + 0x60, sizeof( matrix3x4_t ) );

		if ( melt_i32( vtx, 28 ) != parts )
			return "vtx parts";
		const long long vtx_parts = melt_i32( vtx, 32 );
		const auto fits           = [ vtx_size ]( const long long offset, const long long length ) {
            return offset >= 0 && length >= 0 && offset + length <= static_cast< long long >( vtx_size );
		};

		const auto vertices = reinterpret_cast< const melt_vvd_vertex_t* >( reinterpret_cast< const unsigned char* >( vvd ) + vvd->m_vertex_start );
		std::vector< int > local( static_cast< std::size_t >( ( std::max )( vvd->m_lod_vertices[ 0 ], 0 ) ), -1 );

		for ( int part = 0; part < parts; part++ ) {
			const auto body_part = reinterpret_cast< const melt_body_part_t* >( hdr + part_index + 16ll * part );
			if ( body_part->m_models <= 0 || body_part->m_base <= 0 )
				continue;

			const int pick = ( body / body_part->m_base ) % body_part->m_models;
			const auto model_bytes  = reinterpret_cast< const unsigned char* >( body_part ) + body_part->m_model_index + 148ll * pick;
			const auto studio_model = reinterpret_cast< const melt_studio_model_t* >( model_bytes );
			const int first         = studio_model->m_vertex_index / static_cast< int >( sizeof( melt_vvd_vertex_t ) );
			const int count         = studio_model->m_vertices;
			if ( first < 0 || count < 0 || first + count > static_cast< int >( local.size( ) ) )
				return "range";

			for ( int i = first; i < first + count; i++ ) {
				const melt_vvd_vertex_t& vertex = vertices[ i ];
				melt_raw_vert_t raw{ };
				raw.m_pos    = c_vector( vertex.m_pos );
				raw.m_normal = c_vector( vertex.m_normal );
				raw.m_count  = std::clamp< int >( vertex.m_bones, 1, 3 );
				for ( int k = 0; k < raw.m_count; k++ ) {
					raw.m_bone[ k ]   = vertex.m_bone[ k ];
					raw.m_weight[ k ] = vertex.m_bone[ k ] < bones ? vertex.m_weight[ k ] : 0.f;
				}
				local[ i ] = static_cast< int >( out.m_verts.size( ) );
				out.m_verts.push_back( raw );
			}

			const long long vp = vtx_parts + 8ll * part;
			if ( !fits( vp, 8 ) || melt_i32( vtx, vp ) != body_part->m_models )
				return "vtx models";
			const long long vm = vp + melt_i32( vtx, vp + 4 ) + 8ll * pick;
			if ( !fits( vm, 8 ) || melt_i32( vtx, vm ) < 1 )
				return "vtx lods";
			const long long lod = vm + melt_i32( vtx, vm + 4 );
			if ( !fits( lod, 12 ) || melt_i32( vtx, lod ) != studio_model->m_meshes )
				return "vtx meshes";

			for ( int m = 0; m < studio_model->m_meshes; m++ ) {
				const int mesh_vertex = melt_i32( model_bytes, studio_model->m_mesh_index + 116ll * m + 12 );
				const long long ms    = lod + melt_i32( vtx, lod + 4 ) + 9ll * m;
				if ( !fits( ms, 9 ) )
					return "vtx mesh";

				for ( int g = 0; g < melt_i32( vtx, ms ); g++ ) {
					const long long sg = ms + melt_i32( vtx, ms + 4 ) + 33ll * g;
					if ( !fits( sg, 33 ) )
						return "vtx group";
					const int nv = melt_i32( vtx, sg ), ni = melt_i32( vtx, sg + 8 );
					const long long vo = sg + melt_i32( vtx, sg + 4 ), io = sg + melt_i32( vtx, sg + 12 );
					if ( nv < 0 || ni < 0 || ni % 3 || !fits( vo, 9ll * nv ) || !fits( io, 2ll * ni ) )
						return "vtx strip";

					for ( int k = 0; k < ni; k++ ) {
						const int index = melt_u16( vtx, io + 2ll * k );
						if ( index >= nv )
							return "vtx index";
						const int global = first + mesh_vertex + melt_u16( vtx, vo + 9ll * index + 4 );
						if ( global < first || global >= first + count )
							return "vtx vertex";
						out.m_tris.push_back( static_cast< unsigned int >( local[ global ] ) );
					}
				}
			}
		}

		if ( out.m_verts.empty( ) || out.m_tris.size( ) < 3 )
			return "empty";
		out.m_raw_verts = static_cast< int >( out.m_verts.size( ) );
		return nullptr;
	}

	/* no vvd / vtx: one closed capsule per hitbox, pose = identity */
	void melt_capsule_mesh( const std::vector< melt_capsule_t >& capsules, const int bones, melt_template_t& out )
	{
		constexpr int around = 12, cap_rows = 4, rows = 2 * cap_rows + 2;
		out.m_pose.assign( static_cast< std::size_t >( ( std::max )( bones, 1 ) ), melt_identity( ) );

		for ( const melt_capsule_t& capsule : capsules ) {
			const c_vector axis = capsule.m_b - capsule.m_a;
			const float length  = axis.length( );
			const c_vector dir  = length > 1e-4f ? axis * ( 1.f / length ) : c_vector( 0.f, 0.f, 1.f );
			const c_vector u    = dir.cross_product( std::abs( dir.m_z ) < 0.9f ? c_vector( 0.f, 0.f, 1.f ) : c_vector( 1.f, 0.f, 0.f ) ).normalized( );
			const c_vector v    = dir.cross_product( u );
			const unsigned int base = static_cast< unsigned int >( out.m_verts.size( ) );

			for ( int k = 0; k < rows; k++ ) {
				const bool top       = k > cap_rows;
				const float theta    = 1.5707963f * ( top ? static_cast< float >( k - cap_rows - 1 ) / cap_rows : static_cast< float >( k ) / cap_rows - 1.f );
				const c_vector& from = top ? capsule.m_b : capsule.m_a;
				for ( int s = 0; s < around; s++ ) {
					const float phi      = 6.2831853f * static_cast< float >( s ) / around;
					const c_vector rim   = ( u * std::cos( phi ) + v * std::sin( phi ) ) * std::cos( theta ) + dir * std::sin( theta );
					melt_raw_vert_t raw{ };
					raw.m_pos         = from + rim * capsule.m_radius;
					raw.m_normal      = rim;
					raw.m_count       = 1;
					raw.m_bone[ 0 ]   = static_cast< unsigned char >( std::clamp( capsule.m_bone, 0, 255 ) );
					raw.m_weight[ 0 ] = 1.f;
					out.m_verts.push_back( raw );
				}
			}

			for ( int k = 0; k + 1 < rows; k++ )
				for ( int s = 0; s < around; s++ ) {
					const unsigned int a = base + k * around + s, b = base + k * around + ( s + 1 ) % around;
					const unsigned int c = a + around, d = b + around;
					for ( const unsigned int index : { a, c, d, a, d, b } )
						out.m_tris.push_back( index );
				}
		}
		out.m_raw_verts = static_cast< int >( out.m_verts.size( ) );
	}

	/* weld ( vvd splits the surface at uv seams, the capture gives uv per vertex ), adjacency, volume share per vertex */
	void melt_finish( melt_template_t& skin )
	{
		const std::size_t raw = skin.m_verts.size( );
		const auto quant      = [ ]( const float value ) {
            const long long q = static_cast< long long >( std::floor( value * k_melt_weld + 0.5f ) ) + ( 1ll << 20 );
            return static_cast< unsigned long long >( std::clamp( q, 0ll, ( 1ll << 21 ) - 1 ) );
		};

		std::vector< std::pair< unsigned long long, int > > keys( raw );
		for ( std::size_t i = 0; i < raw; i++ ) {
			const c_vector& p = skin.m_verts[ i ].m_pos;
			keys[ i ]         = { ( quant( p.m_x ) << 42 ) | ( quant( p.m_y ) << 21 ) | quant( p.m_z ), static_cast< int >( i ) };
		}
		std::sort( keys.begin( ), keys.end( ) );

		std::vector< int > remap( raw );
		std::vector< melt_raw_vert_t > welded{ };
		welded.reserve( raw );
		for ( std::size_t k = 0; k < raw; k++ ) {
			if ( !k || keys[ k ].first != keys[ k - 1 ].first )
				welded.push_back( skin.m_verts[ keys[ k ].second ] );
			remap[ keys[ k ].second ] = static_cast< int >( welded.size( ) ) - 1;
		}

		std::vector< unsigned int > tris{ };
		tris.reserve( skin.m_tris.size( ) );
		for ( std::size_t t = 0; t + 2 < skin.m_tris.size( ); t += 3 ) {
			const unsigned int a = remap[ skin.m_tris[ t ] ], b = remap[ skin.m_tris[ t + 1 ] ], c = remap[ skin.m_tris[ t + 2 ] ];
			if ( a == b || b == c || a == c )
				continue;
			tris.push_back( a );
			tris.push_back( b );
			tris.push_back( c );
		}
		skin.m_verts = std::move( welded );
		skin.m_tris  = std::move( tris );

		const std::size_t n = skin.m_verts.size( );
		skin.m_share.assign( n, 0.f );
		std::vector< unsigned long long > edges{ };
		edges.reserve( skin.m_tris.size( ) * 2 );
		float area = 0.f, volume = 0.f;
		for ( std::size_t t = 0; t < skin.m_tris.size( ); t += 3 ) {
			const unsigned int index[ 3 ] = { skin.m_tris[ t ], skin.m_tris[ t + 1 ], skin.m_tris[ t + 2 ] };
			const c_vector &a = skin.m_verts[ index[ 0 ] ].m_pos, &b = skin.m_verts[ index[ 1 ] ].m_pos, &c = skin.m_verts[ index[ 2 ] ].m_pos;
			const float third = ( b - a ).cross_product( c - a ).length( ) / 6.f;
			volume += a.dot_product( b.cross_product( c ) ) / 6.f;
			area += 3.f * third;
			for ( int k = 0; k < 3; k++ ) {
				skin.m_share[ index[ k ] ] += third;
				const unsigned long long from = index[ k ], to = index[ ( k + 1 ) % 3 ];
				edges.push_back( ( from << 32 ) | to );
				edges.push_back( ( to << 32 ) | from );
			}
		}
		for ( float& share : skin.m_share )
			share = area > 0.f ? share / area : 0.f;
		skin.m_volume = std::abs( volume );

		std::sort( edges.begin( ), edges.end( ) );
		edges.erase( std::unique( edges.begin( ), edges.end( ) ), edges.end( ) );
		skin.m_adj_start.assign( n + 1, 0 );
		skin.m_adj.resize( edges.size( ) );
		for ( std::size_t e = 0; e < edges.size( ); e++ ) {
			skin.m_adj_start[ static_cast< std::size_t >( edges[ e ] >> 32 ) + 1 ]++;
			skin.m_adj[ e ] = static_cast< int >( edges[ e ] & 0xffffffffull );
		}
		for ( std::size_t i = 0; i < n; i++ )
			skin.m_adj_start[ i + 1 ] += skin.m_adj_start[ i ];
	}

	int melt_world_cell( const float value )
	{
		return static_cast< int >( std::floor( value / k_melt_cell + 0.5f ) );
	}

	melt_cell_t* melt_cell_at( melt_sim_t& sim, const int x, const int y )
	{
		const int gx = x - sim.m_ox, gy = y - sim.m_oy;
		if ( gx < 0 || gy < 0 || gx >= k_melt_grid || gy >= k_melt_grid || sim.m_cells.empty( ) )
			return nullptr;
		return &sim.m_cells[ static_cast< std::size_t >( gx ) * k_melt_grid + gy ];
	}

	void melt_ask( melt_sim_t& sim, const int x, const int y, const int from_x, const int from_y, const float z )
	{
		melt_cell_t* cell = melt_cell_at( sim, x, y );
		if ( !cell || cell->m_state != melt_cell_unknown )
			return;
		cell->m_state = melt_cell_asked;
		sim.m_probes.push_back( { x, y, from_x, from_y, z } );
	}

	/* bilinear over open cell centers; a corner that is a wall, unknown or a ledge ( > climb ) takes the own cell's floor */
	float melt_ground( melt_sim_t& sim, const c_vector& p )
	{
		const melt_cell_t* own = melt_cell_at( sim, melt_world_cell( p.m_x ), melt_world_cell( p.m_y ) );
		const float base       = own && own->m_state == melt_cell_open ? own->m_floor : sim.m_base_floor;

		const float fx = p.m_x / k_melt_cell, fy = p.m_y / k_melt_cell;
		const int x0 = static_cast< int >( std::floor( fx ) ), y0 = static_cast< int >( std::floor( fy ) );
		const float tx = fx - static_cast< float >( x0 ), ty = fy - static_cast< float >( y0 );

		float h[ 2 ][ 2 ];
		for ( int i = 0; i < 2; i++ )
			for ( int j = 0; j < 2; j++ ) {
				const melt_cell_t* cell = melt_cell_at( sim, x0 + i, y0 + j );
				h[ i ][ j ] = cell && cell->m_state == melt_cell_open && std::abs( cell->m_floor - base ) <= k_melt_climb ? cell->m_floor : base;
			}
		return ( h[ 0 ][ 0 ] * ( 1.f - tx ) + h[ 1 ][ 0 ] * tx ) * ( 1.f - ty ) + ( h[ 0 ][ 1 ] * ( 1.f - tx ) + h[ 1 ][ 1 ] * tx ) * ty;
	}

	float melt_hash( const int x, const int y )
	{
		unsigned int h = static_cast< unsigned int >( x ) * 374761393u + static_cast< unsigned int >( y ) * 668265263u;
		h              = ( h ^ ( h >> 13 ) ) * 1274126177u;
		return static_cast< float >( ( h ^ ( h >> 16 ) ) & 0xffffu ) / 65535.f;
	}

	float melt_noise( const float x, const float y )
	{
		const float fx = std::floor( x ), fy = std::floor( y );
		const int ix = static_cast< int >( fx ), iy = static_cast< int >( fy );
		float tx = x - fx, ty = y - fy;
		tx             = tx * tx * ( 3.f - 2.f * tx );
		ty             = ty * ty * ( 3.f - 2.f * ty );
		const float lo = melt_hash( ix, iy ) + ( melt_hash( ix + 1, iy ) - melt_hash( ix, iy ) ) * tx;
		const float hi = melt_hash( ix, iy + 1 ) + ( melt_hash( ix + 1, iy + 1 ) - melt_hash( ix, iy + 1 ) ) * tx;
		return lo + ( hi - lo ) * ty;
	}

	unsigned int melt_pack( const float* rgb )
	{
		const auto channel = [ ]( const float value ) { return static_cast< unsigned int >( std::clamp( value, 0.f, 255.f ) ); };
		return 0xff000000u | ( channel( rgb[ 0 ] ) << 16 ) | ( channel( rgb[ 1 ] ) << 8 ) | channel( rgb[ 2 ] );
	}

	/* bones + skin set: per vertex state, positions as of now ( capture fallback ), drip streaks */
	void melt_start( melt_sim_t& sim )
	{
		const melt_template_t& skin = *sim.m_skin;
		const std::size_t n         = skin.m_verts.size( );
		std::vector< matrix3x4_t > mats{ };
		melt_mats( sim, mats );

		sim.m_gone.assign( n, 0 );
		sim.m_rgb.assign( n * 3, 128.f );
		sim.m_uv.assign( n * 2, 0.f );
		sim.m_tex.assign( n, 0.f );
		sim.m_drip.resize( n );
		sim.m_at_capture.resize( n );
		for ( std::size_t i = 0; i < n; i++ ) {
			const c_vector p     = melt_skin( skin.m_verts[ i ], mats, false );
			sim.m_at_capture[ i ] = p;
			const float d        = std::clamp( ( melt_noise( p.m_x / k_melt_drip_cell, p.m_y / k_melt_drip_cell ) - 0.55f ) / 0.3f, 0.f, 1.f );
			sim.m_drip[ i ]      = d * d * ( 3.f - 2.f * d );
		}

		const std::size_t cells = static_cast< std::size_t >( k_melt_pool_grid ) * k_melt_pool_grid;
		sim.m_h.assign( cells, 0.f );
		sim.m_paint.assign( cells * 3, 0.f );
		sim.m_floor.assign( cells, 0.f );
		sim.m_open.assign( cells, 0 );
		sim.m_top = 1e9f;
	}

	/* captured texel per vertex; a vertex hidden behind the body itself at capture ( software z ) or never sampled takes its nearest trusted neighbor's color */
	void melt_texture( melt_sim_t& sim )
	{
		const melt_template_t& skin = *sim.m_skin;
		const std::size_t n         = skin.m_verts.size( );
		if ( sim.m_samples.size( ) != n || sim.m_at_capture.size( ) != n )
			return;

		const auto& m = sim.m_view;
		std::vector< float > sx( n, 0.f ), sy( n, 0.f ), sw( n, 0.f );
		for ( std::size_t i = 0; i < n; i++ ) {
			const c_vector& p = sim.m_at_capture[ i ];
			sw[ i ]           = m[ 3 ][ 0 ] * p.m_x + m[ 3 ][ 1 ] * p.m_y + m[ 3 ][ 2 ] * p.m_z + m[ 3 ][ 3 ];
			if ( sw[ i ] <= 0.001f )
				continue;
			sx[ i ] = ( 0.5f + 0.5f * ( m[ 0 ][ 0 ] * p.m_x + m[ 0 ][ 1 ] * p.m_y + m[ 0 ][ 2 ] * p.m_z + m[ 0 ][ 3 ] ) / sw[ i ] ) * sim.m_screen[ 0 ];
			sy[ i ] = ( 0.5f - 0.5f * ( m[ 1 ][ 0 ] * p.m_x + m[ 1 ][ 1 ] * p.m_y + m[ 1 ][ 2 ] * p.m_z + m[ 1 ][ 3 ] ) / sw[ i ] ) * sim.m_screen[ 1 ];
		}

		const float left = sim.m_crop[ 0 ], top = sim.m_crop[ 1 ], width = sim.m_crop[ 2 ], height = sim.m_crop[ 3 ];
		const bool textured = sim.m_textured && width > 0.f && height > 0.f;
		const float scale   = textured ? ( std::min )( 1.f, static_cast< float >( k_melt_zbuf ) / ( std::max )( width, height ) ) : 0.f;
		const int zw = textured ? static_cast< int >( std::ceil( width * scale ) ) + 1 : 0, zh = textured ? static_cast< int >( std::ceil( height * scale ) ) + 1 : 0;
		std::vector< float > zbuf( static_cast< std::size_t >( zw ) * zh, 1e30f );

		for ( std::size_t t = 0; textured && t + 2 < skin.m_tris.size( ); t += 3 ) {
			const unsigned int a = skin.m_tris[ t ], b = skin.m_tris[ t + 1 ], c = skin.m_tris[ t + 2 ];
			if ( sw[ a ] <= 0.001f || sw[ b ] <= 0.001f || sw[ c ] <= 0.001f )
				continue;
			const float ax = ( sx[ a ] - left ) * scale, ay = ( sy[ a ] - top ) * scale, bx = ( sx[ b ] - left ) * scale, by = ( sy[ b ] - top ) * scale;
			const float cx = ( sx[ c ] - left ) * scale, cy = ( sy[ c ] - top ) * scale;
			const float area = ( bx - ax ) * ( cy - ay ) - ( by - ay ) * ( cx - ax );
			if ( std::abs( area ) < 1e-6f )
				continue;
			const int x0 = ( std::max )( 0, static_cast< int >( std::floor( ( std::min )( { ax, bx, cx } ) ) ) );
			const int x1 = ( std::min )( zw - 1, static_cast< int >( std::ceil( ( std::max )( { ax, bx, cx } ) ) ) );
			const int y0 = ( std::max )( 0, static_cast< int >( std::floor( ( std::min )( { ay, by, cy } ) ) ) );
			const int y1 = ( std::min )( zh - 1, static_cast< int >( std::ceil( ( std::max )( { ay, by, cy } ) ) ) );
			for ( int y = y0; y <= y1; y++ )
				for ( int x = x0; x <= x1; x++ ) {
					const float px = static_cast< float >( x ) + 0.5f, py = static_cast< float >( y ) + 0.5f;
					const float w0 = ( ( bx - px ) * ( cy - py ) - ( by - py ) * ( cx - px ) ) / area;
					const float w1 = ( ( cx - px ) * ( ay - py ) - ( cy - py ) * ( ax - px ) ) / area;
					const float w2 = 1.f - w0 - w1;
					if ( w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f )
						continue;
					float& depth = zbuf[ static_cast< std::size_t >( y ) * zw + x ];
					depth        = ( std::min )( depth, w0 * sw[ a ] + w1 * sw[ b ] + w2 * sw[ c ] );
				}
		}

		std::vector< int > source( n, -1 ), queue{ };
		queue.reserve( n );
		for ( std::size_t i = 0; i < n; i++ ) {
			if ( ( sim.m_samples[ i ] >> 24 ) == 0u || sw[ i ] <= 0.001f )
				continue;
			if ( textured ) {
				const int x = std::clamp( static_cast< int >( ( sx[ i ] - left ) * scale ), 0, zw - 1 );
				const int y = std::clamp( static_cast< int >( ( sy[ i ] - top ) * scale ), 0, zh - 1 );
				if ( sw[ i ] > zbuf[ static_cast< std::size_t >( y ) * zw + x ] + k_melt_seen_slack + 0.01f * sw[ i ] )
					continue;
			}
			source[ i ] = static_cast< int >( i );
			queue.push_back( static_cast< int >( i ) );
		}
		sim.m_stats.m_trusted = static_cast< int >( queue.size( ) );

		for ( std::size_t head = 0; head < queue.size( ); head++ ) {
			const int i = queue[ head ];
			for ( int k = skin.m_adj_start[ i ]; k < skin.m_adj_start[ i + 1 ]; k++ ) {
				const int j = skin.m_adj[ k ];
				if ( source[ j ] >= 0 )
					continue;
				source[ j ] = source[ i ];
				queue.push_back( j );
			}
		}

		/* uv also from the source: an own projection off the crop lerps the clamped edge texels across the triangle = streaks */
		for ( std::size_t i = 0; i < n; i++ ) {
			const int from = source[ i ];
			if ( from < 0 )
				continue;
			const unsigned int pixel = sim.m_samples[ from ];
			for ( int c = 0; c < 3; c++ )
				sim.m_rgb[ i * 3 + c ] = static_cast< float >( ( pixel >> ( 16 - 8 * c ) ) & 0xffu );
			sim.m_tex[ i ] = from == static_cast< int >( i ) && textured ? 1.f : 0.f;
			if ( textured ) {
				sim.m_uv[ i * 2 ]     = ( sx[ from ] - left ) / width;
				sim.m_uv[ i * 2 + 1 ] = ( sy[ from ] - top ) / height;
			}
		}
	}

	float melt_pool_x( const melt_sim_t& sim, const int i )
	{
		return static_cast< float >( sim.m_ox * 2 + i ) * k_melt_pool_cell;
	}

	float melt_pool_y( const melt_sim_t& sim, const int j )
	{
		return static_cast< float >( sim.m_oy * 2 + j ) * k_melt_pool_cell;
	}

	bool melt_pool_open( melt_sim_t& sim, const int i, const int j, float& floor )
	{
		if ( i < 0 || j < 0 || i >= k_melt_pool_grid || j >= k_melt_pool_grid )
			return false;
		const float x = melt_pool_x( sim, i ), y = melt_pool_y( sim, j );
		const melt_cell_t* cell = melt_cell_at( sim, melt_world_cell( x ), melt_world_cell( y ) );
		if ( !cell || cell->m_state != melt_cell_open )
			return false;
		floor = melt_ground( sim, c_vector( x, y, cell->m_floor ) );
		return true;
	}

	void melt_pool_add( melt_sim_t& sim, const std::size_t cell, const float volume, const float* rgb )
	{
		const float dh = volume / ( k_melt_pool_cell * k_melt_pool_cell );
		if ( dh <= 0.f )
			return;
		const float mix = dh / ( sim.m_h[ cell ] + dh );
		sim.m_h[ cell ] += dh;
		for ( int c = 0; c < 3; c++ )
			sim.m_paint[ cell * 3 + c ] += ( rgb[ c ] - sim.m_paint[ cell * 3 + c ] ) * mix;
	}

	/* bilinear into open cells; what lands on a wall / unknown cell goes to the nearest open one, else the start cell */
	void melt_deposit( melt_sim_t& sim, const float x, const float y, const float volume, const float* rgb )
	{
		const float fx = x / k_melt_pool_cell - static_cast< float >( sim.m_ox * 2 ), fy = y / k_melt_pool_cell - static_cast< float >( sim.m_oy * 2 );
		const int i0 = static_cast< int >( std::floor( fx ) ), j0 = static_cast< int >( std::floor( fy ) );
		const float tx = fx - static_cast< float >( i0 ), ty = fy - static_cast< float >( j0 );

		float lost  = 0.f;
		float floor = 0.f;
		for ( int di = 0; di < 2; di++ )
			for ( int dj = 0; dj < 2; dj++ ) {
				const float w = ( di ? tx : 1.f - tx ) * ( dj ? ty : 1.f - ty );
				if ( w <= 0.f )
					continue;
				if ( melt_pool_open( sim, i0 + di, j0 + dj, floor ) )
					melt_pool_add( sim, static_cast< std::size_t >( i0 + di ) * k_melt_pool_grid + ( j0 + dj ), volume * w, rgb );
				else
					lost += volume * w;
			}
		if ( lost <= 0.f )
			return;

		for ( int ring = 1; ring <= 3; ring++ )
			for ( int di = -ring; di <= ring; di++ )
				for ( int dj = -ring; dj <= ring; dj++ )
					if ( ( std::max )( std::abs( di ), std::abs( dj ) ) == ring && melt_pool_open( sim, i0 + di, j0 + dj, floor ) ) {
						melt_pool_add( sim, static_cast< std::size_t >( i0 + di ) * k_melt_pool_grid + ( j0 + dj ), lost, rgb );
						return;
					}

		if ( melt_pool_open( sim, k_melt_pool_grid / 2, k_melt_pool_grid / 2, floor ) )
			melt_pool_add( sim, static_cast< std::size_t >( k_melt_pool_grid / 2 ) * k_melt_pool_grid + k_melt_pool_grid / 2, lost, rgb );
		else
			sim.m_stats.m_lost += lost;
	}

	/* the real mesh on the ragdoll's bones, sinking bottom first: what goes under the floor is buried and its volume share lands in the pool */
	void melt_body( melt_sim_t& sim, melt_mesh_t& mesh )
	{
		if ( sim.m_melted ) {
			sim.m_stats.m_live = 0;
			return;
		}

		const melt_template_t& skin = *sim.m_skin;
		const std::size_t n         = skin.m_verts.size( );
		std::vector< matrix3x4_t > mats{ };
		melt_mats( sim, mats );

		std::vector< c_vector > pos( n ), dir( n );
		std::vector< float > ground( n ), wet( n, 0.f );
		float top = 0.f;
		for ( std::size_t i = 0; i < n; i++ ) {
			pos[ i ]              = melt_skin( skin.m_verts[ i ], mats, false );
			const c_vector normal = melt_skin( skin.m_verts[ i ], mats, true );
			const float flat      = std::sqrt( normal.m_x * normal.m_x + normal.m_y * normal.m_y );
			dir[ i ]              = flat > 0.2f ? c_vector( normal.m_x / flat, normal.m_y / flat, 0.f ) : c_vector( );
			melt_ask( sim, melt_world_cell( pos[ i ].m_x ), melt_world_cell( pos[ i ].m_y ), k_melt_from_above, 0, pos[ i ].m_z );
			ground[ i ] = melt_ground( sim, pos[ i ] );
			if ( !sim.m_gone[ i ] )
				top = ( std::max )( top, pos[ i ].m_z - ground[ i ] );
		}
		sim.m_top = ( std::min )( sim.m_top, top );

		const float x      = std::clamp( sim.m_age / k_melt_sink_time, 0.f, 1.f );
		const float e      = std::pow( x, k_melt_sink_ease );
		const float sink   = e * ( std::max )( sim.m_top, 1.f );
		const float drip   = k_melt_drip * std::sin( 3.1415927f * x );
		const float squash = 1.f - k_melt_squash * e;
		const float flare  = k_melt_flare * ( std::min )( x / 0.1f, 1.f );

		int live = 0;
		for ( std::size_t i = 0; i < n; i++ ) {
			const c_vector p = pos[ i ];
			const float h    = p.m_z - ground[ i ] - sink - drip * sim.m_drip[ i ];
			if ( !sim.m_gone[ i ] && x > 0.f && ( h <= 0.f || x >= 1.f ) ) {
				sim.m_gone[ i ] = 1;
				melt_deposit( sim, p.m_x + dir[ i ].m_x * flare, p.m_y + dir[ i ].m_y * flare, sim.m_volume * skin.m_share[ i ], &sim.m_rgb[ i * 3 ] );
			}
			if ( sim.m_gone[ i ] ) {
				pos[ i ] = c_vector( p.m_x + dir[ i ].m_x * flare, p.m_y + dir[ i ].m_y * flare, ground[ i ] - k_melt_bury );
				continue;
			}

			live++;
			const float lifted = h * squash;
			const float band   = std::clamp( 1.f - lifted / k_melt_flare_band, 0.f, 1.f );
			pos[ i ]           = c_vector( p.m_x + dir[ i ].m_x * flare * band * band, p.m_y + dir[ i ].m_y * flare * band * band, ground[ i ] + lifted );
			wet[ i ]           = std::clamp( 1.f - lifted / k_melt_wet_band, 0.f, 1.f ) * ( std::min )( x * 5.f, 1.f );
		}

		const float lambda = k_melt_soften * e;
		if ( lambda > 0.f && live ) {
			std::vector< c_vector > next( pos );
			for ( int pass = 0; pass < k_melt_soften_passes; pass++ ) {
				for ( std::size_t i = 0; i < n; i++ ) {
					const int from = skin.m_adj_start[ i ], to = skin.m_adj_start[ i + 1 ];
					if ( sim.m_gone[ i ] || from == to ) {
						next[ i ] = pos[ i ];
						continue;
					}
					c_vector mean{ };
					for ( int k = from; k < to; k++ )
						mean += pos[ skin.m_adj[ k ] ];
					mean *= 1.f / static_cast< float >( to - from );
					next[ i ] = pos[ i ] + ( mean - pos[ i ] ) * lambda;
				}
				pos.swap( next );
			}
		}

		std::vector< c_vector > normals( n );
		for ( std::size_t t = 0; t + 2 < skin.m_tris.size( ); t += 3 ) {
			const unsigned int a = skin.m_tris[ t ], b = skin.m_tris[ t + 1 ], c = skin.m_tris[ t + 2 ];
			const c_vector face  = ( pos[ b ] - pos[ a ] ).cross_product( pos[ c ] - pos[ a ] );
			normals[ a ] += face;
			normals[ b ] += face;
			normals[ c ] += face;
		}

		mesh.m_vertices.resize( n );
		for ( std::size_t i = 0; i < n; i++ ) {
			const c_vector normal = normals[ i ].normalized( );
			mesh.m_vertices[ i ]  = { pos[ i ].m_x, pos[ i ].m_y, pos[ i ].m_z, normal.m_x, normal.m_y, normal.m_z, melt_pack( &sim.m_rgb[ i * 3 ] ),
                                      sim.m_uv[ i * 2 ], sim.m_uv[ i * 2 + 1 ], sim.m_tex[ i ], wet[ i ] };
		}
		mesh.m_body_vertices = static_cast< int >( n );
		mesh.m_indices.reserve( skin.m_tris.size( ) );
		for ( std::size_t t = 0; t + 2 < skin.m_tris.size( ); t += 3 )
			if ( !sim.m_gone[ skin.m_tris[ t ] ] || !sim.m_gone[ skin.m_tris[ t + 1 ] ] || !sim.m_gone[ skin.m_tris[ t + 2 ] ] )
				for ( int k = 0; k < 3; k++ )
					mesh.m_indices.push_back( skin.m_tris[ t + k ] );
		sim.m_stats.m_live = live;
		sim.m_melted       = !live && x >= 1.f;
	}

	bool melt_pool_bounds( const melt_sim_t& sim, const int margin, int& lo_i, int& lo_j, int& hi_i, int& hi_j )
	{
		lo_i = lo_j = k_melt_pool_grid;
		hi_i = hi_j = -1;
		for ( int i = 0; i < k_melt_pool_grid; i++ )
			for ( int j = 0; j < k_melt_pool_grid; j++ )
				if ( sim.m_h[ static_cast< std::size_t >( i ) * k_melt_pool_grid + j ] > 0.f ) {
					lo_i = ( std::min )( lo_i, i );
					hi_i = ( std::max )( hi_i, i );
					lo_j = ( std::min )( lo_j, j );
					hi_j = ( std::max )( hi_j, j );
				}
		if ( hi_i < 0 )
			return false;
		lo_i = ( std::max )( lo_i - margin, 0 );
		lo_j = ( std::max )( lo_j - margin, 0 );
		hi_i = ( std::min )( hi_i + margin, k_melt_pool_grid - 1 );
		hi_j = ( std::min )( hi_j + margin, k_melt_pool_grid - 1 );
		return true;
	}

	/* thick fluid on a height grid: flux ~ ( h - yield )^3 * surface drop, volume exact, stops at ~k_melt_stop thick; runs downhill and off ledges */
	void melt_pool( melt_sim_t& sim, const float dt )
	{
		constexpr int g = k_melt_pool_grid;
		auto &h = sim.m_h, &paint = sim.m_paint;
		sim.m_stats.m_change = 0.f;
		int lo_i, lo_j, hi_i, hi_j;
		if ( !melt_pool_bounds( sim, 2, lo_i, lo_j, hi_i, hi_j ) )
			return;

		const auto at = [ ]( const int i, const int j ) { return static_cast< std::size_t >( i ) * g + j; };
		float deepest = 0.f;
		for ( int i = lo_i; i <= hi_i; i++ )
			for ( int j = lo_j; j <= hi_j; j++ ) {
				sim.m_open[ at( i, j ) ] = melt_pool_open( sim, i, j, sim.m_floor[ at( i, j ) ] ) ? 1 : 0;
				deepest                  = ( std::max )( deepest, h[ at( i, j ) ] );
			}

		/* wet cell next to an unknown one: walk a probe in from this cell */
		for ( int i = lo_i; i <= hi_i; i++ )
			for ( int j = lo_j; j <= hi_j; j++ ) {
				if ( h[ at( i, j ) ] <= 0.f || !sim.m_open[ at( i, j ) ] )
					continue;
				const int own_x = melt_world_cell( melt_pool_x( sim, i ) ), own_y = melt_world_cell( melt_pool_y( sim, j ) );
				for ( const auto& step : { std::array< int, 2 >{ 1, 0 }, std::array< int, 2 >{ -1, 0 }, std::array< int, 2 >{ 0, 1 }, std::array< int, 2 >{ 0, -1 } } ) {
					const int x = melt_world_cell( melt_pool_x( sim, i + step[ 0 ] ) ), y = melt_world_cell( melt_pool_y( sim, j + step[ 1 ] ) );
					if ( x != own_x || y != own_y )
						melt_ask( sim, x, y, own_x, own_y, sim.m_floor[ at( i, j ) ] );
				}
			}

		const float film  = std::clamp( deepest - k_melt_stop, 0.f, k_melt_film_cap );
		const float rate  = k_melt_flow * film * film * film;
		const float limit = rate > 0.f ? ( std::min )( k_melt_max_dt, 0.2f * k_melt_pool_cell * k_melt_pool_cell / rate ) : k_melt_max_dt;
		const int sub     = std::clamp( static_cast< int >( std::ceil( dt / limit ) ), 1, k_melt_max_sub );
		const float step  = dt / static_cast< float >( sub );
		sim.m_stats.m_sub = sub;

		/* worker thread only */
		const std::size_t cells = static_cast< std::size_t >( g ) * g;
		static std::vector< float > flux_x{ }, flux_y{ }, out{ }, next_h{ }, next_paint{ };
		flux_x.assign( cells, 0.f );
		flux_y.assign( cells, 0.f );
		out.assign( cells, 0.f );
		next_h.assign( h.begin( ), h.end( ) );
		next_paint.assign( paint.begin( ), paint.end( ) );

		const auto flux = [ & ]( const std::size_t a, const std::size_t b ) {
			if ( !sim.m_open[ a ] || !sim.m_open[ b ] )
				return 0.f;
			const float drop       = ( sim.m_floor[ a ] + h[ a ] ) - ( sim.m_floor[ b ] + h[ b ] );
			const std::size_t from = drop > 0.f ? a : b;
			const float moving     = std::clamp( h[ from ] - k_melt_stop, 0.f, k_melt_film_cap );
			const float amount     = k_melt_flow * moving * moving * moving * std::abs( drop ) / ( k_melt_pool_cell * k_melt_pool_cell ) * step;
			return drop > 0.f ? amount : -amount;
		};

		for ( int s = 0; s < sub; s++ ) {
			for ( int i = lo_i; i <= hi_i; i++ )
				for ( int j = lo_j; j <= hi_j; j++ ) {
					const std::size_t a = at( i, j );
					out[ a ]            = 0.f;
					flux_x[ a ]         = i < hi_i ? flux( a, at( i + 1, j ) ) : 0.f;
					flux_y[ a ]         = j < hi_j ? flux( a, at( i, j + 1 ) ) : 0.f;
				}
			for ( int i = lo_i; i <= hi_i; i++ )
				for ( int j = lo_j; j <= hi_j; j++ ) {
					const std::size_t a = at( i, j );
					out[ flux_x[ a ] > 0.f ? a : at( ( std::min )( i + 1, hi_i ), j ) ] += std::abs( flux_x[ a ] );
					out[ flux_y[ a ] > 0.f ? a : at( i, ( std::min )( j + 1, hi_j ) ) ] += std::abs( flux_y[ a ] );
				}

			for ( int i = lo_i; i <= hi_i; i++ ) {
				std::copy( h.begin( ) + at( i, lo_j ), h.begin( ) + at( i, hi_j ) + 1, next_h.begin( ) + at( i, lo_j ) );
				std::copy( paint.begin( ) + at( i, lo_j ) * 3, paint.begin( ) + at( i, hi_j ) * 3 + 3, next_paint.begin( ) + at( i, lo_j ) * 3 );
			}
			const auto move = [ & ]( const std::size_t a, const std::size_t b, const float signed_amount ) {
				const std::size_t from = signed_amount > 0.f ? a : b, to = signed_amount > 0.f ? b : a;
				const float avail      = ( std::max )( h[ from ] - k_melt_stop, 0.f );
				const float amount     = std::abs( signed_amount ) * ( out[ from ] > avail ? avail / out[ from ] : 1.f );
				if ( amount <= 0.f || h[ from ] <= 0.f )
					return;
				/* color rides into dry cells only: upwind mixing every substep averaged the whole puddle into one mud color */
				if ( next_h[ to ] <= 0.f )
					std::copy( paint.begin( ) + from * 3, paint.begin( ) + from * 3 + 3, next_paint.begin( ) + to * 3 );
				next_h[ from ] -= amount;
				next_h[ to ] += amount;
			};
			for ( int i = lo_i; i <= hi_i; i++ )
				for ( int j = lo_j; j <= hi_j; j++ ) {
					const std::size_t a = at( i, j );
					if ( flux_x[ a ] != 0.f )
						move( a, at( i + 1, j ), flux_x[ a ] );
					if ( flux_y[ a ] != 0.f )
						move( a, at( i, j + 1 ), flux_y[ a ] );
				}

			float change = 0.f;
			for ( int i = lo_i; i <= hi_i; i++ )
				for ( int j = lo_j; j <= hi_j; j++ )
					change = ( std::max )( change, std::abs( next_h[ at( i, j ) ] - h[ at( i, j ) ] ) );
			sim.m_stats.m_change = change / step;
			h.swap( next_h );
			paint.swap( next_paint );
		}

		float volume = 0.f;
		int wet      = 0;
		for ( int i = lo_i; i <= hi_i; i++ )
			for ( int j = lo_j; j <= hi_j; j++ ) {
				volume += h[ at( i, j ) ];
				wet += h[ at( i, j ) ] > k_melt_edge ? 1 : 0;
			}
		sim.m_stats.m_pool_volume = volume * k_melt_pool_cell * k_melt_pool_cell;
		sim.m_stats.m_wet_cells   = wet;
	}

	/* marching squares over cell centers at k_melt_edge: round outline, rim slopes to the floor */
	void melt_pool_mesh( melt_sim_t& sim, melt_mesh_t& mesh )
	{
		constexpr int g = k_melt_pool_grid;
		int lo_i, lo_j, hi_i, hi_j;
		if ( !melt_pool_bounds( sim, 1, lo_i, lo_j, hi_i, hi_j ) )
			return;

		const int w = hi_i - lo_i + 1, d = hi_j - lo_j + 1;
		const auto local = [ & ]( const int i, const int j ) { return static_cast< std::size_t >( i - lo_i ) * d + ( j - lo_j ); };
		const auto cell  = [ ]( const int i, const int j ) { return static_cast< std::size_t >( i ) * g + j; };
		const std::size_t count = static_cast< std::size_t >( w ) * d;
		std::vector< float > floor( count, 0.f ), smooth( count, 0.f ), rgb( count * 3, 0.f );
		std::vector< unsigned char > open( count, 0 );
		std::vector< c_vector > normal( count, c_vector( 0.f, 0.f, 1.f ) );
		std::vector< int > center( count, -1 ), edge_x( count, -1 ), edge_y( count, -1 );

		for ( int i = lo_i; i <= hi_i; i++ )
			for ( int j = lo_j; j <= hi_j; j++ )
				open[ local( i, j ) ] = melt_pool_open( sim, i, j, floor[ local( i, j ) ] ) ? 1 : 0;

		const auto usable = [ & ]( const int i, const int j, const float own ) {
			return i >= lo_i && j >= lo_j && i <= hi_i && j <= hi_j && open[ local( i, j ) ] && std::abs( floor[ local( i, j ) ] - own ) <= k_melt_climb;
		};

		for ( int i = lo_i; i <= hi_i; i++ )
			for ( int j = lo_j; j <= hi_j; j++ ) {
				const std::size_t l = local( i, j );
				if ( !open[ l ] )
					continue;
				float sum = 0.f, weight = 0.f;
				for ( int di = -1; di <= 1; di++ )
					for ( int dj = -1; dj <= 1; dj++ ) {
						if ( !usable( i + di, j + dj, floor[ l ] ) )
							continue;
						const float k = ( di ? 1.f : 2.f ) * ( dj ? 1.f : 2.f );
						sum += sim.m_h[ cell( i + di, j + dj ) ] * k;
						weight += k;
					}
				smooth[ l ] = weight > 0.f ? sum / weight : 0.f;
				for ( int c = 0; c < 3; c++ )
					rgb[ l * 3 + c ] = sim.m_paint[ cell( i, j ) * 3 + c ];
			}

		for ( int i = lo_i; i <= hi_i; i++ )
			for ( int j = lo_j; j <= hi_j; j++ ) {
				const std::size_t l = local( i, j );
				if ( !open[ l ] )
					continue;
				const float own = floor[ l ] + smooth[ l ];
				const auto eta  = [ & ]( const int ni, const int nj ) { return usable( ni, nj, floor[ l ] ) ? floor[ local( ni, nj ) ] + smooth[ local( ni, nj ) ] : own; };
				const float gx  = ( eta( i + 1, j ) - eta( i - 1, j ) ) / ( 2.f * k_melt_pool_cell );
				const float gy  = ( eta( i, j + 1 ) - eta( i, j - 1 ) ) / ( 2.f * k_melt_pool_cell );
				normal[ l ]     = c_vector( -gx, -gy, 1.f ).normalized( );
			}

		const auto inside = [ & ]( const int i, const int j ) { return open[ local( i, j ) ] && smooth[ local( i, j ) ] > k_melt_edge; };
		const auto emit   = [ & ]( const c_vector& p, const std::size_t l ) {
            mesh.m_vertices.push_back( { p.m_x, p.m_y, p.m_z, normal[ l ].m_x, normal[ l ].m_y, normal[ l ].m_z, melt_pack( &rgb[ l * 3 ] ), 0.f, 0.f, 0.f, 1.f } );
            return static_cast< int >( mesh.m_vertices.size( ) ) - 1;
		};
		const auto corner = [ & ]( const int i, const int j ) {
			const std::size_t l = local( i, j );
			if ( center[ l ] < 0 )
				center[ l ] = emit( c_vector( melt_pool_x( sim, i ), melt_pool_y( sim, j ), floor[ l ] + smooth[ l ] ), l );
			return center[ l ];
		};
		/* w = wet corner, o = the other one ( dry, or wet on another floor level: edge stops halfway, never bridges the step ) */
		const auto crossing = [ & ]( const int wi, const int wj, const int oi, const int oj ) {
			const bool along_x   = wi != oi;
			const bool shared    = !inside( oi, oj );
			int& slot            = along_x ? edge_x[ local( ( std::min )( wi, oi ), wj ) ] : edge_y[ local( wi, ( std::min )( wj, oj ) ) ];
			if ( shared && slot >= 0 )
				return slot;
			const std::size_t lw = local( wi, wj ), lo = local( oi, oj );
			const bool level     = usable( oi, oj, floor[ lw ] );
			const float dry      = level ? smooth[ lo ] : 0.f;
			const float t = std::clamp( ( k_melt_edge - smooth[ lw ] ) / ( std::min )( dry - smooth[ lw ], -1e-6f ), 0.f, level || !open[ lo ] ? 1.f : 0.5f );
			const float base     = level ? floor[ lw ] + ( floor[ lo ] - floor[ lw ] ) * t : floor[ lw ];
			const float x        = melt_pool_x( sim, wi ) + ( melt_pool_x( sim, oi ) - melt_pool_x( sim, wi ) ) * t;
			const float y        = melt_pool_y( sim, wj ) + ( melt_pool_y( sim, oj ) - melt_pool_y( sim, wj ) ) * t;
			const int made       = emit( c_vector( x, y, base + k_melt_lift ), lw );
			if ( shared )
				slot = made;
			return made;
		};

		int split = 0;
		for ( int i = lo_i; i < hi_i; i++ )
			for ( int j = lo_j; j < hi_j; j++ ) {
				const int ci[ 4 ] = { i, i + 1, i + 1, i }, cj[ 4 ] = { j, j, j + 1, j + 1 };
				int group[ 4 ], levels = 0;
				float low[ 4 ], high[ 4 ];
				for ( int k = 0; k < 4; k++ ) {
					group[ k ] = -1;
					if ( !inside( ci[ k ], cj[ k ] ) )
						continue;
					const float own = floor[ local( ci[ k ], cj[ k ] ) ];
					for ( int lv = 0; lv < levels && group[ k ] < 0; lv++ )
						if ( own - low[ lv ] <= k_melt_climb && high[ lv ] - own <= k_melt_climb ) {
							group[ k ] = lv;
							low[ lv ]  = ( std::min )( low[ lv ], own );
							high[ lv ] = ( std::max )( high[ lv ], own );
						}
					if ( group[ k ] < 0 ) {
						low[ levels ] = high[ levels ] = own;
						group[ k ]                     = levels++;
					}
				}
				split += levels > 1 ? 1 : 0;

				for ( int lv = 0; lv < levels; lv++ ) {
					int poly[ 8 ], size = 0;
					for ( int k = 0; k < 4; k++ ) {
						const int n     = ( k + 1 ) & 3;
						const bool a_in = group[ k ] == lv, b_in = group[ n ] == lv;
						if ( a_in )
							poly[ size++ ] = corner( ci[ k ], cj[ k ] );
						if ( a_in != b_in )
							poly[ size++ ] = a_in ? crossing( ci[ k ], cj[ k ], ci[ n ], cj[ n ] ) : crossing( ci[ n ], cj[ n ], ci[ k ], cj[ k ] );
					}
					for ( int t = 1; t + 1 < size; t++ ) {
						mesh.m_indices.push_back( static_cast< unsigned int >( poly[ 0 ] ) );
						mesh.m_indices.push_back( static_cast< unsigned int >( poly[ t ] ) );
						mesh.m_indices.push_back( static_cast< unsigned int >( poly[ t + 1 ] ) );
					}
				}
			}
		sim.m_stats.m_split = split;
	}

	bool melt_still( const melt_sim_t& sim )
	{
		return sim.m_age >= k_melt_sink_time + 0.5f && sim.m_stats.m_live == 0 && sim.m_stats.m_change < k_melt_still;
	}

	std::shared_ptr< const melt_mesh_t > melt_advance( melt_sim_t& sim, const float dt )
	{
		auto mesh = std::make_shared< melt_mesh_t >( );
		if ( !sim.m_skin || sim.m_skin->m_verts.empty( ) )
			return mesh;

		const auto start = std::chrono::steady_clock::now( );
		if ( !sim.m_colors_ready ) {
			melt_texture( sim );
			sim.m_colors_ready = true;
		}
		sim.m_age += dt;
		melt_body( sim, *mesh );
		const auto body = std::chrono::steady_clock::now( );
		melt_pool( sim, dt );
		const auto pool = std::chrono::steady_clock::now( );
		melt_pool_mesh( sim, *mesh );

		sim.m_stats.m_body_ms = std::chrono::duration< float, std::milli >( body - start ).count( );
		sim.m_stats.m_pool_ms = std::chrono::duration< float, std::milli >( pool - body ).count( );
		sim.m_stats.m_mesh_ms = std::chrono::duration< float, std::milli >( std::chrono::steady_clock::now( ) - pool ).count( );
		return mesh;
	}
	/* ---- melt sim end ---- */

	constexpr unsigned int k_melt_see = e_contents::contents_solid | e_contents::contents_moveable;
	constexpr unsigned int k_melt_hit = e_mask::mask_solid;
	constexpr std::size_t k_melt_cap  = 6;
	constexpr DWORD k_melt_fvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX2 | D3DFVF_TEXCOORDSIZE2( 0 ) | D3DFVF_TEXCOORDSIZE2( 1 );

	enum e_melt_stage : int { melt_dead = -1, melt_wait_ragdoll, melt_wait_capture, melt_run, melt_rest };

	struct melt_t {
		int m_victim = 0, m_team = 0, m_stage = melt_wait_ragdoll, m_capture_base = -1, m_slot = -1, m_id = 0, m_jobs = 0;
		bool m_busy            = false;
		unsigned int m_ragdoll = 0;
		float m_wait = 0.f, m_skin_wait = 0.f, m_age = 0.f, m_accum = 0.f, m_settle = 0.f, m_end = 0.f, m_speed = 1.f, m_alpha = 1.f;
		std::vector< matrix3x4_t > m_bones{ };
		std::vector< unsigned char > m_seen{ };
		std::unique_ptr< melt_sim_t > m_sim{ };
		std::shared_ptr< const melt_mesh_t > m_mesh{ };
	};

	/* m_skin null + !m_failed = being welded on the worker */
	struct melt_skin_slot_t {
		char m_name[ 260 ]{ };
		int m_checksum = 0, m_body = 0;
		bool m_failed  = false;
		std::shared_ptr< const melt_template_t > m_skin{ };
	};

	/* render thread only */
	IDirect3DTexture9* g_melt_textures[ k_melt_cap ]{ };
	IDirect3DVertexShader9* g_melt_vs     = nullptr;
	IDirect3DPixelShader9* g_melt_ps      = nullptr;
	bool g_melt_shader_failed             = false;
	IDirect3DVertexShader9* g_kfx_vs      = nullptr;
	IDirect3DPixelShader9* g_kfx_ps       = nullptr;
	bool g_kfx_shader_failed              = false;

	std::vector< melt_t > g_melts{ };
	std::vector< melt_skin_slot_t > g_melt_skins{ };
	std::vector< unsigned int > g_melt_hidden{ };
	std::vector< c_base_entity* > g_melt_skip{ };
	c_vector g_melt_eye{ };
	bool g_melt_eye_valid = false;
	int g_melt_slot_next  = 0;

	/* m_state 1 = render thread owns everything below it, 0 and 2 = main thread */
	struct melt_capture_t {
		std::atomic< int > m_state{ 0 };
		int m_slot = 0;
		std::vector< float > m_uv{ };
		std::vector< unsigned int > m_rgb{ };
		float m_view[ 4 ][ 4 ]{ }, m_crop[ 4 ]{ }, m_screen[ 2 ]{ };
	} g_melt_capture{ };

	c_base_entity* from_handle( const unsigned int handle )
	{
		if ( !handle || handle == 0xffffffffu )
			return nullptr;

		return reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity_from_handle( handle ) );
	}

	c_base_entity* melt_ragdoll( c_base_entity* victim )
	{
		c_base_entity* ragdoll = from_handle( victim->get_ragdoll_handle( ) );
		if ( !ragdoll )
			return nullptr;

		const auto client_class = static_cast< c_client_networkable* >( ragdoll )->get_client_class( );
		if ( !client_class || client_class->m_class_id != e_class_ids::ccs_ragdoll || from_handle( ragdoll->get_ragdoll_player_handle( ) ) != victim )
			return nullptr;

		return ragdoll;
	}

	c_vector melt_eye( )
	{
		if ( g_melt_eye_valid )
			return g_melt_eye;

		return g_ctx.m_local ? g_ctx.m_local->get_eye_position( false ) : c_vector( );
	}

	/* every networked entity but brush ents ( "*n" models ): a falling dropped gun probed as floor = goo pillar in mid air */
	void melt_refresh_skip( )
	{
		g_melt_skip.clear( );

		const int highest = g_interfaces.m_client_entity_list->get_highest_entity_index( );
		for ( int i = 1; i <= highest; i++ ) {
			const auto entity = g_interfaces.m_client_entity_list->get< c_base_entity >( i );
			if ( !entity )
				continue;
			const model_t* model = entity->get_model( );
			if ( !model || model->m_name[ 0 ] != '*' )
				g_melt_skip.push_back( entity );
		}

		std::sort( g_melt_skip.begin( ), g_melt_skip.end( ) );
	}

	/* pointer compares only: static props reach the filter as non-entity handles */
	void melt_trace( const c_vector& from, const c_vector& to, const unsigned int mask, trace_t& trace )
	{
		static c_trace_filter filter( [ ]( c_base_entity* entity, int ) { return !std::binary_search( g_melt_skip.begin( ), g_melt_skip.end( ), entity ); } );

		g_interfaces.m_engine_trace->trace_ray( ray_t( from, to ), mask, &filter, &trace );
	}

	/* hidden at dme, the engine sets the ragdoll up only while it is on screen and only for the drawn lod: stale bones = view dependent body + streaks */
	bool melt_fresh_bones( c_base_entity* entity, std::vector< matrix3x4_t >& out )
	{
		alignas( 16 ) static matrix3x4_t bones[ 256 ];
		if ( !entity->setup_bones( bones, 256, 0x7FF00, g_interfaces.m_global_vars_base->m_current_time ) )
			return false;
		const int count = entity->get_cached_bone_data( ).count( );
		if ( count <= 0 || count > 256 )
			return false;
		out.assign( bones, bones + count );
		return true;
	}

	void melt_uv( const view_matrix_t& m, const c_vector& p, float& u, float& v )
	{
		const float w = m[ 3 ][ 0 ] * p.m_x + m[ 3 ][ 1 ] * p.m_y + m[ 3 ][ 2 ] * p.m_z + m[ 3 ][ 3 ];
		if ( w < 0.001f )
			return;

		const float x = ( m[ 0 ][ 0 ] * p.m_x + m[ 0 ][ 1 ] * p.m_y + m[ 0 ][ 2 ] * p.m_z + m[ 0 ][ 3 ] ) / w;
		const float y = ( m[ 1 ][ 0 ] * p.m_x + m[ 1 ][ 1 ] * p.m_y + m[ 1 ][ 2 ] * p.m_z + m[ 1 ][ 3 ] ) / w;
		if ( std::fabs( x ) >= 1.f || std::fabs( y ) >= 1.f )
			return;

		u = 0.5f + 0.5f * x;
		v = 0.5f - 0.5f * y;
	}

	struct melt_skin_job_t {
		std::shared_ptr< melt_template_t > m_made{ };
		char m_name[ 260 ]{ };
		int m_checksum = 0, m_body = 0;
		float m_ms     = 0.f;
	};

	/* m_sim: body + pool + mesh for melt m_id; m_skin: a model's template to weld */
	struct melt_job_t {
		int m_id   = 0;
		float m_dt = 0.f;
		std::unique_ptr< melt_sim_t > m_sim{ };
		std::unique_ptr< melt_skin_job_t > m_skin{ };
		std::shared_ptr< const melt_mesh_t > m_mesh{ };
	};

	/* one detached worker; it must be stopped ( kill_effects_shutdown ) before the dll goes */
	struct melt_worker_t {
		std::mutex m_lock{ };
		std::condition_variable m_wake{ };
		std::vector< melt_job_t > m_todo{ }, m_done{ };
		bool m_started = false, m_stop = false;
		std::atomic< bool > m_finished{ false };
	} g_melt_worker{ };

	int g_melt_id_next = 1;

	void melt_worker_loop( )
	{
		n_perf::background_thread( );

		for ( ;; ) {
			melt_job_t job{ };
			{
				std::unique_lock< std::mutex > lock( g_melt_worker.m_lock );
				g_melt_worker.m_wake.wait( lock, [ ] { return g_melt_worker.m_stop || !g_melt_worker.m_todo.empty( ); } );
				if ( g_melt_worker.m_stop )
					break;
				job = std::move( g_melt_worker.m_todo.front( ) );
				g_melt_worker.m_todo.erase( g_melt_worker.m_todo.begin( ) );
			}

			if ( job.m_skin ) {
				const auto start = std::chrono::steady_clock::now( );
				melt_finish( *job.m_skin->m_made );
				job.m_skin->m_ms = std::chrono::duration< float, std::milli >( std::chrono::steady_clock::now( ) - start ).count( );
			} else if ( job.m_sim )
				job.m_mesh = melt_advance( *job.m_sim, job.m_dt );

			std::lock_guard< std::mutex > lock( g_melt_worker.m_lock );
			g_melt_worker.m_done.push_back( std::move( job ) );
		}

		g_melt_worker.m_finished.store( true );
	}

	bool melt_queue( melt_job_t&& job )
	{
		std::lock_guard< std::mutex > lock( g_melt_worker.m_lock );
		if ( g_melt_worker.m_stop )
			return false;
		if ( !g_melt_worker.m_started ) {
			g_melt_worker.m_started = true;
			std::thread( melt_worker_loop ).detach( );
		}
		g_melt_worker.m_todo.push_back( std::move( job ) );
		g_melt_worker.m_wake.notify_one( );
		return true;
	}

	void melt_submit( melt_t& melt, const float dt )
	{
		melt_job_t job{ };
		job.m_id  = melt.m_id;
		job.m_dt  = dt;
		job.m_sim = std::move( melt.m_sim );
		if ( melt_queue( std::move( job ) ) )
			melt.m_busy = true;
		else
			melt.m_sim = std::move( job.m_sim );
	}

	void melt_collect( )
	{
		std::vector< melt_job_t > done{ };
		{
			std::lock_guard< std::mutex > lock( g_melt_worker.m_lock );
			done.swap( g_melt_worker.m_done );
		}

		for ( melt_job_t& job : done ) {
			if ( job.m_skin ) {
				const melt_skin_job_t& skin = *job.m_skin;
				for ( melt_skin_slot_t& slot : g_melt_skins )
					if ( !slot.m_skin && !slot.m_failed && std::strcmp( slot.m_name, skin.m_name ) == 0 && slot.m_checksum == skin.m_checksum && slot.m_body == skin.m_body ) {
						slot.m_failed = skin.m_made->m_verts.empty( ) || skin.m_made->m_tris.empty( );
						if ( !slot.m_failed )
							slot.m_skin = skin.m_made;
						botox_dbg_log( "MELT: skin %s body %d verts %d -> welded %d tris %d mesh volume %.0f %.2f ms ( worker )", slot.m_name, slot.m_body,
						               skin.m_made->m_raw_verts, static_cast< int >( skin.m_made->m_verts.size( ) ), static_cast< int >( skin.m_made->m_tris.size( ) / 3 ),
						               skin.m_made->m_volume, skin.m_ms );
						break;
					}
				continue;
			}

			for ( melt_t& melt : g_melts ) {
				if ( melt.m_id != job.m_id )
					continue;

				melt.m_busy = false;
				melt.m_sim  = std::move( job.m_sim );
				melt.m_mesh = std::move( job.m_mesh );
				if ( melt.m_sim && melt.m_mesh && ( melt.m_jobs++ % 60 ) == 0 ) {
					const melt_stats_t& stats = melt.m_sim->m_stats;
					botox_dbg_log( "MELT: job %d age %.2f live %d trusted %d | pool %.0f / %.0f u3 lost %.0f wet %d split %d sub %d change %.2f | body %.2f pool %.2f mesh %.2f ms "
					               "verts %d tris %d probes %d ( worker )",
					               melt.m_jobs, melt.m_sim->m_age, stats.m_live, stats.m_trusted, stats.m_pool_volume, melt.m_sim->m_volume, stats.m_lost, stats.m_wet_cells,
					               stats.m_split, stats.m_sub, stats.m_change, stats.m_body_ms, stats.m_pool_ms, stats.m_mesh_ms, static_cast< int >( melt.m_mesh->m_vertices.size( ) ),
					               static_cast< int >( melt.m_mesh->m_indices.size( ) / 3 ), static_cast< int >( melt.m_sim->m_probes.size( ) ) );
				}
			}
		}
	}

	/* main thread, between jobs: a sim that is on the worker is never touched */
	void melt_answer( melt_sim_t& sim, int& budget )
	{
		auto& probes     = sim.m_probes;
		std::size_t done = 0;

		for ( ; done < probes.size( ) && budget > 0; done++ ) {
			const melt_probe_t& probe = probes[ done ];
			melt_cell_t* cell         = melt_cell_at( sim, probe.m_x, probe.m_y );
			if ( !cell || cell->m_state != melt_cell_asked )
				continue;

			budget--;
			cell->m_state = melt_cell_blocked;

			const float x            = static_cast< float >( probe.m_x ) * k_melt_cell, y = static_cast< float >( probe.m_y ) * k_melt_cell;
			const melt_cell_t* from  = probe.m_from_x == k_melt_from_above ? nullptr : melt_cell_at( sim, probe.m_from_x, probe.m_from_y );
			float top                = probe.m_z + 24.f;
			trace_t trace{ };

			if ( from && from->m_state == melt_cell_open ) {
				top = from->m_floor + 12.f;
				melt_trace( c_vector( static_cast< float >( probe.m_from_x ) * k_melt_cell, static_cast< float >( probe.m_from_y ) * k_melt_cell, top ),
				            c_vector( x, y, top ), k_melt_hit, trace );
				if ( trace.m_start_solid || trace.m_fraction < 1.f )
					continue;
			}

			melt_trace( c_vector( x, y, top ), c_vector( x, y, top - 280.f ), k_melt_hit, trace );
			if ( trace.m_start_solid )
				continue;

			cell->m_floor = trace.m_end.m_z;
			cell->m_state = melt_cell_open;
		}

		probes.erase( probes.begin( ), probes.begin( ) + static_cast< std::ptrdiff_t >( done ) );
	}

	/* game thread only ( find_mdl / get_studio_hdr / filesystem ); find_mdl adds a ref, dropped before return.
	   null + pending = being welded on the worker, ask again next frame; null alone = use the hitbox capsules */
	std::shared_ptr< const melt_template_t > melt_vvd( c_base_entity* entity, const studiohdr_t* hdr, const model_t* model, bool& pending )
	{
		pending              = false;
		c_model_cache* cache = g_interfaces.m_model_cache;
		if ( !cache )
			return nullptr;

		c_client_renderable* renderable = entity->get_client_renderable( );
		const int body                  = renderable ? ( std::max )( renderable->get_body( ), 0 ) : 0;

		for ( const melt_skin_slot_t& slot : g_melt_skins )
			if ( std::strcmp( slot.m_name, model->m_name ) == 0 && slot.m_checksum == hdr->i_checksum && slot.m_body == body ) {
				pending = !slot.m_skin && !slot.m_failed;
				return slot.m_skin;
			}

		melt_skin_slot_t slot{ };
		std::snprintf( slot.m_name, sizeof( slot.m_name ), "%s", model->m_name );
		slot.m_checksum = hdr->i_checksum;
		slot.m_body     = body;
		if ( g_melt_skins.size( ) >= 8 )
			g_melt_skins.erase( g_melt_skins.begin( ) );

		std::string path( model->m_name );
		const std::size_t dot = path.rfind( ".mdl" );
		std::vector< unsigned char > vtx{ };
		if ( dot == std::string::npos || !read_game_file( path.replace( dot, 4, ".dx90.vtx" ).c_str( ), vtx ) ) {
			botox_dbg_log( "MELT: skin skipped ( no vtx %s )", path.c_str( ) );
			slot.m_failed = true;
			g_melt_skins.push_back( slot );
			return nullptr;
		}

		const unsigned short handle = cache->find_mdl( model->m_name );
		if ( handle == 0xffffu ) {
			botox_dbg_log( "MELT: skin skipped ( find_mdl %s )", model->m_name );
			return nullptr;
		}

		auto made = std::make_shared< melt_template_t >( );
		cache->begin_lock( );
		const bool same = cache->get_studio_hdr( handle ) == hdr;
		const auto vvd  = same ? static_cast< const melt_vvd_header_t* >( cache->get_vertex_data( handle ) ) : nullptr;
		const char* fail = !vvd ? ( same ? "no vvd" : "other hdr" )
		                        : melt_parse( reinterpret_cast< const unsigned char* >( hdr ), vvd, vtx.data( ), vtx.size( ), body, *made );
		cache->end_lock( );
		cache->release( handle );

		if ( !fail && static_cast< int >( made->m_pose.size( ) ) != hdr->n_bones )
			fail = "bone layout";
		if ( fail ) {
			botox_dbg_log( "MELT: skin skipped ( %s ) %s", fail, model->m_name );
			slot.m_failed = std::strcmp( fail, "no vvd" ) != 0;
			if ( slot.m_failed )
				g_melt_skins.push_back( slot );
			return nullptr;
		}

		auto job = std::make_unique< melt_skin_job_t >( );
		job->m_made = std::move( made );
		std::snprintf( job->m_name, sizeof( job->m_name ), "%s", slot.m_name );
		job->m_checksum = slot.m_checksum;
		job->m_body     = body;
		melt_job_t queued{ };
		queued.m_skin = std::move( job );
		if ( !melt_queue( std::move( queued ) ) )
			return nullptr;

		g_melt_skins.push_back( slot );
		pending = true;
		return nullptr;
	}

	bool melt_bone_seen( const studiohdr_t* hdr, const std::vector< signed char >& seen, int bone )
	{
		for ( int guard = 0; bone >= 0 && bone < static_cast< int >( seen.size( ) ) && guard < 256; guard++ ) {
			if ( seen[ bone ] >= 0 )
				return seen[ bone ] > 0;
			const mstudiobone_t* data = hdr->get_bone( bone );
			bone                      = data ? data->m_parent : -1;
		}
		return true;
	}

	/* 1 built, 0 failed, -1 skin still on the worker */
	int melt_build( melt_t& melt, c_base_entity* victim, c_base_entity* ragdoll, const bool may_wait )
	{
		c_base_entity* entity    = ragdoll ? ragdoll : victim;
		const matrix3x4_t* bones = nullptr;
		int bone_count           = 0;

		std::vector< matrix3x4_t > fresh{ };
		if ( ragdoll && melt_fresh_bones( ragdoll, fresh ) ) {
			bones      = fresh.data( );
			bone_count = static_cast< int >( fresh.size( ) );
		}

		if ( !bones && !melt.m_bones.empty( ) ) {
			bones      = melt.m_bones.data( );
			bone_count = static_cast< int >( melt.m_bones.size( ) );
		}

		const model_t* model   = entity->get_model( );
		const studiohdr_t* hdr = model ? g_interfaces.m_model_info->get_studio_model( model ) : nullptr;
		hitbox_resolver_t resolver{ };
		if ( !bones || !model || !hdr || !resolver.setup( entity ) )
			return 0;

		const float life = std::clamp( GET_VARIABLE( g_variables.m_death_particles_lifetime, float ), 1.f, 10.f );
		const float goo  = std::clamp( GET_VARIABLE( g_variables.m_death_particles_melt_goo, float ), 0.5f, 3.f );
		melt.m_speed     = std::clamp( GET_VARIABLE( g_variables.m_death_particles_melt_speed, float ), 0.25f, 3.f );

		bool pending = false;
		auto skin    = melt_vvd( entity, hdr, model, pending );
		if ( pending && may_wait )
			return -1;

		std::vector< melt_capsule_t > capsules{ };
		std::vector< signed char > bone_seen( static_cast< std::size_t >( bone_count ), -1 );
		const c_vector eye = melt_eye( );

		for ( int hitbox = 0; hitbox < resolver.m_set->m_hit_boxes; hitbox++ ) {
			const mstudiobbox_t* box = resolver.m_set->get_hitbox( hitbox );
			if ( !box || box->m_bone < 0 || box->m_bone >= bone_count )
				continue;

			c_vector lo = box->m_bb_min, hi = box->m_bb_max;
			float radius = box->m_radius;
			if ( radius <= 0.f ) {
				/* box hitbox ( hands / feet ): capsule down its longest side */
				const c_vector mid = ( lo + hi ) * 0.5f;
				float half[ 3 ]    = { std::abs( hi.m_x - lo.m_x ) * 0.5f, std::abs( hi.m_y - lo.m_y ) * 0.5f, std::abs( hi.m_z - lo.m_z ) * 0.5f };
				const int axis     = static_cast< int >( std::max_element( half, half + 3 ) - half );
				radius             = ( std::max )( ( half[ 0 ] + half[ 1 ] + half[ 2 ] - half[ axis ] ) * 0.5f, 0.5f );
				float along[ 3 ]   = { 0.f, 0.f, 0.f };
				along[ axis ]      = ( std::max )( half[ axis ] - radius, 0.f );
				lo                 = mid - c_vector( along[ 0 ], along[ 1 ], along[ 2 ] );
				hi                 = mid + c_vector( along[ 0 ], along[ 1 ], along[ 2 ] );
			}
			capsules.push_back( { lo, hi, radius, box->m_bone } );

			bool seen = true;
			if ( !eye.is_zero( ) ) {
				trace_t trace{ };
				melt_trace( eye, g_math.vector_transform( ( lo + hi ) * 0.5f, bones[ box->m_bone ] ), k_melt_see, trace );
				seen = !trace.m_start_solid && trace.m_fraction >= 0.99f;
			}
			bone_seen[ box->m_bone ] = static_cast< signed char >( ( std::max )( static_cast< int >( bone_seen[ box->m_bone ] ), seen ? 1 : 0 ) );
		}

		const bool from_vvd = skin != nullptr;
		if ( !skin ) {
			auto fallback = std::make_shared< melt_template_t >( );
			melt_capsule_mesh( capsules, bone_count, *fallback );
			melt_finish( *fallback );
			skin = std::move( fallback );
		}
		if ( skin->m_verts.empty( ) )
			return 0;

		auto sim      = std::make_unique< melt_sim_t >( );
		sim->m_skin   = skin;
		sim->m_volume = k_melt_body_volume * goo;
		sim->m_bones.assign( bones, bones + bone_count );
		melt_start( *sim );

		const std::size_t n = skin->m_verts.size( );
		melt.m_seen.resize( n );
		float low = 1e9f;
		c_vector mid{ };
		for ( std::size_t i = 0; i < n; i++ ) {
			const melt_raw_vert_t& vert = skin->m_verts[ i ];
			const int strongest         = static_cast< int >( std::max_element( vert.m_weight, vert.m_weight + vert.m_count ) - vert.m_weight );
			melt.m_seen[ i ]            = melt_bone_seen( hdr, bone_seen, vert.m_bone[ strongest ] ) ? 1 : 0;
			low                         = ( std::min )( low, sim->m_at_capture[ i ].m_z );
			mid += sim->m_at_capture[ i ];
		}
		mid *= 1.f / static_cast< float >( n );

		trace_t trace{ };
		melt_trace( c_vector( mid.m_x, mid.m_y, low + 24.f ), c_vector( mid.m_x, mid.m_y, low - 200.f ), k_melt_hit, trace );
		sim->m_base_floor = trace.m_start_solid ? low : trace.m_end.m_z;

		const int cx = melt_world_cell( mid.m_x ), cy = melt_world_cell( mid.m_y );
		sim->m_ox = cx - k_melt_grid / 2;
		sim->m_oy = cy - k_melt_grid / 2;
		sim->m_cells.assign( static_cast< std::size_t >( k_melt_grid ) * k_melt_grid, melt_cell_t{ } );
		for ( int dx = -1; dx <= 1; dx++ )
			for ( int dy = -1; dy <= 1; dy++ )
				melt_ask( *sim, cx + dx, cy + dy, k_melt_from_above, 0, low + 8.f );
		int budget = 9;
		melt_answer( *sim, budget );

		melt.m_settle = k_melt_sink_time + 4.f;
		melt.m_end    = k_melt_sink_time + 1.f + life * melt.m_speed;
		botox_dbg_log( "MELT: build skin %s verts %d tris %d goo %.0f u3 ( mesh %.0f ) floor %.1f low %.1f bones %s %d skip %d", from_vvd ? "vvd" : "hitbox",
		               static_cast< int >( n ), static_cast< int >( skin->m_tris.size( ) / 3 ), sim->m_volume, skin->m_volume, sim->m_base_floor, low,
		               fresh.empty( ) ? "victim" : "fresh", bone_count, static_cast< int >( g_melt_skip.size( ) ) );
		melt.m_sim = std::move( sim );
		return 1;
	}

	/* rgb: capture samples per vertex ( alpha 0 = none ), null = capture failed: team color, the worker finishes texels on its first job */
	void melt_colors( melt_t& melt, const unsigned int* rgb )
	{
		if ( !melt.m_sim )
			return;

		melt_sim_t& sim        = *melt.m_sim;
		const std::size_t n    = sim.m_skin->m_verts.size( );
		const c_color fallback = melt.m_team == 2 ? c_color( 122, 104, 78 ) : melt.m_team == 3 ? c_color( 72, 88, 116 ) : c_color( 128, 128, 128 );
		for ( std::size_t i = 0; i < n; i++ ) {
			const float shade = rand_range( 0.92f, 1.05f );
			for ( int c = 0; c < 3; c++ )
				sim.m_rgb[ i * 3 + c ] = ( std::min )( 255.f, fallback[ c ] * shade );
		}

		sim.m_samples.assign( n, 0u );
		int valid = 0;
		if ( rgb )
			for ( std::size_t i = 0; i < n; i++ ) {
				sim.m_samples[ i ] = rgb[ i ];
				valid += ( rgb[ i ] >> 24 ) ? 1 : 0;
			}
		sim.m_colors_ready = false;
		botox_dbg_log( "MELT: colors %s valid %d/%d", rgb ? "capture" : "fallback", valid, static_cast< int >( n ) );
	}

	constexpr const char* k_melt_vs = R"(
float4 row0 : register( c0 );
float4 row1 : register( c1 );
float4 row2 : register( c2 );
float4 row3 : register( c3 );

struct vs_in {
	float3 position : POSITION;
	float3 normal : NORMAL;
	float4 color : COLOR0;
	float2 uv : TEXCOORD0;
	float2 extra : TEXCOORD1;
};

struct vs_out {
	float4 position : POSITION;
	float3 normal : TEXCOORD0;
	float3 world : TEXCOORD1;
	float4 color : TEXCOORD2;
	float4 uv : TEXCOORD3;
};

vs_out main( vs_in i )
{
	const float4 p = float4( i.position, 1.0 );
	vs_out o;
	o.position = float4( dot( row0, p ), dot( row1, p ), dot( row2, p ), dot( row3, p ) );
	o.normal   = i.normal;
	o.world    = i.position;
	o.color    = float4( i.color.rgb, i.extra.y );
	o.uv       = float4( i.uv, i.extra.x, 0.0 );
	return o;
}
)";

	constexpr const char* k_melt_ps = R"(
sampler2D capture : register( s0 );
float4 eye : register( c0 );
float4 params : register( c1 );
float4 light : register( c2 );

float4 main( float3 normal : TEXCOORD0, float3 world : TEXCOORD1, float4 color : TEXCOORD2, float4 uv : TEXCOORD3 ) : COLOR
{
	const float3 v = normalize( eye.xyz - world );
	float3 n       = normalize( normal );
	n              = dot( n, v ) < 0.0 ? -n : n;

	const float wet  = color.a;
	const float3 tex = tex2D( capture, uv.xy ).rgb;
	const float3 base = lerp( color.rgb, tex, saturate( uv.z * params.y ) );

	const float ndl  = saturate( dot( n, light.xyz ) );
	const float3 h   = normalize( light.xyz + v );
	const float spec = pow( max( saturate( dot( n, h ) ), 1e-4 ), lerp( 16.0, 72.0, wet ) ) * lerp( 0.04, 0.3, wet );
	const float rim  = pow( 1.0 - saturate( dot( n, v ) ), 4.0 ) * 0.05 * wet;

	return float4( base * ( ( 0.78 + 0.3 * ndl ) * ( 1.0 - 0.15 * wet ) ) + spec + rim, params.x );
}
)";

	/* render thread: billboards from kfx_quad, shape picked by kind ( e_kfx_kind ) */
	constexpr const char* k_kfx_vs = R"(
float4 row0 : register( c0 );
float4 row1 : register( c1 );
float4 row2 : register( c2 );
float4 row3 : register( c3 );
float4 right : register( c4 );
float4 up : register( c5 );

struct vs_in {
	float3 position : POSITION;
	float4 color : COLOR0;
	float4 quad : TEXCOORD0;
	float2 shape : TEXCOORD1;
};

struct vs_out {
	float4 position : POSITION;
	float4 color : TEXCOORD0;
	float3 shape : TEXCOORD1;
};

vs_out main( vs_in i )
{
	const float4 p = float4( i.position + ( right.xyz * i.quad.x + up.xyz * i.quad.y ) * i.quad.z, 1.0 );
	vs_out o;
	o.position = float4( dot( row0, p ), dot( row1, p ), dot( row2, p ), dot( row3, p ) );
	o.color    = i.color;
	o.shape    = float3( i.shape, i.quad.w );
	return o;
}
)";

	constexpr const char* k_kfx_ps = R"(
// iq lattice hash, no sin, no integer-input streaks
float hash( float2 p )
{
	p = 50.0 * frac( p * 0.3183099 + float2( 0.71, 0.113 ) );
	return frac( p.x * p.y * ( p.x + p.y ) );
}

float noise( float2 p )
{
	const float2 i = floor( p );
	const float2 f = frac( p );
	const float2 u = f * f * ( 3.0 - 2.0 * f );
	return lerp( lerp( hash( i ), hash( i + float2( 1, 0 ) ), u.x ), lerp( hash( i + float2( 0, 1 ) ), hash( i + 1.0 ), u.x ), u.y );
}

float fbm( float2 p )
{
	float v = 0.0, w = 0.5;
	for ( int k = 0; k < 4; k++ ) {
		v += w * noise( p );
		p = float2( 1.6 * p.x + 1.2 * p.y, -1.2 * p.x + 1.6 * p.y ) + 17.1;
		w *= 0.5;
	}
	return v / 0.9375;
}

float4 main( float4 color : TEXCOORD0, float3 shape : TEXCOORD1 ) : COLOR
{
	const float r    = length( shape.xy );
	const float edge = saturate( ( 1.0 - r ) / max( fwidth( r ), 1e-4 ) + 0.5 );

	// glow: soul rings 0.4 / 1 / 1.7 / 2.8 radii at alpha 1 / 0.5 / 0.16 / 0, quad spans 2.8
	const float g    = r * 2.8;
	const float glow = g < 0.4 ? 1.0 : g < 1.0 ? lerp( 1.0, 0.5, ( g - 0.4 ) / 0.6 ) : g < 1.7 ? lerp( 0.5, 0.16, ( g - 1.0 ) / 0.7 ) : lerp( 0.16, 0.0, saturate( ( g - 1.7 ) / 1.1 ) );

	const float kind = shape.z;
	if ( kind > 2.5 ) {
		// smoke: warped fbm puff, frac( kind ) = spin -> rotation + noise offset ( both wrap at 1 ), edge 0 before r = 1
		const float spin = frac( kind ) * 6.2831853;
		const float2 cs  = float2( cos( spin ), sin( spin ) );
		const float2 q   = float2( shape.x * cs.x - shape.y * cs.y, shape.x * cs.y + shape.y * cs.x ) * 1.7 + cs * 3.0;
		const float n    = fbm( q + ( noise( q * 0.8 + 5.2 ) - 0.5 ) );
		const float d    = r / 0.75 + ( 0.5 - n ) * 0.6;
		const float body = 1.0 - smoothstep( 0.25, 1.0, d );
		const float a    = saturate( color.a * body * ( 0.45 + 0.8 * n ) );
		return float4( color.rgb * ( 0.72 + 0.36 * n + 0.1 * shape.y ), a );
	}

	const float a = kind < 0.5 ? color.a : kind < 1.5 ? color.a * edge : color.a * glow;
	return float4( color.rgb, a );
}
)";

	ID3DXBuffer* compile_shader( const char* source, const char* profile, const char* tag )
	{
		ID3DXBuffer *code = nullptr, *errors = nullptr;
		const HRESULT result =
			D3DXCompileShader( source, static_cast< UINT >( std::strlen( source ) ), nullptr, nullptr, "main", profile, 0, &code, &errors, nullptr );
		if ( errors ) {
			if ( FAILED( result ) )
				botox_dbg_log( "%s: %s compile: %s", tag, profile, static_cast< const char* >( errors->GetBufferPointer( ) ) );
			errors->Release( );
		}
		if ( FAILED( result ) && code ) {
			code->Release( );
			code = nullptr;
		}
		return code;
	}

	bool build_shaders( IDirect3DDevice9* device, const char* vs_source, const char* ps_source, IDirect3DVertexShader9*& vs_out,
	                    IDirect3DPixelShader9*& ps_out, bool& failed, const char* tag )
	{
		if ( vs_out && ps_out )
			return true;
		if ( failed )
			return false;

		ID3DXBuffer* vs = compile_shader( vs_source, "vs_3_0", tag );
		ID3DXBuffer* ps = compile_shader( ps_source, "ps_3_0", tag );

		const bool ok = vs && ps && SUCCEEDED( device->CreateVertexShader( static_cast< const DWORD* >( vs->GetBufferPointer( ) ), &vs_out ) ) &&
		                SUCCEEDED( device->CreatePixelShader( static_cast< const DWORD* >( ps->GetBufferPointer( ) ), &ps_out ) );

		if ( vs )
			vs->Release( );
		if ( ps )
			ps->Release( );

		if ( !ok ) {
			if ( vs_out )
				vs_out->Release( );
			if ( ps_out )
				ps_out->Release( );
			vs_out = nullptr;
			ps_out = nullptr;
			failed = true;
		}

		botox_dbg_log( "%s: shaders %s", tag, ok ? "built" : "FAILED" );
		return ok;
	}

	bool melt_shaders( IDirect3DDevice9* device )
	{
		return build_shaders( device, k_melt_vs, k_melt_ps, g_melt_vs, g_melt_ps, g_melt_shader_failed, "MELT" );
	}

	/* render thread: every d3d state a world draw touches, put back after */
	struct world_state_t {
		static constexpr D3DRENDERSTATETYPE k_states[] = { D3DRS_ZENABLE,           D3DRS_ZWRITEENABLE,     D3DRS_ZFUNC,           D3DRS_ALPHABLENDENABLE,
		                                                   D3DRS_SRCBLEND,          D3DRS_DESTBLEND,        D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE,
		                                                   D3DRS_STENCILENABLE,     D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_SCISSORTESTENABLE,
		                                                   D3DRS_CLIPPLANEENABLE,   D3DRS_FILLMODE,         D3DRS_FOGENABLE,       D3DRS_SEPARATEALPHABLENDENABLE };
		static constexpr D3DSAMPLERSTATETYPE k_samplers[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER,
		                                                      D3DSAMP_SRGBTEXTURE };

		DWORD m_states[ std::size( k_states ) ]{ }, m_samplers[ std::size( k_samplers ) ]{ }, m_fvf = 0;
		float m_vs_constants[ 6 ][ 4 ]{ }, m_ps_constants[ 3 ][ 4 ]{ };
		IDirect3DVertexShader9* m_vs        = nullptr;
		IDirect3DPixelShader9* m_ps         = nullptr;
		IDirect3DVertexDeclaration9* m_decl = nullptr;
		IDirect3DBaseTexture9* m_texture    = nullptr;
		n_stream_guard::state_t m_streams{ };

		void capture( IDirect3DDevice9* device )
		{
			for ( std::size_t i = 0; i < std::size( k_states ); i++ )
				device->GetRenderState( k_states[ i ], &m_states[ i ] );
			for ( std::size_t i = 0; i < std::size( k_samplers ); i++ )
				device->GetSamplerState( 0, k_samplers[ i ], &m_samplers[ i ] );
			device->GetVertexShader( &m_vs );
			device->GetPixelShader( &m_ps );
			device->GetVertexDeclaration( &m_decl );
			device->GetFVF( &m_fvf );
			device->GetTexture( 0, &m_texture );
			device->GetVertexShaderConstantF( 0, m_vs_constants[ 0 ], 6 );
			device->GetPixelShaderConstantF( 0, m_ps_constants[ 0 ], 3 );
			m_streams.capture( device );
		}

		/* z test vs scene depth ( walls, props, viewmodel all wrote it before DoPostScreenSpaceEffects ), blended, rgb only */
		static void set( IDirect3DDevice9* device )
		{
			device->SetRenderState( D3DRS_ZENABLE, D3DZB_TRUE );
			device->SetRenderState( D3DRS_ZFUNC, D3DCMP_LESSEQUAL );
			device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
			device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
			device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
			device->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
			device->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
			device->SetRenderState( D3DRS_STENCILENABLE, FALSE );
			device->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE );
			device->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );
			device->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );
			device->SetRenderState( D3DRS_CLIPPLANEENABLE, 0 );
			device->SetRenderState( D3DRS_FILLMODE, D3DFILL_SOLID );
			device->SetRenderState( D3DRS_FOGENABLE, FALSE );
			device->SetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, FALSE );
		}

		void restore( IDirect3DDevice9* device )
		{
			for ( std::size_t i = 0; i < std::size( k_states ); i++ )
				device->SetRenderState( k_states[ i ], m_states[ i ] );
			for ( std::size_t i = 0; i < std::size( k_samplers ); i++ )
				device->SetSamplerState( 0, k_samplers[ i ], m_samplers[ i ] );
			device->SetVertexShaderConstantF( 0, m_vs_constants[ 0 ], 6 );
			device->SetPixelShaderConstantF( 0, m_ps_constants[ 0 ], 3 );
			device->SetVertexShader( m_vs );
			device->SetPixelShader( m_ps );
			if ( m_decl )
				device->SetVertexDeclaration( m_decl );
			else
				device->SetFVF( m_fvf );
			device->SetTexture( 0, m_texture );
			m_streams.restore( device );

			for ( IUnknown* held : std::initializer_list< IUnknown* >{ m_vs, m_ps, m_decl, m_texture } )
				if ( held )
					held->Release( );
		}
	};

	struct melt_draw_t {
		std::shared_ptr< const melt_mesh_t > m_mesh;
		int m_slot;
		float m_alpha;
	};

	/* render thread: every touched d3d state must go back */
	void melt_draw( const std::vector< melt_draw_t >& draws, const std::array< float, 16 >& matrix, const c_vector& eye )
	{
		IDirect3DDevice9* device = g_interfaces.m_direct_device;
		if ( !device || draws.empty( ) || !melt_shaders( device ) )
			return;

		static DWORD max_index = 0;
		if ( !max_index ) {
			D3DCAPS9 caps{ };
			max_index = SUCCEEDED( device->GetDeviceCaps( &caps ) ) ? caps.MaxVertexIndex : 0xffffu;
		}

		world_state_t saved{ };
		saved.capture( device );
		world_state_t::set( device );
		device->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
		device->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
		device->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
		device->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
		device->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
		device->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, FALSE );

		device->SetFVF( k_melt_fvf );
		device->SetVertexShader( g_melt_vs );
		device->SetPixelShader( g_melt_ps );
		device->SetVertexShaderConstantF( 0, matrix.data( ), 4 );

		const float light_length = std::sqrt( 0.35f * 0.35f + 0.25f * 0.25f + 0.9f * 0.9f );
		const float eye_light[ 2 ][ 4 ] = { { eye.m_x, eye.m_y, eye.m_z, 0.f }, { 0.35f / light_length, 0.25f / light_length, 0.9f / light_length, 0.f } };
		device->SetPixelShaderConstantF( 0, eye_light[ 0 ], 1 );
		device->SetPixelShaderConstantF( 2, eye_light[ 1 ], 1 );

		static std::vector< unsigned short > short_indices{ };
		static bool logged = false;

		for ( const melt_draw_t& draw : draws ) {
			const auto& vertices = draw.m_mesh->m_vertices;
			const auto& indices  = draw.m_mesh->m_indices;
			if ( vertices.empty( ) || indices.size( ) < 3 )
				continue;

			const bool wide = vertices.size( ) > 0xffffu;
			if ( wide && vertices.size( ) > max_index )
				continue;

			IDirect3DTexture9* texture = draw.m_slot >= 0 && draw.m_slot < static_cast< int >( k_melt_cap ) ? g_melt_textures[ draw.m_slot ] : nullptr;
			const float params[ 4 ]    = { draw.m_alpha, texture ? 1.f : 0.f, 0.f, 0.f };
			device->SetPixelShaderConstantF( 1, params, 1 );
			device->SetTexture( 0, texture );
			device->SetRenderState( D3DRS_ZWRITEENABLE, draw.m_alpha >= 0.999f ? TRUE : FALSE );

			HRESULT result = S_OK;
			if ( wide )
				result = device->DrawIndexedPrimitiveUP( D3DPT_TRIANGLELIST, 0, static_cast< UINT >( vertices.size( ) ), static_cast< UINT >( indices.size( ) / 3 ),
				                                         indices.data( ), D3DFMT_INDEX32, vertices.data( ), sizeof( melt_vertex_t ) );
			else {
				short_indices.assign( indices.begin( ), indices.end( ) );
				result = device->DrawIndexedPrimitiveUP( D3DPT_TRIANGLELIST, 0, static_cast< UINT >( vertices.size( ) ),
				                                         static_cast< UINT >( short_indices.size( ) / 3 ), short_indices.data( ), D3DFMT_INDEX16, vertices.data( ),
				                                         sizeof( melt_vertex_t ) );
			}

			if ( !logged ) {
				logged = true;
				botox_dbg_log( "MELT: world draw hr %08lx verts %d tris %d tex %d", static_cast< unsigned long >( result ), static_cast< int >( vertices.size( ) ),
				               static_cast< int >( indices.size( ) / 3 ), texture ? 1 : 0 );
			}
		}

		saved.restore( device );
	}

	constexpr DWORD k_kfx_fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2 | D3DFVF_TEXCOORDSIZE4( 0 ) | D3DFVF_TEXCOORDSIZE2( 1 );
	static_assert( sizeof( kfx_vertex_t ) == 40, "k_kfx_fvf layout" );

	/* render thread. no z write = translucent like engine sprites, scene depth still hides them behind walls / viewmodel */
	void kfx_draw( const std::vector< kfx_vertex_t >& quads, const std::array< float, 16 >& matrix, const c_vector& right, const c_vector& up )
	{
		IDirect3DDevice9* device = g_interfaces.m_direct_device;
		if ( !device || quads.empty( ) || !build_shaders( device, k_kfx_vs, k_kfx_ps, g_kfx_vs, g_kfx_ps, g_kfx_shader_failed, "KFX" ) )
			return;

		world_state_t saved{ };
		saved.capture( device );
		world_state_t::set( device );
		device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );

		device->SetFVF( k_kfx_fvf );
		device->SetVertexShader( g_kfx_vs );
		device->SetPixelShader( g_kfx_ps );
		device->SetVertexShaderConstantF( 0, matrix.data( ), 4 );
		const float axes[ 2 ][ 4 ] = { { right.m_x, right.m_y, right.m_z, 0.f }, { up.m_x, up.m_y, up.m_z, 0.f } };
		device->SetVertexShaderConstantF( 4, axes[ 0 ], 2 );

		constexpr std::size_t chunk = 3 * 0x8000;
		HRESULT result              = S_OK;
		for ( std::size_t first = 0; first < quads.size( ) && SUCCEEDED( result ); first += chunk ) {
			const std::size_t count = ( std::min )( chunk, quads.size( ) - first );
			result = device->DrawPrimitiveUP( D3DPT_TRIANGLELIST, static_cast< UINT >( count / 3 ), &quads[ first ], sizeof( kfx_vertex_t ) );
		}

		static HRESULT logged = 1;
		if ( result != logged ) {
			logged = result;
			botox_dbg_log( "KFX: world draw hr %08lx quads %d", static_cast< unsigned long >( result ), static_cast< int >( quads.size( ) / 6 ) );
		}

		saved.restore( device );
	}

	void spawn_melt( c_base_entity* victim, const int victim_index )
	{
		if ( g_melts.size( ) >= k_melt_cap )
			g_melts.erase( g_melts.begin( ) );

		melt_t melt{ };
		melt.m_victim = victim_index;
		melt.m_id     = g_melt_id_next++;
		melt.m_team   = victim->get_team( );

		if ( auto& bones = victim->get_cached_bone_data( ); bones.count( ) > 0 && bones.base( ) )
			melt.m_bones.assign( bones.base( ), bones.base( ) + bones.count( ) );

		botox_dbg_log( "MELT: spawn victim %d bones %d", victim_index, static_cast< int >( melt.m_bones.size( ) ) );
		g_melts.push_back( std::move( melt ) );
	}

	void melt_paint( const float dt )
	{
		for ( auto it = g_melt_hidden.begin( ); it != g_melt_hidden.end( ); )
			it = from_handle( *it ) ? it + 1 : g_melt_hidden.erase( it );

		if ( g_melts.empty( ) )
			return;

		melt_collect( );
		melt_refresh_skip( );
		const c_vector eye = melt_eye( );

		if ( g_melt_capture.m_state.load( std::memory_order_acquire ) == 2 ) {
			for ( melt_t& melt : g_melts ) {
				if ( melt.m_stage != melt_wait_capture || melt.m_capture_base < 0 || !melt.m_sim )
					continue;

				const std::size_t base  = static_cast< std::size_t >( melt.m_capture_base );
				const std::size_t count = melt.m_sim->m_skin->m_verts.size( );
				const bool fits         = base + count <= g_melt_capture.m_rgb.size( );
				melt_colors( melt, fits ? g_melt_capture.m_rgb.data( ) + base : nullptr );
				std::memcpy( melt.m_sim->m_view, g_melt_capture.m_view, sizeof( melt.m_sim->m_view ) );
				std::memcpy( melt.m_sim->m_crop, g_melt_capture.m_crop, sizeof( melt.m_sim->m_crop ) );
				std::memcpy( melt.m_sim->m_screen, g_melt_capture.m_screen, sizeof( melt.m_sim->m_screen ) );
				melt.m_sim->m_textured = fits && g_melt_capture.m_crop[ 2 ] > 0.f;
				melt.m_slot            = melt.m_sim->m_textured ? g_melt_capture.m_slot : -1;
				melt.m_capture_base    = -1;
				melt.m_stage           = melt_run;
			}

			g_melt_capture.m_state.store( 0, std::memory_order_release );
		}

		bool want_capture = false;
		int budget        = k_melt_probe_budget;

		for ( melt_t& melt : g_melts ) {
			const auto victim = g_interfaces.m_client_entity_list->get< c_base_entity >( melt.m_victim );

			if ( melt.m_stage == melt_wait_ragdoll ) {
				melt.m_wait += dt;
				c_base_entity* ragdoll = victim ? melt_ragdoll( victim ) : nullptr;
				if ( !ragdoll && melt.m_wait < 0.3f )
					continue;

				if ( ragdoll )
					melt.m_ragdoll = victim->get_ragdoll_handle( );

				const int built = victim ? melt_build( melt, victim, ragdoll, melt.m_skin_wait < 1.3f ) : 0;
				if ( built < 0 ) {
					melt.m_skin_wait += dt;
					continue;
				}
				melt.m_stage = built > 0 ? melt_wait_capture : melt_dead;
				melt.m_wait  = 0.f;

				int seen = 0;
				for ( const unsigned char flag : melt.m_seen )
					seen += flag;
				botox_dbg_log( "MELT: build %s victim %d ragdoll %d verts %d seen %d eye %d", melt.m_stage == melt_dead ? "FAIL" : "ok", melt.m_victim, ragdoll ? 1 : 0,
				               melt.m_sim ? static_cast< int >( melt.m_sim->m_skin->m_verts.size( ) ) : 0, seen, eye.is_zero( ) ? 0 : 1 );
			}

			if ( !melt.m_ragdoll && victim && melt_ragdoll( victim ) )
				melt.m_ragdoll = victim->get_ragdoll_handle( );

			/* a sim on the worker is never touched: m_sim is null until melt_collect hands it back */
			if ( melt.m_sim && melt.m_stage >= melt_wait_capture ) {
				melt_answer( *melt.m_sim, budget );
				static std::vector< matrix3x4_t > fresh{ };
				if ( c_base_entity* ragdoll = from_handle( melt.m_ragdoll ); ragdoll && melt_fresh_bones( ragdoll, fresh ) && fresh.size( ) == melt.m_sim->m_bones.size( ) )
					melt.m_sim->m_bones.swap( fresh );
			}

			if ( melt.m_stage == melt_wait_capture ) {
				melt.m_wait += dt;
				if ( melt.m_wait <= 0.35f ) {
					want_capture |= melt.m_capture_base < 0;
					continue;
				}

				melt_colors( melt, nullptr );
				melt.m_capture_base = -1;
				melt.m_stage        = melt_run;
			}

			if ( melt.m_stage < melt_run )
				continue;

			if ( melt.m_ragdoll && std::find( g_melt_hidden.begin( ), g_melt_hidden.end( ), melt.m_ragdoll ) == g_melt_hidden.end( ) ) {
				if ( g_melt_hidden.size( ) >= 64 )
					g_melt_hidden.erase( g_melt_hidden.begin( ) );
				g_melt_hidden.push_back( melt.m_ragdoll );
			}

			if ( melt.m_stage == melt_run ) {
				melt.m_accum = ( std::min )( melt.m_accum + dt * melt.m_speed, 0.1f );
				if ( melt.m_sim && !melt.m_busy ) {
					melt.m_age += melt.m_accum;
					melt_submit( melt, melt.m_accum );
					melt.m_accum = 0.f;
				}

				const bool still = melt.m_sim && melt_still( *melt.m_sim );
				if ( melt.m_age >= melt.m_settle || still )
					melt.m_stage = melt_rest;
			} else {
				melt.m_age += dt * melt.m_speed;
				if ( melt.m_sim && !melt.m_busy && !melt.m_mesh )
					melt_submit( melt, 0.f );
			}

			if ( melt.m_age >= melt.m_end )
				melt.m_stage = melt_dead;

			melt.m_alpha = std::clamp( ( melt.m_end - melt.m_age ) / ( 0.8f * melt.m_speed ), 0.f, 1.f );
		}

		if ( want_capture && g_melt_capture.m_state.load( std::memory_order_acquire ) == 0 ) {
			const view_matrix_t& matrix = g_interfaces.m_engine_client->get_world_to_screen_matrix( );
			auto& uv                    = g_melt_capture.m_uv;
			uv.clear( );
			for ( int row = 0; row < 4; row++ )
				for ( int col = 0; col < 4; col++ )
					g_melt_capture.m_view[ row ][ col ] = matrix[ row ][ col ];

			std::vector< matrix3x4_t > mats{ };
			for ( melt_t& melt : g_melts ) {
				if ( melt.m_stage != melt_wait_capture || melt.m_capture_base >= 0 || !melt.m_sim )
					continue;

				melt_sim_t& sim = *melt.m_sim;
				melt_mats( sim, mats );
				melt.m_capture_base = static_cast< int >( uv.size( ) / 2 );
				for ( std::size_t i = 0; i < sim.m_skin->m_verts.size( ); i++ ) {
					const melt_raw_vert_t& vert = sim.m_skin->m_verts[ i ];
					const c_vector p = melt_skin( vert, mats, false ), normal = melt_skin( vert, mats, true ), view = eye - p;
					sim.m_at_capture[ i ] = p;
					float u = -1.f, v = -1.f;
					if ( melt.m_seen[ i ] && ( eye.is_zero( ) || normal.dot_product( view ) > k_melt_facing * view.length( ) * normal.length( ) ) )
						melt_uv( matrix, p, u, v );
					uv.push_back( u );
					uv.push_back( v );
				}
			}

			int on_screen = 0;
			for ( std::size_t i = 0; i < uv.size( ); i += 2 )
				on_screen += uv[ i ] >= 0.f ? 1 : 0;
			botox_dbg_log( "MELT: capture request %d verts, %d facing on screen", static_cast< int >( uv.size( ) / 2 ), on_screen );

			g_melt_capture.m_slot = g_melt_slot_next;
			g_melt_slot_next      = ( g_melt_slot_next + 1 ) % static_cast< int >( k_melt_cap );
			g_melt_capture.m_state.store( 1, std::memory_order_release );
		}

		g_melts.erase( std::remove_if( g_melts.begin( ), g_melts.end( ), [ ]( const melt_t& melt ) { return melt.m_stage == melt_dead; } ),
		               g_melts.end( ) );
	}
}

void kill_effects_paint( )
{
	const bool in_game = g_interfaces.m_engine_client->is_in_game( );
	const float now    = g_interfaces.m_global_vars_base->m_real_time;
	const float cur    = g_interfaces.m_global_vars_base->m_current_time;

	if ( !in_game || !GET_VARIABLE( g_variables.m_death_particles, bool ) || cur + 1.f < g_kfx_last_cur ) {
		g_kfx.clear( );
		g_melts.clear( );
		g_melt_hidden.clear( );
	}

	const float dt   = g_kfx_last_real < 0.f ? 0.f : std::clamp( now - g_kfx_last_real, 0.f, 0.05f );
	g_kfx_last_real  = now;
	g_kfx_last_cur   = cur;

	{
		PERF_ZONE( zone_paint_melt );
		melt_paint( dt );
	}

	g_kfx_quads.clear( );
	if ( g_kfx.empty( ) )
		return;

	const bool glow = GET_VARIABLE( g_variables.m_death_particles_glow, bool );

	for ( kfx_particle_t& particle : g_kfx ) {
		particle.m_age += dt;
		if ( particle.m_age >= particle.m_life )
			continue;

		if ( particle.m_style == k_mc_style ) {
			for ( particle.m_tick_acc += dt * 20.f; particle.m_tick_acc >= 1.f && particle.m_age < particle.m_life; particle.m_tick_acc -= 1.f )
				mc_tick( particle );
			const c_vector at = particle.m_prev + ( particle.m_pos - particle.m_prev ) * particle.m_tick_acc;
			if ( particle.m_tick_acc >= 0.f && particle.m_age < particle.m_life ) {
				const auto channel = [ & ]( const int i ) { return static_cast< ImU32 >( std::clamp( particle.m_tint[ i ], 0.f, 1.f ) * 255.f ); };
				kfx_sprite( at, particle.m_size, 8, [ & ]( const int x, const int y ) -> ImU32 {
					const std::uint32_t rgb = k_mc_crit[ y ][ x ];
					if ( !rgb )
						return 0u;
					const auto mix = [ & ]( const int shift, const int i ) { return ( ( rgb >> shift ) & 0xff ) * channel( i ) / 255u; };
					return IM_COL32( mix( 16, 0 ), mix( 8, 1 ), mix( 0, 2 ), 255 );
				} );
			}
			continue;
		}

		if ( particle.m_style == k_mc_xp_style ) {
			for ( particle.m_tick_acc += dt * 20.f; particle.m_tick_acc >= 1.f; particle.m_tick_acc -= 1.f )
				mc_xp_tick( particle );
			// ExperienceOrbRenderer: quad 0.3 block, raised 0.1 + 0.075, tint pulses with age, vertex alpha 128
			const float scale = particle.m_size * 0.3f * k_mc_block;
			const c_vector at = particle.m_prev + ( particle.m_pos - particle.m_prev ) * particle.m_tick_acc +
			                    c_vector( 0.f, 0.f, 0.1f * k_mc_block + scale * 0.25f );
			const float phase = particle.m_age * 20.f / 2.f;
			const auto red    = static_cast< ImU32 >( ( std::sin( phase ) + 1.f ) * 0.5f * 255.f );
			const auto blue   = static_cast< ImU32 >( ( std::sin( phase + 4.18879f ) + 1.f ) * 0.1f * 255.f );
			const float fade  = std::clamp( ( particle.m_life - particle.m_age ) / ( std::min )( 1.5f, particle.m_life * 0.5f ), 0.f, 1.f );
			const auto alpha  = static_cast< ImU32 >( fade * fade * ( 3.f - 2.f * fade ) * 128.f );
			if ( alpha )
				kfx_sprite( at, scale * 0.5f, 16, [ & ]( const int x, const int y ) -> ImU32 {
					const char key = k_mc_xp_orb[ particle.m_icon ][ y ][ x ];
					if ( key == '.' )
						return 0u;
					const std::uint32_t rgb = k_mc_xp_palette[ key - 'a' ];
					return IM_COL32( ( ( rgb >> 16 ) & 0xff ) * red / 255u, ( rgb >> 8 ) & 0xff, ( rgb & 0xff ) * blue / 255u, alpha );
				} );
			continue;
		}

		const bool smoke = particle.m_style == k_smoke_style;
		if ( particle.m_style < 0 )
			step_ball( particle, dt );
		else if ( smoke )
			step_smoke( particle, dt );
		else
			step_soul( particle, dt );

		const float t = particle.m_age / particle.m_life;

		float alpha = particle.m_style < 0 ? std::clamp( ( 1.f - t ) / 0.25f, 0.f, 1.f )
		                                   : ( std::min )( particle.m_age / 0.1f, 1.f ) * std::pow( 1.f - t, 1.5f );
		if ( particle.m_style == 1 || particle.m_style == 2 )
			alpha *= rand_range( 0.55f, 1.f );

		const float size = smoke ? particle.m_size * ( 1.f + 0.7f * t ) : particle.m_size;
		if ( alpha <= 0.01f )
			continue;

		const c_color& color = particle.m_color;
		if ( smoke ) {
			// slow swirl, half spin each way
			const float spin = particle.m_seed + particle.m_age * ( particle.m_seed < 0.5f ? 0.04f : -0.04f );
			kfx_quad( particle.m_pos, size, -1.f, -1.f, 1.f, 1.f, kfx_smoke, color.get_u32( alpha * 0.55f ), spin - std::floor( spin ) );
			continue;
		}
		if ( glow && particle.m_style >= 0 ) {
			kfx_quad( particle.m_pos, size, -2.8f, -2.8f, 2.8f, 2.8f, kfx_glow, color.get_u32( alpha ) );
			continue;
		}
		kfx_quad( particle.m_pos, size, -1.f, -1.f, 1.f, 1.f, kfx_disc, color.get_u32( alpha ) );
		if ( particle.m_style < 0 ) {
			// up-left shine, 0.35 radius
			const c_color highlight( ( std::min )( 255, color[ 0 ] + 90 ), ( std::min )( 255, color[ 1 ] + 90 ), ( std::min )( 255, color[ 2 ] + 90 ), static_cast< int >( color[ 3 ] ) );
			kfx_quad( particle.m_pos, size, -0.7f, 0.f, 0.f, 0.7f, kfx_disc, highlight.get_u32( alpha * 0.8f ) );
		}
	}

	g_kfx.erase( std::remove_if( g_kfx.begin( ), g_kfx.end( ), [ ]( const kfx_particle_t& p ) { return p.m_age >= p.m_life; } ), g_kfx.end( ) );
}

void kill_effects_world( const c_vector& origin, const c_angle& angles )
{
	g_melt_eye       = origin;
	g_melt_eye_valid = true;

	std::vector< melt_draw_t > draws{ };
	for ( const melt_t& melt : g_melts )
		if ( melt.m_stage >= melt_run && melt.m_mesh && !melt.m_mesh->m_indices.empty( ) && melt.m_alpha > 0.f )
			draws.push_back( { melt.m_mesh, melt.m_slot, melt.m_alpha } );

	if ( draws.empty( ) && g_kfx_quads.empty( ) )
		return;

	std::array< float, 16 > matrix{ };
	std::memcpy( matrix.data( ), &g_interfaces.m_engine_client->get_world_to_screen_matrix( ).data[ 0 ][ 0 ], sizeof( float ) * 16 );

	c_vector right{ }, up{ };
	g_math.angle_vectors( angles, nullptr, &right, &up );
	const auto quads = g_kfx_quads.empty( ) ? nullptr : std::make_shared< const std::vector< kfx_vertex_t > >( g_kfx_quads );

	auto job = [ draws = std::move( draws ), quads, matrix, origin, right, up ] {
		if ( !draws.empty( ) )
			melt_draw( draws, matrix, origin );
		if ( quads )
			kfx_draw( *quads, matrix, right, up );
	};
	if ( !n_render_queue::submit( job ) )
		job( );
}

bool kill_effects_hides_ragdoll( const int entity_index )
{
	if ( entity_index <= 0 || g_melt_hidden.empty( ) )
		return false;

	const auto entity = g_interfaces.m_client_entity_list->get< c_base_entity >( entity_index );
	if ( !entity )
		return false;

	for ( const unsigned int handle : g_melt_hidden )
		if ( from_handle( handle ) == entity )
			return true;

	return false;
}

void kill_effects_release_textures( )
{
	for ( IDirect3DTexture9*& texture : g_melt_textures ) {
		if ( texture )
			texture->Release( );
		texture = nullptr;
	}

	if ( g_melt_vs )
		g_melt_vs->Release( );
	if ( g_melt_ps )
		g_melt_ps->Release( );
	g_melt_vs            = nullptr;
	g_melt_ps            = nullptr;
	g_melt_shader_failed = false;

	if ( g_kfx_vs )
		g_kfx_vs->Release( );
	if ( g_kfx_ps )
		g_kfx_ps->Release( );
	g_kfx_vs            = nullptr;
	g_kfx_ps            = nullptr;
	g_kfx_shader_failed = false;
}

void kill_effects_shutdown( )
{
	{
		std::lock_guard< std::mutex > lock( g_melt_worker.m_lock );
		if ( !g_melt_worker.m_started || g_melt_worker.m_stop )
			return;
		g_melt_worker.m_stop = true;
	}
	g_melt_worker.m_wake.notify_all( );

	for ( int waited = 0; !g_melt_worker.m_finished.load( ) && waited < 200; ++waited )
		Sleep( 10 );
	Sleep( 20 );
}

bool kill_effects_wants_ragdoll_dme( )
{
	return !g_melts.empty( ) || !g_melt_hidden.empty( );
}

void kill_effects_capture( IDirect3DDevice9* device )
{
	if ( !device || g_melt_capture.m_state.load( std::memory_order_acquire ) != 1 )
		return;

	const auto& uv          = g_melt_capture.m_uv;
	auto& rgb               = g_melt_capture.m_rgb;
	const std::size_t count = uv.size( ) / 2;
	rgb.assign( count, 0u );
	std::memset( g_melt_capture.m_crop, 0, sizeof( g_melt_capture.m_crop ) );

	const int slot              = std::clamp( g_melt_capture.m_slot, 0, static_cast< int >( k_melt_cap ) - 1 );
	IDirect3DTexture9*& texture = g_melt_textures[ slot ];
	if ( texture ) {
		texture->Release( );
		texture = nullptr;
	}

	IDirect3DSurface9 *back_buffer = nullptr, *resolve = nullptr, *crop = nullptr, *sysmem = nullptr;
	D3DSURFACE_DESC description{ };
	bool ok = false;

	if ( SUCCEEDED( device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer ) ) && back_buffer &&
	     SUCCEEDED( back_buffer->GetDesc( &description ) ) &&
	     ( description.Format == D3DFMT_X8R8G8B8 || description.Format == D3DFMT_A8R8G8B8 ) && description.Width && description.Height ) {
		std::vector< long > pixels( count * 2, -1 );
		long left = 0x7fffffff, top = 0x7fffffff, right = -1, bottom = -1;

		for ( std::size_t i = 0; i < count; i++ ) {
			if ( uv[ i * 2 ] < 0.f )
				continue;

			const long x      = std::clamp( static_cast< long >( uv[ i * 2 ] * description.Width ), 0l, static_cast< long >( description.Width ) - 1 );
			const long y      = std::clamp( static_cast< long >( uv[ i * 2 + 1 ] * description.Height ), 0l, static_cast< long >( description.Height ) - 1 );
			pixels[ i * 2 ]     = x;
			pixels[ i * 2 + 1 ] = y;
			left                = ( std::min )( left, x );
			top                 = ( std::min )( top, y );
			right               = ( std::max )( right, x );
			bottom              = ( std::max )( bottom, y );
		}

		if ( right >= 0 ) {
			constexpr long pad = 8;
			left               = ( std::max )( left - pad, 0l );
			top                = ( std::max )( top - pad, 0l );
			right              = ( std::min )( right + pad, static_cast< long >( description.Width ) - 1 );
			bottom             = ( std::min )( bottom + pad, static_cast< long >( description.Height ) - 1 );

			const RECT rect{ left, top, right + 1, bottom + 1 };
			const UINT width        = static_cast< UINT >( right - left + 1 );
			const UINT height       = static_cast< UINT >( bottom - top + 1 );
			const bool multisampled = description.MultiSampleType != D3DMULTISAMPLE_NONE;

			ok = SUCCEEDED( device->CreateTexture( width, height, 1, D3DUSAGE_RENDERTARGET, description.Format, D3DPOOL_DEFAULT, &texture, nullptr ) ) &&
			     SUCCEEDED( texture->GetSurfaceLevel( 0, &crop ) ) &&
			     SUCCEEDED( device->CreateOffscreenPlainSurface( width, height, description.Format, D3DPOOL_SYSTEMMEM, &sysmem, nullptr ) );

			/* msaa back buffer: full resolve first, sub-rect blits from msaa are not portable */
			if ( ok && multisampled )
				ok = SUCCEEDED( device->CreateRenderTarget( description.Width, description.Height, description.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
				                                            &resolve, nullptr ) ) &&
				     SUCCEEDED( device->StretchRect( back_buffer, nullptr, resolve, nullptr, D3DTEXF_NONE ) );

			ok = ok && SUCCEEDED( device->StretchRect( multisampled ? resolve : back_buffer, &rect, crop, nullptr, D3DTEXF_NONE ) );
			if ( ok ) {
				const float crop_rect[ 4 ] = { static_cast< float >( left ), static_cast< float >( top ), static_cast< float >( width ), static_cast< float >( height ) };
				std::memcpy( g_melt_capture.m_crop, crop_rect, sizeof( crop_rect ) );
				g_melt_capture.m_screen[ 0 ] = static_cast< float >( description.Width );
				g_melt_capture.m_screen[ 1 ] = static_cast< float >( description.Height );
			}

			D3DLOCKED_RECT locked{ };
			if ( ok && SUCCEEDED( device->GetRenderTargetData( crop, sysmem ) ) && SUCCEEDED( sysmem->LockRect( &locked, nullptr, D3DLOCK_READONLY ) ) ) {
				const auto bits = static_cast< const unsigned char* >( locked.pBits );

				for ( std::size_t i = 0; i < count; i++ ) {
					if ( pixels[ i * 2 ] < 0 )
						continue;

					const unsigned int pixel =
						*reinterpret_cast< const unsigned int* >( bits + ( pixels[ i * 2 + 1 ] - top ) * locked.Pitch + ( pixels[ i * 2 ] - left ) * 4 );
					rgb[ i ] = 0xff000000u | ( pixel & 0x00ffffffu );
				}

				sysmem->UnlockRect( );
			}
		}
	}

	if ( sysmem )
		sysmem->Release( );
	if ( crop )
		crop->Release( );
	if ( resolve )
		resolve->Release( );
	if ( back_buffer )
		back_buffer->Release( );

	if ( !ok && texture ) {
		texture->Release( );
		texture = nullptr;
	}

	g_melt_capture.m_state.store( 2, std::memory_order_release );
}

static recv_var_proxy_fn g_original_ragdoll_force_proxy = nullptr;

static void __cdecl ragdoll_force_proxy( const c_recv_proxy_data* data, void* structure, void* output )
{
	const int mode = GET_VARIABLE( g_variables.m_ragdoll_gravity, int );

	if ( mode > 0 && structure && g_ctx.m_local ) {
		const auto ragdoll = static_cast< c_base_entity* >( structure );
		const auto player =
			reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity_from_handle( ragdoll->get_ragdoll_player_handle( ) ) );

		if ( player && player != g_ctx.m_local && g_ctx.m_local->is_enemy( player ) ) {
			const float strength = std::clamp( GET_VARIABLE( g_variables.m_ragdoll_gravity_strength, float ), 0.1f, 5.f );
			float* force         = const_cast< c_recv_proxy_data* >( data )->m_value.m_vector;

			if ( mode == 1 ) {
				force[ 0 ] *= 60.f * strength;
				force[ 1 ] *= 60.f * strength;
				force[ 2 ] = ( std::max )( force[ 2 ] * 2.f, std::hypot( force[ 0 ], force[ 1 ] ) * 0.25f );
			} else {
				force[ 0 ] *= 0.1f;
				force[ 1 ] *= 0.1f;
				force[ 2 ] = 250000.f * strength;
			}
		}
	}

	if ( g_original_ragdoll_force_proxy )
		g_original_ragdoll_force_proxy( data, structure, output );
}

static recv_prop_t* ragdoll_force_prop( )
{
	for ( auto client_class = g_interfaces.m_base_client->get_all_classes( ); client_class; client_class = client_class->m_next ) {
		if ( !client_class->m_network_name || std::strcmp( client_class->m_network_name, "CCSRagdoll" ) || !client_class->m_recv_table )
			continue;

		const auto table = client_class->m_recv_table;
		for ( int i = 0; i < table->m_props_count; i++ ) {
			recv_prop_t* prop = &table->m_props[ i ];
			if ( prop->m_var_name && !std::strcmp( prop->m_var_name, "m_vecForce" ) )
				return prop;
		}
		break;
	}

	return nullptr;
}

void ragdoll_force_hook( )
{
	recv_prop_t* prop = ragdoll_force_prop( );
	if ( !prop || prop->m_proxy_fn == &ragdoll_force_proxy )
		return;

	g_original_ragdoll_force_proxy = prop->m_proxy_fn;
	prop->m_proxy_fn               = &ragdoll_force_proxy;
}

void ragdoll_force_unhook( )
{
	recv_prop_t* prop = ragdoll_force_prop( );
	if ( prop && prop->m_proxy_fn == &ragdoll_force_proxy && g_original_ragdoll_force_proxy )
		prop->m_proxy_fn = g_original_ragdoll_force_proxy;
}

void on_death_particles( const int victim_index, const int attacker_index )
{
	if ( !GET_VARIABLE( g_variables.m_death_particles, bool ) || victim_index == g_interfaces.m_engine_client->get_local_player( ) )
		return;

	const auto victim = g_interfaces.m_client_entity_list->get< c_base_entity >( victim_index );
	if ( !victim || victim->is_dormant( ) )
		return;

	switch ( GET_VARIABLE( g_variables.m_death_particles_type, int ) ) {
	case 3: spawn_smoke( victim ); return;
	case 5: spawn_soul( victim ); return;
	case 7: spawn_melt( victim, victim_index ); return;
	case 8: spawn_mc( victim ); return;
	case 6:
		if ( attacker_index == g_interfaces.m_engine_client->get_local_player( ) )
			spawn_balls( victim );
		return;
	default: break;
	}

	c_vector at = victim->get_abs_origin( );
	at.m_z += 40.f;
	const c_vector up( 0.f, 0.f, 1.f );

	static const char* const k_blood[]     = { "blood_impact_heavy", "blood_impact_medium", "blood_impact_basic" };
	static const char* const k_confetti[]  = { "weapon_confetti_balloons", "weapon_confetti_omni", "weapon_confetti" };
	static const char* const k_explosion[] = { "explosion_hegrenade_interior", "explosion_hegrenade_dirt", "explosion_basic" };
	const auto try_names = [ & ]( const auto& names ) {
		const c_angle angles = names[ 0 ] == k_blood[ 0 ] ? c_angle( -90.f, 0.f, 0.f ) : c_angle( 0.f, 0.f, 0.f );
		for ( const char* name : names )
			if ( dispatch_particle( name, at, angles ) )
				return true;
		return false;
	};

	const int type = GET_VARIABLE( g_variables.m_death_particles_type, int );
	void* fx       = effects( );
	bool done      = false;
	switch ( type ) {
	case 0: done = try_names( k_blood ); break;
	case 2: done = try_names( k_confetti ); break;
	case 4: done = try_names( k_explosion ); break;
	default: break;
	}
	if ( done || !fx )
		return;

	/* sparks, or a pick whose systems this map never precached */
	if ( type == 1 )
		g_virtual.call< void >( fx, 3, std::cref( at ), 8, 8, static_cast< const c_vector* >( nullptr ) );
	else {
		g_virtual.call< void >( fx, 7, std::cref( at ), std::cref( up ), true );
		static int logged = -1;
		if ( logged != type ) {
			logged = type;
			botox_dbg_log( "KFX: death particle %d not precached on this map, energy splash instead", type );
		}
	}
}
