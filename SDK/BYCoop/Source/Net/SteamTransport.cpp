// Steam (AppID 480) transport through the flat C API, loaded at runtime.
// Layouts and rules: coop-anything playbook/networking-steam-spacewar.md
#include "Transport.h"
#include <windows.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <map>

namespace coop
{

namespace
{
#pragma pack( push, 8 )
struct CallbackMsg_t
{
	int32_t  hSteamUser;
	int32_t  iCallback;
	uint8_t* pubParam;
	int32_t  cubParam;
};
struct SteamAPICallCompleted_t
{
	uint64_t hAsyncCall;
	int32_t  iCallback;
	uint32_t cubParam;
};
struct LobbyCreated_t
{
	int32_t  eResult;
	uint64_t ulSteamIDLobby;
};
struct LobbyEnter_t
{
	uint64_t ulSteamIDLobby;
	uint32_t rgfChatPermissions;
	bool     bLocked;
	uint32_t EChatRoomEnterResponse;
};
struct LobbyChatUpdate_t
{
	uint64_t ulSteamIDLobby;
	uint64_t ulSteamIDUserChanged;
	uint64_t ulSteamIDMakingChange;
	uint32_t rgfChatMemberStateChange;
};
struct GameRichPresenceJoinRequested_t
{
	uint64_t steamIDFriend;
	char     rgchConnect[ 256 ];
};
struct GameLobbyJoinRequested_t
{
	uint64_t steamIDLobby;
	uint64_t steamIDFriend;
};
#pragma pack( pop )

constexpr int CB_GameLobbyJoinRequested = 333;
constexpr int CB_GameRichPresenceJoinRequested = 337; // "Join game" / accepted invite from the Steam friends list
constexpr int CB_LobbyEnter             = 504;
constexpr int CB_LobbyChatUpdate        = 506;
constexpr int CB_LobbyCreated           = 513;
constexpr int CB_CallCompleted          = 703;
constexpr int CB_SessionRequest         = 1251;
constexpr int CB_SessionFailed          = 1252;

constexpr int SEND_UNRELIABLE = 0;
constexpr int SEND_NONAGLE    = 1;
constexpr int SEND_RELIABLE   = 8;

constexpr const char* LOBBY_KEY = "bycoop";
constexpr const char* LOBBY_VER = "1";

typedef void* Iface;

#define STEAM_FUNCS( X )                                                                                 \
	X( bool, SteamAPI_IsSteamRunning, () )                                                               \
	X( int, SteamAPI_InitFlat, ( char* ) )                                                               \
	X( void, SteamAPI_Shutdown, () )                                                                     \
	X( int32_t, SteamAPI_GetHSteamPipe, () )                                                             \
	X( void, SteamAPI_ManualDispatch_Init, () )                                                          \
	X( void, SteamAPI_ManualDispatch_RunFrame, ( int32_t ) )                                             \
	X( bool, SteamAPI_ManualDispatch_GetNextCallback, ( int32_t, CallbackMsg_t* ) )                      \
	X( void, SteamAPI_ManualDispatch_FreeLastCallback, ( int32_t ) )                                     \
	X( bool, SteamAPI_ManualDispatch_GetAPICallResult, ( int32_t, uint64_t, void*, int, int, bool* ) )   \
	X( Iface, SteamAPI_SteamUser_v023, () )                                                              \
	X( Iface, SteamAPI_SteamMatchmaking_v009, () )                                                       \
	X( Iface, SteamAPI_SteamFriends_v018, () )                                                           \
	X( Iface, SteamAPI_SteamNetworkingMessages_SteamAPI_v002, () )                                       \
	X( Iface, SteamAPI_SteamNetworkingUtils_SteamAPI_v004, () )                                          \
	X( uint64_t, SteamAPI_ISteamUser_GetSteamID, ( Iface ) )                                             \
	X( uint64_t, SteamAPI_ISteamMatchmaking_CreateLobby, ( Iface, int, int ) )                           \
	X( uint64_t, SteamAPI_ISteamMatchmaking_JoinLobby, ( Iface, uint64_t ) )                             \
	X( void, SteamAPI_ISteamMatchmaking_LeaveLobby, ( Iface, uint64_t ) )                                \
	X( bool, SteamAPI_ISteamMatchmaking_SetLobbyData, ( Iface, uint64_t, const char*, const char* ) )    \
	X( const char*, SteamAPI_ISteamMatchmaking_GetLobbyData, ( Iface, uint64_t, const char* ) )          \
	X( uint64_t, SteamAPI_ISteamMatchmaking_GetLobbyOwner, ( Iface, uint64_t ) )                         \
	X( int, SteamAPI_ISteamMatchmaking_GetNumLobbyMembers, ( Iface, uint64_t ) )                         \
	X( uint64_t, SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex, ( Iface, uint64_t, int ) )            \
	X( bool, SteamAPI_ISteamFriends_SetRichPresence, ( Iface, const char*, const char* ) )               \
	X( void, SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog, ( Iface, uint64_t ) )               \
	X( int, SteamAPI_ISteamFriends_GetFriendCount, ( Iface, int ) )                                         \
	X( uint64_t, SteamAPI_ISteamFriends_GetFriendByIndex, ( Iface, int, int ) )                             \
	X( int, SteamAPI_ISteamFriends_GetFriendPersonaState, ( Iface, uint64_t ) )                             \
	X( const char*, SteamAPI_ISteamFriends_GetFriendPersonaName, ( Iface, uint64_t ) )                      \
	X( bool, SteamAPI_ISteamFriends_GetFriendGamePlayed, ( Iface, uint64_t, void* ) )                       \
	X( bool, SteamAPI_ISteamMatchmaking_InviteUserToLobby, ( Iface, uint64_t, uint64_t ) )                  \
	X( void, SteamAPI_SteamNetworkingIdentity_SetSteamID64, ( void*, uint64_t ) )                        \
	X( uint64_t, SteamAPI_SteamNetworkingIdentity_GetSteamID64, ( void* ) )                              \
	X( int, SteamAPI_ISteamNetworkingMessages_SendMessageToUser, ( Iface, const void*, const void*, uint32_t, int, int ) ) \
	X( int, SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel, ( Iface, int, void**, int ) )    \
	X( bool, SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser, ( Iface, const void* ) )           \
	X( bool, SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser, ( Iface, const void* ) )            \
	X( void, SteamAPI_SteamNetworkingMessage_t_Release, ( void* ) )                                      \
	X( bool, SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32, ( Iface, int, int32_t ) )

struct SteamApi
{
#define DECL( ret, name, args ) ret( __cdecl* name ) args = nullptr;
	STEAM_FUNCS( DECL )
#undef DECL
	HMODULE hDll = nullptr;

