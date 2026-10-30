// See CoopFlow.h. Retail addresses: barnyard.exe b243e121... (PC RU), from OpenBarnyard
// (Helpers/AGameLoader, GameInterface/AGameStateController, SaveLoadSKU, AHackedQuickSave, APlayerProgress).
#include "pch.h"
#include "CoopFlow.h"
#include "NativeMenu.h"
#include "MainMenuButton.h"
#include "RemotePuppets.h"
#include <HookHelpers.h>
#include <BYardSDK/SDKHooks.h>
#include <BYardSDK/AGameStateController.h>
#include <BYardSDK/APlayerManager.h>
#include <AImGUI.h>
#include <windows.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

TOSHI_NAMESPACE_USING

namespace
{
// --- retail ---
void** const ppProgress        = (void**)0x007b5ea8; // APlayerProgress: m_pSaveData +0x1c, m_iCurrentSlot +0x20
void** const ppQuickSave       = (void**)0x007b4ce8; // AHackedQuickSave
char** const ppProfiles        = (char**)0x007b493c; // APlayerProfileManager: gender +0x10, costume +0x48
char** const ppSimPersist      = (char**)0x00783e64; // ASimPersistData: act +0x64
TUINT* const pSimPictures      = (TUINT*)0x007817ec; // AGameLoader::s_uiSimLoadingPictures
TBOOL* const pMainGameLoaded   = (TBOOL*)0x007810e4; // AGameLoader::s_bMainGameLoaded
const char** const ppHideNames = (const char**)0x007727d8; // costume names (American, Angus, ...)
void** const ppController      = (void**)0x00781788;

constexpr TUINT SLOT_SIZE         = 0xd170;
constexpr TUINT SLOT_QUICKSAVE    = 0x1;
constexpr TUINT QUICKSAVE_SIZE    = 0xb638;
constexpr TUINT SLOT_GENDER       = 0xd158;
constexpr TUINT SLOT_COSTUME      = 0xd15c;
constexpr TUINT SLOT_ACT          = 0xd160;
constexpr TUINT SLOT_PICTURES     = 0xd164;
constexpr TUINT QS_PROFILE        = 0x6818; // AHackedQuickSave::GetBufferDataOffset( BUFFER_PROFILE ): gender, costume, name
constexpr int   NUM_HIDES         = 7;

typedef void( __stdcall* t_ReturnToFrontEnd )( const char*, TBOOL, TBOOL );
typedef void( __thiscall* t_SaveCurrent )( void*, TBOOL, TBOOL, TBOOL );
typedef void( __thiscall* t_QsReset )( void*, TBOOL );
typedef void( __thiscall* t_QsSave )( void* );
typedef TBOOL( __thiscall* t_QsToMemory )( void*, void*, TINT );
const auto ReturnToFrontEnd = (t_ReturnToFrontEnd)0x00423de0;
const auto SaveCurrentState = (t_SaveCurrent)0x0042a230;
const auto QsResetBuffers   = (t_QsReset)0x0062ed60;
const auto QsSaveGame       = (t_QsSave)0x0062edc0;
const auto QsSaveToMemory   = (t_QsToMemory)0x0062dfb0;

CoopFlow::Context g_ctx;
void Log( const std::string& s )
{
	if ( g_ctx.fnLog ) g_ctx.fnLog( s );
}

std::wstring L( const wchar_t* a_wszRu, const wchar_t* a_wszEn ) { return NativeMenu::IsRussian() ? a_wszRu : a_wszEn; }
std::wstring Widen( const std::string& s )
{
	if ( s.empty() ) return L"";
	int          n = MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0 );
	std::wstring w( n, L'\0' );
	MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), &w[ 0 ], n );
	return w;
}

double Now()
{
	static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency( &x ); return x; }();
	LARGE_INTEGER        c;
	QueryPerformanceCounter( &c );
	return double( c.QuadPart ) / double( f.QuadPart );
}

