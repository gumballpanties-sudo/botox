#include "esp_preview.h"

#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "../../../globals/logger/logger.h"
#include "../../chams/chams.h"
#include "../../lagcomp/lagcomp.h"
#include "../screen/player_stencil.h"
#include "players.h"
#include "../../../utilities/perf/perf_watch.h"

#include <cstdlib>
#include <cstring>
#include <new>

/* csgo legacy client.dll CMergedMDL ( tier3/mdlutils.h ), checked by disasm of the ctor + vtable: root CMDL +4, m_MDLToWorld +0x6A0,
   merge CUtlVector< MDLData_t > +0x6D4 ( element 0x6D0, size +0xC ), pose params +0x6E8, layers +0x748, size 0x75C. vtable 0 = Draw( matrix ),
   3 = scalar deleting dtor, 4 = SetMDL( name ) ( 5 = handle, reversed ). CMDL: handle +0x3C, Color +0x46, sequence +0x54, time +0x60,
   use seq fps +0x675, time basis +0x67C */
namespace
{
	constexpr int k_model_size = 0x75C;
	constexpr int k_root = 0x4, k_root_to_world = 0x6A0, k_merge = 0x6D4, k_mdl_data = 0x6D0, k_pose = 0x6E8, k_layers = 0x748;
	constexpr int k_handle = 0x3C, k_color = 0x46, k_sequence = 0x54, k_time = 0x60, k_use_fps = 0x675, k_time_basis = 0x67C;

	constexpr const char* k_animset = "models/player/custom_player/uiplayer/animset_uiplayer.mdl";
	constexpr const char* k_agent   = "models/player/custom_player/legacy/tm_phoenix.mdl";
	constexpr const char* k_weapon  = "models/weapons/w_rif_ak47.mdl";
	/* t_loadout_rifle02_idle: animset_uiplayer virtual sequence list ( depth first over includes, dedup by name ), read off pak01 */
	constexpr int k_idle_sequence = 210;

	constexpr float k_run_speed = 215.f; /* ak carrier max speed: ghosts trail right like a target running sideways */
	constexpr float k_rt_scale  = 2.f;
	constexpr float k_fov       = 40.f;
	constexpr const char* k_texture_name = "botox_esp_preview";

	using ctor_t         = void( __thiscall* )( void* );
	using set_merge_t    = unsigned short( __thiscall* )( void*, const char*, void*, void*, bool );
	using attach_t       = void( __thiscall* )( void* );
	using set_up_bones_t = void( __thiscall* )( void*, const matrix3x4_t*, int, matrix3x4_t*, const float*, void*, int );
	using destroy_t      = void( __thiscall* )( void*, unsigned int );

	ctor_t s_ctor                 = nullptr;
	set_merge_t s_set_merge       = nullptr;
	attach_t s_attach             = nullptr;
	set_up_bones_t s_set_up_bones = nullptr;

	alignas( 16 ) matrix3x4_t s_root_bones[ 256 ];
	alignas( 16 ) matrix3x4_t s_bones[ 128 ];
	alignas( 16 ) matrix3x4_t s_oldest[ 128 ];

	template< typename t >
	t& at( std::uint8_t* base, const int offset )
	{
		return *reinterpret_cast< t* >( base + offset );
	}

	std::uint8_t* merge_mdl( std::uint8_t* model, const int index )
	{
		return at< std::uint8_t* >( model, k_merge ) + index * k_mdl_data;
	}

	matrix3x4_t root_matrix( const float yaw, const float shift )
	{
		matrix3x4_t matrix{ };
		g_math.angle_matrix( c_angle( 0.f, yaw, 0.f ), matrix );
		matrix.set_origin( c_vector( 0.f, -shift, 0.f ) );
		return matrix;
	}

	/* the live stream: one record per tick over the backtrack window ( lagcomp time_limit minus fake latency extend ), a tick of run apart */
	struct stream_t {
		int m_records;
		float m_step;

		float shift_of( const int record ) const
		{
			return static_cast< float >( record + 1 ) * this->m_step;
		}
	};

