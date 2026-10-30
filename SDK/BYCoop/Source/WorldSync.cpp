// See WorldSync.h. Retail addresses: barnyard.exe b243e121... (PC RU); layouts from OpenBarnyard
// (Helpers/AGameTimeManager, Helpers/ARegrowthManager, MiniGame/AShakeTreeMicroGame,
// Units/AInventoryGroupComponent, AInventoryComponent, AInventoryItem, Player/APlayerSimProfile).
#include "pch.h"
#include "WorldSync.h"
#include "RemotePuppets.h"
#include <BYardSDK/SDKHooks.h>
#include <BYardSDK/APlayerManager.h>
#include <Toshi/TClass.h>
#include <HookHelpers.h>
#include <Math/TVector4.h>
#include <windows.h>
#include <cstring>
#include <cmath>
#include <map>
#include <vector>

TOSHI_NAMESPACE_USING

namespace
{
// ---------------------------------------------------------------- retail
void** const ppTimeMgr     = (void**)0x00783d3c; // AGameTimeManager
void** const ppWeather     = (void**)0x00783ecc; // AWeatherManager (current +4, next +8)
void** const ppRegrowth    = (void**)0x007922e0; // ARegrowthManager
void** const ppRegrowTypes = (void**)0x007962e8; // ARegrowthManager::ms_pTypes (m_iNumEnums +8)
void** const ppUnitMgr     = (void**)0x0077e344; // AUnitManager
char** const ppProfiles    = (char**)0x007b493c; // APlayerProfileManager (main profile at +4)
void** const ppStrPool     = (void**)0x007ce230; // TPString8Pool**
TBOOL* const pMainGameLoaded = (TBOOL*)0x007810e4;

// AGameTimeManager
constexpr TUINT TM_TOTAL = 0x20, TM_YEAR = 0x24, TM_MONTH = 0x28, TM_DAY = 0x2c, TM_WEEKDAY = 0x30, TM_PHASE = 0x34, TM_DAYTIME = 0x38, TM_PHASETIME = 0x48;
// ARegrowthManager
constexpr TUINT RG_TYPELISTS = 0x1c; // TypeLists[61] { T2SList llEmpty, llGrowing }
constexpr TUINT GO_POS = 0x14, GO_GROWTIME = 0x24;
constexpr TFLOAT TIME_EMPTY = -100000000.0f;
// APlayerSimProfile (main profile = profile manager + 4)
constexpr TUINT PROFILE_PICKUPS = 0x1c;

template <class T> T& At( void* p, TUINT off ) { return *(T*)( (char*)p + off ); }

double Now()
{
	static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency( &x ); return x; }();
	LARGE_INTEGER        c;
	QueryPerformanceCounter( &c );
	return double( c.QuadPart ) / double( f.QuadPart );
}

// ---------------------------------------------------------------- context
coop::Session*                              g_pSession = nullptr;
std::mutex*                                 g_pMutex   = nullptr;
std::function<void( const std::string& )>* g_pLog     = nullptr;
void Log( const std::string& s )
{
	if ( g_pLog ) ( *g_pLog )( s );
}

bool g_bActive  = false; // in the shared world this frame
bool g_bHost    = false;
bool g_bApplying = false; // applying a remote change: our hooks must not echo it
bool g_bHostSameLevel = false; // joiner: the host is on our map (its regrowth snapshots apply)
int  g_iLocalSlot = -1;

enum EvType : uint8_t
{
	EV_TIME = 1,
	EV_TAKE,      // regrowth object taken
	EV_REGROWTH,  // host's regrowth snapshot
	EV_DROP,      // a unit (shaken fruit) appeared
	EV_GONE,      // that unit was picked up / is gone
	EV_INV_DELTA, // joiner -> host: inventory and money changes
	EV_INV,       // host -> all: inventory and money
	EV_PICK,      // a unit placed in the world (note, recipe...) was picked up
	EV_DROPPOS,   // where a shared drop is while it flies / rolls
	EV_NPC,       // host -> all: where the animals / characters are
};

struct Writer
{
	std::vector<uint8_t> b;
	template <class T> Writer& operator<<( const T& v )
	{
		const uint8_t* p = (const uint8_t*)&v;
		b.insert( b.end(), p, p + sizeof( T ) );
		return *this;
	}
	Writer& Str( const char* s )
	{
		size_t l = strlen( s ); uint8_t n = (uint8_t)( l > 255 ? 255 : l );
		*this << n;
		b.insert( b.end(), s, s + n );
		return *this;
	}
};
struct Reader
{
	const uint8_t* p;
	size_t         n, i = 0;
	bool           ok = true;
	template <class T> Reader& operator>>( T& v )
	{
		if ( i + sizeof( T ) > n ) { ok = false; return *this; }
		memcpy( &v, p + i, sizeof( T ) );
		i += sizeof( T );
		return *this;
	}
	std::string Str()
	{
		uint8_t len = 0;
		*this >> len;
		if ( !ok || i + len > n ) { ok = false; return ""; }
		std::string s( (const char*)p + i, len );
		i += len;
		return s;
	}
};

void Send( const Writer& w, bool a_bReliable )
{
	std::lock_guard<std::mutex> lock( *g_pMutex );
	g_pSession->SendEvent( w.b.data(), w.b.size(), a_bReliable );
}

uint32_t Level() { return RemotePuppets::CurrentLevel(); }

// pooled strings (the game's TPString8)
struct PStr
{
	void* p = nullptr;
	explicit PStr( const char* s )
	{
		void* pPool = *(void**)*ppStrPool;
		CALL_THIS( 0x006bc3e0, void*, void, pPool, void**, &p, const char*, s, bool*, nullptr ); // TPString8Pool::Get
	}
	~PStr()
	{
		if ( p && --*(TINT*)p == 0 ) CALL_THIS( 0x006bc380, void*, void, p );
	}
	void* Donate() // a by-value TPString8 argument: the callee releases it
	{
		void* r = p;
		p       = nullptr;
		return r;
	}
};
const char* PooledText( void* a_pPooled ) { return a_pPooled ? *(const char**)( (char*)a_pPooled + 4 ) : ""; }
const char* UnitTypeName( void* a_pUnit )
{
	void* pType = a_pUnit ? At<void*>( a_pUnit, 0x28 ) : nullptr;
	return pType ? PooledText( At<void*>( pType, 0x0c ) ) : "";
}

// ================================================================ time
double g_fNextTime = 0;

void SendTime()
{
	void* tm = *ppTimeMgr;
	if ( !tm ) return;
	Writer w;
	w << (uint8_t)EV_TIME << At<TINT>( tm, TM_YEAR ) << At<TINT>( tm, TM_MONTH ) << At<TINT>( tm, TM_DAY ) << At<TINT>( tm, TM_WEEKDAY ) << At<TINT>( tm, TM_PHASE )
	  << At<TFLOAT>( tm, TM_DAYTIME ) << At<TFLOAT>( tm, TM_PHASETIME ) << At<TFLOAT>( tm, TM_TOTAL );
	void* wm = *ppWeather;
	w << ( wm ? At<TINT>( wm, 4 ) : -1 ) << ( wm ? At<TINT>( wm, 8 ) : -1 );
	Send( w, false );
}

void SafeSetWeather( void* wm, TINT a_iWeather )
{
	__try
	{
		CALL_THIS( 0x0054c550, void*, void, wm, TINT, a_iWeather ); // AWeatherManager::SetCurrentWeather
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
	}
}

void SafeChangePhase( void* tm, TINT a_iPhase )
{
	__try
	{
		CALL_THIS( 0x005463a0, void*, void, tm, TINT, a_iPhase, TBOOL, TFALSE, TBOOL, TTRUE ); // ChangePhaseOfDay: fires the event (lighting)
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
	}
}