const char* TopStateName()
{
	AGameStateController* pCtrl  = *ppController ? AGameStateController::GetSingleton() : TNULL;
	AGameState*           pState = pCtrl ? pCtrl->GetCurrentState() : TNULL;
	return pState ? pState->GetClass()->GetName() : "";
}
bool TopIs( const char* a_szName ) { return !strcmp( TopStateName(), a_szName ); }

// --- the joiner's cow (bycoop.ini [cow]) ---
std::string IniPath()
{
	char sz[ MAX_PATH ];
	GetModuleFileNameA( NULL, sz, MAX_PATH );
	strcpy( strrchr( sz, '\\' ) + 1, "bycoop.ini" );
	return sz;
}
bool HasCow() { return GetPrivateProfileIntA( "cow", "gender", -1, IniPath().c_str() ) >= 0; }
int  CowGender() { return std::clamp( (int)GetPrivateProfileIntA( "cow", "gender", 0, IniPath().c_str() ), 0, 1 ); }
int  CowHide() { return std::clamp( (int)GetPrivateProfileIntA( "cow", "hide", 0, IniPath().c_str() ), 0, NUM_HIDES - 1 ); }
void SaveCow( int a_iGender, int a_iHide )
{
	WritePrivateProfileStringA( "cow", "gender", std::to_string( a_iGender ).c_str(), IniPath().c_str() );
	WritePrivateProfileStringA( "cow", "hide", std::to_string( a_iHide ).c_str(), IniPath().c_str() );
}

// --- joining ---
enum Phase
{
	PH_NONE,
	PH_ASK_SAVE,    // popup "save your game first?" is open
	PH_SAVING,      // the game's save prompt is running
	PH_JOINING,     // transport join, waiting for our slot
	PH_CREATOR,     // cow creator menu is open
	PH_REQUEST,     // world requested
	PH_WAIT_HOST,   // the host isn't in its world yet
	PH_TO_FRONTEND, // leaving our own game first
	PH_START_LOAD,  // in the main menu, start loading the host's world
	PH_LOADING,     // loading
	PH_IN_WORLD,    // playing in the host's world
};
Phase       g_ePhase = PH_NONE;
double      g_fPhaseTime = 0;
std::string g_strJoinLobby;      // lobby to join once the save question is answered
bool        g_bInHostWorld = false;
bool        g_bTeleport    = false;
std::wstring g_wsPendingMessage; // shown once the main menu is back
coop::WorldData* g_pWorld = nullptr; // pending world for LoadSavedGame
bool        g_bWasHostInLevel = false;

void SetPhase( Phase a_e )
{
	g_ePhase     = a_e;
	g_fPhaseTime = Now();
	Log( "join phase " + std::to_string( (int)a_e ) );
}

// pending invites (corner notice)
struct Notice
{
	coop::ITransport::InviteInfo inv;
	double                       fTime;
};
std::vector<Notice>* g_pNotices = nullptr;
bool g_bF1 = false, g_bF2 = false;

bool KeyEdge( int a_iVk, bool& a_rWas )
{
	const bool bDown = ( GetAsyncKeyState( a_iVk ) & 0x8000 ) != 0;
	const bool bEdge = bDown && !a_rWas;
	a_rWas           = bDown;
	HWND hWnd        = GetForegroundWindow();
	return bEdge && hWnd && GetWindowThreadProcessId( hWnd, NULL ) == GetCurrentThreadId();
}

// --- host: snapshot of the world for a joiner ---
bool SafeQuickSnapshot( uint8_t* a_pQuickSave )
{
	__try
	{
		void* pQs = *ppQuickSave;
		if ( !pQs ) return false;
		QsResetBuffers( pQs, TFALSE );
		QsSaveGame( pQs );
		return QsSaveToMemory( pQs, a_pQuickSave, QUICKSAVE_SIZE ) != 0;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return false;
	}
}

