#define _CRT_SECURE_NO_WARNINGS

#include "media_player.h"
#include "audio_bands.h"
#include "kopuz.h"
#include "media_file.h"
#include "../../dependencies/json/json.hpp"
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <appmodel.h>
#include <endpointvolume.h>
#include <d3dx9tex.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <sstream>
#include <thread>
#include <unordered_map>

#pragma comment( lib, "shell32" )
#pragma comment( lib, "ole32" )
#pragma comment( lib, "mmdevapi" )
#pragma comment( lib, "winhttp.lib" )

static_assert( n_audio_bands::k_bands == 64, "m_bands size" );

static std::string to_utf8( const std::wstring_view text )
{
	if ( text.empty( ) )
		return { };

	const int size = WideCharToMultiByte( CP_UTF8, 0, text.data( ), static_cast< int >( text.size( ) ), nullptr, 0, nullptr, nullptr );
	if ( size <= 0 )
		return { };

	std::string out( size, '\0' );
	WideCharToMultiByte( CP_UTF8, 0, text.data( ), static_cast< int >( text.size( ) ), out.data( ), size, nullptr, nullptr );
	return out;
}

namespace
{
	template< typename t >
	void safe_release( t*& pointer )
	{
		if ( pointer ) {
			pointer->Release( );
			pointer = nullptr;
		}
	}

	std::wstring lower( std::wstring text )
	{
		for ( auto& c : text )
			c = static_cast< wchar_t >( std::towlower( c ) );
		return text;
	}

	// "C:\x\Chrome.EXE" / "Spotify.exe" / "MSEdge" -> "chrome" / "spotify" / "msedge"
	std::wstring exe_stem( std::wstring name )
	{
		name = lower( std::move( name ) );
		if ( const auto slash = name.find_last_of( L"\\/" ); slash != std::wstring::npos )
			name.erase( 0, slash + 1 );
		if ( name.size( ) > 4 && name.ends_with( L".exe" ) )
			name.resize( name.size( ) - 4 );
		return name;
	}

	// fix for apple music's desktop app - lyrics don't get properly fetched without this, becaue of an em dash after the artist
	std::wstring strip_album( std::wstring artist )
	{
		const auto dash = artist.find( L'\u2014' );
		if ( dash == std::wstring::npos )
			return artist;

		artist.resize( dash );
		while ( !artist.empty( ) && std::iswspace( artist.back( ) ) )
			artist.pop_back( );

		return artist;
	}

	// a hung app must not freeze the media thread: null on timeout / error
	template< typename op_t >
	auto await_for( const op_t& op ) -> decltype( op.GetResults( ) )
	{
		if ( op.wait_for( std::chrono::seconds( 2 ) ) == winrt::Windows::Foundation::AsyncStatus::Completed )
			return op.GetResults( );

		op.Cancel( );
		return nullptr;
	}

	bool is_playing( const GlobalSystemMediaTransportControlsSession& session )
	{
		try {
			const auto info = session.GetPlaybackInfo( );
			return info && info.PlaybackStatus( ) == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
		} catch ( ... ) {
			return false;
		}
	}

	// GetCurrentSession = what windows last focused, often null or a paused tab while another app plays
	GlobalSystemMediaTransportControlsSession pick_session( const GlobalSystemMediaTransportControlsSessionManager& manager )
	{
		const auto current = manager.GetCurrentSession( );
		if ( current && is_playing( current ) )
			return current;

		const auto sessions = manager.GetSessions( );
		for ( const auto& session : sessions )
			if ( is_playing( session ) )
				return session;

		if ( current )
			return current;

		return sessions.Size( ) ? sessions.GetAt( 0 ) : nullptr;
	}

	std::vector< unsigned char > read_thumbnail( const GlobalSystemMediaTransportControlsSessionMediaProperties& info )
	{
		try {
			const auto reference = info.Thumbnail( );
			if ( !reference )
				return { };

			const auto stream = await_for( reference.OpenReadAsync( ) );
			if ( !stream || !stream.Size( ) || stream.Size( ) > 16u * 1024u * 1024u )
				return { };

			Buffer buffer( static_cast< uint32_t >( stream.Size( ) ) );
			const auto read = await_for( stream.ReadAsync( buffer, buffer.Capacity( ), InputStreamOptions::ReadAhead ) );
			if ( !read )
				return { };

			return { read.data( ), read.data( ) + read.Length( ) };
		} catch ( ... ) {
			return { };
		}
	}

	std::wstring process_path( const DWORD pid )
	{
		const HANDLE process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid );
		if ( !process )
			return { };