void ApplyTime( Reader& r )
{
	TINT   iYear, iMonth, iDay, iWeekDay, iPhase;
	TFLOAT fDayTime, fPhaseTime, fTotal;
	r >> iYear >> iMonth >> iDay >> iWeekDay >> iPhase >> fDayTime >> fPhaseTime >> fTotal;
	TINT iWeather = -1, iNextWeather = -1;
	Reader rw = r;
	rw >> iWeather >> iNextWeather;
	if ( rw.ok && iWeather >= 0 && *ppWeather )
	{
		void* wm = *ppWeather;
		if ( At<TINT>( wm, 4 ) != iWeather ) SafeSetWeather( wm, iWeather );
		At<TINT>( wm, 8 ) = iNextWeather;
	}
	void* tm = *ppTimeMgr;
	if ( !r.ok || !tm || iPhase < 0 || iPhase > 4 ) return;
	const bool bDayChanged = At<TINT>( tm, TM_DAY ) != iDay || At<TINT>( tm, TM_MONTH ) != iMonth || At<TINT>( tm, TM_YEAR ) != iYear;
	if ( At<TINT>( tm, TM_PHASE ) != iPhase ) SafeChangePhase( tm, iPhase );
	// small drift is left to run, a jump (sleep, loading) is applied
	if ( bDayChanged || fabsf( At<TFLOAT>( tm, TM_DAYTIME ) - fDayTime ) > 1.5f || fabsf( At<TFLOAT>( tm, TM_TOTAL ) - fTotal ) > 1.5f )
	{
		At<TINT>( tm, TM_YEAR )       = iYear;
		At<TINT>( tm, TM_MONTH )      = iMonth;
		At<TINT>( tm, TM_DAY )        = iDay;
		At<TINT>( tm, TM_WEEKDAY )    = iWeekDay;
		At<TFLOAT>( tm, TM_DAYTIME )  = fDayTime;
		At<TFLOAT>( tm, TM_PHASETIME ) = fPhaseTime;
		At<TFLOAT>( tm, TM_TOTAL )    = fTotal;
		if ( bDayChanged ) Log( "time: day " + std::to_string( iDay ) + "." + std::to_string( iMonth ) );
	}
}

// ================================================================ regrowth
struct RegrowthObj
{
	TINT  iType;
	void* pObj;
	bool  bGrowing;
};
TFLOAT Key( void* a_pObj ) { return At<TFLOAT>( a_pObj, GO_POS + 8 ) * 1000.0f + At<TFLOAT>( a_pObj, GO_POS ); } // GrowthObject::GetSaveKey

template <class F> void ForEachRegrowth( F&& fn )
{
	void* rg = *ppRegrowth;
	void* ty = *ppRegrowTypes;
	if ( !rg || !ty ) return;
	const TINT n = At<TINT>( ty, 8 ) < 61 ? At<TINT>( ty, 8 ) : 61;
	for ( TINT t = 0; t < n; t++ )
		for ( int l = 0; l < 2; l++ )
		{
			char* pRoot = (char*)rg + RG_TYPELISTS + t * 8 + l * 4;
			int   guard = 0;
			for ( char* p = *(char**)pRoot; p && p != pRoot && guard < 4000; p = *(char**)p, guard++ ) fn( t, (void*)p, l == 1 );
		}
}

std::map<float, double>* g_pRecentTakes = nullptr; // keys we took locally (ignore the host's older snapshot)

bool SafeTake( TINT a_iType, void* a_pObj )
{
	__try
	{
		return CALL_THIS( 0x005e6c30, void*, TBOOL, *ppRegrowth, TINT, a_iType, void*, a_pObj ) != 0; // TakeObject
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return false;
	}
}
bool SafeStartGrowing( TINT a_iType, void* a_pObj, TFLOAT a_fAge )
{
	__try
	{
		return CALL_THIS( 0x005e5090, void*, TBOOL, *ppRegrowth, TINT, a_iType, void*, a_pObj, TFLOAT, a_fAge ) != 0; // StartGrowing
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return false;
	}
}

void SendTake( TFLOAT a_fKey )
{
	if ( !g_bActive || g_bApplying ) return;
	( *g_pRecentTakes )[ a_fKey ] = Now();
	Writer w;
	w << (uint8_t)EV_TAKE << Level() << a_fKey;
	Send( w, true );
}

void ApplyTake( Reader& r )
{
	uint32_t uiLevel;
	TFLOAT   fKey;
	r >> uiLevel >> fKey;
	if ( !r.ok || uiLevel != Level() ) return;
	TINT  iType = -1;
	void* pObj  = nullptr;
	ForEachRegrowth( [ & ]( TINT t, void* o, bool bGrowing ) {
		if ( bGrowing && !pObj && Key( o ) == fKey )
		{
			iType = t;
			pObj  = o;
		}
	} );
	if ( !pObj ) return;
	g_bApplying = true;
	SafeTake( iType, pObj );
	g_bApplying = false;
}

double g_fNextRegrowth = 0;

std::vector<uint8_t>* g_pLastRegrowth = nullptr;
double                g_fLastRegrowthSent = 0;
void SendRegrowth()
{
	Writer w;
	w << (uint8_t)EV_REGROWTH << Level() << At<TFLOAT>( *ppTimeMgr, TM_TOTAL );
	size_t uiCountAt = w.b.size();
	w << (uint16_t)0;
	uint16_t n = 0;
	ForEachRegrowth( [ & ]( TINT t, void* o, bool bGrowing ) {
		w << Key( o ) << (uint8_t)t << ( bGrowing ? At<TFLOAT>( o, GO_GROWTIME ) : TIME_EMPTY );
		n++;
	} );
	memcpy( &w.b[ uiCountAt ], &n, 2 );
	// only when something grew / was taken (the total time in the header always changes: skip it)
	const size_t uiHeader = 1 + 4 + 4;
	std::vector<uint8_t> body( w.b.begin() + uiHeader, w.b.end() );
	if ( !g_pLastRegrowth ) g_pLastRegrowth = new std::vector<uint8_t>();
	if ( body == *g_pLastRegrowth && Now() - g_fLastRegrowthSent < 5.0 ) return;
	*g_pLastRegrowth     = std::move( body );
	g_fLastRegrowthSent = Now();
	Send( w, true );
}

void ApplyRegrowth( Reader& r )
{
	uint32_t uiLevel;
	TFLOAT   fHostTotal;
	uint16_t n = 0;
	r >> uiLevel >> fHostTotal >> n;
	if ( !r.ok || uiLevel != Level() || !*ppTimeMgr ) return;
	std::map<std::pair<float, int>, RegrowthObj> local;
	ForEachRegrowth( [ & ]( TINT t, void* o, bool bGrowing ) { local[ { Key( o ), t } ] = { t, o, bGrowing }; } );
	const TFLOAT fNow   = At<TFLOAT>( *ppTimeMgr, TM_TOTAL );
	int          iTaken = 0, iGrown = 0;
	g_bApplying         = true;
	for ( uint16_t i = 0; i < n && r.ok; i++ )
	{
		TFLOAT  fKey, fGrow;
		uint8_t t;
		r >> fKey >> t >> fGrow;
		auto it = local.find( { fKey, (int)t } );
		if ( !r.ok || it == local.end() ) continue;
		auto rt = g_pRecentTakes->find( fKey );
		if ( rt != g_pRecentTakes->end() && Now() - rt->second < 4.0 ) continue; // our own take hasn't reached the host yet
		RegrowthObj& o = it->second;
		if ( fGrow == TIME_EMPTY )
		{
			if ( o.bGrowing && SafeTake( o.iType, o.pObj ) ) iTaken++;
		}
		else if ( !o.bGrowing )
		{
			if ( SafeStartGrowing( o.iType, o.pObj, fNow - fGrow ) ) iGrown++;
		}
		else if ( fabsf( At<TFLOAT>( o.pObj, GO_GROWTIME ) - fGrow ) > 1.0f )
			At<TFLOAT>( o.pObj, GO_GROWTIME ) = fGrow;
	}
	g_bApplying = false;
	if ( iTaken || iGrown ) Log( "regrowth: host state applied (" + std::to_string( iTaken ) + " taken, " + std::to_string( iGrown ) + " grown)" );
}