bool BuildWorld( coop::WorldData& a_rOut, const float* a_pPos )
{
	a_rOut.slot.assign( SLOT_SIZE, 0 );
	char* pProgress = (char*)*ppProgress;
	if ( !pProgress ) return false;
	const int iSlot = *(TINT*)( pProgress + 0x20 );
	if ( iSlot >= 0 ) memcpy( a_rOut.slot.data(), *(char**)( pProgress + 0x1c ) + 0x44 + iSlot * SLOT_SIZE, SLOT_SIZE );
	if ( !SafeQuickSnapshot( a_rOut.slot.data() + SLOT_QUICKSAVE ) ) return false;
	a_rOut.slot[ 0 ] = 1; // occupied
	*(TINT*)( a_rOut.slot.data() + SLOT_ACT )      = *ppSimPersist ? *(TINT*)( *ppSimPersist + 0x64 ) : 1;
	*(TUINT*)( a_rOut.slot.data() + SLOT_PICTURES ) = *pSimPictures;
	a_rOut.ready = true;
	a_rOut.level = RemotePuppets::CurrentLevel();
	if ( a_pPos ) memcpy( a_rOut.pos, a_pPos, sizeof( a_rOut.pos ) );
	return true;
}

// --- menus ---
NativeMenu::Menu* g_pLobby = nullptr;
bool              g_bLobbyFromPause = false;
bool              g_bReopenLobby    = false;
std::string       g_strLobbyShape;

enum
{
	ID_CREATE = 1,
	ID_PLAYERS,
	ID_INVITE,
	ID_START,
	ID_LEAVE,
	ID_WAIT,
	ID_BACK = 99,
	ID_FRIEND0 = 100,
	ID_GENDER = 200,
	ID_HIDE,
	ID_DONE,
};

std::wstring PlayersText()
{
	auto*        t = g_ctx.pSession->Transport();
	std::wstring w = L( L"Игроки: ", L"Players: " ) + Widen( g_ctx.pLook->name );
	for ( auto& [ slot, rp ] : g_ctx.pSession->Remotes() ) w += L", " + Widen( rp.look.name );
	( void )t;
	return w;
}

// what decides which buttons the lobby has
std::string LobbyShape()
{
	auto* t = g_ctx.pSession->Transport();
	return std::string( t->IsInSession() ? "S" : "-" ) + ( t->IsHost() ? "H" : "-" ) + ( g_bInHostWorld ? "W" : "-" );
}

void OpenFriends();

