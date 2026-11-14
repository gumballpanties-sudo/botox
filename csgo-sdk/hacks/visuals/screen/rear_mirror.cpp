#include "rear_mirror.h"

#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "../../../globals/logger/logger.h"
#include "../players/esp_preview.h"
#include "render_queue.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	constexpr const char* k_texture_name = "botox_rear_mirror";
	constexpr unsigned long k_drawn_window_ms = 250;

	c_cconvar* pixel_visibility_var( )
	{
		static c_cconvar* const var = g_convars[ HASH_BT( "r_dopixelvisibility" ) ];
		return var;
	}
}

bool n_rear_mirror::impl_t::begin( void* view_render, const c_view_setup& setup, std::uint8_t* out )
{
	if ( this->drawing( ) || this->m_failed || g_ctx.m_eject_restoring || !GET_VARIABLE( g_variables.m_rear_mirror, bool ) || !view_render )
		return false;

	/* freeze frame pending / shown: the first RenderView would grab ( or draw ) it, the freeze cam would show the mirror */
	const auto* const render = static_cast< const std::uint8_t* >( view_render );
	if ( render[ k_take_freeze_frame ] ||
	     *reinterpret_cast< const float* >( render + k_freeze_frame_until ) > g_interfaces.m_global_vars_base->m_current_time )
		return false;

	/* first RenderView of the frame = the main view, overlay / monitor views nest inside it */
	const int frame = g_interfaces.m_global_vars_base->m_frame_count;
	if ( frame == this->m_frame )
		return false;
	this->m_frame = frame;

	/* box not drawn lately ( tab, eject, loading ): no second scene */
	if ( GetTickCount( ) - this->m_drawn_tick.load( std::memory_order_relaxed ) > k_drawn_window_ms )
		return false;

	c_material_system* const materials = g_interfaces.m_material_system;
	if ( !materials )
		return false;

	if ( !this->m_texture ) {
		/* reinject: the target outlives the module */
		this->m_texture = materials->find_texture( k_texture_name, TEXTURE_GROUP_RENDER_TARGET, false );
		if ( !this->m_texture || this->m_texture->is_error( ) )
			this->m_texture = materials->create_named_render_target_texture_ex(
				k_texture_name, 1, 1, 4 /* RT_SIZE_FULL_FRAME_BUFFER */, materials->get_back_buffer_format( ),
				1 /* MATERIAL_RT_DEPTH_SEPARATE: msaa main depth never pairs with it */, 0x4 | 0x8 /* clamp s t */, 1 /* CREATERENDERTARGETFLAGS_HDR */ );

		if ( !this->m_texture || this->m_texture->is_error( ) ) {
			this->m_texture = nullptr;
			this->m_failed  = true;
			g_logger.print( "render target failed, rear view mirror off\n", "[mirror]" );
			return false;
		}
	}

	const int target_width  = this->m_texture->get_actual_width( );
	const int target_height = this->m_texture->get_actual_height( );
	if ( target_width < 16 || target_height < 16 )
		return false;

	const int width  = std::clamp( this->m_width.load( std::memory_order_relaxed ), 16, target_width );
	const int height = std::clamp( this->m_height.load( std::memory_order_relaxed ), 16, target_height );

	std::memcpy( out, &setup, k_view_size );

	auto* const view = reinterpret_cast< c_view_setup* >( out );
	view->m_x = view->m_unscaled_x = view->m_y = view->m_unscaled_y = 0;
	view->m_width = view->m_unscaled_width = width;
	view->m_height = view->m_unscaled_height = height;
	view->m_aspect_ratio     = static_cast< float >( width ) / static_cast< float >( height );
	view->m_fov              = std::clamp( GET_VARIABLE( g_variables.m_rear_mirror_fov, float ), 10.f, 170.f );
	view->m_angles           = c_angle( 0.f, std::remainder( setup.m_angles.m_y + 180.f, 360.f ), 0.f );
	view->m_motion_blur_mode = motion_blur_disable;

	/* main view's csm culler faces forward; no off centre / subrect, no bloom + auto exposure ( a second luminance sample a frame
	   would drag the main view's exposure ), no dof */
	*reinterpret_cast< void** >( out + 0xAC ) = nullptr;
	out[ 0x148 ] &= static_cast< std::uint8_t >( ~( 0x1 | 0x2 | 0x4 | 0x8 ) );

	c_material_render_context* const context = materials->get_render_context( );
	if ( !context )
		return false;

	context->push_render_target_and_viewport( );
	context->set_render_target( this->m_texture );

	this->m_context = context;
	this->m_next_u  = static_cast< float >( width ) / static_cast< float >( target_width );
	this->m_next_v  = static_cast< float >( height ) / static_cast< float >( target_height );

	/* smoke / underwater overlays step by frametime each RenderView: the main pass steps them once */
	this->m_view_render = static_cast< std::uint8_t* >( view_render );
	std::memcpy( this->m_fades, this->m_view_render + k_overlay_fades, k_overlay_fades_size );

	/* flashbang after-image = one copy of the target in use per flash: the main pass takes it */
	const auto hold_flash = [ this ]( c_base_entity* player, const int slot ) {
		if ( !player || !player->is_player( ) )
			return;

		std::uint8_t* const grabbed = reinterpret_cast< std::uint8_t* >( player ) + k_flash_grabbed;
		if ( grabbed == this->m_flash[ 0 ] )
			return;

		this->m_flash[ slot ]     = grabbed;
		this->m_flash_was[ slot ] = *grabbed;
		*grabbed                  = 1;
	};

	auto* const local = g_interfaces.m_client_entity_list->get< c_base_entity >( g_interfaces.m_engine_client->get_local_player( ) );
	hold_flash( local, 0 );
	if ( local && local->is_player( ) && local->get_observer_mode( ) != 0 )
		hold_flash( reinterpret_cast< c_base_entity* >(
						g_interfaces.m_client_entity_list->get_client_entity_from_handle( local->get_observer_target_handle( ) ) ),
		            1 );

	/* glow sprites / sun: back view occlusion queries share the main view's id and would replace its results */
	if ( c_cconvar* const pixel_visibility = pixel_visibility_var( ) ) {
		this->m_pixel_visibility = pixel_visibility->get_int( );
		if ( this->m_pixel_visibility )
			pixel_visibility->set_value( 0 );
	}

	this->m_drawing.store( true, std::memory_order_relaxed );
	return true;
}