// ================================================================ shaken fruits (networked units)
std::map<uint32_t, void*>* g_pNetUnits = nullptr; // net id -> unit
std::map<void*, uint32_t>* g_pUnitNets = nullptr;
uint32_t g_uiNextNetId = 1;
bool     g_bShakeSpawn = false;
int      g_iPlayers    = 1; // players in the session (bikes)
int      g_iInSpawnCoin = 0;      // inside ACoinSpawner::SpawnCoin (coins spin, bounce and fade: spawn the same way)
bool     g_bCapture    = false;   // catch the next spawned unit (remote coin)
void*    g_pCaptured   = nullptr;
double   g_fDropUntil  = 0; // units spawned until then came out of something we broke/shook (shared)
std::map<uint32_t, double>* g_pOwnDrops = nullptr; // our drops -> spawn time (their position is streamed)

void Track( uint32_t a_uiId, void* a_pUnit )
{
	( *g_pNetUnits )[ a_uiId ] = a_pUnit;
	( *g_pUnitNets )[ a_pUnit ] = a_uiId;
}
bool Untrack( void* a_pUnit, uint32_t& a_rId )
{
	auto it = g_pUnitNets->find( a_pUnit );
	if ( it == g_pUnitNets->end() ) return false;
	a_rId = it->second;
	g_pNetUnits->erase( it->second );
	g_pUnitNets->erase( it );
	return true;
}

void SendGone( uint32_t a_uiId )
{
	Writer w;
	w << (uint8_t)EV_GONE << a_uiId;
	Send( w, true );
}

void* SafeSpawnByName( const char* a_szType, const TVector4& a_rPos )
{
	__try
	{
		TVector4 vDir( 1, 0, 0, 0 );
		return CALL_THIS( 0x00411970, void*, void*, *ppUnitMgr, const char*, a_szType, const TVector4&, a_rPos, const TVector4&, vDir );
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return nullptr;
	}
}
void SafeDestroyUnit( void* a_pUnit )
{
	__try
	{
		CALL_THIS( 0x00410fe0, void*, void, *ppUnitMgr, void*, a_pUnit );
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
	}
}

void SafeSpawnCoin( void** a_ppType, const TVector4& a_rPos )
{
	__try
	{
		CALL( 0x0066ef10, void, void**, a_ppType, const TVector4&, a_rPos );
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
	}
}

void ApplyDrop( Reader& r )
{
	uint32_t uiId, uiLevel;
	TFLOAT   x, y, z;
	r >> uiId >> uiLevel >> x >> y >> z;
	std::string strType = r.Str();
	if ( !r.ok || uiLevel != Level() || strType.empty() || g_pNetUnits->count( uiId ) ) return;
	uint8_t uiKind = 0;
	Reader  rk     = r;
	rk >> uiKind;
	g_bApplying = true;
	void* pUnit = nullptr;
	if ( rk.ok && uiKind == 1 )
	{
		// ACoinSpawner::SpawnCoin( type, pos ) (__stdcall): spins, bounces, fades out like the original
		PStr     type( strType.c_str() );
		TVector4 vPos( x, y, z, 1.0f );
		g_bCapture  = true;
		g_pCaptured = nullptr;
		SafeSpawnCoin( &type.p, vPos );
		g_bCapture = false;
		pUnit      = g_pCaptured;
	}
	else
		pUnit = SafeSpawnByName( strType.c_str(), TVector4( x, y, z, 1.0f ) );
	g_bApplying = false;
	if ( pUnit ) Track( uiId, pUnit );
}

void ApplyGone( Reader& r )
{
	uint32_t uiId;
	r >> uiId;
	if ( !r.ok ) return;
	auto it = g_pNetUnits->find( uiId );
	if ( it == g_pNetUnits->end() ) return;
	void*    pUnit = it->second;
	uint32_t dummy;
	Untrack( pUnit, dummy );
	g_bApplying = true;
	SafeDestroyUnit( pUnit );
	g_bApplying = false;
}

// A unit just appeared: share it if it came out of a crate/tree we took
void OnSpawned( void* a_pUnit, const TVector4& a_rPos )
{
	if ( !a_pUnit || g_bApplying || !g_bActive || Now() > g_fDropUntil || g_pUnitNets->count( a_pUnit ) ) return;
	uint32_t uiId = ( (uint32_t)( g_iLocalSlot & 0xff ) << 24 ) | ( g_uiNextNetId++ & 0xffffff );
	Track( uiId, a_pUnit );
	( *g_pOwnDrops )[ uiId ] = Now();
	Writer w;
	w << (uint8_t)EV_DROP << uiId << Level() << a_rPos.x << a_rPos.y << a_rPos.z;
	w.Str( UnitTypeName( a_pUnit ) );
	w << (uint8_t)( g_iInSpawnCoin > 0 ? 1 : 0 );
	Send( w, true );
}

const TVector4* UnitPosition( void* a_pUnit )
{
	void* pSteer = a_pUnit ? At<void*>( a_pUnit, 0x1c ) : nullptr;
	if ( !pSteer ) return nullptr;
	typedef const TVector4*( __thiscall * t_GetPos )( void* );
	return ( *(t_GetPos*)( *(char**)pSteer + 0xac ) )( pSteer ); // ASteer::GetPosition
}

void SendDropPositions()
{
	static double s_fNext = 0;
	if ( Now() < s_fNext ) return;
	s_fNext = Now() + 0.05;
	for ( auto it = g_pOwnDrops->begin(); it != g_pOwnDrops->end(); )
	{
		auto u = g_pNetUnits->find( it->first );
		if ( u == g_pNetUnits->end() || Now() - it->second > 5.0 )
		{
			it = g_pOwnDrops->erase( it );
			continue;
		}
		const TVector4* p = UnitPosition( u->second );
		if ( p )
		{
			Writer w;
			w << (uint8_t)EV_DROPPOS << it->first << p->x << p->y << p->z;
			Send( w, false );
		}
		++it;
	}
}

void ApplyDropPos( Reader& r )
{
	uint32_t uiId;
	TFLOAT   x, y, z;
	r >> uiId >> x >> y >> z;
	auto it = g_pNetUnits->find( uiId );
	if ( !r.ok || it == g_pNetUnits->end() ) return;
	void* pSteer = At<void*>( it->second, 0x1c );
	if ( !pSteer ) return;
	typedef void( __thiscall * t_SetV )( void*, const TVector4& );
	( *(t_SetV*)( *(char**)pSteer + 0x84 ) )( pSteer, TVector4( x, y, z, 1.0f ) ); // ASteer::SetPosition
}


// ================================================================ NPCs (host-driven)
// The host's animals and characters are the real ones. A joiner keeps its own units that match the
// host's (same type, close by), turns their AI off and moves them to the host's positions; host units
// it doesn't have are spawned, its own extra ones are hidden.
struct NpcTarget
{
	void*    pUnit = nullptr;
	TVector4 vPos, vDir;
	TFLOAT   fSpeed = 0;
	double   fSeen  = 0;
	bool     bHasPos = false;
};
std::map<uint32_t, NpcTarget>* g_pNpcs     = nullptr; // host unit id -> our unit
std::map<void*, uint32_t>*     g_pNpcUnits = nullptr; // our unit -> host unit id
std::map<void*, bool>*         g_pNpcHidden = nullptr; // our extra units we hid
uint32_t                       g_uiNpcLevel = 0;
double                         g_fNpcLevelAt = 0;

