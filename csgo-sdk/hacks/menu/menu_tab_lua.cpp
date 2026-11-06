#include "menu_internal.h"
#include "../lua/lua.h"
#include "../../game/sdk/includes/includes.h"

#include <shellapi.h>

static void draw_element( n_lua::script_t& script, n_lua::element_t& element )
{
	const char* label = element.m_label.c_str( );
	bool changed      = false;

	switch ( element.m_type ) {
	case n_lua::element_checkbox: changed = ImGui::Checkbox( label, &element.m_bool ); break;
	case n_lua::element_slider_int:
		changed = ImGui::SliderInt( label, &element.m_int, element.m_int_min, element.m_int_max, element.m_format.c_str( ), ImGuiSliderFlags_AlwaysClamp );
		break;
	case n_lua::element_slider_float:
		changed = ImGui::SliderFloat( label, &element.m_float, element.m_float_min, element.m_float_max, element.m_format.c_str( ),
		                              ImGuiSliderFlags_AlwaysClamp );
		break;
	case n_lua::element_combo: {
		const auto item_name = []( void* data, const int index, const char** out ) {
			*out = ( *static_cast< std::vector< std::string >* >( data ) )[ index ].c_str( );
			return true;
		};
		changed = ImGui::Combo( label, &element.m_int, item_name, &element.m_items, static_cast< int >( element.m_items.size( ) ) );
		break;
	}
	case n_lua::element_color: changed = ImGui::ColorEdit4( label, element.m_color, color_picker_alpha_flags ); break;
	case n_lua::element_keybind: {
		const key_bind_t before = element.m_bind;
		ImGui::Label( "%s", label );
		ImGui::Keybind( "##lua keybind", &element.m_bind, true );
		changed = before.m_key != element.m_bind.m_key || before.m_key_style != element.m_bind.m_key_style;
		break;
	}
	case n_lua::element_button:
		if ( ImGui::Button( label, ImVec2( -1.f, 15.f ) ) )
			element.m_clicked = true;
		break;
	case n_lua::element_text: ImGui::Text( "%s", label ); break;
	case n_lua::element_input: changed = ImGui::InputText( label, element.m_text, sizeof( element.m_text ) ); break;
	}

	if ( changed )
		script.m_dirty = true;
}

void n_menu::impl_t::lua_page( )
{
	bool open_folder = false;
	/* g_lua.request locks m_mutex itself (non recursive): queue after the lock below is gone */
	std::vector< std::pair< n_lua::e_request, std::string > > requests;

	{
		/* render thread. the game thread never holds this while lua runs, so waiting here is short */
		std::lock_guard< std::mutex > lock( g_lua.m_mutex );
		auto& scripts = g_lua.m_scripts;

		menu_columns_begin( );

		if ( menu_group_begin( "scripts" ) ) {
			static std::vector< std::string > rows;
			rows.clear( );
			for ( const auto& script : scripts )
				rows.push_back( script->m_state ? script->m_name + " (loaded)" : script->m_name );

			this->m_selected_lua = ImClamp( this->m_selected_lua, 0, ImMax( static_cast< int >( rows.size( ) ) - 1, 0 ) );

			ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail( ).x - 2.f );
			ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_FramePadding, ImVec2( 0.f, 3.f ) );
			ImGui::ListBox( "##lua scripts", &this->m_selected_lua, []( const int index ) { return rows[ index ].c_str( ); },
			                static_cast< int >( rows.size( ) ), 12 );
			ImGui::PopStyleVar( );

			n_lua::script_t* selected = rows.empty( ) ? nullptr : scripts[ this->m_selected_lua ].get( );
			const bool loaded         = selected && selected->m_state;

			if ( ImGui::Button( loaded ? "unload" : "load", ImVec2( -1.f, 15.f ) ) && selected )
				requests.emplace_back( loaded ? n_lua::request_unload : n_lua::request_load, selected->m_name );

			if ( ImGui::Button( "reload", ImVec2( -1.f, 15.f ) ) && selected )
				requests.emplace_back( n_lua::request_reload, selected->m_name );

			if ( ImGui::Button( "refresh", ImVec2( -1.f, 15.f ) ) )
				requests.emplace_back( n_lua::request_refresh, std::string( ) );

			if ( ImGui::Button( "open folder", ImVec2( -1.f, 15.f ) ) )
				open_folder = true;

			if ( rows.empty( ) )
				ImGui::TextWrapped( "no scripts, drop .lua files in %s", g_lua.folder( ).string( ).c_str( ) );

			if ( selected && !selected->m_error.empty( ) ) {
				ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.f, 0.35f, 0.35f, 1.f ) );
				ImGui::TextWrapped( "%s", selected->m_error.c_str( ) );
				ImGui::PopStyleColor( );
			}
		}
		menu_group_end( );

		menu_columns_next( );

		bool any_loaded = false;
		for ( auto& script : scripts ) {
			if ( !script->m_state )
				continue;

			any_loaded = true;

			ImGui::PushID( script->m_name.c_str( ) );
			if ( menu_group_begin( script->m_name.c_str( ) ) ) {
				bool any_shown = false;
				for ( std::size_t i = 0; i < script->m_elements.size( ); i++ ) {
					n_lua::element_t& element = script->m_elements[ i ];
					if ( !element.m_visible )
						continue;

					any_shown = true;
					ImGui::PushID( static_cast< int >( i ) );
					draw_element( *script, element );
					ImGui::PopID( );
				}

				if ( !any_shown )
					ImGui::TextDisabled( "no settings" );
			}
			menu_group_end( );
			ImGui::PopID( );
		}

		if ( !any_loaded ) {
			if ( menu_group_begin( "lua settings" ) )
				ImGui::TextDisabled( "load a script to see its settings" );
			menu_group_end( );
		}

		menu_columns_end( );
	}

	for ( const auto& [ type, name ] : requests )
		g_lua.request( type, name );

	if ( open_folder ) {
		std::error_code ec;
		std::filesystem::create_directories( g_lua.folder( ), ec );
		ShellExecuteA( nullptr, "open", g_lua.folder( ).string( ).c_str( ), nullptr, nullptr, SW_SHOWNORMAL );
	}
}
