// BYCoop: online co-op for Barnyard (2006, PC), up to 4 players, host-authoritative.
// Milestone 1: connect (Steam lobby or local UDP), exchange appearance, stream poses.
// Builds on OpenBarnyard's BYModCore / BYardSDK (credit: InfiniteC0re and contributors).
#include "pch.h"
#include "Net/Session.h"

#include <AImGUI.h>
#include <ModLoader.h>
#include <AHooks.h>
#include <HookHelpers.h>
#include <BYardSDK/APlayerManager.h>
#include <BYardSDK/AGameStateController.h>
#include <Toshi/THPTimer.h>
#include <ToshiTools/T2CommandLine.h>

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>

TOSHI_NAMESPACE_USING

static const T2CommandLine* g_pCommandLine = TNULL;

namespace
{
// NOTE: no globals that allocate. Static initialisers run inside LoadLibrary, before
// CreateModInstance calls TMemory::Initialise, and Toshi's operator new crashes then (error 1114).
std::deque<std::string>& LogLines()
{
	static std::deque<std::string>* s_pLines = new std::deque<std::string>(); // first use is after TMemory init
	return *s_pLines;
}
std::mutex& LogMutex()
{
	static std::mutex* s_pMutex = new std::mutex();
	return *s_pMutex;
}
void Log( const std::string& s )
{
	std::lock_guard<std::mutex> lock( LogMutex() );
	auto& g_log = LogLines();
	g_log.push_back( s );
	if ( g_log.size() > 12 ) g_log.pop_front();
	OutputDebugStringA( ( "[BYCoop] " + s + "\n" ).c_str() );
	if ( FILE* f = fopen( "Logs\\bycoop.txt", "a" ) )
	{
		fprintf( f, "[%lu] %s\n", GetCurrentProcessId(), s.c_str() ); // pid: several copies share the file
		fclose( f );
	}
}

double NowSeconds()
{
	static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency( &x ); return x; }();
	LARGE_INTEGER        c;
	QueryPerformanceCounter( &c );
	return double( c.QuadPart ) / double( f.QuadPart );
}

// Reads the local cow (player slot 0 of the current player set).
bool ReadLocalPose( coop::Pose& out )
{
	APlayerManager* pm = APlayerManager::GetSingleton();
	if ( !pm ) return false;
	AUnitPlayer* p = pm->GetPlayer( 0 );
	if ( !p || !p->GetSteer() ) return false;
	TVector4* pos = p->GetSteer()->GetPosition();
	if ( !pos ) return false;
	TVector4 vel;
	p->GetSteer()->GetVelocityVector( vel );
	out.pos[ 0 ] = pos->x;
	out.pos[ 1 ] = pos->y;
	out.pos[ 2 ] = pos->z;
	out.speed    = std::sqrt( vel.x * vel.x + vel.z * vel.z );
	if ( out.speed > 0.05f ) out.yaw = std::atan2( vel.x, vel.z );
	return true;
}
} // namespace

