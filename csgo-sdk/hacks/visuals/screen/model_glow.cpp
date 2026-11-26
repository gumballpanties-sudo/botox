#include "model_glow.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "../../entity_cache/entity_cache.h"
#include "../edicts/edicts.h"
#include "player_stencil.h"

#include <algorithm>
#include <vector>

namespace
{
	enum : int { part_arms = 0, part_sleeve = 1, part_weapon = 2 };

	// part off = keeps its coverage ( the others' glow stops at it ) but draws nothing
	c_color part_color( const bool on, const c_color& color )
	{
		return on ? color : c_color( 0, 0, 0, 0 );
	}

	bool any_glow_part( )
	{
		return GET_VARIABLE( g_variables.m_viewmodel_glow, bool ) &&
		       ( GET_VARIABLE( g_variables.m_viewmodel_glow_weapon, bool ) || GET_VARIABLE( g_variables.m_viewmodel_glow_gloves, bool ) ||
		         GET_VARIABLE( g_variables.m_viewmodel_glow_sleeves, bool ) );
	}

	/* OBB corners through this view's matrix: in front and inside [-1,1] = draw. a box across the eye plane ( gun at your feet ) is
	   kept, all corners behind is not */
	bool on_screen( c_base_entity* entity, const view_matrix_t& m )
	{
		const auto renderable  = entity->get_client_renderable( );
		const auto unknown     = renderable ? renderable->get_client_unknown( ) : nullptr;
		const auto collideable = unknown ? unknown->get_collideable( ) : nullptr;
		if ( !collideable )
			return false;

		const c_vector mins      = collideable->get_obb_mins( );
		const c_vector maxs      = collideable->get_obb_maxs( );
		const matrix3x4_t& frame = entity->get_coordinate_frame( );
		float lo[ 2 ] = { 1e9f, 1e9f }, hi[ 2 ] = { -1e9f, -1e9f };
		bool front = false, behind = false;

		for ( int i = 0; i < 8; ++i ) {
			const c_vector p = g_math.vector_transform(
				c_vector( ( i & 1 ) ? maxs.m_x : mins.m_x, ( i & 2 ) ? maxs.m_y : mins.m_y, ( i & 4 ) ? maxs.m_z : mins.m_z ), frame );
			const float w = m[ 3 ][ 0 ] * p.m_x + m[ 3 ][ 1 ] * p.m_y + m[ 3 ][ 2 ] * p.m_z + m[ 3 ][ 3 ];

			if ( w < 0.1f ) {
				behind = true;
				continue;
			}

			front = true;
			for ( int axis = 0; axis < 2; ++axis ) {
				const float ndc = ( m[ axis ][ 0 ] * p.m_x + m[ axis ][ 1 ] * p.m_y + m[ axis ][ 2 ] * p.m_z + m[ axis ][ 3 ] ) / w;
				lo[ axis ]      = ( std::min )( lo[ axis ], ndc );
				hi[ axis ]      = ( std::max )( hi[ axis ], ndc );
			}
		}

		return front && ( behind || ( hi[ 0 ] >= -1.f && lo[ 0 ] <= 1.f && hi[ 1 ] >= -1.f && lo[ 1 ] <= 1.f ) );
	}

	// a pass that faulted once is off for good ( n_fx_compat ): stamping on would leave refs nothing clears
	bool pipeline_ok( )
	{
		return g_player_stencil.ready( ) && !g_player_stencil.model_dead( ) && g_player_stencil.model_live( );
	}

	bool any_stencil_part( )
	{
		return GET_VARIABLE( g_variables.m_viewmodel_stencil, bool ) &&
		       ( GET_VARIABLE( g_variables.m_viewmodel_stencil_weapon, bool ) || GET_VARIABLE( g_variables.m_viewmodel_stencil_gloves, bool ) ||
		         GET_VARIABLE( g_variables.m_viewmodel_stencil_sleeves, bool ) );
	}
}

void n_model_glow::impl_t::begin_view_models( const bool scope_lens_mask )
{
	this->m_in_view_models = !scope_lens_mask;
}