		wchar_t path[ MAX_PATH ]{ };
		DWORD size    = MAX_PATH;
		const bool ok = QueryFullProcessImageNameW( process, 0, path, &size );
		CloseHandle( process );
		return ok ? lower( path ) : std::wstring{ };
	}

	// exe paths with an active stream on the default `flow` device, `sounding` = peak above silence too
	std::vector< std::wstring > session_apps( IMMDeviceEnumerator* enumerator, const EDataFlow flow, const bool sounding )
	{
		std::vector< std::wstring > out{ };
		IMMDevice* device                 = nullptr;
		IAudioSessionManager2* manager    = nullptr;
		IAudioSessionEnumerator* sessions = nullptr;
		int count                         = 0;

		if ( SUCCEEDED( enumerator->GetDefaultAudioEndpoint( flow, eConsole, &device ) ) &&
		     SUCCEEDED( device->Activate( __uuidof( IAudioSessionManager2 ), CLSCTX_ALL, nullptr, reinterpret_cast< void** >( &manager ) ) ) &&
		     SUCCEEDED( manager->GetSessionEnumerator( &sessions ) ) && SUCCEEDED( sessions->GetCount( &count ) ) ) {
			for ( int i = 0; i < count; i++ ) {
				IAudioSessionControl* control   = nullptr;
				IAudioSessionControl2* control2 = nullptr;
				IAudioMeterInformation* meter   = nullptr;
				AudioSessionState state         = AudioSessionStateInactive;
				DWORD pid                       = 0;
				float peak                      = 0.f;

				if ( SUCCEEDED( sessions->GetSession( i, &control ) ) && SUCCEEDED( control->GetState( &state ) ) && state == AudioSessionStateActive &&
				     SUCCEEDED( control->QueryInterface( IID_PPV_ARGS( &control2 ) ) ) && control2->IsSystemSoundsSession( ) == S_FALSE &&
				     SUCCEEDED( control2->GetProcessId( &pid ) ) && pid &&
				     ( !sounding || ( SUCCEEDED( control->QueryInterface( IID_PPV_ARGS( &meter ) ) ) && SUCCEEDED( meter->GetPeakValue( &peak ) ) && peak > 0.001f ) ) ) {
					if ( auto path = process_path( pid ); !path.empty( ) )
						out.push_back( std::move( path ) );
				}

				safe_release( meter );
				safe_release( control2 );
				safe_release( control );
			}
		}

		safe_release( sessions );
		safe_release( manager );
		safe_release( device );
		return out;
	}

	std::wstring window_title( const std::wstring& path )
	{
		struct search_t {
			const std::wstring* m_path;
			std::wstring m_title;
		} search{ &path, { } };

		EnumWindows(
			[ ]( HWND window, LPARAM param ) -> BOOL {
				auto& search = *reinterpret_cast< search_t* >( param );

				// own windows skipped before GetWindowText: same-process text is a SendMessage to the game thread
				DWORD pid = 0;
				GetWindowThreadProcessId( window, &pid );
				if ( !IsWindowVisible( window ) || GetWindow( window, GW_OWNER ) || pid == GetCurrentProcessId( ) )
					return TRUE;

				wchar_t text[ 512 ]{ };
				if ( !GetWindowTextW( window, text, 512 ) || process_path( pid ) != *search.m_path )
					return TRUE;

				search.m_title = text;
				return FALSE;
			},
			reinterpret_cast< LPARAM >( &search ) );

		return search.m_title;
	}

	// "12. Artist - Title.mp3 - VLC media player" / "Artist - Title [foobar2000]" -> "Artist", "Title"
	void split_title( std::wstring text, const std::wstring& stem, std::wstring& artist, std::wstring& title )
	{
		std::size_t last = 0;
		for ( const wchar_t* cut : { L" - ", L" [", L" | " } )
			if ( const auto at = text.rfind( cut ); at != std::wstring::npos )
				last = ( std::max )( last, at );
		if ( last && lower( text.substr( last ) ).find( stem ) != std::wstring::npos )
			text.resize( last );

		std::size_t digits = 0;
		while ( digits < text.size( ) && std::iswdigit( text[ digits ] ) )
			digits++;
		if ( digits && text.compare( digits, 2, L". " ) == 0 )
			text.erase( 0, digits + 2 );

		if ( const auto dot = text.rfind( L'.' ); dot != std::wstring::npos ) {
			static constexpr const wchar_t* k_media[ ]{ L".mp3", L".flac", L".wav", L".ogg", L".opus", L".m4a", L".aac", L".wma", L".mp4", L".mkv", L".webm" };
			const auto ext = lower( text.substr( dot ) );
			if ( std::find_if( std::begin( k_media ), std::end( k_media ), [ & ]( const wchar_t* e ) { return ext == e; } ) != std::end( k_media ) )
				text.resize( dot );
		}

		const auto at = text.find( L" - " );
		artist        = at != std::wstring::npos ? text.substr( 0, at ) : std::wstring{ };
		title         = at != std::wstring::npos ? text.substr( at + 3 ) : text;
	}

	/* no GSMTC player ( vlc 3, winamp, broken SMTC ): an app that makes sound and has no mic stream ( voice chat ).
	   one with a song file open beats one with only a window title ( discord tab in a browser ) */
	bool find_audio_app( std::wstring& path, std::wstring& window, std::vector< std::wstring >& files )
	{
		IMMDeviceEnumerator* enumerator = nullptr;
		if ( FAILED( CoCreateInstance( __uuidof( MMDeviceEnumerator ), nullptr, CLSCTX_ALL, __uuidof( IMMDeviceEnumerator ), reinterpret_cast< void** >( &enumerator ) ) ) )
			return false;

		const auto playing = session_apps( enumerator, eRender, true );
		const auto talking = session_apps( enumerator, eCapture, false );
		enumerator->Release( );

		const std::wstring self = process_path( GetCurrentProcessId( ) );
		std::vector< std::wstring > titled{ };
		for ( const auto& app : playing ) {
			if ( app == self || std::find( talking.begin( ), talking.end( ), app ) != talking.end( ) ||
			     std::find( titled.begin( ), titled.end( ), app ) != titled.end( ) )
				continue;

			if ( files = n_media_file::open_media( app ); !files.empty( ) ) {
				path   = app;
				window = window_title( app );
				return true;
			}

			if ( !window_title( app ).empty( ) )
				titled.push_back( app );
		}

		if ( titled.empty( ) )
			return false;

		path   = titled.front( );
		window = window_title( path );
		return true;
	}
}