	stream_t record_stream( )
	{
		const float interval = g_interfaces.m_global_vars_base->m_interval_per_tick;
		const float tick     = g_ctx.m_max_allocations > 0 && interval > 0.f ? interval : 1.f / 64.f;
		const float window   = std::clamp( static_cast< float >( GET_VARIABLE( g_variables.m_backtrack_time_limit, int ) ) / 1000.f, 0.f,
		                                   n_lagcomp::impl_t::k_max_backtrack );

		return { std::clamp( static_cast< int >( window / tick + 0.5f ), 2, n_lagcomp::impl_t::max_records ), k_run_speed * tick };
	}

	/* SEH frames: no unwinding locals here ( C2712 ) */
	bool seh_call( void ( *fn )( void* ), void* context )
	{
		__try {
			fn( context );
			return true;
		} __except ( EXCEPTION_EXECUTE_HANDLER ) {
			return false;
		}
	}

	/* one mdl load per frame ( each is a synchronous vpk read + material load ) */
	void build_root( void* context )
	{
		auto* const model = static_cast< std::uint8_t* >( context );

		s_ctor( model );
		g_virtual.call< void >( model, 4, k_animset, static_cast< void* >( nullptr ), static_cast< void* >( nullptr ) );
	}

	void build_agent( void* context )
	{
		s_set_merge( context, k_agent, nullptr, nullptr, false );
	}

	void build_weapon( void* context )
	{
		s_set_merge( context, k_weapon, nullptr, nullptr, false );
	}

	void build_finish( void* context )
	{
		auto* const model = static_cast< std::uint8_t* >( context );

		at< int >( model, k_root + k_sequence )     = k_idle_sequence;
		at< bool >( model, k_root + k_use_fps )     = true;
		at< float >( model, k_root + k_time_basis ) = at< float >( model, k_root + k_time );

		s_attach( model );
	}

	void destroy_model( void* context )
	{
		g_virtual.call< void >( context, 3, 0u );
	}

	struct bones_job_t {
		std::uint8_t* m_model;
		const matrix3x4_t* m_to_world;
		int m_count;
	};

	void set_up_bones( void* context )
	{
		const auto* const job = static_cast< bones_job_t* >( context );

		s_set_up_bones( job->m_model + k_root, job->m_to_world, job->m_count, s_root_bones, &at< float >( job->m_model, k_pose ),
		                at< void* >( job->m_model, k_layers ), at< int >( job->m_model, k_layers + 0xC ) );
	}

	IDirect3DTexture9* raw_texture( c_texture* texture )
	{
		if ( !texture )
			return nullptr;

		/* CTexture::m_pTextureHandles +0x50 ( GetTextureHandle reads it ), handle = shaderapi Texture_t*, its d3d texture +0xC */
		std::uint8_t** const handles = *reinterpret_cast< std::uint8_t*** >( reinterpret_cast< std::uint8_t* >( texture ) + 0x50 );
		return handles && handles[ 0 ] ? *reinterpret_cast< IDirect3DTexture9** >( handles[ 0 ] + 0xC ) : nullptr;
	}

	studiohdr_t* studio_of( std::uint8_t* cmdl )
	{
		const unsigned short handle = at< unsigned short >( cmdl, k_handle );
		return handle != 0xFFFF ? g_interfaces.m_model_cache->get_studio_hdr( handle ) : nullptr;
	}
}