class ABYCoop : public AModInstance
{
public:
	TBOOL OnLoad() override
	{
		Log( "BYCoop OnLoad" );
		Log( std::string( "command line: " ) + GetCommandLineA() );
		// Settings: bycoop.ini next to barnyard.exe, overridden by -coop-* command-line switches.
		// (On the user's PC the game process does not see the launcher's arguments, so the ini is primary.)
		const char* cl = GetCommandLineA();
		char        szIni[ MAX_PATH ];
		GetModuleFileNameA( NULL, szIni, MAX_PATH );
		strcpy( strrchr( szIni, '\\' ) + 1, "bycoop.ini" );

		char szMode[ 16 ], szJoin[ 64 ];
		GetPrivateProfileStringA( "coop", "name", "Cow", m_look.name, sizeof( m_look.name ), szIni );
		GetPrivateProfileStringA( "coop", "mode", "off", szMode, sizeof( szMode ), szIni );
		GetPrivateProfileStringA( "coop", "join", "", szJoin, sizeof( szJoin ), szIni );
		int iUdpPort = GetPrivateProfileIntA( "coop", "udp_port", 0, szIni );

		if ( const char* p = strstr( cl, "-coop-name " ) ) sscanf( p + 11, "%23s", m_look.name );
		if ( const char* p = strstr( cl, "-coop-udp " ) ) iUdpPort = atoi( p + 10 );
		if ( strstr( cl, "-coop-host" ) ) strcpy( szMode, "host" );
		if ( const char* p = strstr( cl, "-coop-join " ) ) { strcpy( szMode, "join" ); sscanf( p + 11, "%63s", szJoin ); }
		Log( std::string( "settings: name=" ) + m_look.name + " mode=" + szMode + " join=" + szJoin + " udp=" + std::to_string( iUdpPort ) );
		const char* udp = iUdpPort ? "" : TNULL;

		// UDP test mode with mode=auto: the first copy binds udp_port and hosts, later copies on the
		// same PC bind the next free port and join it. Lets one PC run 2-4 copies from one folder.
		coop::ITransport* t     = TNULL;
		std::string       err;
		const TBOOL       bAuto = !_stricmp( szMode, "auto" );
		for ( int i = 0; i < ( udp && bAuto ? coop::MAX_PLAYERS : 1 ); i++ )
		{
			t = udp ? coop::CreateUdpTransport( (uint16_t)( iUdpPort + i ) ) : coop::CreateSteamTransport();
			m_session.reset( new coop::Session( t ) );
			m_session->onLog = Log;
			m_ready          = m_session->Start( err );
			if ( m_ready )
			{
				if ( bAuto && i == 0 ) strcpy( szMode, "host" );
				else if ( bAuto )
				{
					strcpy( szMode, "join" );
					snprintf( szJoin, sizeof( szJoin ), "127.0.0.1:%d", iUdpPort );
					snprintf( m_look.name + strlen( m_look.name ), sizeof( m_look.name ) - strlen( m_look.name ), "%d", i + 1 );
				}
				break;
			}
		}
		Log( m_ready ? "ready: " + t->Describe() + " mode=" + szMode : "network unavailable: " + err );
		m_session->SetLocalAppearance( m_look );

		if ( m_ready && !_stricmp( szMode, "host" ) ) m_session->Host();
		else if ( m_ready && !_stricmp( szMode, "join" ) && *szJoin ) m_session->Join( szJoin );

		// Network runs on its own thread: Barnyard stops its update loop while the window is inactive
		// (e.g. a second copy in the background), which made the host time the player out and re-join.
		if ( m_ready )
		{
			m_bRunNet = true;
			m_oNetThread = std::thread( [ this ] {
				while ( m_bRunNet )
				{
					{
						std::lock_guard<std::mutex> lock( m_oNetMutex );
						m_session->Update( NowSeconds() );
					}
					Sleep( 15 );
				}
			} );
		}
		return TTRUE;
	}

	void OnUnload() override
	{
		m_bRunNet = false;
		if ( m_oNetThread.joinable() ) m_oNetThread.join();
		m_session.reset();
	}

	TBOOL OnUpdate( TFLOAT ) override
	{
		if ( !m_ready ) return TTRUE;
		coop::Pose pose;
		if ( ReadLocalPose( pose ) )
		{
			std::lock_guard<std::mutex> lock( m_oNetMutex );
			m_session->SetLocalPose( pose );
		}
		return TTRUE;
	}

	TBOOL HasSettingsUI() override { return TTRUE; }
	TBOOL IsOverlayVisible() override { return TTRUE; }

	void OnImGuiRenderOverlay( AImGUI* ) override
	{
		std::lock_guard<std::mutex> lock( m_oNetMutex );
		ImGui::SetNextWindowPos( ImVec2( 8, 8 ) );
		ImGui::SetNextWindowBgAlpha( 0.4f );
		ImGui::Begin( "##bycoop_status", TNULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav );
		ImGui::Text( "BYCoop %s | Alt+` menu", m_ready ? m_session->Status().c_str() : "network unavailable" );
		ImGui::End();
	}