bool n_media_player::impl_t::init( )
{
	try {
		this->m_session_manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync( ).get( );
	} catch ( ... ) {
		return false;
	}

	return this->m_session_manager.has_value( );
}

void n_media_player::impl_t::shutdown( )
{
	try {
		this->m_session_manager.reset( );
	} catch ( ... ) {
	}
}

void n_media_player::impl_t::on_update( )
{
	if ( !this->m_session_manager.has_value( ) )
		return;

	const auto clear = [ this ]( ) {
		this->m_has_media  = false;
		this->m_is_playing = false;
		std::lock_guard< std::mutex > lock( this->m_mutex );
		this->m_source_app.clear( );
	};

	struct now_t {
		std::string m_title{ }, m_artist{ };
		std::wstring m_source{ };
		std::optional< std::vector< unsigned char > > m_thumbnail{ }; // nullopt = keep
		long long m_total_ms{ }, m_current_ms{ };
		std::chrono::steady_clock::time_point m_stamp{ std::chrono::steady_clock::now( ) };
		double m_rate{ 1.0 };
		bool m_playing{ };
	};

	const auto publish = [ this ]( now_t& now ) {
		{
			std::lock_guard< std::mutex > lock( this->m_mutex );

			this->m_title      = std::move( now.m_title );
			this->m_artist     = std::move( now.m_artist );
			this->m_source_app = std::move( now.m_source );

			if ( now.m_thumbnail && *now.m_thumbnail != this->m_thumbnail ) {
				this->m_thumbnail       = std::move( *now.m_thumbnail );
				this->m_thumbnail_dirty = true;
			}

			this->m_total_ms       = now.m_total_ms;
			this->m_current_ms     = now.m_current_ms;
			this->m_position_stamp = now.m_stamp;
			this->m_rate           = now.m_rate;
		}

		this->m_is_playing = now.m_playing;
		this->m_has_media  = true;
	};

	// art only on track change or while still missing ( some apps set it after the title ), a bad thumbnail never hides the player
	const auto thumbnail = [ this ]( const std::wstring& key, const auto& read, const bool retry = true ) -> std::optional< std::vector< unsigned char > > {
		if ( key == this->m_thumbnail_key && ( !retry || !this->m_thumbnail.empty( ) || ++this->m_thumbnail_wait < 3 ) )
			return std::nullopt;

		this->m_thumbnail_key  = key;
		this->m_thumbnail_wait = 0;
		return read( );
	};

	const auto from_session = [ & ]( const GlobalSystemMediaTransportControlsSession& session ) {
		const auto info = await_for( session.TryGetMediaPropertiesAsync( ) );
		if ( !info )
			return;

		now_t now{ };
		now.m_source    = session.SourceAppUserModelId( );
		now.m_title     = to_utf8( info.Title( ) );
		now.m_artist    = to_utf8( strip_album( std::wstring{ info.Artist( ) } ) );
		now.m_thumbnail = thumbnail( now.m_source + L'\n' + std::wstring{ info.Title( ) } + L'\n' + std::wstring{ info.Artist( ) },
		                             [ & ] { return read_thumbnail( info ); } );

		const auto timeline = session.GetTimelineProperties( );

		/* Position is at LastUpdatedTime, not now (browsers only push it on play / pause / seek).
		   zero or future stamp = never filled, treat as fresh. */
		auto age = winrt::clock::now( ) - timeline.LastUpdatedTime( );
		if ( timeline.LastUpdatedTime( ).time_since_epoch( ).count( ) == 0 || age.count( ) < 0 )
			age = { };

		now.m_total_ms   = std::chrono::duration_cast< std::chrono::milliseconds >( timeline.EndTime( ) - timeline.StartTime( ) ).count( );
		now.m_current_ms = std::chrono::duration_cast< std::chrono::milliseconds >( timeline.Position( ) - timeline.StartTime( ) ).count( );
		now.m_stamp -= std::chrono::duration_cast< std::chrono::steady_clock::duration >( age );

		if ( const auto playback = session.GetPlaybackInfo( ) ) {
			now.m_playing = playback.PlaybackStatus( ) == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
			if ( const auto rate = playback.PlaybackRate( ); rate && rate.Value( ) > 0.0 )
				now.m_rate = rate.Value( );
		}

		publish( now );
	};

	const auto from_kopuz = [ & ]( n_kopuz::state_t& kopuz ) {
		now_t now{ };
		now.m_source     = kopuz.m_app;
		now.m_title      = std::move( kopuz.m_title );
		now.m_artist     = std::move( kopuz.m_artist );
		now.m_total_ms   = kopuz.m_total_ms;
		now.m_current_ms = kopuz.m_position_ms;
		now.m_playing    = kopuz.m_playing;
		now.m_thumbnail  = thumbnail( L"kopuz\n" + std::to_wstring( kopuz.m_art_version ) + L'\n' + std::wstring( kopuz.m_art_request.begin( ), kopuz.m_art_request.end( ) ),
		                              [ & ] { return n_kopuz::get_artwork( kopuz.m_art_request ); } );
		publish( now );
	};

	const auto from_audio = [ & ]( ) {
		static std::wstring s_app{ }, s_file{ };
		static n_media_file::tags_t s_tags{ };
		static std::chrono::steady_clock::time_point s_file_start{ };
		static bool s_file_timed{ }, s_watching{ };
		static int s_quiet{ };

		std::wstring path{ }, window{ };
		std::vector< std::wstring > files{ };
		if ( !find_audio_app( path, window, files ) ) {
			// gap between songs: hold what's shown a few updates
			if ( ++s_quiet > 3 )
				s_app.clear( );
			return !s_app.empty( ) && this->m_has_media;
		}

		s_quiet = 0;

		// two updates in a row, a notification ding is shorter
		if ( path != s_app ) {
			s_app      = path;
			s_watching = false;
			s_file.clear( );
			return false;
		}

		/* the song file it has open beats the window title. one still open stays ( gapless players open the next early ).
		   position only known for a file that showed up while watching */
		if ( std::find( files.begin( ), files.end( ), s_file ) == files.end( ) ) {
			const std::wstring file = files.empty( ) ? std::wstring{ } : files.front( );
			s_file_timed            = s_watching && !file.empty( );
			s_file_start            = std::chrono::steady_clock::now( );
			s_file                  = file;
			s_tags                  = { };
			if ( !file.empty( ) )
				n_media_file::read_tags( file, s_tags );
		}
		s_watching = true;

		std::wstring artist{ }, title{ };
		if ( !s_file.empty( ) ) {
			artist = s_tags.m_artist;
			title  = s_tags.m_title;
			if ( title.empty( ) )
				split_title( s_file.substr( s_file.find_last_of( L"\\/" ) + 1 ), exe_stem( path ), artist, title );
		} else
			split_title( window, exe_stem( path ), artist, title );

		now_t now{ };
		now.m_source  = path;
		now.m_title   = to_utf8( title );
		now.m_artist  = to_utf8( artist );
		now.m_playing = true;
		if ( s_file_timed ) {
			now.m_total_ms = s_tags.m_total_ms;
			now.m_stamp    = s_file_start;
		}
		now.m_thumbnail = thumbnail( s_file, [ & ] { return s_file.empty( ) ? std::vector< unsigned char >{ } : n_media_file::read_cover( s_file ); }, false );
		publish( now );
		return true;
	};

	/* playing beats paused, SMTC beats the rest. kopuz = own pipe api, its SMTC is broken.
	   audible window ( vlc 3, winamp ) before anything paused */
	try {
		const auto session = pick_session( *this->m_session_manager );
		if ( session && is_playing( session ) )
			return from_session( session );

		n_kopuz::state_t kopuz{ };
		const bool has_kopuz = n_kopuz::get_state( kopuz );
		if ( has_kopuz && kopuz.m_playing )
			return from_kopuz( kopuz );

		if ( from_audio( ) )
			return;

		if ( has_kopuz )
			return from_kopuz( kopuz );

		if ( session )
			return from_session( session );

		clear( );
	} catch ( ... ) {
		clear( );
	}
}