/* one step per call ( paint, <= 60 a second ): sig scans, target, each mdl, finish. false until ready */
bool n_esp_preview::impl_t::init( )
{
	if ( this->m_ready )
		return true;

	if ( this->m_failed )
		return false;

	const auto fail = [ this ]( const char* text ) {
		g_logger.print( text, "[preview]" );
		this->m_failed = true;

		if ( this->m_model ) {
			if ( this->m_constructed )
				seh_call( &destroy_model, this->m_model );
			std::free( this->m_model );
			this->m_model = nullptr;
		}
		return false;
	};

	const auto scan = [ ]( const char* signature ) { return g_modules[ CLIENT_DLL ].find_pattern( signature ); };

	switch ( this->m_stage ) {
	case 0:
		s_ctor = reinterpret_cast< ctor_t >( scan( "53 8B D9 56 57 8D 4B 04 C7 03 ? ? ? ? E8 ? ? ? ? 6A" ) );
		break;
	case 1:
		s_set_merge = reinterpret_cast< set_merge_t >( scan( "55 8B EC 57 8B F9 8B 0D ? ? ? ? 85 C9 75" ) );
		break;
	case 2:
		s_attach = reinterpret_cast< attach_t >( scan( "55 8B EC 83 EC 14 83 3D ? ? ? ? ? 53" ) );
		break;
	case 3:
		s_set_up_bones = reinterpret_cast< set_up_bones_t >(
			scan( "55 8B EC 83 E4 F0 B8 18 2E 00 00 E8 ? ? ? ? 56 57 8B 3D ? ? ? ? 8B F1 8B CF 89 74 24 18 89 7C 24 30" ) );

		if ( !s_ctor || !s_set_merge || !s_attach || !s_set_up_bones || !g_interfaces.m_model_cache )
			return fail( "CMergedMDL signatures missing, preview off\n" );
		break;
	case 4: {
		c_material_system* const materials = g_interfaces.m_material_system;

		/* reinject: the target outlives the module */
		this->m_texture = materials->find_texture( k_texture_name, TEXTURE_GROUP_RENDER_TARGET, false );
		if ( !this->m_texture || this->m_texture->is_error( ) )
			this->m_texture = materials->create_named_render_target_texture_ex(
				k_texture_name, static_cast< int >( k_width * k_rt_scale ), static_cast< int >( k_height * k_rt_scale ), 0 /* RT_SIZE_NO_CHANGE */,
				materials->get_back_buffer_format( ), 1 /* MATERIAL_RT_DEPTH_SEPARATE: scene depth never cuts it */, 0x4 | 0x8 /* clamp s t */,
				1 /* CREATERENDERTARGETFLAGS_HDR */ );

		if ( !this->m_texture || this->m_texture->is_error( ) )
			return fail( "render target failed, preview off\n" );

		this->m_cubemap = materials->find_texture( "editor/cubemap.hdr", TEXTURE_GROUP_CUBE_MAP, false );
		break;
	}
	case 5:
		this->m_model = static_cast< std::uint8_t* >( std::calloc( 1, k_model_size ) );
		if ( !this->m_model )
			return fail( "model alloc failed, preview off\n" );

		this->m_constructed = seh_call( &build_root, this->m_model );
		if ( !this->m_constructed )
			return fail( "model build faulted, preview off\n" );
		break;
	case 6:
		if ( !seh_call( &build_agent, this->m_model ) )
			return fail( "model build faulted, preview off\n" );
		break;
	case 7:
		if ( !seh_call( &build_weapon, this->m_model ) )
			return fail( "model build faulted, preview off\n" );
		break;
	default: {
		if ( !seh_call( &build_finish, this->m_model ) )
			return fail( "model build faulted, preview off\n" );

		studiohdr_t* const root  = studio_of( this->m_model + k_root );
		studiohdr_t* const agent = at< int >( this->m_model, k_merge + 0xC ) >= 1 ? studio_of( merge_mdl( this->m_model, 0 ) ) : nullptr;

		if ( !root || !agent || root->n_bones <= 0 || root->n_bones > 256 || agent->n_bones <= 0 )
			return fail( "models missing, preview off\n" );

		/* bonemerge by name ( SetupBonesWithBoneMerge ): agent bone = animset bone of the same name */
		this->m_agent_bones = agent->n_bones < 128 ? agent->n_bones : 128;
		for ( int i = 0; i < this->m_agent_bones; i++ ) {
			this->m_bone_map[ i ] = -1;

			mstudiobone_t* const bone = agent->get_bone( i );
			const char* const name    = bone ? bone->get_name( ) : nullptr;
			if ( !name )
				continue;

			for ( int j = 0; j < root->n_bones; j++ ) {
				mstudiobone_t* const other   = root->get_bone( j );
				const char* const other_name = other ? other->get_name( ) : nullptr;

				if ( other_name && !_stricmp( name, other_name ) ) {
					this->m_bone_map[ i ] = j;
					break;
				}
			}
		}

		this->m_ready = true;
		return true;
	}
	}

	this->m_stage++;
	return false;
}

