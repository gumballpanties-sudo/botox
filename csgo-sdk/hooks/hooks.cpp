/* precompiled header: must stay the FIRST line, /Yu skips everything above it */
#include "botox.h"

#include "hooks.h"

#include "../hacks/aimbot/aimbot.h"
#include "../hacks/avatar_cache/avatar_cache.h"
#include "../hacks/chams/chams.h"
#include "../hacks/indicators/indicators.h"
#include "../hacks/lagcomp/lagcomp.h"
#include "../hacks/mc_hud/mc_hud.h"
#include "../hacks/misc/misc.h"
#include "../hacks/misc/scaleform/moi_hud.h"
#include "../hacks/misc/scaleform/scaleform.h"
#include "../hacks/movement/movement.h"
#include "../hacks/skins/skins.h"
#include "../hacks/visuals/bullets/bullets.h"
#include "../hacks/visuals/edicts/edicts.h"
#include "../hacks/visuals/screen/ambient_occlusion.h"
#include "../hacks/visuals/screen/bloom.h"
#include "../hacks/visuals/screen/color_correction.h"
#include "../hacks/visuals/screen/depth_of_field.h"
#include "../hacks/visuals/screen/depth_source.h"
#include "../hacks/visuals/screen/depth_view.h"
#include "../hacks/visuals/screen/flip_world.h"
#include "../hacks/visuals/screen/mc_clouds.h"
#include "../hacks/visuals/screen/player_stencil.h"
#include "../hacks/visuals/screen/reflections.h"
#include "../hacks/visuals/screen/render_queue.h"
#include "../hacks/visuals/screen/resolution_spoof.h"
#include "../hacks/visuals/screen/screen.h"
#include "../hacks/visuals/screen/serial_render.h"
#include "../hacks/visuals/screen/true_motion_blur.h"
#include "../hacks/visuals/screen/fx_compat.h"
#include "../hacks/visuals/screen/gpu_timer.h"
#include "../hacks/visuals/weapon_sheen.h"
#include "../hacks/web/websurface.h"
#include "../utilities/memory/relative.h"
#include "../utilities/memory/virtual.h"

#include <vector>

/* g_virtual.get( nullptr, n ) derefs null: an unresolved interface = skipped hook + log, not a crash */
static void* safe_vfunc( void* instance, const unsigned int index )
{
	if ( !instance )
		return nullptr;

	return g_virtual.get( instance, index );
}