void n_media_player::impl_t::update_texture( IDirect3DDevice9* device )
{
	if ( !device )
		return;

	std::vector< unsigned char > thumbnail{ };
	{
		std::lock_guard< std::mutex > lock( this->m_mutex );

		if ( !this->m_thumbnail_dirty )
			return;

		thumbnail               = this->m_thumbnail;
		this->m_thumbnail_dirty = false;
	}

	if ( this->m_image ) {
		this->m_image->Release( );
		this->m_image = nullptr;
	}

	if ( thumbnail.empty( ) )
		return;

	if ( FAILED( D3DXCreateTextureFromFileInMemory( device, thumbnail.data( ), static_cast< UINT >( thumbnail.size( ) ), &this->m_image ) ) )
		this->m_image = nullptr;
}

void n_media_player::impl_t::on_release( )
{
	if ( this->m_image ) {
		this->m_image->Release( );
		this->m_image = nullptr;
	}
}

std::string n_media_player::impl_t::get_title( )
{
	std::lock_guard< std::mutex > lock( this->m_mutex );
	return this->m_title;
}

std::string n_media_player::impl_t::get_artist( )
{
	std::lock_guard< std::mutex > lock( this->m_mutex );
	return this->m_artist;
}

std::wstring n_media_player::impl_t::get_source_app( )
{
	std::lock_guard< std::mutex > lock( this->m_mutex );
	return this->m_source_app;
}

