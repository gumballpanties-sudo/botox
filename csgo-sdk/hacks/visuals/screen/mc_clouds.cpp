#include "mc_clouds.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "mc_clouds_draw.h"
#include "render_queue.h"

#include <cstring>
#include <format>

void n_mc_clouds::impl_t::on_override_view( const c_view_setup* setup )
{
	if ( !setup )
		return;

	this->m_origin[ 0 ] = setup->m_origin.m_x;
	this->m_origin[ 1 ] = setup->m_origin.m_y;
}

void n_mc_clouds::impl_t::on_draw_world_lists( const unsigned long flags )
{
	constexpr unsigned long k_draw_skybox = 0x10; // DRAWWORLDLISTS_DRAW_SKYBOX

	if ( !( flags & k_draw_skybox ) || this->m_failed || GET_VARIABLE( g_variables.m_skybox, int ) != e_skybox_type::skybox_minecraft )
		return;

	static c_cconvar* sky_name = g_convars[ HASH_BT( "sv_skyname" ) ];
	const char* live           = sky_name ? sky_name->get_string( ) : nullptr;

	if ( !live || std::strcmp( live, n_mc_sky::k_name ) != 0 )
		return;

	frame_t frame{ };
	offset( g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_current_time : 0.0, this->m_origin[ 0 ], this->m_origin[ 1 ],
	        frame.m_offset );

	// queued right behind this view's sky + world draws: raw d3d from here would race the render thread
	const auto pass = [ this, frame ] { n_fx_compat::run( g_interfaces.m_direct_device, "mc_clouds", [ & ] { this->execute( frame ); } ); };

	if ( n_render_queue::submit( pass ) )
		return;

	pass( );
}

void n_mc_clouds::impl_t::on_device_lost( )
{
	if ( this->m_cells )
		this->m_cells->Release( );

	if ( this->m_shader )
		this->m_shader->Release( );

	this->m_cells  = nullptr;
	this->m_shader = nullptr;
}

bool n_mc_clouds::impl_t::ensure( IDirect3DDevice9* device )
{
	if ( this->m_shader && this->m_cells )
		return true;

	if ( this->m_failed )
		return false;

	char error[ 512 ]{ };

	if ( !this->m_shader )
		this->m_shader = build_shader( device, error, sizeof( error ) );

	if ( this->m_shader && !this->m_cells )
		this->m_cells = build_cells( device );

	if ( this->m_shader && this->m_cells )
		return true;

	this->on_device_lost( );
	this->m_failed = true;

	g_console.print< n_console::log_level::WARNING >( std::format( "mc clouds off: {}", error[ 0 ] ? error : "cell texture failed" ).c_str( ) );
	return false;
}

void n_mc_clouds::impl_t::execute( const frame_t& frame )
{
	IDirect3DDevice9* device = g_interfaces.m_direct_device;

	if ( device && this->ensure( device ) )
		draw( device, this->m_shader, this->m_cells, frame.m_offset );
}