bool IsCharacter( void* a_pUnit )
{
	typedef Toshi::TClass*( __thiscall * t_GetClass )( void* );
	Toshi::TClass* pClass = ( *(t_GetClass**)a_pUnit )[ 0 ]( a_pUnit );
	return pClass && pClass->IsA( (Toshi::TClass*)0x0077e6f8 ); // ACharacterUnit
}

bool IsSyncedNpc( void* a_pUnit, void* a_pLocalPlayer )
{
	const TUINT uiFlags = At<TUINT>( a_pUnit, 0x14 );
	if ( a_pUnit == a_pLocalPlayer || ( uiFlags & 0x21 ) ) return false; // dying / not in queries (held in an inventory)
	if ( RemotePuppets::IsPuppet( a_pUnit ) || g_pUnitNets->count( a_pUnit ) ) return false;
	if ( !At<void*>( a_pUnit, 0x1c ) ) return false; // no steer
	return IsCharacter( a_pUnit );
}

template <class F> void ForEachUnit( F&& fn )
{
	if ( !*ppUnitMgr ) return;
	char* pRoot = (char*)*ppUnitMgr + 0x38;
	char* pEnd  = pRoot - 4;
	int   guard = 0;
	for ( char* pUnit = *(char**)( pRoot + 4 ); pUnit && pUnit != pEnd && guard < 4000; pUnit = *(char**)( pUnit + 8 ), guard++ ) fn( (void*)pUnit );
}

void* LocalPlayerUnit()
{
	APlayerManager* pm = APlayerManager::GetSingleton();
	return pm ? (void*)pm->GetPlayer( 0 ) : nullptr;
}

TFLOAT MeasuredSpeed( void* a_pSteer )
{
	typedef TFLOAT( __thiscall * t_GetF )( void* );
	return ( *(t_GetF*)( *(char**)a_pSteer + 0xd4 ) )( a_pSteer );
}

struct NpcSent
{
	TFLOAT x, y, z, fSpeed;
	double fAt;
};
std::map<void*, NpcSent>* g_pNpcSent = nullptr; // host: what the joiners last got for each unit

struct NpcOut
{
	TINT        iId;
	TFLOAT      x, y, z, dx, dz, fSpeed;
	const char* szType;
};

// host: the units that moved (or weren't sent for a second)
bool SafeCollectNpcs( std::vector<NpcOut>& a_rOut )
{
	__try
	{
		void*        pPlayer = LocalPlayerUnit();
		const double now     = Now();
		ForEachUnit( [ & ]( void* u ) {
			if ( !IsSyncedNpc( u, pPlayer ) ) return;
			void*           pSteer = At<void*>( u, 0x1c );
			typedef void*( __thiscall * t_GetKin )( void* );
			if ( !( *(t_GetKin*)( *(char**)pSteer + 0x114 ) )( pSteer ) ) return; // ridden / scripted: no kinematics
			const TVector4* p      = UnitPosition( u );
			if ( !p ) return;
			typedef const TVector4*( __thiscall * t_GetV )( void* );
			const TVector4* d      = ( *(t_GetV*)( *(char**)pSteer + 0xb8 ) )( pSteer );
			const TFLOAT    fSpeed = MeasuredSpeed( pSteer );
			NpcSent&        last   = ( *g_pNpcSent )[ u ];
			const TFLOAT    mx = p->x - last.x, my = p->y - last.y, mz = p->z - last.z;
			const bool      bMoved = mx * mx + my * my + mz * mz > 0.0004f || fabsf( fSpeed - last.fSpeed ) > 0.05f;
			if ( !bMoved && now - last.fAt < 1.0 ) return;
			last = { p->x, p->y, p->z, fSpeed, now };
			a_rOut.push_back( { At<TINT>( u, 0x24 ), p->x, p->y, p->z, d ? d->x : 0.0f, d ? d->z : 1.0f, fSpeed, UnitTypeName( u ) } );
		} );
		return true;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return false;
	}
}

void SendNpcs()
{
	static double s_fNext = 0;
	if ( g_iPlayers < 2 || Now() < s_fNext ) return;
	s_fNext = Now() + 1.0 / 20.0;
	std::vector<NpcOut> out;
	if ( !SafeCollectNpcs( out ) )
	{
		g_pNpcSent->clear();
		return;
	}
	for ( size_t i = 0; i < out.size(); i += 40 ) // ~1.5 KB per packet
	{
		Writer         w;
		const uint16_t n = (uint16_t)( out.size() - i < 40 ? out.size() - i : 40 );
		w << (uint8_t)EV_NPC << Level() << n;
		for ( size_t j = i; j < i + n; j++ )
		{
			const NpcOut& o = out[ j ];
			w << o.iId << o.x << o.y << o.z << o.dx << o.dz << o.fSpeed;
			w.Str( o.szType ? o.szType : "" );
		}
		Send( w, false );
	}
	if ( g_pNpcSent->size() > 2000 ) g_pNpcSent->clear();
}

void SetUnitVisible( void* a_pUnit, bool a_bVisible )
{
	__try
	{
		CALL_THIS( 0x00415ac0, void*, void, a_pUnit, TBOOL, a_bVisible ? TTRUE : TFALSE ); // AAnimatedUnit::SetVisible
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
	}
}

void TakeOverNpc( void* a_pUnit )
{
	At<TUINT>( a_pUnit, 0x14 ) &= ~0x4u; // FLAGS_AI_ENABLED off: the host's AI drives it
}

void ApplyNpcs( Reader& r )
{
	uint32_t uiLevel;
	uint16_t n = 0;
	r >> uiLevel >> n;
	if ( !r.ok || uiLevel != Level() ) return;
	if ( uiLevel != g_uiNpcLevel ) // other map: the old pointers are gone
	{
		g_pNpcs->clear();
		g_pNpcUnits->clear();
		g_pNpcHidden->clear();
		g_uiNpcLevel  = uiLevel;
		g_fNpcLevelAt = Now();
	}
	// our candidates
	std::map<void*, std::string> local;
	void* pPlayer = LocalPlayerUnit();
	ForEachUnit( [ & ]( void* u ) {
		if ( IsSyncedNpc( u, pPlayer ) ) local[ u ] = UnitTypeName( u );
	} );
	// forget units that are gone
	for ( auto it = g_pNpcs->begin(); it != g_pNpcs->end(); )
		if ( !local.count( it->second.pUnit ) )
		{
			g_pNpcUnits->erase( it->second.pUnit );
			it = g_pNpcs->erase( it );
		}
		else
			++it;
	const double now = Now();
	for ( uint16_t i = 0; i < n && r.ok; i++ )
	{
		TINT   iId;
		TFLOAT x, y, z, dx, dz, fSpeed;
		r >> iId >> x >> y >> z >> dx >> dz >> fSpeed;
		std::string strType = r.Str();
		if ( !r.ok ) break;
		NpcTarget& t = ( *g_pNpcs )[ (uint32_t)iId ];
		if ( !t.pUnit )
		{
			// the closest unit of ours of that type, else a new one
			void*  pBest = nullptr;
			TFLOAT fBest = 25.0f * 25.0f;
			for ( auto& [ u, type ] : local )
			{
				if ( type != strType || g_pNpcUnits->count( u ) ) continue;
				const TVector4* p = UnitPosition( u );
				if ( !p ) continue;
				const TFLOAT d = ( p->x - x ) * ( p->x - x ) + ( p->z - z ) * ( p->z - z );
				if ( d < fBest )
				{
					fBest = d;
					pBest = u;
				}
			}
			if ( !pBest )
			{
				g_bApplying = true;
				pBest       = SafeSpawnByName( strType.c_str(), TVector4( x, y, z, 1.0f ) );
				g_bApplying = false;
				if ( pBest ) local[ pBest ] = strType;
			}
			if ( !pBest )
			{
				g_pNpcs->erase( (uint32_t)iId );
				continue;
			}
			t.pUnit = pBest;
			( *g_pNpcUnits )[ pBest ] = (uint32_t)iId;
			if ( g_pNpcHidden->count( pBest ) )
			{
				SetUnitVisible( pBest, true );
				g_pNpcHidden->erase( pBest );
			}
			TakeOverNpc( pBest );
		}
		t.vPos    = TVector4( x, y, z, 1.0f );
		t.vDir    = TVector4( dx, 0.0f, dz, 0.0f );
		t.fSpeed  = fSpeed;
		t.fSeen   = now;
		t.bHasPos = true;
	}
	// host units not heard of for 3 s are gone there: hide ours
	for ( auto it = g_pNpcs->begin(); it != g_pNpcs->end(); )
		if ( now - it->second.fSeen > 3.0 )
		{
			SetUnitVisible( it->second.pUnit, false );
			( *g_pNpcHidden )[ it->second.pUnit ] = true;
			g_pNpcUnits->erase( it->second.pUnit );
			it = g_pNpcs->erase( it );
		}
		else
			++it;
	// ours that the host doesn't have (every host unit arrives within a second): hidden, AI off
	if ( now - g_fNpcLevelAt < 2.0 ) return;
	for ( auto& [ u, type ] : local )
		if ( !g_pNpcUnits->count( u ) && !g_pNpcHidden->count( u ) )
		{
			TakeOverNpc( u );
			SetUnitVisible( u, false );
			( *g_pNpcHidden )[ u ] = true;
		}
}