void BuildLobby()
{
	std::lock_guard<std::mutex> lock( *g_ctx.pMutex );
	auto*                       t = g_ctx.pSession->Transport();
	NativeMenu::Def             d;
	d.title = L( L"МУЛЬТИПЛЕЕР", L"MULTIPLAYER" );
	const std::wstring wsWide = L"                                                  ";
	if ( !t->IsInSession() )
	{
		d.items.push_back( { ID_CREATE, L( L"СОЗДАТЬ ЛОББИ", L"CREATE LOBBY" ) } );
	}
	else
	{
		d.items.push_back( { ID_PLAYERS, PlayersText(), wsWide } );
		if ( t->IsHost() )
		{
			d.items.push_back( { ID_INVITE, L( L"ПРИГЛАСИТЬ ДРУЗЕЙ", L"INVITE FRIENDS" ) } );
			if ( !g_bLobbyFromPause ) d.items.push_back( { ID_START, L( L"НАЧАТЬ ИГРУ", L"START GAME" ) } );
			d.focus = 1;
		}
		else
			d.items.push_back( { ID_WAIT, L( L"Ожидание хоста...", L"Waiting for the host..." ), wsWide } );
		d.items.push_back( { ID_LEAVE, L( L"ПОКИНУТЬ ЛОББИ", L"LEAVE LOBBY" ) } );
	}
	d.items.push_back( { ID_BACK, L( L"НАЗАД", L"BACK" ) } );
	g_strLobbyShape = LobbyShape();

	d.onActivate = []( NativeMenu::Menu& m, int id ) -> bool {
		std::lock_guard<std::mutex> lock( *g_ctx.pMutex );
		switch ( id )
		{
			case ID_CREATE:
				g_ctx.pSession->Host();
				Log( "lobby: hosting" );
				g_bReopenLobby = true;
				return true;
			case ID_LEAVE:
				g_ctx.pSession->Leave();
				Log( "lobby: left" );
				g_bReopenLobby = true;
				return true;
			case ID_PLAYERS:
			case ID_WAIT: return false;
			default: return true; // invite, start, back
		}
	};
	d.onUpdate = []( NativeMenu::Menu& m ) {
		std::lock_guard<std::mutex> lock( *g_ctx.pMutex );
		if ( LobbyShape() != g_strLobbyShape )
		{
			g_bReopenLobby = true;
			m.Close();
			return;
		}
		if ( g_ctx.pSession->Transport()->IsInSession() )
		{
			std::wstring w = PlayersText();
			if ( m.texts[ 0 ] != w ) m.SetText( ID_PLAYERS, w );
			if ( !g_ctx.pSession->Transport()->IsHost() )
			{
				std::wstring s = g_ePhase == PH_WAIT_HOST || g_ePhase == PH_REQUEST || g_ePhase == PH_JOINING ? L( L"Ожидание хоста...", L"Waiting for the host..." )
				                                                                                              : Widen( g_ctx.pSession->Status() );
				m.SetText( ID_WAIT, s );
			}
		}
	};
	d.onClose = []( int id ) {
		g_pLobby = nullptr;
		if ( id == ID_INVITE ) OpenFriends();
		else if ( id == ID_START ) MainMenuButton::OpenLoadGame();
		else if ( g_bReopenLobby )
		{
			g_bReopenLobby = false;
			BuildLobby();
		}
	};
	g_pLobby = NativeMenu::Push( std::move( d ) );
}

void OpenFriends()
{
	std::vector<coop::ITransport::Friend> aFriends;
	{
		std::lock_guard<std::mutex> lock( *g_ctx.pMutex );
		aFriends = g_ctx.pSession->Transport()->OnlineFriends();
	}
	std::stable_sort( aFriends.begin(), aFriends.end(), []( auto& a, auto& b ) { return a.inGame > b.inGame; } );
	if ( aFriends.size() > 12 ) aFriends.resize( 12 );

	NativeMenu::Def d;
	d.title = L( L"ДРУЗЬЯ В STEAM", L"STEAM FRIENDS" );
	static std::vector<coop::PeerId>* s_pIds = new std::vector<coop::PeerId>();
	s_pIds->clear();
	for ( size_t i = 0; i < aFriends.size(); i++ )
	{
		std::wstring w = Widen( aFriends[ i ].name ) + ( aFriends[ i ].inGame ? L( L"  (в игре)", L"  (in game)" ) : L"" );
		d.items.push_back( { ID_FRIEND0 + (int)i, w, w + L( L"  - приглашён", L"  - invited" ) } );
		s_pIds->push_back( aFriends[ i ].id );
	}
	if ( aFriends.empty() ) d.items.push_back( { ID_WAIT, L( L"Нет друзей в сети", L"No friends online" ) } );
	d.items.push_back( { ID_BACK, L( L"НАЗАД", L"BACK" ) } );
	d.onActivate = []( NativeMenu::Menu& m, int id ) -> bool {
		if ( id == ID_WAIT ) return false;
		if ( id < ID_FRIEND0 || id >= ID_FRIEND0 + (int)s_pIds->size() ) return true;
		bool bOk;
		{
			std::lock_guard<std::mutex> lock( *g_ctx.pMutex );
			bOk = g_ctx.pSession->Transport()->Invite( ( *s_pIds )[ id - ID_FRIEND0 ] );
		}
		std::wstring w = m.def.items[ id - ID_FRIEND0 ].text + ( bOk ? L( L"  - приглашён", L"  - invited" ) : L( L"  - ошибка", L"  - failed" ) );
		m.SetText( id, w );
		return false;
	};
	d.onClose = []( int ) { BuildLobby(); };
	NativeMenu::Push( std::move( d ) );
}