namespace
{
	struct activate_handler_t : IActivateAudioInterfaceCompletionHandler, IAgileObject {
		std::atomic< ULONG > m_refs{ 1 };
		HANDLE m_done          = CreateEventW( nullptr, TRUE, FALSE, nullptr );
		IAudioClient* m_client = nullptr;

		virtual ~activate_handler_t( )
		{
			safe_release( this->m_client );
			if ( this->m_done )
				CloseHandle( this->m_done );
		}

		HRESULT STDMETHODCALLTYPE QueryInterface( REFIID riid, void** out ) override
		{
			if ( riid == __uuidof( IUnknown ) || riid == __uuidof( IActivateAudioInterfaceCompletionHandler ) )
				*out = static_cast< IActivateAudioInterfaceCompletionHandler* >( this );
			else if ( riid == __uuidof( IAgileObject ) )
				*out = static_cast< IAgileObject* >( this );
			else {
				*out = nullptr;
				return E_NOINTERFACE;
			}

			this->AddRef( );
			return S_OK;
		}

		ULONG STDMETHODCALLTYPE AddRef( ) override { return ++this->m_refs; }

		ULONG STDMETHODCALLTYPE Release( ) override
		{
			const ULONG refs = --this->m_refs;
			if ( refs == 0 )
				delete this;
			return refs;
		}

		HRESULT STDMETHODCALLTYPE ActivateCompleted( IActivateAudioInterfaceAsyncOperation* operation ) override
		{
			HRESULT result  = E_FAIL;
			IUnknown* added = nullptr;
			if ( SUCCEEDED( operation->GetActivateResult( &result, &added ) ) && SUCCEEDED( result ) && added )
				added->QueryInterface( __uuidof( IAudioClient ), reinterpret_cast< void** >( &this->m_client ) );
			safe_release( added );

			SetEvent( this->m_done );
			return S_OK;
		}
	};

	std::wstring window_aumid( HWND window )
	{
		static constexpr PROPERTYKEY k_aumid{ { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 }; // PKEY_AppUserModel_ID

		IPropertyStore* store = nullptr;
		if ( FAILED( SHGetPropertyStoreForWindow( window, IID_PPV_ARGS( &store ) ) ) || !store )
			return { };

		std::wstring out{ };
		PROPVARIANT value{ };
		if ( SUCCEEDED( store->GetValue( k_aumid, &value ) ) && value.vt == VT_LPWSTR && value.pwszVal )
			out = value.pwszVal;

		PropVariantClear( &value );
		store->Release( );
		return out;
	}

	/* GSMTC names the app by AppUserModelId, process loopback wants a pid. match exe name ( "Chrome", "MSEdge", "Spotify.exe" ),
	   packaged id ( "Pkg_hash!App" ), or a window's taskbar id ( firefox hash ). then climb same-exe parents to the root,
	   its tree holds the browser's audio child. 0 = no match */
	DWORD media_pid( const std::wstring& aumid )
	{
		if ( aumid.empty( ) )
			return 0;

		struct proc_t {
			DWORD m_parent;
			std::wstring m_exe;
		};
		std::unordered_map< DWORD, proc_t > procs{ };

		const HANDLE snapshot = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
		if ( snapshot == INVALID_HANDLE_VALUE )
			return 0;

		PROCESSENTRY32W entry{ sizeof( entry ) };
		for ( BOOL ok = Process32FirstW( snapshot, &entry ); ok; ok = Process32NextW( snapshot, &entry ) )
			procs[ entry.th32ProcessID ] = { entry.th32ParentProcessID, exe_stem( entry.szExeFile ) };
		CloseHandle( snapshot );

		const DWORD self = GetCurrentProcessId( );

		// bounded: a dead parent's pid can be reused
		const auto root = [ & ]( DWORD pid ) {
			for ( int depth = 0; pid && depth < 16; depth++ ) {
				const auto child  = procs.find( pid );
				const auto parent = child != procs.end( ) ? procs.find( child->second.m_parent ) : procs.end( );
				if ( parent == procs.end( ) || parent->first == pid || parent->first == self || parent->second.m_exe != child->second.m_exe )
					break;
				pid = parent->first;
			}
			return pid;
		};

		const std::wstring want = lower( aumid );
		const std::wstring stem = exe_stem( aumid );

		// many chrome.exe: the root most of them hang off is the browser, stable across checks so the stream doesn't flap
		std::unordered_map< DWORD, int > roots{ };
		for ( const auto& [ pid, proc ] : procs )
			if ( pid && pid != self && proc.m_exe == stem )
				roots[ root( pid ) ]++;

		DWORD found = 0;
		int best    = 0;
		for ( const auto& [ pid, count ] : roots ) {
			if ( count > best || ( count == best && pid < found ) ) {
				found = pid;
				best  = count;
			}
		}

		if ( found )
			return found;

		if ( want.find( L'!' ) != std::wstring::npos ) {
			for ( const auto& [ pid, proc ] : procs ) {
				if ( !pid || pid == self )
					continue;

				const HANDLE process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid );
				if ( !process )
					continue;

				wchar_t id[ APPLICATION_USER_MODEL_ID_MAX_LENGTH ]{ };
				UINT32 size = APPLICATION_USER_MODEL_ID_MAX_LENGTH;
				const bool match = GetApplicationUserModelId( process, &size, id ) == ERROR_SUCCESS && lower( id ) == want;
				CloseHandle( process );

				if ( match ) {
					found = pid;
					break;
				}
			}
		}

		if ( !found ) {
			struct search_t {
				const std::wstring* m_want;
				DWORD m_pid;
			} search{ &want, 0 };

			EnumWindows(
				[ ]( HWND window, LPARAM param ) -> BOOL {
					auto& search = *reinterpret_cast< search_t* >( param );
					if ( !IsWindowVisible( window ) || lower( window_aumid( window ) ) != *search.m_want )
						return TRUE;

					GetWindowThreadProcessId( window, &search.m_pid );
					return FALSE;
				},
				reinterpret_cast< LPARAM >( &search ) );

			found = search.m_pid == self ? 0 : search.m_pid;
		}

		return root( found );
	}