	bool Load( std::string& err )
	{
		hDll = LoadLibraryA( "steam_api.dll" );
		if ( !hDll )
		{
			err = "steam_api.dll not found next to barnyard.exe";
			return false;
		}
#define LOAD( ret, name, args )                                         \
	name = (decltype( name ))GetProcAddress( hDll, #name );             \
	if ( !name )                                                        \
	{                                                                   \
		err = "steam_api.dll too old, missing export " #name;            \
		return false;                                                   \
	}
		STEAM_FUNCS( LOAD )
#undef LOAD
		return true;
	}
};

struct Identity
{
	uint8_t raw[ 160 ] = {};
};

class SteamTransport : public ITransport
{
public:
	bool Init( std::string& err ) override
	{
		if ( !m_api.Load( err ) ) return false;
		if ( !m_api.SteamAPI_IsSteamRunning() )
		{
			err = "Steam is not running";
			return false;
		}
		// Spacewar
		if ( FILE* f = fopen( "steam_appid.txt", "r" ) ) fclose( f );
		else if ( FILE* w = fopen( "steam_appid.txt", "w" ) )
		{
			fputs( "480", w );
			fclose( w );
		}
		char msg[ 1024 ] = {};
		int  r           = m_api.SteamAPI_InitFlat( msg );
		if ( r != 0 )
		{
			err = std::string( "SteamAPI_InitFlat failed: " ) + msg;
			return false;
		}
		m_api.SteamAPI_ManualDispatch_Init();
		m_pipe    = m_api.SteamAPI_GetHSteamPipe();
		m_user    = m_api.SteamAPI_SteamUser_v023();
		m_mm      = m_api.SteamAPI_SteamMatchmaking_v009();
		m_friends = m_api.SteamAPI_SteamFriends_v018();
		m_msgs    = m_api.SteamAPI_SteamNetworkingMessages_SteamAPI_v002();
		m_utils   = m_api.SteamAPI_SteamNetworkingUtils_SteamAPI_v004();
		m_self    = m_api.SteamAPI_ISteamUser_GetSteamID( m_user );
		// k_ESteamNetworkingConfig: 9 SendBufferSize, 47 RecvBufferSize, 49 RecvMaxMessageSize
		const int32_t big = 32 * 1024 * 1024;
		m_api.SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32( m_utils, 9, big );
		m_api.SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32( m_utils, 47, big );
		m_api.SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32( m_utils, 49, big );
		m_ok = true;
		CheckLaunchArgs();
		return true;
	}

