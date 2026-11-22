#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "movement.h"
#include "../prediction/prediction.h"
#include "texturebug_common.h"
#include "edgebug.h"
#include "edge_surf_core.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

using n_es::es_guide;
using n_es::es_guide_t;

extern void botox_dbg_log( const char* fmt, ... );
extern bool botox_is_ladder_trace( const trace_t& tr );
extern bool g_edge_surf_active;
extern bool g_air_stuck_owns_cmd;

float es_ulp( const float a )
{
	return n_es::ulp( a );
}

float es_hull_gap( const c_vector& o, const c_vector& n, const float d, const c_vector& mins, const c_vector& maxs )
{
	return o.m_x * n.m_x + o.m_y * n.m_y + o.m_z * n.m_z - d + std::min( n.m_x * mins.m_x, n.m_x * maxs.m_x ) + std::min( n.m_y * mins.m_y, n.m_y * maxs.m_y ) +
	       std::min( n.m_z * mins.m_z, n.m_z * maxs.m_z );
}

/* mv->m_flMaxSpeed = weapon speed ( kisak cs_player_shared.cpp:365 ) x stamina scale ( cs_gamemovement.cpp:283 ), never raw m_flMaxspeed */
float es_max_speed( c_base_entity* local )
{
	float speed = std::max( local->get_max_speed( ), 0.f );
	if ( auto* const weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_active_weapon_handle( ) );
	     weapon && g_interfaces.m_weapon_system )
		if ( const c_weapon_data* const data = g_interfaces.m_weapon_system->get_weapon_data( weapon->get_item_definition_index( ) ) )
			if ( const float w = data->m_max_speed[ local->is_scoped( ) ? 1 : 0 ]; w > 1.f )
				speed = std::min( speed, w );
	if ( const float st = local->get_stamina( ); st > 0.f ) {
		const float s = std::clamp( 1.f - st / 100.f, 0.f, 1.f );
		speed *= s * s;
	}
	return speed;
}

static int g_edge_surf_latch_cmd = -1;
int g_edge_surf_pin_cmd          = -1;

bool edge_surf_latched( const c_user_cmd* cmd )
{
	return cmd && g_edge_surf_latch_cmd >= 0 && g_edge_surf_latch_cmd == cmd->m_command_number - 1;
}

/* air_align.cpp: its key near a ps point on a ledge top = that pin is edge surf's ( set in movement pre before its yields ) */
extern bool g_air_align_ledge;

bool edge_surf_armed( )
{
	return GET_VARIABLE( g_variables.m_edge_surf, bool ) && !g_ctx.m_eject_restoring && g_input.check_input( &GET_VARIABLE( g_variables.m_edge_surf_key, key_bind_t ) ) ||
	       g_air_align_ledge;
}

/* edgebug "edgesurf on eb": the eb key arms only the takeover at an eb's pin END + the chain after it, never a fresh approach */
bool edge_surf_eb_armed( )
{
	return GET_VARIABLE( g_variables.m_edgebug_edge_surf, bool ) && GET_VARIABLE( g_variables.edge_bug, bool ) && GET_VARIABLE( g_variables.m_edgebug_style, int ) == 0 &&
	       !g_ctx.m_eject_restoring && g_input.check_input( &GET_VARIABLE( g_variables.edge_bug_key, key_bind_t ) );
}

void edge_surf_skip( const c_user_cmd* cmd, const char* why )
{
	if ( edge_surf_latched( cmd ) && GET_VARIABLE( g_variables.m_debug_log, bool ) )
		botox_dbg_log( "ESURF: skip why=%s cmd=%d", why, cmd->m_command_number );
}

/* later features may rewrite the cmd: one closing ulp over the top = standing on the ledge */
static struct {
	int cmd = -1, buttons = 0;
	float yaw = 0.f, fwd = 0.f, side = 0.f;
} s_es_sent;