	/* process loopback ( win10 20348+ / win11 ): only the media app's process tree, or ( app not found ) everything but csgo's tree.
	   the virtual device has no mix format: ask for 48k stereo pcm16 and let AUTOCONVERTPCM resample */
	struct loopback_t {
		IMMDeviceEnumerator* m_enumerator = nullptr;
		IAudioClient* m_client            = nullptr;
		IAudioCaptureClient* m_capture    = nullptr;
		HANDLE m_event                    = nullptr; // EVENTCALLBACK wants one, the thread still polls
		WAVEFORMATEX m_format{ WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0 };
		std::wstring m_device_id{ };
		std::wstring m_source{ };
		DWORD m_target = 0; // 0 = exclude csgo

		void close( )
		{
			if ( this->m_client )
				this->m_client->Stop( );

			safe_release( this->m_capture );
			safe_release( this->m_client );

			if ( this->m_event ) {
				CloseHandle( this->m_event );
				this->m_event = nullptr;
			}

			this->m_device_id.clear( );
			this->m_source.clear( );
			this->m_target = 0;
		}

		std::wstring default_id( )
		{
			IMMDevice* device = nullptr;
			if ( !this->m_enumerator || FAILED( this->m_enumerator->GetDefaultAudioEndpoint( eRender, eConsole, &device ) ) || !device )
				return { };

			std::wstring out{ };
			LPWSTR id = nullptr;
			if ( SUCCEEDED( device->GetId( &id ) ) && id ) {
				out = id;
				CoTaskMemFree( id );
			}

			device->Release( );
			return out;
		}

		bool open( const std::wstring& source, const DWORD target )
		{
			this->close( );

			if ( !this->m_enumerator && FAILED( CoCreateInstance( __uuidof( MMDeviceEnumerator ), nullptr, CLSCTX_ALL, __uuidof( IMMDeviceEnumerator ),
			                                                      reinterpret_cast< void** >( &this->m_enumerator ) ) ) )
				return false;

			// remembered only to reopen when the default output / media app switches ( thread checks every 2 s )
			this->m_device_id = this->default_id( );
			this->m_source    = source;
			this->m_target    = target;

			AUDIOCLIENT_ACTIVATION_PARAMS params{ };
			params.ActivationType                            = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
			params.ProcessLoopbackParams.TargetProcessId     = target ? target : GetCurrentProcessId( );
			params.ProcessLoopbackParams.ProcessLoopbackMode = target ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

			PROPVARIANT activation{ };
			activation.vt             = VT_BLOB;
			activation.blob.cbSize    = sizeof( params );
			activation.blob.pBlobData = reinterpret_cast< BYTE* >( &params );

			auto* handler                                  = new activate_handler_t{ };
			IActivateAudioInterfaceAsyncOperation* pending = nullptr;

			if ( handler->m_done &&
			     SUCCEEDED( ActivateAudioInterfaceAsync( VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof( IAudioClient ), &activation, handler, &pending ) ) &&
			     WaitForSingleObject( handler->m_done, 5000 ) == WAIT_OBJECT_0 ) {
				this->m_client    = handler->m_client;
				handler->m_client = nullptr;
			}

			safe_release( pending );
			handler->Release( );

			this->m_event = CreateEventW( nullptr, FALSE, FALSE, nullptr );

			constexpr DWORD k_flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
			if ( !this->m_client || !this->m_event ||
			     FAILED( this->m_client->Initialize( AUDCLNT_SHAREMODE_SHARED, k_flags, 2000000, 0, &this->m_format, nullptr ) ) ||
			     FAILED( this->m_client->SetEventHandle( this->m_event ) ) ||
			     FAILED( this->m_client->GetService( __uuidof( IAudioCaptureClient ), reinterpret_cast< void** >( &this->m_capture ) ) ) ||
			     FAILED( this->m_client->Start( ) ) ) {
				this->close( );
				return false;
			}

			return true;
		}

		static float sample( const BYTE* frame )
		{
			const auto* pcm = reinterpret_cast< const short* >( frame );
			return ( static_cast< float >( pcm[ 0 ] ) + static_cast< float >( pcm[ 1 ] ) ) / 65536.f;
		}
	};
}