/* the gun's refs are read here, not at post: ironsight clears the stencil in between ( weapon_ironsightcontroller.cpp:545 ),
   tonemap occlusion ORs 0x80 in. capture zeroes them after the read */
void n_model_glow::impl_t::end_view_models( )
{
	const bool main_call   = this->m_in_view_models;
	this->m_in_view_models = false;

	const int frame = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : -1;

	if ( !main_call )
		return;

	// a second RenderView this frame that skips must not hand post the first one's mask
	this->m_captured_frame = -1;

	if ( this->m_stamped_frame != frame || !viewmodel_wanted( ) )
		return;

	using n_player_stencil::fill_t;
	const fill_t fills[] = { { D3DCMP_EQUAL, k_ref_weapon, D3DCOLORWRITEENABLE_RED, false, k_ref_bits },
		                     { D3DCMP_EQUAL, k_ref_gloves, D3DCOLORWRITEENABLE_GREEN, false, k_ref_bits },
		                     { D3DCMP_EQUAL, k_ref_sleeves, D3DCOLORWRITEENABLE_BLUE, false, k_ref_bits } };

	g_player_stencil.capture( fills, 3, k_clear, frame );
	this->m_captured_frame = frame;
}

bool n_model_glow::impl_t::viewmodel_wanted( )
{
	return any_glow_part( ) || any_stencil_part( );
}

bool n_model_glow::impl_t::dropped_wanted( )
{
	return GET_VARIABLE( g_variables.m_dropped_weapons, bool ) && GET_VARIABLE( g_variables.m_dropped_weapons_glow, bool );
}

int n_model_glow::impl_t::viewmodel_ref( const int part )
{
	// no pipeline = nothing would read or clear the refs
	if ( !this->m_in_view_models || !g_interfaces.m_global_vars_base || !pipeline_ok( ) || !viewmodel_wanted( ) )
		return 0;

	// anything else in DrawViewModels ( stattrak / uid / stickers, lua swaps ) rides with the weapon
	const int ref = part == part_arms ? k_ref_gloves : part == part_sleeve ? k_ref_sleeves : k_ref_weapon;

	this->m_stamped_frame = g_interfaces.m_global_vars_base->m_frame_count;
	return ref;
}

void n_model_glow::impl_t::stamp( void* context, const int ref )
{
	if ( !context )
		return;

	shader_stencil_state_t state{ };

	if ( ref ) {
		state.m_enable          = true;
		state.m_func            = shader_stencilfunc_always;
		state.m_pass_op         = shader_stencilop_set_to_reference;
		state.m_reference_value = ref;
		state.m_write_mask      = k_ref_bits;
	}

	static_cast< c_material_render_context* >( context )->set_stencil_state( state );
}