void ContinueJoinAfterCow();

void OpenCowCreator()
{
	static int s_iGender = 0, s_iHide = 0;
	s_iGender = CowGender();
	s_iHide   = CowHide();
	auto GenderText = []() { return L( L"ПОЛ: ", L"GENDER: " ) + ( s_iGender ? L( L"КОРОВА", L"COW" ) : L( L"БЫК", L"BULL" ) ); };
	auto HideText   = []() { return L( L"ОКРАС: ", L"HIDE: " ) + Widen( ppHideNames[ s_iHide ] ? ppHideNames[ s_iHide ] : "?" ); };

	NativeMenu::Def d;
	d.title = L( L"ТВОЯ КОРОВА", L"YOUR COW" );
	d.items.push_back( { ID_GENDER, GenderText(), L( L"ПОЛ: КОРОВА  ", L"GENDER: BULL  " ) } );
	d.items.push_back( { ID_HIDE, HideText(), L( L"ОКРАС: BeefMstr  ", L"HIDE: BeefMstr  " ) } );
	d.items.push_back( { ID_DONE, L( L"ГОТОВО", L"DONE" ) } );
	d.onActivate = [ GenderText, HideText ]( NativeMenu::Menu& m, int id ) -> bool {
		if ( id == ID_GENDER )
		{
			s_iGender ^= 1;
			m.SetText( ID_GENDER, GenderText() );
			return false;
		}
		if ( id == ID_HIDE )
		{
			s_iHide = ( s_iHide + 1 ) % NUM_HIDES;
			m.SetText( ID_HIDE, HideText() );
			return false;
		}
		return true;
	};
	d.onClose = []( int ) {
		SaveCow( s_iGender, s_iHide );
		Log( "cow: gender " + std::to_string( s_iGender ) + " hide " + std::to_string( s_iHide ) );
		ContinueJoinAfterCow();
	};
	NativeMenu::Push( std::move( d ) );
	SetPhase( PH_CREATOR );
}

void ContinueJoinAfterCow()
{
	std::lock_guard<std::mutex> lock( *g_ctx.pMutex );
	g_ctx.pSession->RequestWorld();
	SetPhase( PH_REQUEST );
}

// joined (or about to): the creator first, then ask the host for its world
void AfterJoined()
{
	if ( !HasCow() ) OpenCowCreator();
	else ContinueJoinAfterCow();
}

void StartJoinFlow( const std::string& a_strLobby, bool a_bInLevel )
{
	g_strJoinLobby = a_strLobby;
	if ( a_bInLevel )
	{
		SetPhase( PH_ASK_SAVE );
		NativeMenu::AskYesNo( L( L"СОВМЕСТНАЯ ИГРА", L"CO-OP" ),
		                      L( L"Сохранить твою игру перед входом в мир друга?", L"Save your game before entering your friend's world?" ),
		                      []( bool bYes ) {
			                      if ( bYes )
			                      {
				                      SaveCurrentState( *ppController, TFALSE, TFALSE, TFALSE );
				                      SetPhase( PH_SAVING );
			                      }
			                      else
				                      SetPhase( PH_SAVING ); // nothing to wait for, continues next frame
		                      } );
		return;
	}
	if ( !g_strJoinLobby.empty() )
	{
		std::lock_guard<std::mutex> lock( *g_ctx.pMutex );
		g_ctx.pSession->Leave();
		g_ctx.pSession->Join( g_strJoinLobby );
		g_strJoinLobby.clear();
		SetPhase( PH_JOINING );
	}
	else
		AfterJoined();
}
} // namespace