	void Shutdown() override
	{
		if ( !m_ok ) return;
		Leave();
		m_api.SteamAPI_Shutdown();
		m_ok = false;
	}

	PeerId      LocalId() const override { return m_self; }
	std::string Describe() const override
	{
		char b[ 96 ];
		snprintf( b, sizeof( b ), "Steam lobby %llu", (unsigned long long)m_lobby );
		return b;
	}

	bool Host( int maxPlayers ) override
	{
		if ( !m_ok || m_lobby ) return false;
		m_wantHost = true;
		m_api.SteamAPI_ISteamMatchmaking_CreateLobby( m_mm, 1 /* friends only */, maxPlayers );
		Log( "Creating lobby..." );
		return true;
	}

	bool Join( const std::string& target ) override
	{
		if ( !m_ok ) return false;
		uint64_t id = _strtoui64( target.c_str(), nullptr, 10 );
		if ( !id ) return false;
		m_wantHost = false;
		m_api.SteamAPI_ISteamMatchmaking_JoinLobby( m_mm, id );
		Log( "Joining lobby " + target );
		return true;
	}

	void Leave() override
	{
		if ( !m_lobby ) return;
		for ( PeerId p : m_members )
			if ( p != m_self ) CloseSession( p );
		m_api.SteamAPI_ISteamMatchmaking_LeaveLobby( m_mm, m_lobby );
		m_api.SteamAPI_ISteamFriends_SetRichPresence( m_friends, "connect", "" );
		m_lobby = 0;
		m_members.clear();
	}

	bool   IsInSession() const override { return m_lobby != 0; }
	bool   IsHost() const override { return m_lobby && HostId() == m_self; }
	PeerId HostId() const override { return m_lobby ? m_api.SteamAPI_ISteamMatchmaking_GetLobbyOwner( m_mm, m_lobby ) : 0; }
	std::vector<PeerId> Members() const override { return m_members; }

	std::vector<Friend> OnlineFriends() override
	{
		std::vector<Friend> out;
		if ( !m_ok ) return out;
		int n = m_api.SteamAPI_ISteamFriends_GetFriendCount( m_friends, 4 /* immediate */ );
		for ( int i = 0; i < n; ++i )
		{
			uint64_t id = m_api.SteamAPI_ISteamFriends_GetFriendByIndex( m_friends, i, 4 );
			if ( m_api.SteamAPI_ISteamFriends_GetFriendPersonaState( m_friends, id ) == 0 ) continue; // offline
			uint8_t info[ 32 ] = {};                                                                    // FriendGameInfo_t
			bool    inGame     = m_api.SteamAPI_ISteamFriends_GetFriendGamePlayed( m_friends, id, info ) && ( *(uint64_t*)info & 0xffffff ) == 480;
			const char* nm     = m_api.SteamAPI_ISteamFriends_GetFriendPersonaName( m_friends, id );
			out.push_back( { id, nm ? nm : "?", inGame } );
		}
		return out;
	}

	bool Invite( PeerId a_id ) override
	{
		if ( !m_lobby ) return false;
		bool ok = m_api.SteamAPI_ISteamMatchmaking_InviteUserToLobby( m_mm, m_lobby, a_id );
		Log( std::string( ok ? "invited " : "invite failed " ) + std::to_string( a_id ) );
		return ok;
	}