void n_esp_preview::impl_t::on_eject( )
{
	this->m_shown_texture = nullptr;
	this->m_ready         = false;

	if ( !this->m_model )
		return;

	if ( this->m_constructed )
		seh_call( &destroy_model, this->m_model );
	std::free( this->m_model );
	this->m_model = nullptr;
}

void n_esp_preview::impl_t::on_paint_traverse( )
{
	const int frame        = this->m_frame.load( );
	const bool wanted      = frame != this->m_seen_frame;
	this->m_seen_frame     = frame;

	if ( !wanted || g_ctx.m_eject_restoring || !g_interfaces.m_material_system || !g_interfaces.m_render_view )
		return;

	/* the target keeps the last frame: 60 renders a second whatever the game fps */
	const float now = g_interfaces.m_global_vars_base->m_real_time;
	if ( now >= this->m_last_render && now - this->m_last_render < 1.f / 60.f )
		return;

	this->m_last_render = now;

	PERF_ZONE( zone_paint_preview );

	if ( this->init( ) )
		this->render( );
}

namespace
{
	struct draw_job_t {
		std::uint8_t* m_model;
		float m_yaw;
		stream_t m_stream;
		c_material_render_context* m_context;
		bool m_stencil;
	};

	void draw_model( std::uint8_t* model, const float yaw, const float shift )
	{
		/* CMDL::Draw stamps its own m_Color over the studio modulation: carry chams "original" tint + blend into it */
		float color[ 3 ] = { 1.f, 1.f, 1.f };
		g_interfaces.m_render_view->get_color_modulation( color );
		const float blend = g_interfaces.m_render_view->get_blend( );

		const auto byte = []( const float v ) { return static_cast< std::uint8_t >( std::clamp( v, 0.f, 1.f ) * 255.f + 0.5f ); };
		const std::uint8_t rgba[ 4 ] = { byte( color[ 0 ] ), byte( color[ 1 ] ), byte( color[ 2 ] ), byte( blend ) };

		std::memcpy( model + k_root + k_color, rgba, sizeof( rgba ) );
		for ( int i = 0; i < at< int >( model, k_merge + 0xC ); i++ )
			std::memcpy( merge_mdl( model, i ) + k_color, rgba, sizeof( rgba ) );

		const matrix3x4_t matrix = root_matrix( yaw, shift );
		g_virtual.call< void >( model, 0, &matrix );
	}

	/* live pass order ( draw_model_execute ) through g_chams.draw_player; backtrack = fake records thinned like the live ones */
	void draw_passes( void* context )
	{
		const auto* const job = static_cast< draw_job_t* >( context );

		const auto draw_live = [ & ]( ) { draw_model( job->m_model, job->m_yaw, 0.f ); };

		/* every pass of the live model stamps 2 ( = engine glow ref for a visible player ): glow reads it, ghosts stay off it */
		const auto stencil = [ & ]( const e_shader_stencil_func func, const e_shader_stencil_op pass ) {
			shader_stencil_state_t state{ };
			state.m_enable          = true;
			state.m_pass_op         = pass;
			state.m_func            = func;
			state.m_reference_value = 2;
			job->m_context->set_stencil_state( state );
		};

		stencil( shader_stencilfunc_always, shader_stencilop_set_to_reference );

		if ( !GET_VARIABLE( g_variables.m_chams_enable, bool ) ) {
			draw_live( );
			job->m_context->set_stencil_state( shader_stencil_state_t{ } );
			return;
		}

		const int records = job->m_stream.m_records;

		const auto draw_backtrack_records = [ & ]( ) {
			const auto ghost = [ & ]( const int record, const float alpha ) {
				g_chams.draw_backtrack_ghost( [ & ]( ) { draw_model( job->m_model, job->m_yaw, job->m_stream.shift_of( record ) ); }, alpha );
			};

			if ( GET_VARIABLE( g_variables.m_chams_backtrack_type, int ) == 1 )
				n_chams::impl_t::for_each_ghost( records, n_lagcomp::impl_t::max_records, ghost );
			else
				ghost( records - 1, 1.f );
		};

		/* a player that just made a sound: sound layers at full alpha */
		const bool sound = n_chams::impl_t::has_layers( GET_VARIABLE( g_variables.m_chams_sound_layers, std::vector< int > ) );

		/* ghosts after the live passes, only off its stamp: always behind the model, even a translucent / xqz one */
		g_chams.draw_player( draw_live, [ ]( ) { }, false, 1.f, sound ? 1.f : 0.f );

		if ( n_chams::impl_t::has_layers( GET_VARIABLE( g_variables.m_chams_backtrack_layers, std::vector< int > ) ) ) {
			stencil( shader_stencilfunc_notequal, shader_stencilop_keep );
			draw_backtrack_records( );
		}

		job->m_context->set_stencil_state( shader_stencil_state_t{ } );
	}
}