void n_media_player::impl_t::visualizer_thread( const std::atomic< bool >& stop, bool ( *wanted )( ) )
{
	using steady = std::chrono::steady_clock;
	constexpr std::size_t k_size = n_audio_bands::k_fft_size;

	const bool com = SUCCEEDED( CoInitializeEx( nullptr, COINIT_MULTITHREADED ) );

	loopback_t loopback{ };
	std::array< float, k_size > ring{ };
	std::array< float, k_size > ordered{ };
	std::size_t write = 0;
	float fresh[ n_audio_bands::k_bands ]{ };
	float levels[ n_audio_bands::k_bands ]{ };

	auto last_step   = steady::now( );
	auto last_audio  = steady::now( );
	auto last_device = steady::now( );

	const auto publish = [ & ]( ) {
		std::lock_guard< std::mutex > lock( this->m_bands_mutex );
		std::copy( std::begin( levels ), std::end( levels ), std::begin( this->m_bands ) );
	};

	while ( !stop.load( std::memory_order_relaxed ) ) {
		const std::wstring source = this->get_source_app( );

		if ( !wanted( ) || source.empty( ) ) {
			loopback.close( );
			std::fill( std::begin( levels ), std::end( levels ), 0.f );
			publish( );
			std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
			last_step = steady::now( );
			continue;
		}

		if ( !loopback.m_client ) {
			if ( !loopback.open( source, media_pid( source ) ) ) {
				std::this_thread::sleep_for( std::chrono::milliseconds( 1000 ) );
				continue;
			}
			last_device = steady::now( );
		}

		const auto now = steady::now( );

		if ( source != loopback.m_source ) {
			loopback.close( );
			continue;
		}

		if ( now - last_device > std::chrono::seconds( 2 ) ) {
			last_device = now;
			if ( loopback.default_id( ) != loopback.m_device_id || media_pid( source ) != loopback.m_target ) {
				loopback.close( );
				continue;
			}
		}

		UINT32 packet = 0;
		HRESULT result = loopback.m_capture->GetNextPacketSize( &packet );

		while ( SUCCEEDED( result ) && packet > 0 ) {
			BYTE* data   = nullptr;
			UINT32 count = 0;
			DWORD flags  = 0;

			result = loopback.m_capture->GetBuffer( &data, &count, &flags, nullptr, nullptr );
			if ( FAILED( result ) )
				break;

			const bool silent = ( flags & AUDCLNT_BUFFERFLAGS_SILENT ) != 0 || !data;
			for ( UINT32 frame = 0; frame < count; frame++ ) {
				ring[ write ] = silent ? 0.f : loopback_t::sample( data + frame * loopback.m_format.nBlockAlign );
				write         = ( write + 1 ) % k_size;
			}

			loopback.m_capture->ReleaseBuffer( count );
			last_audio = now;
			result     = loopback.m_capture->GetNextPacketSize( &packet );
		}

		if ( FAILED( result ) ) {
			loopback.close( );
			std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
			continue;
		}

		if ( now - last_audio > std::chrono::milliseconds( 100 ) )
			ring.fill( 0.f );

		for ( std::size_t i = 0; i < k_size; i++ )
			ordered[ i ] = ring[ ( write + i ) % k_size ];

		n_audio_bands::compute( ordered.data( ), static_cast< float >( loopback.m_format.nSamplesPerSec ), fresh );

		const float dt = std::chrono::duration< float >( now - last_step ).count( );
		last_step      = now;
		for ( std::size_t band = 0; band < n_audio_bands::k_bands; band++ )
			levels[ band ] = ( std::max )( fresh[ band ], levels[ band ] - 2.5f * dt );

		publish( );
		std::this_thread::sleep_for( std::chrono::milliseconds( 15 ) );
	}

	loopback.close( );
	safe_release( loopback.m_enumerator );

	if ( com )
		CoUninitialize( );
}

void n_media_player::impl_t::get_bands( float* out )
{
	std::lock_guard< std::mutex > lock( this->m_bands_mutex );
	std::copy( std::begin( this->m_bands ), std::end( this->m_bands ), out );
}

double n_media_player::impl_t::get_position_ms( )
{
	std::lock_guard< std::mutex > lock( this->m_mutex );

	auto current = static_cast< double >( this->m_current_ms );

	if ( this->m_is_playing )
		current += this->m_rate * static_cast< double >(
			std::chrono::duration_cast< std::chrono::milliseconds >( std::chrono::steady_clock::now( ) - this->m_position_stamp ).count( ) );

	return current;
}

float n_media_player::impl_t::get_progress( )
{
	long long total_ms = 0;
	{
		std::lock_guard< std::mutex > lock( this->m_mutex );
		total_ms = this->m_total_ms;
	}

	if ( total_ms <= 0 )
		return 0.f;

	return std::clamp( static_cast< float >( this->get_position_ms( ) / static_cast< double >( total_ms ) ), 0.f, 1.f );
}