// --- LoadSavedGame: load the host's world instead of our slot, then put our slot back ---
HOOK( 0x00423c70, AGameLoader_LoadSavedGame, void, TBOOL a_bNow )
{
	if ( !a_bNow || !g_pWorld || !*ppProgress )
	{
		CallOriginal( a_bNow );
		return;
	}
	char* pProgress = (char*)*ppProgress;
	TINT& rSlot     = *(TINT*)( pProgress + 0x20 );
	if ( rSlot < 0 ) rSlot = 0;
	char* pSlot = *(char**)( pProgress + 0x1c ) + 0x44 + rSlot * SLOT_SIZE;

	std::vector<uint8_t> backup( pSlot, pSlot + SLOT_SIZE );
	memcpy( pSlot, g_pWorld->slot.data(), SLOT_SIZE );
	const int iGender = CowGender(), iHide = CowHide();
	*(TINT*)( pSlot + SLOT_GENDER )                              = iGender;
	*(TINT*)( pSlot + SLOT_COSTUME )                             = iHide;
	*(TINT*)( pSlot + SLOT_QUICKSAVE + QS_PROFILE )              = iGender;
	*(TINT*)( pSlot + SLOT_QUICKSAVE + QS_PROFILE + 4 )          = iHide;
	CallOriginal( TTRUE );
	memcpy( pSlot, backup.data(), SLOT_SIZE ); // our own save stays as it was

	delete g_pWorld;
	g_pWorld       = nullptr;
	g_bInHostWorld = true;
	g_bTeleport    = true;
	Log( "world: loading the host's world" );
}

// SaveLoadSKU::StartSave: never write the host's world into the joiner's save file
struct SaveLoadStub
{};
MEMBER_HOOK( 0x006527c0, SaveLoadStub, SaveLoadSKU_StartSave, void, TBOOL a_bUnused, TBOOL a_bDontSave, TBOOL a_bTakeSnapshot )
{
	if ( g_bInHostWorld )
	{
		Log( "save skipped: playing in the host's world" );
		a_bDontSave = TTRUE;
	}
	CallOriginal( a_bUnused, a_bDontSave, a_bTakeSnapshot );
}

void CoopFlow::Install( const Context& a_rCtx )
{
	g_ctx      = a_rCtx;
	g_pNotices = new std::vector<Notice>();
	InstallHook<AGameLoader_LoadSavedGame>();
	InstallHook<SaveLoadSKU_StartSave>();
}

void CoopFlow::OpenLobby( bool a_bFromPauseMenu )
{
	g_bLobbyFromPause = a_bFromPauseMenu;
	BuildLobby();
}

bool CoopFlow::InHostWorld() { return g_bInHostWorld; }