void n_esp_preview::impl_t::render( )
{
	std::uint8_t* const model = this->m_model;
	const float yaw           = this->m_yaw.load( );

	at< float >( model, k_root + k_time ) = g_interfaces.m_global_vars_base->m_real_time;

	const stream_t stream = record_stream( );

	/* engine CViewSetup is longer than c_view_setup: zeroed tail = no ortho / custom matrices / bloom */
	alignas( 16 ) std::uint8_t view_buffer[ 0x400 ]{ };
	c_view_setup* const view = new ( view_buffer ) c_view_setup( );

	const int width  = static_cast< int >( k_width * k_rt_scale );
	const int height = static_cast< int >( k_height * k_rt_scale );

	view->m_width = view->m_unscaled_width = width;
	view->m_height = view->m_unscaled_height = height;
	view->m_fov          = k_fov;
	view->m_angles       = c_angle( 0.f, 0.f, 0.f );
	view->m_znear        = 7.f;
	view->m_zfar         = 1000.f;
	view->m_aspect_ratio = static_cast< float >( width ) / static_cast< float >( height );

	view_matrix_t world_to_view{ }, view_to_projection{ }, world_to_projection{ }, world_to_pixels{ };

	/* fit from the engine's own projection scale ( half height = distance / scale ): model at 68 % height, centred whatever is on,
	   ghosts run off the right edge */
	g_interfaces.m_render_view->get_matrices_for_view( view, &world_to_view, &view_to_projection, &world_to_projection, &world_to_pixels );
	const float distance = 36.f / 0.68f * std::fabs( view_to_projection[ 1 ][ 1 ] );

	view->m_origin = c_vector( -distance, 0.f, 38.f );

	g_interfaces.m_render_view->get_matrices_for_view( view, &world_to_view, &view_to_projection, &world_to_projection, &world_to_pixels );

	/* agent bones ( bonemerge by name off the animset ): esp, inner glow capsules */
	const matrix3x4_t to_world = root_matrix( yaw, 0.f );
	studiohdr_t* const root    = studio_of( model + k_root );
	studiohdr_t* const agent   = studio_of( merge_mdl( model, 0 ) );
	bones_job_t bones_job{ model, &to_world, root ? root->n_bones : 0 };

	const bool bones = agent && bones_job.m_count > 0 && seh_call( &set_up_bones, &bones_job );

	if ( bones ) {
		for ( int i = 0; i < this->m_agent_bones; i++ ) {
			const int parent = agent->get_bone( i ) ? agent->get_bone( i )->m_parent : -1;

			s_bones[ i ] = this->m_bone_map[ i ] >= 0 ? s_root_bones[ this->m_bone_map[ i ] ] : parent >= 0 && parent < i ? s_bones[ parent ] : to_world;
		}
	}

	c_material_render_context* const context = g_interfaces.m_material_system->get_render_context( );
	if ( !context )
		return;

	alignas( 16 ) std::uint8_t frustum[ 6 * 16 ]{ };

	context->push_render_target_and_viewport( );
	context->set_render_target( this->m_texture );
	if ( this->m_cubemap )
		context->bind_local_cubemap( this->m_cubemap );
	context->set_lighting_origin( 0.f, 0.f, 40.f );

	g_interfaces.m_render_view->push_3d_view( context, view, 0x1 | 0x2 | 0x20 /* clear color, depth, stencil */, this->m_texture, frustum );

	/* in game the map's auto exposure scales every shader output ( white chams came out grey ): neutral 1 here, put back after */
	float tone_map[ 3 ] = { 1.f, 1.f, 1.f };
	context->get_tone_mapping_scale_linear( tone_map );
	const float neutral[ 3 ] = { 1.f, 1.f, 1.f };
	context->set_tone_mapping_scale_linear( neutral );

	const unsigned int clear = this->m_clear.load( );
	context->clear_color_4ub( clear & 0xFF, ( clear >> 8 ) & 0xFF, ( clear >> 16 ) & 0xFF, 255 );
	context->clear_buffers( true, true, true );

	const c_vector_4d cube[ 6 ] = { { 0.4f, 0.4f, 0.4f, 1.f }, { 0.4f, 0.4f, 0.4f, 1.f }, { 0.4f, 0.4f, 0.4f, 1.f },
		                            { 0.4f, 0.4f, 0.4f, 1.f }, { 0.4f, 0.4f, 0.4f, 1.f }, { 0.4f, 0.4f, 0.4f, 1.f } };
	const c_vector ambient[ 6 ]   = { { 0.4f, 0.4f, 0.4f }, { 0.4f, 0.4f, 0.4f }, { 0.4f, 0.4f, 0.4f },
		                              { 0.4f, 0.4f, 0.4f }, { 0.4f, 0.4f, 0.4f }, { 0.4f, 0.4f, 0.4f } };

	context->set_ambient_light_cube( cube );
	g_interfaces.m_studio_render->set_ambient_light_colors( ambient );
	g_interfaces.m_studio_render->set_local_lights( 0, nullptr );
	g_interfaces.m_model_render->suppress_engine_lighting( true );

	draw_job_t draw_job{ model, yaw, stream, context, g_preview_stencil.wanted( ) };
	const bool drawn = seh_call( &draw_passes, &draw_job );

	/* outline / glow / inner glow: the live pass on this target ( its own stencil now holds the model ), sizes in body px */
	if ( drawn && draw_job.m_stencil )
		g_preview_stencil.after_glow( world_to_projection, bones ? n_players::hitbox_capsules( agent, 0, s_bones, this->m_agent_bones )
		                                                         : std::vector< n_players::hitbox_capsule_t >{ },
		                              k_rt_scale );

	g_interfaces.m_model_render->suppress_engine_lighting( false );
	g_interfaces.m_model_render->forced_material_override( nullptr );
	n_chams::impl_t::reset_original_color( );

	context->set_tone_mapping_scale_linear( tone_map );

	g_interfaces.m_render_view->pop_view( context, frustum );

	if ( this->m_cubemap )
		context->bind_local_cubemap( nullptr );

	context->pop_render_target_and_viewport( );
	context->release( );

	if ( !drawn ) {
		g_logger.print( "model draw faulted, preview off\n", "[preview]" );
		this->m_failed = true;
		this->m_ready  = false;
		this->m_model  = nullptr; /* leaked on purpose: its state is unknown */
		this->m_shown_texture = nullptr;
		return;
	}

	this->m_shown_texture = this->m_texture;

	/* 2d esp: same draw_player as the live loop, projected through the preview camera onto the k_width x k_height body */
	this->m_draw_data.clear( );

	[ & ]( ) {
		if ( !GET_VARIABLE( g_variables.m_players, bool ) )
			return;

		if ( !bones )
			return;

		const n_players::projector_t w2s{ &world_to_projection, k_width, k_height };

		/* live gates: distance cut + fade on a target 600 u out */
		constexpr float distance = 600.f;
		const float max_distance = GET_VARIABLE( g_variables.m_players_max_distance, float );

		float alpha = 1.f;
		if ( max_distance > 0.f && distance > max_distance )
			return;
		if ( GET_VARIABLE( g_variables.m_players_distance_fade, bool ) && max_distance > 0.f && distance > max_distance * 0.75f )
			alpha = 1.f - ( distance - max_distance * 0.75f ) / ( max_distance * 0.25f );

		n_players::esp_player_t player{ };
		player.m_alpha = alpha;

		/* c_base_entity::get_bounding_box on the standing hull */
		player.m_has_obb  = true;
		player.m_obb_mins = c_vector( -16.f, -16.f, 0.f );
		player.m_obb_maxs = c_vector( 16.f, 16.f, 72.f );
		player.m_frame    = to_world;

		float left = FLT_MAX, top = FLT_MAX, right = -FLT_MAX, bottom = -FLT_MAX;
		for ( int i = 0; i < 8; i++ ) {
			const c_vector point( ( i & 1 ) ? player.m_obb_maxs.m_x : player.m_obb_mins.m_x, ( i & 2 ) ? player.m_obb_maxs.m_y : player.m_obb_mins.m_y,
			                      ( i & 4 ) ? player.m_obb_maxs.m_z : player.m_obb_mins.m_z );

			c_vector_2d screen{ };
			if ( !w2s( g_math.vector_transform( point, to_world ), screen ) )
				return;

			left   = std::min( left, screen.m_x );
			top    = std::min( top, screen.m_y );
			right  = std::max( right, screen.m_x );
			bottom = std::max( bottom, screen.m_y );
		}

		player.m_box.m_left   = static_cast< int >( left );
		player.m_box.m_top    = static_cast< int >( top );
		player.m_box.m_right  = static_cast< int >( right );
		player.m_box.m_bottom = static_cast< int >( bottom );
		player.m_box.m_width  = player.m_box.m_right - player.m_box.m_left;
		player.m_box.m_height = player.m_box.m_bottom - player.m_box.m_top;

		const auto& enabled_flags = GET_VARIABLE( g_variables.m_player_flags, std::vector< bool > );
		const bool immunity       = GET_VARIABLE( g_variables.m_players_flags, bool ) &&
		                      e_player_flags::player_flag_immunity < static_cast< int >( enabled_flags.size( ) ) &&
		                      enabled_flags[ e_player_flags::player_flag_immunity ];

		player.m_name          = "botox";
		player.m_health        = 76;
		player.m_health_factor = 0.76f;
		player.m_visible       = true;
		player.m_god           = immunity; /* "(GOD)" only shows next to the immune flag that explains it */
		player.m_distance      = distance;
		player.m_avatar        = g_render.m_terrorist_avatar;

		player.m_item_definition_index = e_item_definition_index::weapon_ak47;
		player.m_weapon_data           = g_interfaces.m_weapon_system->get_weapon_data( player.m_item_definition_index );
		player.m_ammo_factor           = 23.f / 30.f;

		player.m_money = 16000;
		for ( bool& flag : player.m_flags )
			flag = true;
		player.m_flags[ e_player_flags::player_flag_immunity ] = immunity;

		player.m_studio = agent;
		player.m_bones  = s_bones;

		for ( int i = 0; i < this->m_agent_bones; i++ ) {
			s_oldest[ i ] = s_bones[ i ];
			s_oldest[ i ][ 1 ][ 3 ] -= stream.shift_of( stream.m_records - 1 );
		}
		player.m_backtrack_bones = s_oldest;

		if ( GET_VARIABLE( g_variables.m_players_hitboxes, bool ) && GET_VARIABLE( g_variables.m_players_hitboxes_mode, int ) == 0 )
			player.m_capsules = n_players::hitbox_capsules( agent, 0, s_bones, this->m_agent_bones );

		const mstudiohitboxset_t* const set = agent->get_hitbox_set( 0 );
		const mstudiobbox_t* const head     = set ? set->get_hitbox( hitbox_head ) : nullptr;

		if ( head && head->m_bone >= 0 && head->m_bone < this->m_agent_bones ) {
			const c_vector head_position = g_math.vector_transform( ( head->m_bb_min + head->m_bb_max ) * 0.5f, s_bones[ head->m_bone ] );

			if ( GET_VARIABLE( g_variables.m_players_backtrack_trail, bool ) )
				for ( int i = 0; i < stream.m_records; i++ )
					player.m_trail.push_back( head_position - c_vector( 0.f, stream.shift_of( i ), 0.f ) );

			player.m_has_head    = true;
			player.m_head        = head_position;
			player.m_head_radius = head->m_radius;
			player.m_view_right  = c_vector( 0.f, -1.f, 0.f ); /* camera looks down +x */
		}

		n_players::draw_player( player, w2s, this->m_draw_data );
	}( );

	/* io.Fonts only changes under the unique font lock, paint holds it shared */
	std::lock_guard< std::mutex > lock( this->m_mutex );
	this->m_shown.swap( this->m_draw_data );
	this->m_shown_atlas = ImGui::GetIO( ).Fonts;
}