namespace
{
	std::wstring url_encode( const std::string& text )
	{
		std::wstring out{ };
		for ( const unsigned char c : text ) {
			if ( isalnum( c ) || c == '-' || c == '_' || c == '.' || c == '~' )
				out += static_cast< wchar_t >( c );
			else {
				wchar_t hex[ 4 ]{ };
				swprintf_s( hex, L"%%%02X", c );
				out += hex;
			}
		}
		return out;
	}

	std::string lrclib_get( const std::wstring& path )
	{
		std::string out{ };

		HINTERNET session = WinHttpOpen( L"botox", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
		if ( !session )
			return out;

		WinHttpSetTimeouts( session, 5000, 5000, 10000, 10000 );

		HINTERNET connection = WinHttpConnect( session, L"lrclib.net", INTERNET_DEFAULT_HTTPS_PORT, 0 );
		HINTERNET request    = connection ? WinHttpOpenRequest( connection, L"GET", path.c_str( ), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
		                                                        WINHTTP_FLAG_SECURE )
		                                  : nullptr;

		if ( request && WinHttpSendRequest( request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0 ) &&
		     WinHttpReceiveResponse( request, nullptr ) ) {
			DWORD status = 0, status_size = sizeof( status );

			if ( WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
			                          WINHTTP_NO_HEADER_INDEX ) &&
			     status == 200 ) {
				char buffer[ 8192 ]{ };
				DWORD read = 0;

				while ( WinHttpReadData( request, buffer, sizeof( buffer ), &read ) && read && out.size( ) < 1024u * 1024u )
					out.append( buffer, read );
			}
		}

		if ( request )
			WinHttpCloseHandle( request );
		if ( connection )
			WinHttpCloseHandle( connection );
		WinHttpCloseHandle( session );

		return out;
	}

	std::vector< n_media_player::impl_t::lyric_line_t > parse_lrc( const std::string& lrc )
	{
		std::vector< n_media_player::impl_t::lyric_line_t > out{ };
		std::istringstream stream( lrc );
		std::string line{ };

		while ( std::getline( stream, line ) ) {
			if ( !line.empty( ) && line.back( ) == '\r' )
				line.pop_back( );

			std::vector< double > stamps{ };
			std::size_t at = 0;
			while ( at < line.size( ) && line[ at ] == '[' ) {
				const auto close = line.find( ']', at );
				if ( close == std::string::npos )
					break;

				int minutes   = 0;
				float seconds = 0.f;
				if ( sscanf_s( line.substr( at + 1, close - at - 1 ).c_str( ), "%d:%f", &minutes, &seconds ) == 2 )
					stamps.push_back( ( minutes * 60.0 + seconds ) * 1000.0 );

				at = close + 1;
			}

			std::string text = line.substr( at );
			if ( !text.empty( ) && text.front( ) == ' ' )
				text.erase( 0, 1 );

			for ( const double stamp : stamps )
				out.push_back( { stamp, text } );
		}

		std::stable_sort( out.begin( ), out.end( ), [ ]( const auto& a, const auto& b ) { return a.m_start_ms < b.m_start_ms; } );
		return out;
	}

	std::vector< n_media_player::impl_t::lyric_line_t > fetch_lyrics( const std::string& title, const std::string& artist, const long long total_ms )
	{
		const std::wstring base = L"/api/get?artist_name=" + url_encode( artist ) + L"&track_name=" + url_encode( title );

		std::string body{ };
		if ( total_ms > 0 )
			body = lrclib_get( base + L"&duration=" + std::to_wstring( ( total_ms + 500 ) / 1000 ) );
		if ( body.empty( ) )
			body = lrclib_get( base );

		const auto json = nlohmann::json::parse( body, nullptr, false );
		if ( !json.is_object( ) )
			return { };

		const auto synced = json.find( "syncedLyrics" );
		if ( synced == json.end( ) || !synced->is_string( ) )
			return { };

		return parse_lrc( synced->get< std::string >( ) );
	}
}

void n_media_player::impl_t::update_lyrics( const bool wanted )
{
	std::string title{ }, artist{ };
	long long total_ms = 0;
	{
		std::lock_guard< std::mutex > lock( this->m_mutex );
		title    = this->m_title;
		artist   = this->m_artist;
		total_ms = this->m_total_ms;
	}

	// no timeline = position unknown, synced lines would be wrong
	const std::string key = wanted && this->m_has_media && !title.empty( ) && total_ms > 0 ? title + '\n' + artist : std::string{ };
	if ( key == this->m_lyrics_key )
		return;

	this->m_lyrics_key = key;

	{
		std::lock_guard< std::mutex > lock( this->m_lyrics_mutex );
		this->m_lyrics.clear( );
		++this->m_lyrics_generation;
	}

	if ( key.empty( ) )
		return;

	auto lines = fetch_lyrics( title, artist, total_ms );

	std::lock_guard< std::mutex > lock( this->m_lyrics_mutex );
	this->m_lyrics = std::move( lines );
	++this->m_lyrics_generation;
}

int n_media_player::impl_t::get_lyrics( std::vector< lyric_line_t >& out, const int have_generation )
{
	std::lock_guard< std::mutex > lock( this->m_lyrics_mutex );

	if ( have_generation != this->m_lyrics_generation )
		out = this->m_lyrics;

	return this->m_lyrics_generation;
}