	void OpenInviteDialog() override
	{
		if ( m_lobby ) m_api.SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog( m_friends, m_lobby );
	}

	bool Send( PeerId to, const void* data, size_t size, bool reliable ) override
	{
		if ( !m_ok || !IsMember( to ) ) return false;
		Identity id;
		m_api.SteamAPI_SteamNetworkingIdentity_SetSteamID64( id.raw, to );
		int flags = SEND_NONAGLE | ( reliable ? SEND_RELIABLE : SEND_UNRELIABLE );
		for ( int tries = 0; tries < 20; ++tries )
		{
			int r = m_api.SteamAPI_ISteamNetworkingMessages_SendMessageToUser( m_msgs, id.raw, data, (uint32_t)size, flags, 0 );
			if ( r == 1 /* k_EResultOK */ ) return true;
			if ( !reliable || r != 25 /* LimitExceeded */ ) return false;
			Sleep( 5 );
		}
		return false;
	}

	void Receive( std::vector<Packet>& out ) override
	{
		if ( !m_ok ) return;
		void* msgs[ 64 ];
		int   n;
		while ( ( n = m_api.SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel( m_msgs, 0, msgs, 64 ) ) > 0 )
		{
			for ( int i = 0; i < n; ++i )
			{
				uint8_t* m    = (uint8_t*)msgs[ i ];
				uint8_t* data = *(uint8_t**)( m + 0 );
				int32_t  size = *(int32_t*)( m + 4 );
				PeerId   from = SenderOf( m );
				if ( from && IsMember( from ) && data && size > 0 )
					out.push_back( { from, std::vector<uint8_t>( data, data + size ) } );
				m_api.SteamAPI_SteamNetworkingMessage_t_Release( m );
			}
		}
	}

	void Pump() override
	{
		if ( !m_ok ) return;
		m_api.SteamAPI_ManualDispatch_RunFrame( m_pipe );
		CallbackMsg_t cb;
		while ( m_api.SteamAPI_ManualDispatch_GetNextCallback( m_pipe, &cb ) )
		{
			if ( cb.iCallback == CB_CallCompleted )
			{
				auto*                done = (SteamAPICallCompleted_t*)cb.pubParam;
				std::vector<uint8_t> buf( done->cubParam );
				bool                 failed = false;
				if ( m_api.SteamAPI_ManualDispatch_GetAPICallResult( m_pipe, done->hAsyncCall, buf.data(), (int)buf.size(), done->iCallback, &failed ) && !failed )
					Handle( done->iCallback, buf.data() );
			}
			else
				Handle( cb.iCallback, cb.pubParam );
			m_api.SteamAPI_ManualDispatch_FreeLastCallback( m_pipe );
		}
	}

private:
	void Log( const std::string& s )
	{
		if ( onLog ) onLog( s );
	}

	// 32-bit steam_api.dll: sender identity at 12 (observed), headers suggest 16. Probe both.
	PeerId SenderOf( uint8_t* m )
	{
		if ( m_senderOffset )
			return m_api.SteamAPI_SteamNetworkingIdentity_GetSteamID64( m + m_senderOffset );
		for ( int off : { 12, 16 } )
		{
			PeerId p = m_api.SteamAPI_SteamNetworkingIdentity_GetSteamID64( m + off );
			if ( p )
			{
				m_senderOffset = off;
				Log( "Steam message sender offset = " + std::to_string( off ) );
				return p;
			}
		}
		return 0;
	}

	bool IsMember( PeerId p ) const { return std::find( m_members.begin(), m_members.end(), p ) != m_members.end(); }

	void CloseSession( PeerId p )
	{
		Identity id;
		m_api.SteamAPI_SteamNetworkingIdentity_SetSteamID64( id.raw, p );
		m_api.SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser( m_msgs, id.raw );
	}