void n_esp_preview::impl_t::draw_body( )
{
	this->m_frame.store( ImGui::GetFrameCount( ) );

	ImDrawList* const draw_list = ImGui::GetWindowDrawList( );
	const ImVec2 pos            = ImGui::GetCursorScreenPos( );
	const ImVec2 end( pos.x + k_width, pos.y + k_height );

	ImGui::InvisibleButton( "##esp preview body", ImVec2( k_width, k_height ) );
	if ( ImGui::IsItemActive( ) )
		this->m_yaw.store( this->m_yaw.load( ) + ImGui::GetIO( ).MouseDelta.x * 0.5f );

	const ImU32 background = ImGui::GetColorU32( ImGuiCol_ChildBg ) | IM_COL32_A_MASK;
	this->m_clear.store( background );

	/* group box look ( RenderChildBorder ): child bg + menu outline, window rounding - 2 */
	const float rounding = ImMax( ImGui::GetStyle( ).WindowRounding - 2.f, 0.f );

	draw_list->AddRectFilled( pos, end, background, rounding );

	if ( IDirect3DTexture9* const texture = raw_texture( this->m_shown_texture.load( ) ) )
		draw_list->AddImageRounded( texture, pos, end, ImVec2( 0.f, 0.f ), ImVec2( 1.f, 1.f ), IM_COL32_WHITE, rounding );
	else {
		const char* const text = this->m_failed ? "preview unavailable" : "loading";
		const ImVec2 size      = ImGui::CalcTextSize( text );
		draw_list->AddText( ImVec2( pos.x + ( k_width - size.x ) * 0.5f, pos.y + ( k_height - size.y ) * 0.5f ), ImGui::GetColorU32( ImGuiCol_TextDisabled ),
		                    text );
	}

	{
		std::lock_guard< std::mutex > lock( this->m_mutex );

		if ( this->m_shown_atlas != ImGui::GetIO( ).Fonts )
			this->m_shown.clear( );

		if ( !this->m_shown.empty( ) ) {
			/* esp is laid out on 0..k_width: emit unclipped ( text culls on the clip rect ), then move it onto the body and clip there */
			draw_list->PushClipRect( ImVec2( -100000.f, -100000.f ), ImVec2( 100000.f, 100000.f ), false );

			const int first_vertex  = draw_list->VtxBuffer.Size;
			const int first_command = draw_list->CmdBuffer.Size - 1;

			g_render.draw_objects( draw_list, this->m_shown );

			for ( int i = first_vertex; i < draw_list->VtxBuffer.Size; i++ ) {
				draw_list->VtxBuffer[ i ].pos.x += pos.x;
				draw_list->VtxBuffer[ i ].pos.y += pos.y;
			}

			for ( int i = first_command; i < draw_list->CmdBuffer.Size; i++ )
				draw_list->CmdBuffer[ i ].ClipRect = ImVec4( pos.x, pos.y, end.x, end.y );

			draw_list->PopClipRect( );
		}
	}

	// last: esp never paints over the edge
	draw_list->AddRect( pos, end, ImGui::GetColorU32( ImGuiCol_MenuOutline ), rounding );
}