// every frame: glide our copies to the host's positions
void MoveNpcs( TFLOAT a_fDelta )
{
	typedef void( __thiscall * t_SetV )( void*, const TVector4& );
	const TFLOAT k = a_fDelta * 12.0f > 1.0f ? 1.0f : a_fDelta * 12.0f;
	for ( auto& [ id, t ] : *g_pNpcs )
	{
		if ( !t.bHasPos || !t.pUnit ) continue;
		void* pSteer = At<void*>( t.pUnit, 0x1c );
		const TVector4* p = UnitPosition( t.pUnit );
		if ( !pSteer || !p ) continue;
		TVector4 v = *p;
		const TFLOAT dx = t.vPos.x - v.x, dy = t.vPos.y - v.y, dz = t.vPos.z - v.z;
		if ( dx * dx + dy * dy + dz * dz > 6.0f * 6.0f ) v = t.vPos; // too far: jump
		else
		{
			v.x += dx * k;
			v.y += dy * k;
			v.z += dz * k;
		}
		v.w = 1.0f;
		At<TUINT>( t.pUnit, 0x14 ) &= ~0x4u; // something may have turned the AI back on
		( *(t_SetV*)( *(char**)pSteer + 0x84 ) )( pSteer, v );
		if ( t.vDir.x != 0.0f || t.vDir.z != 0.0f ) ( *(t_SetV*)( *(char**)pSteer + 0x8c ) )( pSteer, t.vDir );
		// walk / run animation from the measured speed (as the remote players' cows)
		typedef void*( __thiscall * t_GetKin )( void* );
		typedef void( __thiscall * t_SetF )( void*, TFLOAT );
		void* pKin = ( *(t_GetKin*)( *(char**)pSteer + 0x114 ) )( pSteer );
		if ( pKin )
		{
			( *(t_SetF*)( *(char**)pKin + 0x1c ) )( pKin, t.fSpeed );
			( *(t_SetF*)( *(char**)pKin + 0x20 ) )( pKin, t.fSpeed );
		}
	}
}

bool SafeMoveNpcs( TFLOAT a_fDelta )
{
	__try
	{
		MoveNpcs( a_fDelta );
		return true;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return false;
	}
}

// a unit placed in the world was picked up here: remove the same one for the others
void SendPick( void* a_pUnit )
{
	const TVector4* p = UnitPosition( a_pUnit );
	if ( !p ) return;
	Writer w;
	w << (uint8_t)EV_PICK << Level() << p->x << p->y << p->z;
	w.Str( UnitTypeName( a_pUnit ) );
	Send( w, true );
	Log( std::string( "picked: " ) + UnitTypeName( a_pUnit ) );
}

void ApplyPick( Reader& r )
{
	uint32_t uiLevel;
	TFLOAT   x, y, z;
	r >> uiLevel >> x >> y >> z;
	std::string strType = r.Str();
	if ( !r.ok || uiLevel != Level() || strType.empty() || !*ppUnitMgr ) return;
	// closest unit of that type (units: TNodeList at manager +0x38, node at unit +4)
	char*  pRoot = (char*)*ppUnitMgr + 0x38;
	char*  pEnd  = pRoot - 4;
	void*  pBest = nullptr;
	TFLOAT fBest = 2.5f * 2.5f;
	int    guard = 0;
	for ( char* pUnit = *(char**)( pRoot + 4 ); pUnit && pUnit != pEnd && guard < 4000; pUnit = *(char**)( pUnit + 8 ), guard++ )
	{
		if ( strType != UnitTypeName( pUnit ) ) continue;
		const TVector4* p = UnitPosition( pUnit );
		if ( !p ) continue;
		const TFLOAT d = ( p->x - x ) * ( p->x - x ) + ( p->y - y ) * ( p->y - y ) + ( p->z - z ) * ( p->z - z );
		if ( d < fBest )
		{
			fBest = d;
			pBest = pUnit;
		}
	}
	if ( !pBest ) return;
	g_bApplying = true;
	SafeDestroyUnit( pBest );
	g_bApplying = false;
	Log( "picked by another player: " + strType );
}


// ================================================================ inventory + money
typedef std::map<std::string, int> Counts;
Counts* g_pBaseline     = nullptr; // what we last agreed with the host (joiner) / last broadcast (host)
int     g_iBaseMoney    = 0;
bool    g_bHaveBaseline = false;
double  g_fNextInv      = 0;

void* PlayerInventory()
{
	APlayerManager* pm = APlayerManager::GetSingleton();
	void*           pPlayer = pm ? (void*)pm->GetPlayer( 0 ) : nullptr;
	if ( !pPlayer || RemotePuppets::IsPuppet( pPlayer ) ) return nullptr;
	return CALL_THIS( 0x00413cd0, void*, void*, pPlayer, void*, (void*)0x007b4a54 ); // AUnit::FindComponent( &TGetClass( AInventoryGroupComponent ) )
}

int g_iInvError = 0; // why the last read failed (diagnostics)
bool SafeReadInventory( Counts& a_rOut )
{
	__try
	{
		g_iInvError  = 1;
		void* pGroup = PlayerInventory();
		if ( !pGroup ) return false;
		g_iInvError = 2;
		void* pData = At<void*>( pGroup, 0x10 );
		char* pCats = At<char*>( pGroup, 0x14 );
		if ( !pData || !pCats ) return false;
		g_iInvError = 3;
		const TINT n = At<TINT>( pData, 0x0c );
		for ( TINT c = 0; c < n && c < 16; c++ )
		{
			char* pCat  = pCats + c * 0x2c;
			char* pRoot = pCat + 0x14;     // TQList root node (m_pNext, m_pPrev)
			char* pEnd  = pRoot - 4;       // the root seen as an AInventoryItem
			int   guard = 0;
			// the links point at the TNode inside the item (item + 4), see AInventoryComponent::GetItem 0x623d50
			for ( char* pItem = *(char**)pRoot - 4; pItem && pItem != pEnd && guard < 256; pItem = *(char**)( pItem + 4 ) - 4, guard++ )
			{
				void* pProxy = At<void*>( pItem, 0x0c );
				void* pUnit  = pProxy ? At<void*>( pProxy, 0x14 ) : nullptr;
				const char* sz = UnitTypeName( pUnit );
				if ( *sz ) a_rOut[ sz ] += At<TINT>( pItem, 0x10 );
			}
		}
		g_iInvError = 0;
		return true;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return false;
	}
}