int n_model_glow::impl_t::stamp_dropped( )
{
	std::vector< c_client_renderable* > guns;

	// on screen only, this view's matrix: no gun in view = no pass
	const view_matrix_t m = g_interfaces.m_engine_client->get_world_to_screen_matrix( );

	g_entity_cache.enumerate( e_enumeration_type::type_edicts, [ & ]( c_base_entity* entity ) {
		if ( n_edicts::impl_t::dropped_gun( entity ) && on_screen( entity, m ) )
			if ( const auto renderable = entity->get_client_renderable( ) )
				guns.push_back( renderable );
	} );

	if ( guns.empty( ) )
		return 0;

	c_material* const material               = g_interfaces.m_material_system->find_material( "dev/glow_color", TEXTURE_GROUP_OTHER, true );
	c_material_render_context* const context = g_interfaces.m_material_system->get_render_context( );

	if ( !material || !context ) {
		if ( context )
			context->release( );
		return 0;
	}

	/* RenderGlowEffects' stencil pass ( glow_outline_effect.cpp:540-653 ): glow_color forced, blend 0 ( = no colour, what the
	   engine relies on ), depth tested, no depth write. not override_color_write_enable: its sdk slot 202 is a no-op in retail */
	const float blend = g_interfaces.m_render_view->get_blend( );
	g_interfaces.m_render_view->set_blend( 0.f );
	g_interfaces.m_model_render->forced_material_override( material );
	context->override_depth_enable( true, false );
	g_ctx.m_is_glow_being_drawn = true;

	// STUDIO_RENDER | STUDIO_NOLIGHTING_OR_CUBEMAP | STUDIO_SKIP_FLEXES | STUDIO_DONOTMODIFYSTENCILSTATE | STUDIO_SKIP_DECALS
	constexpr int flags = 0x1 | 0x200 | 0x400 | 0x800 | 0x10000000;
	const renderable_instance_t instance{ 255 };

	shader_stencil_state_t state{ };
	state.m_enable     = true;
	state.m_func       = shader_stencilfunc_always;
	state.m_pass_op    = shader_stencilop_set_to_reference;
	state.m_write_mask = k_ref_bits;

	// whole silhouette hidden ( z fail stamps too ), then the px that pass depth seen
	state.m_z_fail_op       = shader_stencilop_set_to_reference;
	state.m_reference_value = k_ref_dropped_hidden;
	context->set_stencil_state( state );

	for ( c_client_renderable* gun : guns )
		gun->draw_model( flags, instance );

	state.m_z_fail_op       = shader_stencilop_keep;
	state.m_reference_value = k_ref_dropped_seen;
	context->set_stencil_state( state );

	for ( c_client_renderable* gun : guns )
		gun->draw_model( flags, instance );

	context->set_stencil_state( shader_stencil_state_t{ } );
	g_ctx.m_is_glow_being_drawn = false;
	context->override_depth_enable( false, false );
	g_interfaces.m_model_render->forced_material_override( nullptr );
	g_interfaces.m_render_view->set_blend( blend );
	context->release( );

	return static_cast< int >( guns.size( ) );
}