bool n_hooks::impl_t::on_attach( )
{
	/* CREATE EVERYTHING FIRST, ENABLE LAST: one MH_EnableHook( MH_ALL_HOOKS ) at the bottom.
	   per-hook enable ran the game half hooked mid init (crashes) and froze threads per hook */
	std::vector< c_detour_hook* > created = { };
	created.reserve( 48 );

	const auto initialise_hook = [ &created ]( c_detour_hook& detour_class, void* function, void* detour, const char* hook_name ) {
		if ( !function ) {
			g_console.print< n_console::log_level::WARNING >(
				std::vformat( "no address for {:s}, hook skipped", std::make_format_args( hook_name ) ).c_str( ) );
			return false;
		}

		if ( !detour_class.create( function, detour, false ) ) {
			g_console.print< n_console::log_level::WARNING >(
				std::vformat( "failed to hook {:s} @ {:p}", std::make_format_args( hook_name, function ) ).c_str( ) );
			return false;
		}

		created.push_back( &detour_class );

		g_console.print( std::vformat( "{:s} hooked @ {:p}", std::make_format_args( hook_name, function ) ).c_str( ) );
		return true;
	};

	if ( MH_Initialize( ) != MH_OK ) {
		g_console.print< n_console::log_level::WARNING >( "failed to initialise minhook" );
		return false;
	}

	initialise_hook( m_tier0_warning, g_modules[ TIER0_DLL ].find_export( HASH_BT( "Warning" ) ), &n_detoured_functions::tier0_warning,
	                 "tier0::Warning()" );

	initialise_hook( m_alloc_key_values_memory, safe_vfunc( g_interfaces.m_key_values_system, 2 ), &n_detoured_functions::alloc_key_values_memory,
	                 "IKeyValuesSystem::AllocKeyValuesMemory()" );

	initialise_hook( m_create_move_proxy, safe_vfunc( g_interfaces.m_base_client, 22 ), &n_detoured_functions::create_move_proxy,
	                 "CHLClient::CreateMove()" );

	initialise_hook( m_run_command, safe_vfunc( g_interfaces.m_prediction, 19 ), &n_detoured_functions::run_command, "IPrediction::RunCommand()" );

	initialise_hook( m_get_local_view_angles, safe_vfunc( g_interfaces.m_prediction, 12 ), &n_detoured_functions::get_local_view_angles,
	                 "IPrediction::GetLocalViewAngles()" );

	initialise_hook( m_emit_sound, safe_vfunc( g_interfaces.m_engine_sound, 5 ), &n_detoured_functions::emit_sound, "IEngineSound::EmitSound()" );

	initialise_hook( m_frame_stage_notify, safe_vfunc( g_interfaces.m_base_client, 37 ), &n_detoured_functions::frame_stage_notify,
	                 "CHLClient::FrameStageNotify()" );

	initialise_hook( m_dispatch_user_message, safe_vfunc( g_interfaces.m_base_client, 38 ), &n_detoured_functions::dispatch_user_message,
	                 "CHLClient::DispatchUserMessage()" );

	initialise_hook( m_paint_traverse, safe_vfunc( g_interfaces.m_panel, 41 ), &n_detoured_functions::paint_traverse, "IPanel::PaintTraverse()" );

	initialise_hook( m_on_add_entity, g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 51 8B 45 0C 53 56 8B F1 57" ),
	                 &n_detoured_functions::on_add_entity, "IClientEntityList::OnAddEntity()" );

	initialise_hook( m_on_remove_entity, g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 51 8B 45 0C 53 8B D9 56 57 83 F8 FF 75 07" ),
	                 &n_detoured_functions::on_remove_entity, "IClientEntityList::OnRemoveEntity()" );

	initialise_hook( m_level_init_pre_entity, safe_vfunc( g_interfaces.m_base_client, 5 ), &n_detoured_functions::level_init_pre_entity,
	                 "CHLClient::LevelInitPreEntity()" );

	initialise_hook( m_level_shutdown, safe_vfunc( g_interfaces.m_base_client, 7 ), &n_detoured_functions::level_shutdown,
	                 "CHLClient::LevelShutdown()" );

	initialise_hook( m_get_vcollide, safe_vfunc( g_interfaces.m_model_info, 6 ), &n_detoured_functions::get_vcollide,
	                 "CModelInfo::GetVCollide()" );

	initialise_hook( m_find_mdl, safe_vfunc( g_interfaces.m_model_cache, 10 ), &n_detoured_functions::find_mdl,
	                 "CMDLCache::FindMDL()" );

	initialise_hook(
		m_particle_collection_simulate,
		g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 83 E4 F8 83 EC 30 56 57 8B F9 0F 28 E1 8B 0D ? ? ? ? F3 0F 11 64 24 ? 89 7C 24 18 8B 81" ),
		&n_detoured_functions::particle_collection_simulate, "CParticleCollection::Simulate()" );

	initialise_hook( m_process_movement, safe_vfunc( g_interfaces.m_game_movement, 1 ), &n_detoured_functions::process_movement,
	                 "CGameMovement::ProcessMovement()" );

	initialise_hook( m_modify_eye_position, g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 83 E4 F8 83 EC 70 56 57 8B F9 89 7C 24 14" ),
	                 &n_detoured_functions::modify_eye_position, "CBaseAnimating::ModifyEyePos()" );

	initialise_hook( m_override_mouse_input, safe_vfunc( g_interfaces.m_client_mode, 23 ), &n_detoured_functions::override_mouse_input,
	                 "IClientModeShared::OverrideMouseInput()" );

	/* runs right after CalcViewModelView in CViewRender::SetUpView: viewmodel offset lands here */
	initialise_hook( m_override_view, safe_vfunc( g_interfaces.m_client_mode, 18 ), &n_detoured_functions::override_view,
	                 "IClientModeShared::OverrideView()" );

	initialise_hook( m_do_post_screen_space_effects, safe_vfunc( g_interfaces.m_client_mode, 44 ),
	                 &n_detoured_functions::do_post_screen_space_effects, "IClientModeShared::DoPostScreenSpaceEffects()" );

	initialise_hook( m_view_draw_fade, safe_vfunc( g_interfaces.m_render_view, 29 ), &n_detoured_functions::view_draw_fade,
	                 "CRender::ViewDrawFade()" );

	/* 43/44 = Push3DView overloads, 45 jmps IRender+0x64 (ret 0x14, 5 args), checked against engine.dll */
	initialise_hook( m_push_2d_view, safe_vfunc( g_interfaces.m_render_view, 45 ), &n_detoured_functions::push_2d_view,
	                 "IVRenderView::Push2DView()" );

	/* 14 forwards ( ctx, list, flags, float water z ) to IRender+0x24, ret 0x10, checked against engine.dll */
	initialise_hook( m_draw_world_lists, safe_vfunc( g_interfaces.m_render_view, 14 ), &n_detoured_functions::draw_world_lists,
	                 "IVRenderView::DrawWorldLists()" );

	initialise_hook( m_world_to_screen_matrix, safe_vfunc( g_interfaces.m_engine_client, 37 ), &n_detoured_functions::world_to_screen_matrix,
	                 "IVEngineClient::WorldToScreenMatrix()" );

	initialise_hook( m_glow_effect_spectator, g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 83 EC 14 53 8B 5D 0C 56 57 85 DB 74" ),
	                 &n_detoured_functions::glow_effect_spectator, "CCSPlayer::GlowEffectSpectator()" );

	initialise_hook( m_fire_event_intern, safe_vfunc( g_interfaces.m_game_event_manager, 9 ), &n_detoured_functions::fire_event_intern,
	                 "CGameEventManager::FireEventIntern()" );

	initialise_hook( m_level_init_post_entity, safe_vfunc( g_interfaces.m_base_client, 6 ), &n_detoured_functions::level_init_post_entity,
	                 "CHLClient::LevelInitPostEntity()" );

	initialise_hook( m_draw_set_color, safe_vfunc( g_interfaces.m_surface, 15 ), &n_detoured_functions::draw_set_color,
	                 "ISurface::DrawSetColor()" );

	initialise_hook( m_draw_filled_rect, safe_vfunc( g_interfaces.m_surface, 16 ), &n_detoured_functions::draw_filled_rect,
	                 "ISurface::DrawFilledRect()" );

	initialise_hook( m_draw_textured_rect, safe_vfunc( g_interfaces.m_surface, 41 ), &n_detoured_functions::draw_textured_rect,
	                 "ISurface::DrawTexturedRect()" );

	initialise_hook( m_draw_line, safe_vfunc( g_interfaces.m_surface, 19 ), &n_detoured_functions::draw_line, "ISurface::DrawLine()" );

	initialise_hook( m_draw_textured_polygon, safe_vfunc( g_interfaces.m_surface, 106 ), &n_detoured_functions::draw_textured_polygon,
	                 "ISurface::DrawTexturedPolygon()" );

	initialise_hook( m_draw_model_execute, safe_vfunc( g_interfaces.m_model_render, 21 ), &n_detoured_functions::draw_model_execute,
	                 "CModelRender::DrawModelExecute()" );

	initialise_hook( m_set_image_data_r8g8b8a8,
	                 g_modules[ PANORAMA_DLL ].find_pattern( "55 8B EC 83 E4 F8 81 ? ? ? ? ? 53 56 57 8B F9 8B ? ? ? ? ? 8B" ),
	                 &n_detoured_functions::set_image_data_r8g8b8a8, "CImageData::SetImageDataR8G8B8A8()" );

	{
		auto ui_engine = g_interfaces.m_panorama ? g_interfaces.m_panorama->access_ui_engine( ) : nullptr;
		initialise_hook( m_load_file_into_buffer, safe_vfunc( ui_engine ? ui_engine->ui_filesystem( ) : nullptr, 0 ),
		                 &n_detoured_functions::load_file_into_buffer, "CSource2UIFileSystem::LoadFileIntoBuffer()" );
	}

	initialise_hook( m_net_earliertempents, safe_vfunc( g_convars[ HASH_BT( "net_earliertempents" ) ], 13 ),
	                 &n_detoured_functions::net_earliertempents, "net_earliertempents::GetBool()" );

	initialise_hook( m_list_leaves_in_box, g_modules[ ENGINE_DLL ].find_pattern( "55 8B EC 83 EC ? 8B 4D ? 8D 55" ),
	                 &n_detoured_functions::list_leaves_in_box, "CEngineBSPTree::ListLeavesInBox()" );

	if ( const auto draw_view_models_call = g_modules[ CLIENT_DLL ].find_pattern( "E8 ? ? ? ? 8B 43 10 8D 4D 04" ) )
		initialise_hook( m_draw_view_models,
		                 reinterpret_cast< void* >( g_relative.get( reinterpret_cast< unsigned int >( draw_view_models_call + 0x1 ) ) ),
		                 &n_detoured_functions::draw_view_models, "CViewRender::DrawViewModels()" );

	initialise_hook( m_is_hltv, safe_vfunc( g_interfaces.m_engine_client, 93 ), &n_detoured_functions::is_hltv, "CBaseClient::IsHLTV()" );

	initialise_hook( m_is_paused, safe_vfunc( g_interfaces.m_engine_client, 90 ), &n_detoured_functions::is_paused, "CBaseClient::IsPaused()" );

	initialise_hook( m_is_playing_demo, safe_vfunc( g_interfaces.m_engine_client, 82 ), &n_detoured_functions::is_playing_demo,
	                 "CBaseClient::IsPlayingDemo()" );

	initialise_hook( m_push_notice, g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 83 E4 F8 B8 ? ? ? ? E8 ? ? ? ? 53 8B D9 8B 0D" ),
	                 &n_detoured_functions::push_notice, "CHudChat::PushNotice()" );

	initialise_hook( m_set_visuals_data, g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 81 EC ? ? ? ? 53 8B D9 56 57 8B 53 5C" ),
	                 &n_detoured_functions::set_visuals_data, "CCSWeaponVisualsDataProcessor::SetVisualsData()" );

	if ( g_interfaces.m_file_system )
		initialise_hook( m_loose_files_allowed, safe_vfunc( g_interfaces.m_file_system, 128 ), &n_detoured_functions::loose_files_allowed,
		                 "CBaseFileSystem::LooseFilesAllowed()" );

	initialise_hook( m_check_for_pure_server_whitelist, g_modules[ ENGINE_DLL ].find_pattern( "8B 0D ? ? ? ? 56 83 B9 ? ? ? ? ? 7E 6E" ),
	                 &n_detoured_functions::check_for_pure_server_whitelist, "CL_CheckForPureServerWhitelist()" );

	initialise_hook( m_lock_cursor, safe_vfunc( g_interfaces.m_surface, 67 ), &n_detoured_functions::lock_cursor, "ISurface::LockCursor()" );
	initialise_hook( m_reset, safe_vfunc( g_interfaces.m_direct_device, 16 ), &n_detoured_functions::reset, "IDirect3DDevice9::Reset()" );

	initialise_hook( m_create_texture, safe_vfunc( g_interfaces.m_direct_device, 23 ), &n_detoured_functions::create_texture,
	                 "IDirect3DDevice9::CreateTexture()" );

	initialise_hook( m_set_depth_stencil_surface, safe_vfunc( g_interfaces.m_direct_device, 39 ), &n_detoured_functions::set_depth_stencil_surface,
	                 "IDirect3DDevice9::SetDepthStencilSurface()" );
	initialise_hook( m_end_scene, safe_vfunc( g_interfaces.m_direct_device, 42 ), &n_detoured_functions::end_scene,
	                 "IDirect3DDevice9::EndScene()" );

	initialise_hook( m_present, safe_vfunc( g_interfaces.m_direct_device, 17 ), &n_detoured_functions::present,
	                 "IDirect3DDevice9::Present()" );

	initialise_hook( m_draw_indexed_primitive, safe_vfunc( g_interfaces.m_direct_device, 82 ), &n_detoured_functions::draw_indexed_primitive,
	                 "IDirect3DDevice9::DrawIndexedPrimitive()" );

	if ( g_interfaces.m_client_state && g_interfaces.m_engine_client &&
	     ( g_interfaces.m_engine_client->is_in_game( ) || g_interfaces.m_engine_client->is_connected( ) ) )
		g_interfaces.m_client_state->m_delta_tick = -1;

	if ( const auto sv_gravity = g_convars[ HASH_BT( "sv_gravity" ) ] ) {
		g_ctx.gravity_per_tick              = sv_gravity->get_float( ) * g_interfaces.m_global_vars_base->m_interval_per_tick;
		g_ctx.inverse_half_gravity_per_tick = ( -g_ctx.gravity_per_tick ) * 0.5f;
	} else
		g_console.print< n_console::log_level::WARNING >( "sv_gravity not found, gravity per tick left at its default" );

	g_skins.animation_hook( );

	ragdoll_force_hook( );

	g_skins.publish_anim_donor( );

	g_movement.load_pixelsurf_points( );

	const MH_STATUS status = MH_EnableHook( MH_ALL_HOOKS );

	if ( status != MH_OK ) {
		g_console.print< n_console::log_level::WARNING >(
			std::format( "failed to enable hooks, status {:s}", MH_StatusToString( status ) ).c_str( ) );

		MH_RemoveHook( MH_ALL_HOOKS );
		return false;
	}

	for ( c_detour_hook* hook : created )
		hook->mark_hooked( );

	g_console.print( std::format( "{:d} hooks enabled", created.size( ) ).c_str( ) );

	return true;
}