int Money() { return *ppProfiles ? At<TINT>( *ppProfiles + 4, PROFILE_PICKUPS ) : 0; }

bool SafeChangeItems( void* a_pGroup, void* a_pType, int a_iDelta )
{
	__try
	{
		typedef TBOOL( __thiscall * t_Add )( void*, void* const*, TINT, void* );
		typedef TINT( __thiscall * t_Remove )( void*, void* const*, TINT, TBOOL );
		void** vt = *(void***)a_pGroup;
		if ( a_iDelta > 0 ) ( (t_Add)vt[ 0x40 / 4 ] )( a_pGroup, &a_pType, a_iDelta, nullptr ); // AddItems( type, n, "" )
		else ( (t_Remove)vt[ 0x50 / 4 ] )( a_pGroup, &a_pType, -a_iDelta, TTRUE );           // RemoveItems( type, n, destroy )
		return true;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return false;
	}
}

void ChangeItems( const std::string& a_strType, int a_iDelta )
{
	void* pGroup = PlayerInventory();
	if ( !pGroup || !a_iDelta ) return;
	PStr type( a_strType.c_str() );
	if ( !SafeChangeItems( pGroup, type.p, a_iDelta ) ) Log( "inventory: change failed for " + a_strType );
}

void SafeSetMoney( int a_iValue )
{
	__try
	{
		if ( *ppProfiles ) CALL_THIS( 0x00620f80, void*, void, *ppProfiles + 4, TINT, a_iValue, TBOOL, TFALSE, TBOOL, TTRUE ); // SetPickups
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
	}
}
void SafeAddMoney( int a_iDelta )
{
	__try
	{
		if ( *ppProfiles ) CALL_THIS( 0x00621170, void*, void, *ppProfiles + 4, TINT, a_iDelta ); // AddPickups
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
	}
}

void SendInventory( const Counts& a_rCounts, int a_iMoney )
{
	Writer w;
	w << (uint8_t)EV_INV << (int32_t)a_iMoney << (uint16_t)a_rCounts.size();
	for ( auto& [ k, v ] : a_rCounts ) w.Str( k.c_str() ) << (int32_t)v;
	Send( w, true );
}

void UpdateInventory()
{
	if ( Now() < g_fNextInv ) return;
	g_fNextInv = Now() + 0.1;
	Counts now;
	const bool bRead = SafeReadInventory( now );
	static double s_fNextDiag = 0;
	if ( Now() >= s_fNextDiag )
	{
		s_fNextDiag = Now() + 10.0;
		std::string str = bRead ? "inventory:" : "inventory: read failed at step " + std::to_string( g_iInvError );
		for ( auto& [ k, v ] : now ) str += " " + k + "=" + std::to_string( v );
		Log( str + " money=" + std::to_string( Money() ) );
	}
	const int iMoney = Money();
	if ( !bRead ) now = *g_pBaseline; // (money still syncs)
	if ( !g_bHaveBaseline )
	{
		*g_pBaseline    = now;
		g_iBaseMoney    = iMoney;
		g_bHaveBaseline = true;
		if ( g_bHost ) SendInventory( now, iMoney );
		return;
	}
	static double s_fNextFull = 0;
	if ( g_bHost && Now() >= s_fNextFull ) // joiners that came later get it too
	{
		s_fNextFull = Now() + 10.0;
		SendInventory( now, iMoney );
	}
	// joiner: money changes go to the host as they happen (see the APlayerSimProfile hooks)
	if ( now == *g_pBaseline && ( iMoney == g_iBaseMoney || !g_bHost ) ) return;
	if ( g_bHost )
	{
		SendInventory( now, iMoney ); // our inventory is the shared one
	}
	else
	{
		// what we picked up / used since we last matched the host
		Writer w;
		w << (uint8_t)EV_INV_DELTA << (int32_t)0;
		Counts diff;
		for ( auto& [ k, v ] : now )
			if ( v != ( g_pBaseline->count( k ) ? ( *g_pBaseline )[ k ] : 0 ) ) diff[ k ] = v - ( g_pBaseline->count( k ) ? ( *g_pBaseline )[ k ] : 0 );
		for ( auto& [ k, v ] : *g_pBaseline )
			if ( !now.count( k ) ) diff[ k ] = -v;
		w << (uint16_t)diff.size();
		for ( auto& [ k, v ] : diff ) w.Str( k.c_str() ) << (int32_t)v;
		Send( w, true );
		Log( "inventory: sent " + std::to_string( diff.size() ) + " change(s) to the host" );
	}
	*g_pBaseline = now;
	g_iBaseMoney = iMoney;
}

void ApplyInventoryDelta( Reader& r ) // host
{
	int32_t  iMoney = 0;
	uint16_t n      = 0;
	r >> iMoney >> n;
	if ( !r.ok ) return;
	if ( iMoney ) SafeAddMoney( iMoney );
	for ( uint16_t i = 0; i < n && r.ok; i++ )
	{
		std::string k = r.Str();
		int32_t     v = 0;
		r >> v;
		if ( r.ok ) ChangeItems( k, v );
	}
	g_fNextInv = 0; // broadcast the result soon
}

void ApplyInventory( Reader& r ) // joiner: become the host's inventory
{
	int32_t  iMoney = 0;
	uint16_t n      = 0;
	r >> iMoney >> n;
	Counts host;
	for ( uint16_t i = 0; i < n && r.ok; i++ )
	{
		std::string k = r.Str();
		int32_t     v = 0;
		r >> v;
		if ( r.ok ) host[ k ] = v;
	}
	if ( !r.ok ) return;
	Counts now;
	if ( !SafeReadInventory( now ) ) return;
	for ( auto& [ k, v ] : host )
		if ( ( now.count( k ) ? now[ k ] : 0 ) != v ) ChangeItems( k, v - ( now.count( k ) ? now[ k ] : 0 ) );
	for ( auto& [ k, v ] : now )
		if ( !host.count( k ) ) ChangeItems( k, -v );
	if ( Money() != iMoney )
	{
		g_bApplying = true;
		SafeSetMoney( iMoney );
		g_bApplying = false;
	}
	// whatever we have now matches the host
	Counts after;
	SafeReadInventory( after );
	*g_pBaseline    = after;
	g_iBaseMoney    = Money();
	g_bHaveBaseline = true;
}

// ================================================================ dispatch
void Handle( const coop::GameEvent& e )
{
	if ( e.data.empty() ) return;
	Reader r{ e.data.data(), e.data.size() };
	uint8_t t = 0;
	r >> t;
	switch ( t )
	{
		case EV_TIME:
			if ( !g_bHost && e.slot == 0 ) ApplyTime( r );
			break;
		case EV_TAKE: ApplyTake( r ); break;
		case EV_REGROWTH:
			if ( !g_bHost && e.slot == 0 ) ApplyRegrowth( r );
			break;
		case EV_DROP: ApplyDrop( r ); break;
		case EV_PICK: ApplyPick( r ); break;
		case EV_DROPPOS: ApplyDropPos( r ); break;
		case EV_NPC:
			if ( !g_bHost && e.slot == 0 ) ApplyNpcs( r );
			break;
		case EV_GONE: ApplyGone( r ); break;
		case EV_INV_DELTA:
			if ( g_bHost ) ApplyInventoryDelta( r );
			break;
		case EV_INV:
			if ( !g_bHost && e.slot == 0 ) ApplyInventory( r );
			break;
	}
}
} // namespace