	void OnImGuiRender( AImGUI* ) override
	{
		std::unique_lock<std::mutex> lock( m_oNetMutex );
		ImGui::Text( "Barnyard Co-op (up to 4 players)" );
		if ( !m_ready )
		{
			ImGui::TextColored( ImVec4( 1, 0.4f, 0.4f, 1 ), "Network unavailable (see log). Is Steam running?" );
			DrawLog();
			return;
		}
		ImGui::Text( "Status: %s", m_session->Status().c_str() );

		if ( ImGui::InputText( "Name", m_look.name, sizeof( m_look.name ) ) ) m_session->SetLocalAppearance( m_look );

		auto* t = m_session->Transport();
		if ( !t->IsInSession() )
		{
			if ( ImGui::Button( "Host game" ) ) m_session->Host();
			ImGui::InputText( "Lobby id", m_joinTarget, sizeof( m_joinTarget ) );
			ImGui::SameLine();
			if ( ImGui::Button( "Join" ) ) m_session->Join( m_joinTarget );
			ImGui::TextWrapped( "Tip: accept a Steam invite or use 'Join Game' on the host in your friends list." );
		}
		else
		{
			ImGui::Text( "%s", t->Describe().c_str() );
			if ( t->IsHost() )
			{
				if ( ImGui::Button( "Invite friends (Steam overlay)" ) ) t->OpenInviteDialog();
				ImGui::SameLine();
				if ( ImGui::Button( "Copy lobby id" ) ) CopyLobbyId( t->Describe() );
			}
			if ( ImGui::Button( "Leave" ) ) m_session->Leave();

			ImGui::Separator();
			for ( auto& [ slot, rp ] : m_session->Remotes() )
			{
				coop::Pose p = rp.Sample( NowSeconds(), m_session->interpDelay );
				ImGui::Text( "P%d %-12s %s  (%.1f, %.1f, %.1f)", slot + 1, rp.look.name, rp.look.gender ? "F" : "M", p.pos[ 0 ], p.pos[ 1 ], p.pos[ 2 ] );
			}
		}
		DrawLog();
	}

private:
	void DrawLog()
	{
		ImGui::Separator();
		std::lock_guard<std::mutex> lock( LogMutex() );
		for ( auto& s : LogLines() ) ImGui::TextUnformatted( s.c_str() );
	}

	static void CopyLobbyId( const std::string& desc )
	{
		auto        sp = desc.rfind( ' ' );
		std::string id = sp == std::string::npos ? desc : desc.substr( sp + 1 );
		if ( !OpenClipboard( NULL ) ) return;
		EmptyClipboard();
		HGLOBAL h = GlobalAlloc( GMEM_MOVEABLE, id.size() + 1 );
		memcpy( GlobalLock( h ), id.c_str(), id.size() + 1 );
		GlobalUnlock( h );
		SetClipboardData( CF_TEXT, h );
		CloseClipboard();
	}

	std::unique_ptr<coop::Session> m_session;
	std::thread                    m_oNetThread;
	std::mutex                     m_oNetMutex;
	std::atomic<bool>              m_bRunNet{ false };
	coop::Appearance               m_look;
	bool                           m_ready             = false;
	char                           m_joinTarget[ 64 ] = {};
};

extern "C"
{
	MODLOADER_EXPORT AModInstance* CreateModInstance( const T2CommandLine* a_pCommandLine )
	{
		TMemory::Initialise( 8 * 1024 * 1024, 0 );

		TUtil::TOSHIParams toshiParams;
		toshiParams.szCommandLine = "";
		toshiParams.szLogFileName = "bycoop";
		toshiParams.szLogAppName  = "BYCoop";
		TUtil::ToshiCreate( toshiParams );

		g_pCommandLine = a_pCommandLine;
		return new ABYCoop();
	}

	MODLOADER_EXPORT const TCHAR* GetModName() { return "BYCoop"; }

	MODLOADER_EXPORT TUINT32 GetModVersion() { return TVERSION( 0, 1 ); }
}