void edge_surf_final_stamp( c_user_cmd* cmd )
{
	constexpr int k_keys = in_forward | in_back | in_moveleft | in_moveright | in_duck;
	if ( !cmd || s_es_sent.cmd != cmd->m_command_number )
		return;
	const auto wish = []( const float fwd, const float side, const float yaw ) {
		const float a = deg2rad( yaw );
		return std::pair{ fwd * std::cosf( a ) + side * std::sinf( a ), fwd * std::sinf( a ) - side * std::cosf( a ) };
	};
	const auto [ wx0, wy0 ] = wish( s_es_sent.fwd, s_es_sent.side, s_es_sent.yaw );
	const auto [ wx1, wy1 ] = wish( cmd->m_forward_move, cmd->m_side_move, cmd->m_view_point.m_y );
	const bool keys         = ( cmd->m_buttons & k_keys ) != ( s_es_sent.buttons & k_keys );
	if ( !keys && std::fabsf( wx1 - wx0 ) + std::fabsf( wy1 - wy0 ) < 1e-4f )
		return;
	if ( GET_VARIABLE( g_variables.m_debug_log, bool ) )
		botox_dbg_log( "ESURF: stomp cmd=%d wish %.5f,%.5f -> %.5f,%.5f keys %x -> %x yaw %.3f -> %.3f", cmd->m_command_number, wx0, wy0, wx1, wy1,
		               s_es_sent.buttons & k_keys, cmd->m_buttons & k_keys, s_es_sent.yaw, cmd->m_view_point.m_y );
	cmd->m_forward_move = s_es_sent.fwd;
	cmd->m_side_move    = s_es_sent.side;
	/* same view = exact bits ( a pin press is ulp sized ) */
	if ( cmd->m_view_point.m_y != s_es_sent.yaw )
		n_tb::move_fix_to_yaw( s_es_sent.yaw, *cmd );
	cmd->m_buttons = ( cmd->m_buttons & ~k_keys ) | ( s_es_sent.buttons & k_keys );
}

namespace
{
	n_es::v3 to3( const c_vector& v ) { return n_es::mk( v.m_x, v.m_y, v.m_z ); }
	c_vector tov( const n_es::v3& v ) { return c_vector( v.x, v.y, v.z ); }

	/* the surface name, never dispFlags: only disp code writes those, a closer brush hit later in the same trace keeps them ( cmodel.cpp:1670 ).
	   exact name ( cmodel_disp.cpp:40 ): a brush material with "displacement" in its path is a brush */
	bool es_disp_hit( const trace_t& t )
	{
		return t.surface.m_name && !strcmp( t.surface.m_name, "**displacement**" );
	}
	/* movement collides with it: world, a static prop, or an entity outside debris / pushaway / weapon ( COLLISION_GROUP_PLAYER_MOVEMENT passes those,
	   cs_gamerules.cpp:15118 "Don't stand on COLLISION_GROUP_WEAPONs" ) */
	bool es_moves_ent( c_base_entity* const e )
	{
		if ( !e || e->get_index( ) == 0 )
			return true;
		const int g = e->get_collision_group( );
		return g != e_collision_group::collision_group_debris && g != e_collision_group::collision_group_debris_trigger &&
		       g != e_collision_group::collision_group_interactive_debris && g != e_collision_group::collision_group_pushaway &&
		       g != e_collision_group::collision_group_weapon;
	}

	/* tb's world + props filter minus what player movement passes ( weapons, debris, pushaway ): a dropped gun is no ledge, top or floor */
	struct es_filter_t : n_tb::c_trace_filter_tb_world_props {
		using c_trace_filter_tb_world_props::c_trace_filter_tb_world_props;
		bool should_hit_entity( c_base_entity* entity, int mask ) override
		{
			if ( !c_trace_filter_tb_world_props::should_hit_entity( entity, mask ) )
				return false;
			/* a static prop handle has no entity behind it */
			if ( ( entity->get_ref_ehandle( ) >> 16 ) == ( 1u << 15 ) )
				return true;
			return es_moves_ent( entity );
		}
	};

	/* n_es adapter: sims = the tb family's prediction ( restore / pred_simulate / snapshots ), traces = tb_trace with world + props */
	struct es_world_t {
		c_base_entity* local = nullptr;
		int frame            = 0;
		c_user_cmd base{ };
		float dt = 0.015625f, pin = -6.25f, k_acc = 0.f, max_speed = 250.f;
		float duck_speed = 0.f, last_duck = 0.f, now = 0.f, duck_gap = 0.4f;
		bool debug = false;
		int sims   = 0;
		es_filter_t filter;

		explicit es_world_t( c_base_entity* l ) : local( l ), filter( l ) { }

