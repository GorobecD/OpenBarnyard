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

TOSHI_NAMESPACE_USING

static const T2CommandLine* g_pCommandLine = TNULL;

namespace
{
std::deque<std::string> g_log;
void Log( const std::string& s )
{
	g_log.push_back( s );
	if ( g_log.size() > 12 ) g_log.pop_front();
	OutputDebugStringA( ( "[BYCoop] " + s + "\n" ).c_str() );
	if ( FILE* f = fopen( "Logs\\bycoop.txt", "a" ) )
	{
		fprintf( f, "%s\n", s.c_str() );
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
		// -coop-udp <port> : local test mode (several copies on one PC). Default: Steam.
		const char* cl   = GetCommandLineA();
		const char* udp  = strstr( cl, "-coop-udp " );
		const char* name = strstr( cl, "-coop-name " );
		if ( name ) sscanf( name + 11, "%23s", m_look.name );
		else strcpy( m_look.name, "Cow" );

		coop::ITransport* t = udp ? coop::CreateUdpTransport( (uint16_t)atoi( udp + 10 ) ) : coop::CreateSteamTransport();
		m_session.reset( new coop::Session( t ) );
		m_session->onLog = Log;

		std::string err;
		m_ready = m_session->Start( err );
		Log( m_ready ? "ready: " + t->Describe() : "network unavailable: " + err );
		m_session->SetLocalAppearance( m_look );

		// auto host/join for scripted tests: -coop-host / -coop-join <target>
		if ( m_ready && strstr( cl, "-coop-host" ) ) m_session->Host();
		if ( const char* j = strstr( cl, "-coop-join " ) )
		{
			char target[ 64 ] = {};
			sscanf( j + 11, "%63s", target );
			if ( m_ready ) m_session->Join( target );
		}
		return TTRUE;
	}

	void OnUnload() override { m_session.reset(); }

	TBOOL OnUpdate( TFLOAT ) override
	{
		if ( !m_ready ) return TTRUE;
		coop::Pose pose;
		if ( ReadLocalPose( pose ) ) m_session->SetLocalPose( pose );
		m_session->Update( NowSeconds() );
		return TTRUE;
	}

	TBOOL HasSettingsUI() override { return TTRUE; }
	TBOOL IsOverlayVisible() override { return TTRUE; }

	void OnImGuiRenderOverlay( AImGUI* ) override
	{
		ImGui::SetNextWindowPos( ImVec2( 8, 8 ) );
		ImGui::SetNextWindowBgAlpha( 0.4f );
		ImGui::Begin( "##bycoop_status", TNULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav );
		ImGui::Text( "BYCoop %s | Alt+` menu", m_ready ? m_session->Status().c_str() : "network unavailable" );
		ImGui::End();
	}

	void OnImGuiRender( AImGUI* ) override
	{
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
		for ( auto& s : g_log ) ImGui::TextUnformatted( s.c_str() );
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