// ---------------------------------------------------------------- hooks
struct RegrowthStub
{};
// TakeObject: someone took a fruit/flower/crate here
MEMBER_HOOK( 0x005e6c30, RegrowthStub, ARegrowthManager_TakeObject, TBOOL, TINT a_iType, void* a_pObject )
{
	const TBOOL bRes = CallOriginal( a_iType, a_pObject );
	if ( bRes && a_pObject )
	{
		SendTake( Key( a_pObject ) );
		if ( !g_bApplying ) g_fDropUntil = Now() + 0.5; // the crate's contents / the fruit spawn next
	}
	return bRes;
}
// TakeRandomObject: a fruit shaken out of a tree (a unit is spawned right after)
MEMBER_HOOK( 0x005e4fa0, RegrowthStub, ARegrowthManager_TakeRandomObject, TBOOL, TUINT a_uiGroup, TVector4* a_pOutPos )
{
	const TBOOL bRes = CallOriginal( a_uiGroup, a_pOutPos );
	if ( bRes && a_pOutPos )
	{
		SendTake( a_pOutPos->z * 1000.0f + a_pOutPos->x );
		if ( !g_bApplying ) g_fDropUntil = Now() + 0.5;
	}
	return bRes;
}
// Random growth only happens in the host's world; the joiners get it from the snapshots
MEMBER_HOOK( 0x005e51b0, RegrowthStub, ARegrowthManager_GrowObjects, TINT, TUINT a_uiGroup, TINT a_iCount, TFLOAT a_fAge, TINT a_iType )
{
	if ( g_bActive && !g_bHost && !g_bApplying && g_bHostSameLevel ) return 0;
	return CallOriginal( a_uiGroup, a_iCount, a_fAge, a_iType );
}
MEMBER_HOOK( 0x005e5340, RegrowthStub, ARegrowthManager_GrowGroup, TINT, TUINT a_uiGroup )
{
	if ( g_bActive && !g_bHost && !g_bApplying ) return 0;
	return CallOriginal( a_uiGroup );
}
MEMBER_HOOK( 0x005e53a0, RegrowthStub, ARegrowthManager_GrowAllInType, void, TINT a_iType, TINT a_iCount )
{
	if ( g_bActive && !g_bHost && !g_bApplying && g_bHostSameLevel ) return;
	CallOriginal( a_iType, a_iCount );
}
MEMBER_HOOK( 0x005e5ea0, RegrowthStub, ARegrowthManager_RespawnInstant, void )
{
	if ( g_bActive && !g_bHost && !g_bApplying ) return;
	CallOriginal();
}
MEMBER_HOOK( 0x005e5f60, RegrowthStub, ARegrowthManager_RespawnDayRespawn, void )
{
	if ( g_bActive && !g_bHost && !g_bApplying ) return;
	CallOriginal();
}

struct UnitMgrStub
{};
// SpawnUnit overloads: what comes out of a crate / tree we took is shared (OnSpawned)
MEMBER_HOOK( 0x00411a70, UnitMgrStub, AUnitManager_SpawnUnitType, void*, void* a_pType, const TVector4& a_rPos, const TVector4& a_rDir )
{
	void* pUnit = CallOriginal( a_pType, a_rPos, a_rDir );
	if ( g_bCapture && !g_pCaptured ) g_pCaptured = pUnit;
	OnSpawned( pUnit, a_rPos );
	return pUnit;
}
MEMBER_HOOK( 0x00411970, UnitMgrStub, AUnitManager_SpawnUnitName, void*, const char* a_szType, const TVector4& a_rPos, const TVector4& a_rDir )
{
	void* pUnit = CallOriginal( a_szType, a_rPos, a_rDir );
	OnSpawned( pUnit, a_rPos );
	return pUnit;
}
MEMBER_HOOK( 0x004119d0, UnitMgrStub, AUnitManager_SpawnUnitPString, void*, void* a_pType, const TVector4& a_rPos, const TVector4& a_rDir )
{
	void* pUnit = CallOriginal( a_pType, a_rPos, a_rDir );
	OnSpawned( pUnit, a_rPos );
	return pUnit;
}
MEMBER_HOOK( 0x00411a30, UnitMgrStub, AUnitManager_SpawnUnitTransform, void*, void* a_pType, const TVector4* a_pTransform )
{
	void* pUnit = CallOriginal( a_pType, a_pTransform );
	if ( a_pTransform ) OnSpawned( pUnit, *a_pTransform ); // AUnitTransform starts with the position
	return pUnit;
}
// DestroyUnitImmediate: a shared fruit is gone (faded out / destroyed)
MEMBER_HOOK( 0x00410f00, UnitMgrStub, AUnitManager_DestroyUnitImmediate, void, void* a_pUnit )
{
	uint32_t uiId;
	if ( a_pUnit && g_pUnitNets && Untrack( a_pUnit, uiId ) && !g_bApplying ) SendGone( uiId );
	CallOriginal( a_pUnit );
}

struct BikeStub
{};
// the game parks one bike per rack (the racks are all over the farm): park ours next to the first one
void AlignBikes( void* a_pMgr )
{
	char*      pBikes = (char*)a_pMgr + 0x20; // Bike[ 56 ] { type, rack, unit, parked }
	const TINT n      = *(TINT*)( (char*)a_pMgr + 0x544 );
	if ( n < 2 || g_iPlayers < 2 ) return;
	void*           pFirst  = *(void**)( pBikes + 8 );
	void*           pSteer0 = pFirst ? At<void*>( pFirst, 0x1c ) : nullptr;
	const TVector4* pPos    = UnitPosition( pFirst );
	if ( !pPos || !pSteer0 || !*(TBOOL*)( pBikes + 0x0c ) ) return;
	typedef const TVector4*( __thiscall * t_GetV )( void* );
	typedef void( __thiscall * t_SetV )( void*, const TVector4& );
	const TVector4* pDir = ( *(t_GetV*)( *(char**)pSteer0 + 0xb8 ) )( pSteer0 );
	TVector4        vSide( pDir ? -pDir->z : 1.0f, 0.0f, pDir ? pDir->x : 0.0f, 0.0f );
	for ( TINT i = 1; i < n; i++ )
	{
		if ( !*(TBOOL*)( pBikes + i * 0x10 + 0x0c ) ) continue; // ridden / left somewhere
		void* pBike  = *(void**)( pBikes + i * 0x10 + 8 );
		void* pSteer = pBike ? At<void*>( pBike, 0x1c ) : nullptr;
		if ( !pSteer ) continue;
		TVector4 v( pPos->x + vSide.x * 1.6f * i, pPos->y, pPos->z + vSide.z * 1.6f * i, 1.0f );
		( *(t_SetV*)( *(char**)pSteer + 0x84 ) )( pSteer, v );
		if ( pDir ) ( *(t_SetV*)( *(char**)pSteer + 0x8c ) )( pSteer, *pDir );
	}
}
// ABikeManager::SpawnBikesAtRacks (ASimState spawns 1): one bike per player
MEMBER_HOOK( 0x00540a10, BikeStub, ABikeManager_SpawnBikesAtRacks, void, TINT a_iNumBikes )
{
	if ( a_iNumBikes == 1 && g_iPlayers > 1 )
	{
		a_iNumBikes = g_iPlayers;
		Log( "bikes: " + std::to_string( a_iNumBikes ) );
	}
	CallOriginal( a_iNumBikes );
	AlignBikes( this );
}
// ABikeManager::RespawnBikes: every 2 s the parked bikes go back to the racks closest to the player
MEMBER_HOOK( 0x00540410, BikeStub, ABikeManager_RespawnBikes, void, TFLOAT a_fDelta, TBOOL a_bForce )
{
	CallOriginal( a_fDelta, a_bForce );
	AlignBikes( this );
}