		n_es::st_t read( ) const
		{
			n_es::st_t s;
			s.o          = to3( local->get_origin( ) );
			s.v          = to3( local->get_velocity( ) );
			const int fl = local->get_flags( );
			s.ground     = ( fl & fl_onground ) != 0;
			s.duck       = ( fl & fl_ducking ) != 0;
			s.walk       = local->get_move_type( ) == e_move_types::move_type_walk;
			return s;
		}
		n_es::st_t start( )
		{
			n_tb::tb_restore( frame );
			return read( );
		}
		void reset( ) { n_tb::tb_restore( frame ); }
		c_user_cmd cmd_of( const n_es::mv_t& m ) const
		{
			c_user_cmd c = base;
			c.m_buttons &= ~( in_forward | in_back | in_moveleft | in_moveright | in_jump | in_duck );
			if ( m.duck )
				c.m_buttons |= in_duck;
			c.m_forward_move = m.wish;
			c.m_side_move    = 0.f;
			if ( m.wish > 0.f )
				n_tb::move_fix_to_yaw( m.yaw, c );
			return c;
		}
		n_es::st_t sim( const n_es::mv_t& m )
		{
			c_user_cmd c = cmd_of( m );
			n_tb::pred_simulate( &c );
			++sims;
			return read( );
		}
		bool save( const int i ) { return g_prediction.snapshot_save( i ); }
		void load( const int i )
		{
			n_tb::tb_restore( frame );
			g_prediction.snapshot_load( i );
			n_tb::s_pred_dirty = true;
		}
		/* each prop / entity a sweep met, once per 10 s: names a prop= face ( sprop = the bsp static prop index, tools/wc_engine/bsp_props_box.py ) */
		static void note_obj( c_base_entity* const he, const bool ent, const int solid, const int hitbox )
		{
			static int s_seen[ 32 ]{ };
			static int s_n                   = 0;
			static unsigned long long s_reset = 0ull;
			if ( const unsigned long long t = GetTickCount64( ); t >= s_reset ) {
				s_n     = 0;
				s_reset = t + 10000ull;
			}
			const int code = ent ? he->get_index( ) : -hitbox;
			if ( std::find( s_seen, s_seen + std::min( s_n, 32 ), code ) != s_seen + std::min( s_n, 32 ) )
				return;
			s_seen[ s_n++ % 32 ] = code;
			if ( !ent ) {
				botox_dbg_log( "ESURF: obj sprop=%d", hitbox - 1 );
				return;
			}
			const auto* const cc = static_cast< c_client_networkable* >( he )->get_client_class( );
			botox_dbg_log( "ESURF: obj ent=%d %s solid=%d group=%d", code, cc && cc->m_network_name ? cc->m_network_name : "?", solid, he->get_collision_group( ) );
		}
		/* cs_gamerules.cpp g_CSViewVectors */
		n_es::hull_t hull( const bool duck ) const { return { n_es::mk( -16.f, -16.f, 0.f ), n_es::mk( 16.f, 16.f, duck ? 54.f : 72.f ) }; }
		bool trace( const n_es::v3& a, const n_es::v3& b, const n_es::v3& mins, const n_es::v3& maxs, n_es::hit_t& h )
		{
			trace_t tr{ };
			ray_t ray( tov( a ), tov( b ), tov( mins ), tov( maxs ) );
			n_tb::tb_trace( ray, mask_playersolid, &filter, &tr );
			h.frac        = tr.m_fraction;
			h.start_solid = tr.m_start_solid;
			h.n           = to3( tr.m_plane.m_normal );
			h.end         = to3( tr.m_end );
			h.d           = n_tb::hull_plane_dist( tr, tov( mins ), tov( maxs ) );
			h.disp        = es_disp_hit( tr );
			h.ladder      = botox_is_ladder_trace( tr );
			h.moves       = es_moves_ent( tr.m_hit_entity );
			/* world = box rules: a world brush or a brush entity ( SOLID_BSP, CM_TransformedBoxTrace ). vp = vphysics: a static prop ( reads as the world
			   entity with hitbox = prop index + 1, kisak enginetrace.cpp:1321 ) or SOLID_VPHYSICS. an entity bbox / obb = neither */
			c_base_entity* const he = tr.m_hit_entity;
			const bool ent          = he && he->get_index( ) != 0;
			const int solid         = ent && he->get_collideable( ) ? he->get_collideable( )->get_solid( ) : 0;
			h.world                 = tr.m_hitbox == 0 && ( !ent || solid == 1 /* SOLID_BSP */ );
			h.vp                    = ( !ent && tr.m_hitbox != 0 ) || solid == 6 /* SOLID_VPHYSICS */;
			if ( debug && tr.m_fraction < 1.f && !h.world )
				note_obj( he, ent, solid, tr.m_hitbox );
			return tr.m_fraction < 1.f;
		}
		int sims_left( ) const { return n_tb::sims_left( ); }
		int used( ) const { return sims; }
		/* owned ticks log every tick, hands off lines 4 a second */
		void note( const char act, const char* line ) const
		{
			static unsigned long long s_next = 0ull;
			static char s_last               = 0;
			const unsigned long long t       = GetTickCount64( );
			if ( act == 'Y' && act == s_last && t < s_next )
				return;
			s_next = t + 250ull;
			s_last = act;
			botox_dbg_log( "%s", line );
		}
	};