void n_hooks::impl_t::release_render_resources( )
{
	if ( g_ctx.m_render_release_done.exchange( true ) )
		return;

	/* device outlives the module: unreleased targets leak for the session. same drops as reset */
	g_resolution_spoof.on_device_lost( );
	g_color_correction.on_device_lost( );
	g_depth_of_field.on_device_lost( );
	g_depth_view.on_device_lost( );
	g_reflections.on_device_lost( );
	g_ambient_occlusion.on_device_lost( );
	g_true_motion_blur.on_device_lost( );
	g_bloom.on_device_lost( );
	g_player_stencil.on_device_lost( );
	g_depth_source.on_device_lost( );
	g_weapon_sheen.on_device_lost( );
	g_flip_world.on_device_lost( );
	g_mc_clouds.on_device_lost( );
	n_gpu_timer::on_device_lost( );
	n_fx_compat::on_device_lost( );

	g_avatar_cache.release_all( );

	g_indicators.release_textures( );

	kill_effects_release_textures( );

	g_misc.release_spectator_textures( );

	g_mc_hud.release_textures( );

	Web_InvalidateTexture( );

	// imgui device objects + team avatars, on the thread that created them
	g_render.on_release( );
}

void n_hooks::impl_t::on_release( )
{
	/* FIRST: the render thread's call queue holds pointers into this dll (played a frame later).
	   stop feeding it, wait up to 1s for it to drain */
	n_render_queue::stop( );

	for ( int waited = 0; n_render_queue::pending( ) > 0 && waited < 100; ++waited )
		Sleep( 10 );

	// restore recv proxies before teardown, else crash on unload
	g_skins.animation_unhook( );
	ragdoll_force_unhook( );

	g_movement.save_pixelsurf_points( );

	g_screen.on_release( );
	g_flip_world.on_release( );

	g_edicts.sun_angle( true );
	g_edicts.skybox_3d( true );
	g_edicts.fullbright( true );
	g_edicts.skybox( true );

	g_bullets.on_release( );

	g_mc_hud.on_release( );

	g_aimbot.on_level_init( );

	g_serial_render.on_release( );

	g_misc.performance( true );

	g_moi_hud.on_release( );
	g_scaleform.on_release( );

	MH_DisableHook( MH_ALL_HOOKS );
	MH_RemoveHook( MH_ALL_HOOKS );
	MH_Uninitialize( );

	for ( int waited = 0; g_ctx.m_hooks_in_flight.load( std::memory_order_acquire ) > 0 && waited < 1000; ++waited )
		Sleep( 10 );

	Sleep( 100 );

	kill_effects_shutdown( );
	n_route::route_calc_shutdown( );

	g_lagcomp.on_release( );

	g_chams.release( );
	g_weapon_sheen.release_material( );

	Web_Destroy( );

	/* fallback: render thread goes first; minimised (no end_scene) never does */
	this->release_render_resources( );
}