void CoopFlow::Update( bool a_bInLevel, const float* a_pPos )
{
	coop::Session* s = g_ctx.pSession;
	bool           bInSession, bHost, bJoined;
	{
		std::lock_guard<std::mutex> lock( *g_ctx.pMutex );
		auto* t    = s->Transport();
		bInSession = t->IsInSession();
		bHost      = t->IsHost();
		bJoined    = bInSession && !bHost && s->LocalSlot() >= 0;

		coop::ITransport::InviteInfo inv;
		while ( t->PollInvite( inv ) ) g_pNotices->push_back( { inv, Now() } );

		// host: answer world requests, announce our world when we enter it
		const bool bHostInWorld = bHost && a_bInLevel && *pMainGameLoaded;
		coop::PeerId from;
		while ( bHost && s->PopWorldRequest( from ) )
		{
			coop::WorldData w;
			if ( !bHostInWorld || !BuildWorld( w, a_pPos ) )
			{
				w       = coop::WorldData{};
				w.ready = false;
			}
			s->SendWorld( from, w );
		}
		if ( bHostInWorld && !g_bWasHostInLevel ) s->BroadcastWorldReady();
		g_bWasHostInLevel = bHostInWorld;
	}

	// invite notice: F1 accept, F2 decline, 30 s
	const bool bF1 = KeyEdge( VK_F1, g_bF1 ), bF2 = KeyEdge( VK_F2, g_bF2 );
	if ( !g_pNotices->empty() )
	{
		Notice n = g_pNotices->front();
		if ( bF1 )
		{
			g_pNotices->clear();
			Log( "invite accepted: " + n.inv.fromName );
			StartJoinFlow( n.inv.lobby, a_bInLevel && !g_bInHostWorld );
		}
		else if ( bF2 || Now() - n.fTime > 30.0 )
			g_pNotices->erase( g_pNotices->begin() );
	}

	// joined by other means (Steam overlay, UDP test): same flow
	if ( g_ePhase == PH_NONE && bJoined && !g_bInHostWorld && ( a_bInLevel || TopIs( "AFrontEndMainMenuState2" ) ) ) StartJoinFlow( "", a_bInLevel );

	switch ( g_ePhase )
	{
		case PH_SAVING:
			// wait for the save prompt to finish (and the "can't save now" popup)
			if ( Now() - g_fPhaseTime > 0.5 && !TopIs( "SaveLoadSKU" ) && !TopIs( "ASleepCycleChooseState" ) && !TopIs( "AMessagePopupState" ) )
			{
				g_ePhase = PH_NONE;
				StartJoinFlow( g_strJoinLobby, false );
			}
			break;
		case PH_JOINING:
			if ( bJoined ) AfterJoined();
			else if ( Now() - g_fPhaseTime > 20.0 )
			{
				Log( "join timed out" );
				NativeMenu::Message( L( L"СОВМЕСТНАЯ ИГРА", L"CO-OP" ), L( L"Не удалось подключиться к другу.", L"Could not connect to your friend." ) );
				SetPhase( PH_NONE );
			}
			break;
		case PH_REQUEST:
		case PH_WAIT_HOST:
		{
			std::lock_guard<std::mutex> lock( *g_ctx.pMutex );
			coop::WorldData w;
			if ( s->PopWorld( w ) )
			{
				if ( !w.ready || w.slot.size() != SLOT_SIZE ) SetPhase( PH_WAIT_HOST );
				else
				{
					delete g_pWorld;
					g_pWorld = new coop::WorldData( std::move( w ) );
					if ( a_bInLevel )
					{
						ReturnToFrontEnd( TNULL, TTRUE, TFALSE );
						SetPhase( PH_TO_FRONTEND );
					}
					else
						SetPhase( PH_START_LOAD );
				}
			}
			if ( s->PopWorldReady() && g_ePhase == PH_WAIT_HOST )
			{
				s->RequestWorld();
				SetPhase( PH_REQUEST );
			}
			if ( g_ePhase == PH_REQUEST && Now() - g_fPhaseTime > 10.0 ) // lost? ask again
			{
				s->RequestWorld();
				g_fPhaseTime = Now();
			}
			break;
		}
		case PH_TO_FRONTEND:
			if ( TopIs( "AFrontEndMainMenuState2" ) ) SetPhase( PH_START_LOAD );
			break;
		case PH_START_LOAD:
			if ( g_pLobby && !g_pLobby->bClosing ) g_pLobby->Close();
			else if ( TopIs( "AFrontEndMainMenuState2" ) && Now() - g_fPhaseTime > 0.3 )
			{
				CALL( 0x00423c70, void, TBOOL, TFALSE ); // AGameLoader::LoadSavedGame( FALSE ): fade, then load (our hook)
				SetPhase( PH_LOADING );
			}
			break;
		case PH_LOADING:
			if ( a_bInLevel && g_bInHostWorld ) SetPhase( PH_IN_WORLD );
			break;
		default: break;
	}

	// first frames in the host's world: stand next to the host
	if ( g_bTeleport && a_bInLevel && g_bInHostWorld )
	{
		std::lock_guard<std::mutex> lock( *g_ctx.pMutex );
		for ( auto& [ slot, rp ] : s->Remotes() )
			if ( slot == 0 && rp.hasPose && rp.b.levelHash == RemotePuppets::CurrentLevel() )
			{
				APlayerManager* pm = APlayerManager::GetSingleton();
				AUnitPlayer*    p  = pm ? pm->GetPlayer( 0 ) : TNULL;
				if ( p && p->GetSteer() )
				{
					TVector4 v( rp.b.pos[ 0 ] + 2.5f, rp.b.pos[ 1 ], rp.b.pos[ 2 ] + 1.0f, 1.0f );
					typedef void( __thiscall * t_SetPos )( void*, const TVector4& );
					void* pSteer = p->GetSteer();
					( *(t_SetPos*)( *(char**)pSteer + 0x84 ) )( pSteer, v );
					Log( "world: placed next to the host" );
				}
				g_bTeleport = false;
			}
	}

	// session lost while in the host's world: back to the menu
	if ( g_bInHostWorld && !bInSession && a_bInLevel && g_ePhase == PH_IN_WORLD )
	{
		g_wsPendingMessage = L( L"Хост вышел из игры.", L"The host has left the game." );
		ReturnToFrontEnd( TNULL, TTRUE, TFALSE );
		SetPhase( PH_TO_FRONTEND );
		delete g_pWorld;
		g_pWorld = nullptr;
	}
	// back in the main menu: the host's world is gone
	if ( TopIs( "AFrontEndMainMenuState2" ) && g_bInHostWorld && g_ePhase != PH_START_LOAD && g_ePhase != PH_LOADING )
	{
		g_bInHostWorld = false;
		if ( bInSession && !bHost )
		{
			std::lock_guard<std::mutex> lock( *g_ctx.pMutex );
			s->Leave(); // the joiner quit to the menu
		}
		SetPhase( PH_NONE );
	}
	if ( !g_wsPendingMessage.empty() && TopIs( "AFrontEndMainMenuState2" ) )
	{
		NativeMenu::Message( L( L"СОВМЕСТНАЯ ИГРА", L"CO-OP" ), g_wsPendingMessage );
		g_wsPendingMessage.clear();
	}
	if ( !bInSession && ( g_ePhase == PH_REQUEST || g_ePhase == PH_WAIT_HOST ) ) SetPhase( PH_NONE );
}