	n_es::mem_t s_mem{ };
	float s_viz_time  = -1.f;
	bool s_viz_pinned = false;
}

/* true = edge surf owns this cmd ( tb + head bounce skip it ), pinned = a proven pin, held = nothing after tb may rewrite it */
bool edge_surf_online( c_user_cmd* cmd, bool& pinned, bool& held )
{
	pinned = held = false;
	c_base_entity* const local = g_ctx.m_local;
	g_edge_surf_active         = false;
	const bool key   = edge_surf_armed( );
	const bool armed = cmd && local && local->is_alive( ) && local->get_collideable( ) && ( key || edge_surf_eb_armed( ) );
	/* START must be the server's END, like texture bug */
	g_prediction.sync_arm( n_prediction::impl_t::k_sync_es, armed );
	const auto off = [ & ]( const char* why ) {
		edge_surf_skip( cmd, why );
		g_edge_surf_latch_cmd = -1;
		s_es_sent.cmd         = -1;
		s_mem.own             = false;
		return false;
	};
	const auto& start = g_prediction.backup_data;
	if ( !armed || ( start.m_flags & fl_onground ) ) {
		n_es::new_fall( s_mem );
		return off( !armed ? "key" : "ground" );
	}
	if ( start.m_move_type != e_move_types::move_type_walk )
		return off( "move" );
	/* act Q stood you up: your held duck stays off until the feet pass that top ( a re-duck = feet +9 + 2.0 duck speed a toggle ), only while we own it */
	if ( s_mem.up_cmd == cmd->m_command_number - 1 && start.m_origin.m_z > s_mem.up_top && edge_surf_latched( cmd ) ) {
		s_mem.up_cmd = cmd->m_command_number;
		cmd->m_buttons &= ~in_duck;
	}
	const bool debug   = GET_VARIABLE( g_variables.m_debug_log, bool );
	const float pin    = g_prediction.get_engine_target_predict_z_velocity( );
	const bool pin_now = std::fabsf( start.m_velocity.m_z - pin ) < 1e-3f;
	/* eb key alone: a fresh pin START ( an edgebug just ended, its plan spent ) or our chain, nothing else */
	static int s_pin_cmd = -1;
	const bool fresh     = pin_now && s_pin_cmd != cmd->m_command_number - 1;
	if ( pin_now )
		s_pin_cmd = cmd->m_command_number;
	if ( !key && !edge_surf_latched( cmd ) && ( !fresh || ( g_edgebug.m_found && !g_edgebug.replay_spent( ) ) ) )
		return off( "eb" );
	/* a locked edgebug plan flies first: a tick we own makes it drop ( EB: drop why=taken ) */
	if ( g_edgebug.m_found && !pin_now ) {
		static int s_eb_cmd = -1;
		if ( debug && s_eb_cmd != cmd->m_command_number - 1 )
			botox_dbg_log( "ESURF: eb first cmd=%d vz=%.1f", cmd->m_command_number, start.m_velocity.m_z );
		s_eb_cmd = cmd->m_command_number;
		return off( "eb" );
	}
	if ( g_air_stuck_owns_cmd )
		return off( "as" );

	static auto sv_airaccelerate = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
	es_world_t w( local );
	w.frame = g_interfaces.m_prediction->m_commands_predicted - 1;
	/* friction / duck timer / stamina off the START, not this cmd's own END ( texture bug runs after the prediction ) */
	w.reset( );
	w.base       = *cmd;
	w.dt         = g_interfaces.m_global_vars_base->m_interval_per_tick;
	w.pin        = pin;
	w.k_acc      = ( sv_airaccelerate ? sv_airaccelerate->get_float( ) : 12.f ) * w.dt * local->get_surface_friction( );
	w.max_speed  = es_max_speed( local );
	w.duck_speed = local->get_duck_speed( );
	w.last_duck  = local->get_last_duck_time( );
	w.now        = static_cast< float >( local->get_tick_base( ) ) * w.dt;
	w.duck_gap   = g_convars.float_or( HASH_BT( "sv_timebetweenducks" ), 0.4f );
	w.debug      = debug;
	if ( w.dt <= 0.f || w.k_acc <= 0.f )
		return off( "dt" );
	n_es::in_t in;
	in.range    = std::clamp( GET_VARIABLE( g_variables.m_edge_surf_range, float ), 8.f, 512.f );
	in.reach    = std::clamp( GET_VARIABLE( g_variables.m_edge_surf_reach, float ), 0.f, 32.f );
	in.turn     = std::clamp( GET_VARIABLE( g_variables.m_edge_surf_delta_strafe, float ), 0.f, 15.f );
	in.takeover = !key;
	const n_es::out_t r = n_es::tick( w, s_mem, cmd->m_command_number, in );
	w.reset( );
	s_viz_time   = g_interfaces.m_global_vars_base->m_real_time;
	s_viz_pinned = r.pin;
	if ( !r.own ) {
		g_edge_surf_latch_cmd = -1;
		s_es_sent.cmd         = -1;
		return false;
	}
	const c_user_cmd c  = w.cmd_of( r.mv );
	cmd->m_forward_move = c.m_forward_move;
	cmd->m_side_move    = c.m_side_move;
	cmd->m_buttons      = c.m_buttons;
	pinned              = r.pin;
	held                = true;
	g_edge_surf_active  = true;
	if ( r.pin )
		g_edge_surf_pin_cmd = cmd->m_command_number;
	g_edge_surf_latch_cmd = s_mem.own ? cmd->m_command_number : -1;
	s_es_sent             = { cmd->m_command_number, cmd->m_buttons, cmd->m_view_point.m_y, cmd->m_forward_move, cmd->m_side_move };
	return true;
}

