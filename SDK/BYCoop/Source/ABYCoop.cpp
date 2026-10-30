// BYCoop: online co-op for Barnyard (2006, PC), up to 4 players, host-authoritative.
// Milestone 1: connect (Steam lobby or local UDP), exchange appearance, stream poses.
// Builds on OpenBarnyard's BYModCore / BYardSDK (credit: InfiniteC0re and contributors).
#include "pch.h"
#include "Net/Session.h"
#include "MainMenuButton.h"
#include "DevTools.h"
#include "RemotePuppets.h"
#include "PlayerAssets.h"
#include "CoopFlow.h"
#include "NativeMenu.h"

#include <AImGUI.h>
#include <ModLoader.h>
#include <AHooks.h>
#include <HookHelpers.h>
#include <BYardSDK/APlayerManager.h>
#include <BYardSDK/ACamera.h>
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
	if ( !p || !p->GetSteer() || RemotePuppets::IsPuppet( p ) ) return false;
	TVector4* pos = p->GetSteer()->GetPosition();
	if ( !pos ) return false;
	TVector4 vel;
	p->GetSteer()->GetVelocityVector( vel );
	out.pos[ 0 ] = pos->x;
	out.pos[ 1 ] = pos->y;
	out.pos[ 2 ] = pos->z;
	out.speed    = std::sqrt( vel.x * vel.x + vel.z * vel.z );
	{
		// what the animation uses: ASteer::GetMeasuredSpeed (vtable +0xd4)
		typedef TFLOAT( __thiscall * t_GetF )( void* );
		void* pS  = p->GetSteer();
		out.speed = ( *(t_GetF*)( *(char**)pS + 0xd4 ) )( pS );
	}
	// facing: ASteer::GetDirection (vtable +0xb8)
	typedef const TVector4*( __thiscall * t_GetDir )( void* );
	void*           pSteer = p->GetSteer();
	const TVector4* pDir   = ( *(t_GetDir*)( *(char**)pSteer + 0xb8 ) )( pSteer );
	if ( pDir && ( pDir->x != 0.0f || pDir->z != 0.0f ) ) out.yaw = std::atan2( pDir->x, pDir->z );
	else if ( out.speed > 0.05f ) out.yaw = std::atan2( vel.x, vel.z );
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
		GetPrivateProfileStringA( "dev", "spawn", "", m_szDevSpawn, sizeof( m_szDevSpawn ), szIni );

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

		PlayerAssets::Install( Log );

		// Our name: bycoop.ini name=, else the Steam persona name
		if ( m_ready && ( !*m_look.name || !strcmp( m_look.name, "Cow" ) ) )
		{
			std::string strPersona = t->LocalName();
			if ( !strPersona.empty() ) strncpy( m_look.name, strPersona.c_str(), sizeof( m_look.name ) - 1 );
			m_session->SetLocalAppearance( m_look );
		}

		// Lobby, invites, the joiner's cow and entering the host's world (game-styled menus)
		if ( m_ready )
		{
			CoopFlow::Context ctx;
			ctx.pSession = m_session.get();
			ctx.pMutex   = &m_oNetMutex;
			ctx.pLook    = &m_look;
			ctx.fnLog    = Log;
			CoopFlow::Install( ctx );
		}
		MainMenuButton::Install( [ this ]( bool a_bPause ) {
			if ( m_ready ) CoopFlow::OpenLobby( a_bPause );
			else
				NativeMenu::Message( NativeMenu::IsRussian() ? L"\x041c\x0423\x041b\x042c\x0422\x0418\x041f\x041b\x0415\x0415\x0420" : L"MULTIPLAYER",
				                     NativeMenu::IsRussian() ? L"\x0421\x0435\x0442\x044c \x043d\x0435\x0434\x043e\x0441\x0442\x0443\x043f\x043d\x0430. \x0417\x0430\x043f\x0443\x0441\x0442\x0438\x0442\x0435 Steam." : L"Network unavailable. Start Steam and restart the game." );
		} );

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
						if ( m_bOpenInvite && m_session->Transport()->IsInSession() )
						{
							m_bOpenInvite = false;
							m_session->Transport()->OpenInviteDialog();
						}
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
		DevTools::Update( Log, m_szDevSpawn );
		if ( !m_ready ) return TTRUE;
		coop::Pose pose;
		const bool bInLevel = ReadLocalPose( pose );
		pose.levelHash      = bInLevel ? RemotePuppets::CurrentLevel() : 0;
		LogEscape();
		CoopFlow::Update( bInLevel, pose.pos );
		// our breed/gender from the character creator (player profile manager, see RemotePuppets.cpp)
		if ( char* pProfiles = *(char**)0x007b493c )
		{
			coop::Appearance look = m_look;
			look.hide             = (uint8_t)*(TINT*)( pProfiles + 0x48 );
			look.gender           = (uint8_t)*(TINT*)( pProfiles + 0x10 );
			if ( look.hide != m_look.hide || look.gender != m_look.gender )
			{
				m_look = look;
				std::lock_guard<std::mutex> lock( m_oNetMutex );
				m_session->SetLocalAppearance( m_look );
				Log( "appearance: costume " + std::to_string( look.hide ) + " gender " + std::to_string( look.gender ) );
			}
		}
		{
			std::lock_guard<std::mutex> lock( m_oNetMutex );
			m_session->SetLocalPose( pose ); // levelHash 0 outside a level hides our cow on the others
			// assets of the other players' cows (gender model + hide), loaded on top of ours
			int aG[ coop::MAX_PLAYERS ], aC[ coop::MAX_PLAYERS ], n = 0;
			for ( auto& [ slot, rp ] : m_session->Remotes() )
				if ( n < coop::MAX_PLAYERS ) { aG[ n ] = rp.look.gender; aC[ n ] = rp.look.hide; n++; }
			PlayerAssets::SetWanted( aG, aC, n );
			if ( bInLevel ) PlayerAssets::Update();
			APlayerManager* pm    = APlayerManager::GetSingleton();
			void*           pLocal = bInLevel && pm ? (void*)pm->GetPlayer( 0 ) : TNULL;
			RemotePuppets::Update( pLocal, pose.levelHash, m_session->Remotes(), NowSeconds(), m_session->interpDelay, Log );
		}
		return TTRUE;
	}

	// Diagnostics for "cutscene can't be skipped": which game state gets the Esc/Enter press
	void LogEscape()
	{
		static bool s_bEsc = false, s_bRet = false;
		const bool  bEsc = ( GetAsyncKeyState( VK_ESCAPE ) & 0x8000 ) != 0;
		const bool  bRet = ( GetAsyncKeyState( VK_RETURN ) & 0x8000 ) != 0;
		if ( ( bEsc && !s_bEsc ) || ( bRet && !s_bRet ) )
		{
			AGameStateController* pCtrl  = AGameStateController::GetSingleton();
			AGameState*           pState = pCtrl ? pCtrl->GetCurrentState() : TNULL;
			std::string szExtra;
			if ( pState && !strcmp( pState->GetClass()->GetName(), "ASSeqState" ) )
			{
				// ASSeqState::ProcessInput skips on Esc when m_fTime >= 1 && m_bCanSkip (see OpenBarnyard)
				char sz[ 96 ];
				snprintf( sz, sizeof( sz ), " time=%.1f canskip=%d skipped=%d", *(TFLOAT*)( (char*)pState + 0x1e4 ), *(TBOOL*)( (char*)pState + 0x1e9 ), *(TBOOL*)0x00781ba4 );
				szExtra = sz;
			}
			Log( std::string( bEsc && !s_bEsc ? "Esc" : "Enter" ) + " in state " + ( pState ? pState->GetClass()->GetName() : "?" ) +
			     " imgui=" + ( g_pImGui && g_pImGui->IsEnabled() ? "on" : "off" ) + szExtra );
		}
		s_bEsc = bEsc;
		s_bRet = bRet;
	}

	TBOOL HasSettingsUI() override { return TTRUE; }
	TBOOL IsOverlayVisible() override { return TTRUE; }

	// Names above the other players' cows: world -> screen with the current ACamera
	// (matrix rows: right, down, forward, position; the world is Y-down). TODO verify FOV axis in game.
	void DrawNameTags()
	{
		ACameraManager* pCamMgr = ACameraManager::GetSingleton();
		ACamera*        pCam    = pCamMgr ? pCamMgr->GetCurrentCamera() : TNULL;
		if ( !pCam ) return;
		const TMatrix44& m  = pCam->m_Matrix;
		const TVector4&  vR = m.AsBasisVector4( BASISVECTOR_RIGHT );
		const TVector4&  vD = m.AsBasisVector4( BASISVECTOR_UP );
		const TVector4&  vF = m.AsBasisVector4( BASISVECTOR_FORWARD );
		const TVector4&  vP = m.AsBasisVector4( BASISVECTOR_TRANSLATION );
		ImVec2           sz = ImGui::GetIO().DisplaySize;
		TFLOAT           f  = ( sz.y * 0.5f ) / std::tan( ( pCam->m_fFOV > 0.01f ? pCam->m_fFOV : 1.0f ) * 0.5f );
		ImDrawList*      dl = ImGui::GetForegroundDrawList();

		for ( auto& [ slot, rp ] : m_session->Remotes() )
		{
			if ( !rp.hasPose || !RemotePuppets::HasPuppet( slot ) ) continue; // only over a cow that is really there
			coop::Pose p = rp.Sample( NowSeconds(), m_session->interpDelay );
			TFLOAT     x = p.pos[ 0 ] - vP.x, y = p.pos[ 1 ] - 2.2f - vP.y, z = p.pos[ 2 ] - vP.z; // 2.2 above the cow
			TFLOAT     cz = x * vF.x + y * vF.y + z * vF.z;
			if ( cz < 0.5f || cz > 80.0f ) continue;
			TFLOAT sx = sz.x * 0.5f + ( x * vR.x + y * vR.y + z * vR.z ) / cz * f;
			TFLOAT sy = sz.y * 0.5f + ( x * vD.x + y * vD.y + z * vD.z ) / cz * f;
			ImVec2 ts = ImGui::CalcTextSize( rp.look.name );
			ImVec2 at( sx - ts.x * 0.5f, sy - ts.y );
			dl->AddRectFilled( ImVec2( at.x - 6, at.y - 3 ), ImVec2( at.x + ts.x + 6, at.y + ts.y + 3 ), IM_COL32( 0, 0, 0, 120 ), 6.0f );
			dl->AddText( at, IM_COL32( 255, 200, 0, 255 ), rp.look.name );
		}
	}

	void DrawInviteWindow()
	{
		if ( !m_bShowInvite || !m_ready ) return;
		auto* t = m_session->Transport();
		ImGui::SetNextWindowSize( ImVec2( 420, 360 ), ImGuiCond_FirstUseEver );
		ImGui::SetNextWindowPos( ImVec2( 60, 120 ), ImGuiCond_FirstUseEver );
		bool bOpen = true;
		if ( ImGui::Begin( "Barnyard Co-op: invite friends", &bOpen ) )
		{
			ImGui::Text( "%s", m_session->Status().c_str() );
			ImGui::TextWrapped( "Friends need Barnyard with BYCoop installed and Steam running. They can also accept from the Steam friends list." );
			ImGui::Separator();
			if ( NowSeconds() - m_fFriendsTime > 2.0 )
			{
				m_aFriends     = t->OnlineFriends();
				m_fFriendsTime = NowSeconds();
			}
			if ( m_aFriends.empty() ) ImGui::TextDisabled( "No friends online" );
			for ( auto& f : m_aFriends )
			{
				ImGui::PushID( (void*)(uintptr_t)f.id );
				ImGui::Text( "%s%s", f.name.c_str(), f.inGame ? "  (in game)" : "" );
				ImGui::SameLine( 300 );
				if ( ImGui::Button( "Invite" ) ) t->Invite( f.id );
				ImGui::PopID();
			}
			ImGui::Separator();
			if ( ImGui::Button( "Close" ) ) bOpen = false;
		}
		ImGui::End();
		if ( !bOpen )
		{
			m_bShowInvite = false;
			if ( g_pImGui && g_pImGui->IsEnabled() ) g_pImGui->Toggle();
		}
	}

	void OnImGuiRenderOverlay( AImGUI* ) override
	{
		std::lock_guard<std::mutex> lock( m_oNetMutex );
		if ( m_ready )
		{
			DrawNameTags();
			CoopFlow::DrawOverlay();
		}
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
				if ( ImGui::Button( "Invite friends" ) ) m_bShowInvite = true;
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
	bool                           m_bOpenInvite = false;
	char                           m_szDevSpawn[ 64 ] = {};
	bool                           m_bShowInvite      = false;
	std::vector<coop::ITransport::Friend> m_aFriends;
	double                         m_fFriendsTime = 0;
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