void n_rear_mirror::impl_t::end( )
{
	this->m_drawing.store( false, std::memory_order_relaxed );

	if ( this->m_pixel_visibility > 0 )
		if ( c_cconvar* const pixel_visibility = pixel_visibility_var( ) )
			pixel_visibility->set_value( this->m_pixel_visibility );
	this->m_pixel_visibility = -1;

	for ( int slot = 1; slot >= 0; --slot )
		if ( this->m_flash[ slot ] ) {
			*this->m_flash[ slot ] = this->m_flash_was[ slot ];
			this->m_flash[ slot ]  = nullptr;
		}

	if ( this->m_view_render ) {
		std::memcpy( this->m_view_render + k_overlay_fades, this->m_fades, k_overlay_fades_size );
		this->m_view_render = nullptr;
	}

	if ( !this->m_context )
		return;

	/* in order with the image on the render thread: a resize never shows the old frame at the new uv */
	c_texture* const texture = this->m_texture;
	const float u = this->m_next_u, v = this->m_next_v;
	const auto publish = [ this, texture, u, v ] {
		this->m_u.store( u, std::memory_order_relaxed );
		this->m_v.store( v, std::memory_order_relaxed );
		this->m_shown.store( texture );
	};
	if ( !n_render_queue::submit( publish ) )
		publish( );

	this->m_context->pop_render_target_and_viewport( );
	this->m_context->release( );
	this->m_context = nullptr;
}

void n_rear_mirror::impl_t::draw( )
{
	if ( !GET_VARIABLE( g_variables.m_rear_mirror, bool ) || !g_interfaces.m_engine_client || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	const ImVec2 size( static_cast< float >( std::max( GET_VARIABLE( g_variables.m_rear_mirror_width, int ), 16 ) ),
	                   static_cast< float >( std::max( GET_VARIABLE( g_variables.m_rear_mirror_height, int ), 16 ) ) );
	const float rounding = std::clamp( GET_VARIABLE( g_variables.m_rear_mirror_rounding, float ), 0.f, std::min( size.x, size.y ) * 0.5f );

	ImGui::SetNextWindowPos( ImVec2( ( ImGui::GetIO( ).DisplaySize.x - size.x ) * 0.5f, 16.f ), ImGuiCond_FirstUseEver );
	ImGui::SetNextWindowSize( size, ImGuiCond_Always );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.f, 0.f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.f );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowMinSize, ImVec2( 16.f, 16.f ) );

	int flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
	            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground;
	if ( !g_menu.m_opened )
		flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;

	if ( ImGui::Begin( "botox-rear-mirror", nullptr, flags ) ) {
		ImDrawList* const list = ImGui::GetWindowDrawList( );
		const ImVec2 pos       = ImGui::GetWindowPos( );
		const ImVec2 end( pos.x + size.x, pos.y + size.y );

		const float scale = g_render.m_dpi_panel_scale;
		this->m_width.store( static_cast< int >( size.x * scale + 0.5f ), std::memory_order_relaxed );
		this->m_height.store( static_cast< int >( size.y * scale + 0.5f ), std::memory_order_relaxed );
		this->m_drawn_tick.store( GetTickCount( ), std::memory_order_relaxed );

		if ( IDirect3DTexture9* const texture = n_esp_preview::raw_texture( this->m_shown.load( ) ) ) {
			const float u   = this->m_u.load( std::memory_order_relaxed ), v = this->m_v.load( std::memory_order_relaxed );
			const bool flip = GET_VARIABLE( g_variables.m_rear_mirror_flip, bool );

			/* scene alpha = whatever the shaders wrote ( depth to alpha ): opaque colour, vertex alpha only */
			list->AddCallback( [ ]( const ImDrawList*, const ImDrawCmd* ) {
				g_interfaces.m_direct_device->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2 );
			}, nullptr );
			list->AddImageRounded( texture, pos, end, ImVec2( flip ? u : 0.f, 0.f ), ImVec2( flip ? 0.f : u, v ), IM_COL32_WHITE, rounding );
			list->AddCallback( ImDrawCallback_ResetRenderState, nullptr );
		} else
			list->AddRectFilled( pos, end, IM_COL32( 0, 0, 0, 160 ), rounding );

		list->AddRect( pos, end, IM_COL32( 0, 0, 0, 255 ), rounding );
	}
	ImGui::End( );
	ImGui::PopStyleVar( 3 );
}