/* edgebug.cpp search sim END: true = an exact feet pin ( vz = -g dt / 2, airborne ). lift = standing beside a ledge face whose top it rides, drifting
   off it slower than 0.046 s of ducked brake ( lab ER_EB sweep: 0 false lifts, 64 + 128, weapon 150..250 ): edge surf's air duck takes it from there */
bool edge_surf_eb_end( bool& lift )
{
	lift                       = false;
	c_base_entity* const local = g_ctx.m_local;
	if ( !local || !local->get_collideable( ) || !g_interfaces.m_global_vars_base )
		return false;
	const c_vector o = local->get_origin( ), v = local->get_velocity( );
	const float pin  = g_prediction.get_engine_target_predict_z_velocity( );
	if ( ( local->get_flags( ) & fl_onground ) || std::fabsf( v.m_z - pin ) >= 1e-3f )
		return false;
	if ( local->get_flags( ) & fl_ducking )
		return true;
	es_world_t w( local );
	w.dt  = g_interfaces.m_global_vars_base->m_interval_per_tick;
	w.pin = pin;
	n_es::st_t s;
	s.o    = to3( o );
	s.v    = to3( v );
	s.walk = true;
	const n_es::hull_t h = w.hull( false );
	n_es::edge_t c[ n_es::k_cand ];
	const int nc = n_es::scan( w, s, h, 2.05f, true, c, n_es::k_cand );
	int best     = -1;
	float bg     = FLT_MAX;
	for ( int i = 0; i < nc; ++i )
		if ( const float g = n_es::gap( c[ i ], s.o, h ); std::fabsf( o.m_z - ( c[ i ].top + n_es::k_eps ) ) < n_es::k_ztol && g < bg ) {
			bg   = g;
			best = i;
		}
	/* only vphysics: the lift's 9u fall back steps under 2u, its over the top pin needs more */
	if ( best < 0 || n_es::vphys( c[ best ] ) )
		return true;
	static auto sv_airaccelerate = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
	const float away  = -n_es::closing( c[ best ], s.v );
	const float brake = ( sv_airaccelerate ? sv_airaccelerate->get_float( ) : 12.f ) * local->get_surface_friction( ) * es_max_speed( local ) * n_es::k_duck_crop;
	lift              = away >= -1e-3f && away <= 0.046f * brake;
	return true;
}