struct InventoryStub
{};
// one of the categories of our cow's inventory
bool IsPlayerCategory( void* a_pCategory )
{
	void* pGroup = PlayerInventory();
	if ( !pGroup ) return false;
	char* pCats = At<char*>( pGroup, 0x14 );
	void* pData = At<void*>( pGroup, 0x10 );
	if ( !pCats || !pData ) return false;
	return (char*)a_pCategory >= pCats && (char*)a_pCategory < pCats + At<TINT>( pData, 0x0c ) * 0x2c;
}
// AInventoryComponent::AddUnit: a shared fruit was picked up (the unit goes into the inventory)
MEMBER_HOOK( 0x006247d0, InventoryStub, AInventoryComponent_AddUnit, TBOOL, void* a_pUnit, TINT a_iCount, void* a_strInventory )
{
	uint32_t uiId;
	if ( a_pUnit && g_pUnitNets && Untrack( a_pUnit, uiId ) && !g_bApplying ) SendGone( uiId );
	return CallOriginal( a_pUnit, a_iCount, a_strInventory );
}

// Money: what the joiner earns / spends goes to the host (its money is everyone's)
void SendMoneyDelta( int a_iDelta )
{
	if ( !a_iDelta || !g_bActive || g_bHost || g_bApplying ) return;
	Writer w;
	w << (uint8_t)EV_INV_DELTA << (int32_t)a_iDelta << (uint16_t)0;
	Send( w, true );
	Log( "money: " + std::to_string( a_iDelta ) + " sent to the host" );
}
struct ProfileStub
{};
MEMBER_HOOK( 0x00621170, ProfileStub, APlayerSimProfile_AddPickups, void, TINT a_iPickups )
{
	SendMoneyDelta( a_iPickups );
	CallOriginal( a_iPickups );
}
MEMBER_HOOK( 0x006211e0, ProfileStub, APlayerSimProfile_AddPickupsToTarget, void, TINT a_iPickups )
{
	SendMoneyDelta( a_iPickups );
	CallOriginal( a_iPickups );
}
MEMBER_HOOK( 0x00620f80, ProfileStub, APlayerSimProfile_SetPickups, void, TINT a_iPickups, TBOOL a_bCountUp, TBOOL a_bUpdateHud )
{
	SendMoneyDelta( a_iPickups - *(TINT*)( (char*)this + PROFILE_PICKUPS ) );
	CallOriginal( a_iPickups, a_bCountUp, a_bUpdateHud );
}

// ACoinSpawner::SpawnCoin (static, __stdcall): coins out of a crate
HOOK( 0x0066ef10, ACoinSpawner_SpawnCoin, void, void* a_pType, const TVector4& a_rPos )
{
	g_iInSpawnCoin++;
	CallOriginal( a_pType, a_rPos );
	g_iInSpawnCoin--;
}

// AInventoryGroupComponent::CollectUnit (static, __stdcall): our cow picked a unit up
HOOK( 0x00625c10, AInventoryGroupComponent_CollectUnit, TBOOL, void* a_pUnit, void* a_pParentState )
{
	uint32_t uiId;
	if ( a_pUnit && g_pUnitNets && g_bActive && !g_bApplying )
	{
		if ( Untrack( a_pUnit, uiId ) ) SendGone( uiId ); // shared drop
		else SendPick( a_pUnit );                          // placed in the world (notes, recipes, items...)
	}
	return CallOriginal( a_pUnit, a_pParentState );
}

void WorldSync::Install( coop::Session* a_pSession, std::mutex* a_pMutex, std::function<void( const std::string& )> a_fnLog )
{
	g_pSession      = a_pSession;
	g_pMutex        = a_pMutex;
	g_pLog          = new std::function<void( const std::string& )>( a_fnLog );
	g_pRecentTakes  = new std::map<float, double>();
	g_pNetUnits     = new std::map<uint32_t, void*>();
	g_pUnitNets     = new std::map<void*, uint32_t>();
	g_pBaseline     = new Counts();
	g_pOwnDrops     = new std::map<uint32_t, double>();
	g_pNpcs         = new std::map<uint32_t, NpcTarget>();
	g_pNpcUnits     = new std::map<void*, uint32_t>();
	g_pNpcHidden    = new std::map<void*, bool>();
	g_pNpcSent      = new std::map<void*, NpcSent>();
	InstallHook<ARegrowthManager_TakeObject>();
	InstallHook<ARegrowthManager_TakeRandomObject>();
	InstallHook<ARegrowthManager_GrowObjects>();
	InstallHook<ARegrowthManager_GrowAllInType>();
	InstallHook<AUnitManager_SpawnUnitType>();
	InstallHook<AUnitManager_SpawnUnitName>();
	InstallHook<AUnitManager_SpawnUnitPString>();
	InstallHook<AUnitManager_SpawnUnitTransform>();
	InstallHook<AUnitManager_DestroyUnitImmediate>();
	InstallHook<AInventoryComponent_AddUnit>();
	InstallHook<AInventoryGroupComponent_CollectUnit>();
	InstallHook<ACoinSpawner_SpawnCoin>();
	InstallHook<APlayerSimProfile_AddPickups>();
	InstallHook<APlayerSimProfile_AddPickupsToTarget>();
	InstallHook<APlayerSimProfile_SetPickups>();
	InstallHook<ABikeManager_SpawnBikesAtRacks>();
	InstallHook<ABikeManager_RespawnBikes>();
}

void WorldSync::Update( bool a_bInWorld )
{
	bool bHost;
	{
		std::lock_guard<std::mutex> lock( *g_pMutex );
		auto* t      = g_pSession->Transport();
		bHost        = t->IsHost();
		g_iLocalSlot = g_pSession->LocalSlot();
		const bool bShared = t->IsInSession() && g_iLocalSlot >= 0 && !g_pSession->Remotes().empty();
		g_iPlayers         = t->IsInSession() ? (int)g_pSession->Remotes().size() + 1 : 1;
		g_bHostSameLevel   = false;
		for ( auto& [ slot, rp ] : g_pSession->Remotes() )
			if ( slot == 0 && rp.hasPose && rp.b.levelHash == RemotePuppets::CurrentLevel() ) g_bHostSameLevel = true;
		a_bInWorld   = a_bInWorld && bShared; // (also in the barn, where the main game flag is off)
	}
	if ( a_bInWorld != g_bActive )
	{
		g_bActive       = a_bInWorld;
		g_bHaveBaseline = false;
		g_pNetUnits->clear();
		g_pUnitNets->clear();
		g_pNpcs->clear();
		g_pNpcUnits->clear();
		g_pNpcHidden->clear();
		g_pNpcSent->clear();
		g_uiNpcLevel = 0;
		Log( std::string( "world sync " ) + ( g_bActive ? "on" : "off" ) );
	}
	g_bHost = bHost;
	if ( !g_bActive ) return;

	// incoming
	std::vector<coop::GameEvent> events;
	{
		std::lock_guard<std::mutex> lock( *g_pMutex );
		coop::GameEvent e;
		while ( g_pSession->PopEvent( e ) ) events.push_back( std::move( e ) );
	}
	for ( auto& e : events ) Handle( e );

	// outgoing
	const double now = Now();
	if ( g_bHost && now >= g_fNextTime )
	{
		g_fNextTime = now + 1.0;
		SendTime();
	}
	if ( g_bHost && now >= g_fNextRegrowth )
	{
		g_fNextRegrowth = now + 0.25;
		SendRegrowth();
	}
	UpdateInventory();
	SendDropPositions();
	static double s_fLastFrame = Now();
	const TFLOAT  fDelta       = (TFLOAT)( now - s_fLastFrame );
	s_fLastFrame               = now;
	if ( g_bHost ) SendNpcs();
	else if ( g_bHostSameLevel && !SafeMoveNpcs( fDelta ) )
	{
		Log( "npc: move failed, resetting" );
		g_pNpcs->clear();
		g_pNpcUnits->clear();
	}

	for ( auto it = g_pRecentTakes->begin(); it != g_pRecentTakes->end(); )
		if ( now - it->second > 10.0 ) it = g_pRecentTakes->erase( it );
		else ++it;
}