void n_model_glow::impl_t::on_post_screen_space_effects( )
{
	using n_player_stencil::fill_t;
	using n_player_stencil::look_t;

	const int frame = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : -1;

	// stamps wait for ready( ): with player glow / stencil off nothing else would build the shaders ( on = the player pass does,
	// one compile a frame )
	if ( !g_player_stencil.ready( ) && !( n_players::esp_on( ) && g_player_stencil.wanted( ) ) && ( viewmodel_wanted( ) || dropped_wanted( ) ) )
		g_player_stencil.warm( );

	// a pass skipped draw ( so its clear ): stamps are off until the target / depth-stencil checks pass again
	if ( g_player_stencil.ready( ) && !g_player_stencil.model_live( ) && ( viewmodel_wanted( ) || dropped_wanted( ) ) )
		g_player_stencil.probe( );

	const bool captured    = this->m_captured_frame == frame;
	this->m_captured_frame = -1;

	// mask captured at the end of DrawViewModels; first, before the dropped pass builds its own mask over it
	if ( captured && viewmodel_wanted( ) ) {
		look_t look{ };
		look.m_glow    = any_glow_part( );
		look.m_outline = any_stencil_part( );

		const bool weapon  = GET_VARIABLE( g_variables.m_viewmodel_glow_weapon, bool );
		const bool gloves  = GET_VARIABLE( g_variables.m_viewmodel_glow_gloves, bool );
		const bool sleeves = GET_VARIABLE( g_variables.m_viewmodel_glow_sleeves, bool );

		look.m_glow_inner[ 0 ] = part_color( weapon, GET_VARIABLE( g_variables.m_viewmodel_glow_weapon_color, c_color ) );
		look.m_glow_inner[ 1 ] = part_color( gloves, GET_VARIABLE( g_variables.m_viewmodel_glow_gloves_color, c_color ) );
		look.m_glow_inner[ 2 ] = part_color( sleeves, GET_VARIABLE( g_variables.m_viewmodel_glow_sleeves_color, c_color ) );
		look.m_glow_outer[ 0 ] = part_color( weapon, GET_VARIABLE( g_variables.m_viewmodel_glow_weapon_outer_color, c_color ) );
		look.m_glow_outer[ 1 ] = part_color( gloves, GET_VARIABLE( g_variables.m_viewmodel_glow_gloves_outer_color, c_color ) );
		look.m_glow_outer[ 2 ] = part_color( sleeves, GET_VARIABLE( g_variables.m_viewmodel_glow_sleeves_outer_color, c_color ) );

		look.m_gradient       = GET_VARIABLE( g_variables.m_viewmodel_glow_gradient, bool );
		look.m_gradient_inner = GET_VARIABLE( g_variables.m_viewmodel_glow_gradient_inner, float );
		look.m_normal_blend   = GET_VARIABLE( g_variables.m_viewmodel_glow_normal_blend, bool );
		look.m_glow_thickness = GET_VARIABLE( g_variables.m_viewmodel_glow_thickness, float );

		look.m_wave          = GET_VARIABLE( g_variables.m_viewmodel_glow_wave, bool );
		look.m_wave_color    = GET_VARIABLE( g_variables.m_viewmodel_glow_wave_color, c_color );
		look.m_wave_speed    = GET_VARIABLE( g_variables.m_viewmodel_glow_wave_speed, float );
		look.m_wave_size     = GET_VARIABLE( g_variables.m_viewmodel_glow_wave_size, float );
		look.m_wave_strength = GET_VARIABLE( g_variables.m_viewmodel_glow_wave_strength, float );
		look.m_wave_angle    = GET_VARIABLE( g_variables.m_viewmodel_glow_wave_angle, float );

		look.m_pulse       = GET_VARIABLE( g_variables.m_viewmodel_glow_pulse, bool );
		look.m_pulse_speed = GET_VARIABLE( g_variables.m_viewmodel_glow_pulse_speed, float );
		look.m_pulse_min   = GET_VARIABLE( g_variables.m_viewmodel_glow_pulse_min, float );

		look.m_outline_color[ 0 ] = part_color( GET_VARIABLE( g_variables.m_viewmodel_stencil_weapon, bool ),
		                                        GET_VARIABLE( g_variables.m_viewmodel_stencil_weapon_color, c_color ) );
		look.m_outline_color[ 1 ] = part_color( GET_VARIABLE( g_variables.m_viewmodel_stencil_gloves, bool ),
		                                        GET_VARIABLE( g_variables.m_viewmodel_stencil_gloves_color, c_color ) );
		look.m_outline_color[ 2 ] = part_color( GET_VARIABLE( g_variables.m_viewmodel_stencil_sleeves, bool ),
		                                        GET_VARIABLE( g_variables.m_viewmodel_stencil_sleeves_color, c_color ) );
		look.m_outline_thickness  = GET_VARIABLE( g_variables.m_viewmodel_stencil_thickness, float );

		look.m_outline_wave          = GET_VARIABLE( g_variables.m_viewmodel_stencil_wave, bool );
		look.m_outline_wave_color    = GET_VARIABLE( g_variables.m_viewmodel_stencil_wave_color, c_color );
		look.m_outline_wave_speed    = GET_VARIABLE( g_variables.m_viewmodel_stencil_wave_speed, float );
		look.m_outline_wave_size     = GET_VARIABLE( g_variables.m_viewmodel_stencil_wave_size, float );
		look.m_outline_wave_strength = GET_VARIABLE( g_variables.m_viewmodel_stencil_wave_strength, float );
		look.m_outline_wave_angle    = GET_VARIABLE( g_variables.m_viewmodel_stencil_wave_angle, float );

		g_player_stencil.submit_captured( look, frame );
	}

	if ( g_ctx.m_local && pipeline_ok( ) && dropped_wanted( ) && stamp_dropped( ) > 0 ) {
		look_t look{ };
		look.m_glow            = true;
		look.m_glow_inner[ 0 ] = GET_VARIABLE( g_variables.m_dropped_weapons_glow_vis_color, c_color );
		look.m_glow_inner[ 1 ] = GET_VARIABLE( g_variables.m_dropped_weapons_glow_invis_color, c_color );

		const fill_t fills[] = { { D3DCMP_EQUAL, k_ref_dropped_seen, D3DCOLORWRITEENABLE_RED, false, k_ref_bits },
			                     { D3DCMP_EQUAL, k_ref_dropped_hidden, D3DCOLORWRITEENABLE_GREEN, false, k_ref_bits } };

		g_player_stencil.submit( look, fills, 2, k_clear );
	}
}