	void RefreshMembers()
	{
		std::vector<PeerId> now;
		int                 n = m_api.SteamAPI_ISteamMatchmaking_GetNumLobbyMembers( m_mm, m_lobby );
		for ( int i = 0; i < n; ++i ) now.push_back( m_api.SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex( m_mm, m_lobby, i ) );
		for ( PeerId p : now )
			if ( p != m_self && !IsMember( p ) && onPeerJoined ) onPeerJoined( p );
		for ( PeerId p : m_members )
			if ( p != m_self && std::find( now.begin(), now.end(), p ) == now.end() )
			{
				CloseSession( p );
				if ( onPeerLeft ) onPeerLeft( p );
			}
		m_members = now;
	}

	void Handle( int id, void* p )
	{
		switch ( id )
		{
			case CB_LobbyCreated:
			{
				auto* c = (LobbyCreated_t*)p;
				if ( c->eResult != 1 || m_lobby == c->ulSteamIDLobby ) break; // arrives twice
				m_lobby = c->ulSteamIDLobby;
				m_api.SteamAPI_ISteamMatchmaking_SetLobbyData( m_mm, m_lobby, LOBBY_KEY, LOBBY_VER );
				char conn[ 64 ];
				snprintf( conn, sizeof( conn ), "+connect_lobby %llu", (unsigned long long)m_lobby );
				m_api.SteamAPI_ISteamFriends_SetRichPresence( m_friends, "connect", conn );
				Log( "Lobby created: " + std::to_string( m_lobby ) );
				RefreshMembers();
				break;
			}
			case CB_LobbyEnter:
			{
				auto* e = (LobbyEnter_t*)p;
				if ( e->EChatRoomEnterResponse != 1 )
				{
					Log( "Lobby join failed, code " + std::to_string( e->EChatRoomEnterResponse ) );
					break;
				}
				const char* v = m_api.SteamAPI_ISteamMatchmaking_GetLobbyData( m_mm, e->ulSteamIDLobby, LOBBY_KEY );
				if ( !v || strcmp( v, LOBBY_VER ) != 0 )
				{
					Log( "Not a BYCoop lobby (or version mismatch), leaving" );
					m_api.SteamAPI_ISteamMatchmaking_LeaveLobby( m_mm, e->ulSteamIDLobby );
					break;
				}
				m_lobby = e->ulSteamIDLobby;
				Log( "Entered lobby " + std::to_string( m_lobby ) );
				RefreshMembers();
				break;
			}
			case CB_LobbyChatUpdate:
			{
				auto* u = (LobbyChatUpdate_t*)p;
				if ( u->ulSteamIDLobby == m_lobby ) RefreshMembers();
				break;
			}
			case CB_GameLobbyJoinRequested:
			{
				auto* j = (GameLobbyJoinRequested_t*)p;
				Leave();
				Join( std::to_string( j->steamIDLobby ) );
				break;
			}
			case CB_GameRichPresenceJoinRequested:
			{
				auto*       j = (GameRichPresenceJoinRequested_t*)p;
				const char* k = strstr( j->rgchConnect, "+connect_lobby " );
				if ( k )
				{
					Leave();
					Join( std::string( k + 15, strcspn( k + 15, " \"" ) ) );
				}
				break;
			}
			case CB_SessionRequest:
			{
				// identity at offset 0; host accepts lobby members, clients accept only the host
				PeerId from = m_api.SteamAPI_SteamNetworkingIdentity_GetSteamID64( p );
				if ( IsMember( from ) && ( IsHost() || from == HostId() ) )
					m_api.SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser( m_msgs, p );
				break;
			}
			case CB_SessionFailed:
				Log( "Steam session failed" );
				break;
		}
	}

	void CheckLaunchArgs()
	{
		const char* cl = GetCommandLineA();
		const char* k  = strstr( cl, "+connect_lobby " );
		if ( k ) Join( std::string( k + 15, strcspn( k + 15, " \"" ) ) );
	}

	SteamApi            m_api;
	bool                m_ok   = false;
	int32_t             m_pipe = 0;
	Iface               m_user = nullptr, m_mm = nullptr, m_friends = nullptr, m_msgs = nullptr, m_utils = nullptr;
	PeerId              m_self  = 0;
	uint64_t            m_lobby = 0;
	bool                m_wantHost = false;
	int                 m_senderOffset = 0;
	std::vector<PeerId> m_members;
};
} // namespace

ITransport* CreateSteamTransport() { return new SteamTransport(); }

} // namespace coop