void CoopFlow::DrawOverlay()
{
	if ( !g_pNotices || g_pNotices->empty() ) return;
	const Notice& n   = g_pNotices->front();
	ImVec2        sz  = ImGui::GetIO().DisplaySize;
	const int     iLeft = (int)( 30.0 - ( Now() - n.fTime ) );
	ImGui::SetNextWindowPos( ImVec2( sz.x - 20, 20 ), ImGuiCond_Always, ImVec2( 1, 0 ) );
	ImGui::SetNextWindowBgAlpha( 0.75f );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding, 10.0f );
	ImGui::PushStyleColor( ImGuiCol_WindowBg, IM_COL32( 92, 54, 24, 230 ) );
	ImGui::PushStyleColor( ImGuiCol_Border, IM_COL32( 255, 200, 0, 255 ) );
	ImGui::Begin( "##bycoop_invite", TNULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav );
	const bool bRu = NativeMenu::IsRussian();
	ImGui::SetWindowFontScale( 1.4f );
	ImGui::TextColored( ImVec4( 1, 0.78f, 0, 1 ), "%s", n.inv.fromName.c_str() );
	ImGui::SetWindowFontScale( 1.15f );
	ImGui::TextUnformatted( bRu ? "приглашает тебя в свой мир" : "invites you to their world" );
	ImGui::Text( bRu ? "F1 - принять     F2 - отклонить   (%d)" : "F1 - accept     F2 - decline   (%d)", iLeft > 0 ? iLeft : 0 );
	ImGui::End();
	ImGui::PopStyleColor( 2 );
	ImGui::PopStyleVar();
}