/* visualize: every ledge the scan found ( dim ), the pick ( your color while edge surf owns the cmd, green on a pin, grey once it let go ), the arrival
   point on the crossing tick, the act. a 32u stretch of the edge under the hull, not the ledge's real length */
void edge_surf_render( )
{
	if ( !GET_VARIABLE( g_variables.m_edge_surf_visualize, bool ) || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) || !g_interfaces.m_global_vars_base ||
	     !g_interfaces.m_engine_client->is_in_game( ) )
		return;
	const float age = g_interfaces.m_global_vars_base->m_real_time - s_viz_time;
	if ( s_viz_time < 0.f || age < 0.f || age > 0.15f )
		return;
	const c_color& col = GET_VARIABLE( g_variables.m_edge_surf_visualize_color, c_color );
	const auto line    = [ ]( const c_vector& a, const c_vector& b, const unsigned int c, const float w ) {
		c_vector_2d sa, sb;
		if ( g_render.world_to_screen( a, sa ) && g_render.world_to_screen( b, sb ) )
			g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line, std::make_any< line_draw_object_t >( line_draw_object_t{ sa, sb, c, w } ) );
	};
	const c_vector org = g_ctx.m_local->get_origin( );
	const auto at      = [ & ]( const n_es::edge_t& e, const float s, const float z ) {
		return c_vector( e.n.x * e.d - e.n.y * s, e.n.y * e.d + e.n.x * s, z );
	};
	const auto edge = [ & ]( const n_es::edge_t& e, const float s, const unsigned int color, const float w ) {
		line( at( e, s - 16.f, e.top ), at( e, s + 16.f, e.top ), color, w );
		line( at( e, s - 16.f, e.top ), at( e, s - 16.f, e.top - 8.f ), color, 1.f );
		line( at( e, s + 16.f, e.top ), at( e, s + 16.f, e.top - 8.f ), color, 1.f );
	};
	const n_es::v3 o3 = to3( org );
	for ( int i = 0; i < s_mem.n_cand; ++i )
		if ( !s_mem.pick_ok || !n_es::same( s_mem.cand[ i ], s_mem.pick ) )
			edge( s_mem.cand[ i ], n_es::along( s_mem.cand[ i ], o3 ), col.get_u32( 0.35f ), 1.f );
	if ( !s_mem.pick_ok )
		return;
	const n_es::edge_t& f   = s_mem.pick;
	const bool own          = s_mem.act != 'N' && s_mem.act != 'Y';
	const unsigned int pick = s_viz_pinned ? c_color( 90, 255, 120, 255 ).get_u32( ) : own ? col.get_u32( ) : c_color( 160, 160, 160, 200 ).get_u32( );
	const float s           = s_mem.arrive_ok ? n_es::along( f, s_mem.arrive ) : n_es::along( f, o3 );
	edge( f, s, pick, 2.f );
	if ( c_vector_2d sp; s_mem.arrive_ok && g_render.world_to_screen( at( f, s, f.top ), sp ) )
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_filled_circle, std::make_any< filled_circle_draw_object_t >( filled_circle_draw_object_t( sp, 3.f, pick, 12 ) ) );
	auto* const font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	c_vector_2d sm;
	if ( !font || !g_render.world_to_screen( at( f, s, f.top + 6.f ), sm ) )
		return;
	char buf[ 32 ];
	std::snprintf( buf, sizeof( buf ), "esurf %c%s", s_mem.act, s_viz_pinned ? " pin" : "" );
	text_draw_object_t text;
	text.m_font       = font;
	text.m_position   = c_vector_2d( sm.m_x - font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, buf ).x * 0.5f, sm.m_y - font->FontSize );
	text.m_text       = buf;
	text.m_color      = pick;
	text.m_draw_flags = text_flag_dropshadow;
	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_text, std::make_any< text_draw_object_t >( text ) );
}

/* air_align.cpp flies this guide against a point's wall */
char edge_surf_guide( const float err, const float gap, const float r_min, const float v_old, const float vxy, const int kc, const float authority,
                      const float turn, const float reach, const bool owned, const float ipt, float& v )
{
	const es_guide_t g = es_guide( err, gap, r_min, v_old, vxy, kc, authority, turn, reach, owned, true, ipt );
	v                  = g.v;
	return g.act;
}
