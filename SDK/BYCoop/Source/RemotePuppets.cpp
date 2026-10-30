#include "pch.h"
#include "RemotePuppets.h"
#include "PlayerAssets.h"
#include <BYardSDK/SDKHooks.h>
#include <HookHelpers.h>
#include <Math/TVector4.h>
#include <cmath>
#include <map>

TOSHI_NAMESPACE_USING

namespace
{
// Retail addresses (barnyard.exe b243e121...), see games/barnyard/NOTES.md
void** const   ppUnitManager   = (void**)0x0077e344;
constexpr TUINT OFF_UNIT_STEER = 0x1c;
// ASteer vtable offsets (OpenBarnyard UnitMovement/Steering/ASteer.h)
constexpr TUINT VT_SETPOSITION  = 0x84;
constexpr TUINT VT_SETDIRECTION = 0x8c;
constexpr TUINT VT_GETKINEMATICS = 0x114;
// ASteerKinematics vtable offsets: the walk/run animation follows the measured speed
// (ASteer::GetMovementType: >0.1 walk, > max aesthetic walk speed run)
constexpr TUINT VT_KIN_SETMEASURED   = 0x1c; // 3D kinematics
constexpr TUINT VT_KIN_BLENDMEASURED = 0x20; // 2D/3D

typedef TBOOL( __thiscall* t_SetVec )( void*, const TVector4& );

struct Puppet
{
	void* pUnit = nullptr;
	char  szName[ 24 ] = {};
	int   iGender = -1, iCostume = -1; // look the unit was created with
	bool  bOwnModel = false;           // has its own copy of the model (can have another hide than ours)
	bool  bVisible = true;
	// riding a bike: the "american_?_bike" unit (the cow on the bike), shown instead of the cow
	void*  pRide        = nullptr;
	bool   bRideShown   = false;
	double fRideFailed  = -100.0;
	void*  pTakenBike   = nullptr; // our parked bike hidden while this player rides it
	TVector4 vRidePos, vRideDir;
};
// by slot. Allocated on first use: a global std::map allocates in its constructor, which runs
// before TMemory is initialised and made LoadLibrary fail (error 1114) once already.
std::map<int, Puppet>& Puppets()
{
	static std::map<int, Puppet>* s_p = new std::map<int, Puppet>();
	return *s_p;
}
#define g_oPuppets Puppets()
uint32_t              g_uiPuppetLevel = 0; // level the puppets were spawned in (0: none)

void* Steer( void* a_pUnit ) { return a_pUnit ? *(void**)( (char*)a_pUnit + OFF_UNIT_STEER ) : nullptr; }

void SteerCall( void* a_pSteer, TUINT a_uiVt, const TVector4& a_rV )
{
	if ( !a_pSteer ) return;
	auto fn = *(t_SetVec*)( *(char**)a_pSteer + a_uiVt );
	fn( a_pSteer, a_rV );
}

void SetMeasuredSpeed( void* a_pSteer, TFLOAT a_fSpeed )
{
	if ( !a_pSteer ) return;
	typedef void*( __thiscall * t_GetKin )( void* );
	typedef void( __thiscall * t_SetF )( void*, TFLOAT );
	void* pKin = ( *(t_GetKin*)( *(char**)a_pSteer + VT_GETKINEMATICS ) )( a_pSteer );
	if ( !pKin ) return;
	( *(t_SetF*)( *(char**)pKin + VT_KIN_SETMEASURED ) )( pKin, a_fSpeed );
	( *(t_SetF*)( *(char**)pKin + VT_KIN_BLENDMEASURED ) )( pKin, a_fSpeed );
}

std::map<int, double>& Failed()
{
	static std::map<int, double>* s_p = new std::map<int, double>();
	return *s_p;
}
double g_fLevelSince = 0;
bool   g_bSpawningPuppet = false;
int    g_iAltModelSlot   = 0;     // >0: the puppet being spawned gets its own copy of the model (its own hide)
bool   g_bAltModelUsed   = false;
int    g_iFallbackModel  = 0;
constexpr TUINT OFF_UNIT_ANIMOBJ = 0x94; // AAnimatedUnit::m_pAnimatableObject

// SEH guards: a crash inside retail spawning must not take the game down (no C++ objects here)
// AUnitTransform (0x30 bytes, see OpenBarnyard Units/AUnitTransform.h)
struct UnitTransform
{
	TVector4 vPos, vDir;
	TINT     iUnk20 = 0, iModelIndex = -1, iUnk28 = -1;
	TBOOL    bCreateObstruction = TTRUE;
	char     pad[ 3 ] = {};
};
static_assert( sizeof( UnitTransform ) == 0x30, "AUnitTransform layout" );

// AUnitManager::SpawnUnit with an explicit model: the "player" type lists the male (0) and female (1)
// cows, as AGameLoader spawns the local one. (The plain SpawnUnit always takes model 0.)
void* SafeSpawn( void* a_pMgr, UnitTransform* a_pTransform )
{
	__try
	{
		void* pType = CALL_THIS( 0x0040fee0, void*, void*, a_pMgr, const char*, "player" ); // FindUnit
		if ( !pType ) return nullptr;
		void* pUnit = CALL_THIS( 0x00411700, void*, void*, a_pMgr, void*, pType, UnitTransform*, a_pTransform ); // CreateUnitFromTemplate
		if ( !pUnit ) pUnit = CALL_THIS( 0x00410d70, void*, void*, a_pMgr, void*, pType, UnitTransform*, a_pTransform ); // CreateUnitInstance
		return pUnit;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return nullptr;
	}
}
void SafeDestroy( void* a_pMgr, void* a_pUnit )
{
	__try
	{
		CALL_THIS( 0x00410fe0, void*, void, a_pMgr, void*, a_pUnit ); // AUnitManager::DestroyUnit
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
	}
}
bool SafeApplyMaterial( void* a_pUnit, int a_iSkin )
{
	__try
	{
		return CALL_THIS( 0x00415b10, void*, TBOOL, a_pUnit, TINT, a_iSkin, TBOOL, TTRUE ) != 0; // AAnimatedUnit::ApplyMaterial
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return false;
	}
}

// AAnimatedUnit::SetVisible 0x415ac0. Puppets are never destroyed inside a level: the game keeps
// pointers to units in its interaction system and crashed later (AAttachedUnitRequirement::Evaluate on
// a destroyed cow with no animatable object). A puppet that is not needed is hidden instead.
void SafeSetVisible( void* a_pUnit, bool a_bVisible )
{
	__try
	{
		CALL_THIS( 0x00415ac0, void*, void, a_pUnit, TBOOL, a_bVisible ? TTRUE : TFALSE );
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
	}
}


// ---------------------------------------------------------------- bikes
// ABikeManager (singleton 0x00783c70): the player getting on a bike is replaced by a unit of the
// cow riding it ("american_m_bike"/"american_f_bike") with an ABikeSteer (vtable 0x007250c8) whose
// APhysicsBike is at +0xc4; the bike model ("bike1".."bike5", only m_iLoadedBikeType +0x608 is
// loaded) is attached to its body_loc by ABikeHelpers::AttachBikeModel (stdcall 0x005262b0).
constexpr TUINT VT_SETSPEED = 0x100;
void** const    ppBikeManager = (void**)0x00783c70;

int LoadedBikeType() { return *ppBikeManager ? *(TINT*)( (char*)*ppBikeManager + 0x608 ) : -1; }

void FreezeRide( void* a_pSteer )
{
	void* pPhysics = *(void**)( (char*)a_pSteer + 0xc4 );
	if ( pPhysics ) CALL_THIS( 0x0065f560, void*, void, pPhysics, TBOOL, TTRUE ); // APhysicsVehicle::SetFrozen
}

void* SafeSpawnRide( void* a_pMgr, const char* a_szType, UnitTransform* a_pTransform, int a_iBikeType )
{
	__try
	{
		void* pType = CALL_THIS( 0x0040fee0, void*, void*, a_pMgr, const char*, a_szType ); // FindUnit
		if ( !pType ) return nullptr;
		void* pUnit = CALL_THIS( 0x00411700, void*, void*, a_pMgr, void*, pType, UnitTransform*, a_pTransform ); // CreateUnitFromTemplate
		if ( !pUnit ) pUnit = CALL_THIS( 0x00410d70, void*, void*, a_pMgr, void*, pType, UnitTransform*, a_pTransform );
		if ( !pUnit ) return nullptr;
		if ( !*(void**)( (char*)pUnit + OFF_UNIT_ANIMOBJ ) || !Steer( pUnit ) || **(void***)( (char*)pUnit + OFF_UNIT_STEER ) != (void*)0x007250c8 )
		{
			CALL_THIS( 0x00410fe0, void*, void, a_pMgr, void*, pUnit ); // same frame, nothing refers to it yet
			return nullptr;
		}
		CALL_THIS( 0x00413ba0, void*, void, pUnit, TBOOL, TFALSE ); // AUnit::SetAIEnabled: no input, no AI
		CALL( 0x005262b0, void, void*, pUnit, TINT, a_iBikeType );  // ABikeHelpers::AttachBikeModel
		FreezeRide( Steer( pUnit ) );
		return pUnit;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return nullptr;
	}
}

bool SafeMoveRide( void* a_pUnit, const TVector4& a_rPos, const TVector4& a_rDir, TFLOAT a_fSpeed )
{
	__try
	{
		void* pSteer = Steer( a_pUnit );
		if ( !pSteer ) return false;
		FreezeRide( pSteer ); // our positions drive it, not its physics
		( *(t_SetVec*)( *(char**)pSteer + VT_SETPOSITION ) )( pSteer, a_rPos );
		typedef void( __thiscall * t_SetDir )( void*, const TVector4& );
		( *(t_SetDir*)( *(char**)pSteer + VT_SETDIRECTION ) )( pSteer, a_rDir );
		typedef void( __thiscall * t_SetF )( void*, TFLOAT );
		( *(t_SetF*)( *(char**)pSteer + VT_SETSPEED ) )( pSteer, a_fSpeed ); // ABikeSteer::m_fSpeed: pedalling animation
		return true;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return false;
	}
}

// our bikes: { type, rack, unit, parked } at +0x20, count +0x544
void* Bike( int i ) { return *(void**)( (char*)*ppBikeManager + 0x20 + i * 0x10 + 8 ); }
bool  BikeParked( int i ) { return *(TBOOL*)( (char*)*ppBikeManager + 0x20 + i * 0x10 + 0x0c ) != 0; }
int   NumBikes() { return *ppBikeManager ? *(TINT*)( (char*)*ppBikeManager + 0x544 ) : 0; }

// The "player" unit type lists 4 models: male, female, and the barn's male, female
// (AGameLoader: the barn explore minigame spawns the player with 2/3)
int ModelIndex( int a_iGender ) { return ( *(TBOOL*)0x007810e6 ? 2 : 0 ) + ( a_iGender == 1 ? 1 : 0 ); }

// The hide is a texture of the model's material, and a model is shared by every cow of that gender:
// retexturing a puppet of OUR gender would retexture our own cow too. So only cows of the other
// gender get their hide (that model is used by puppets only).
void ApplyHide( void* a_pUnit, int a_iGender, int a_iCostume, int a_iOwnGender, const std::function<void( const std::string& )>& a_fnLog, bool a_bOwnModel = false )
{
	if ( a_iGender == a_iOwnGender && !a_bOwnModel ) return;
	if ( !SafeApplyMaterial( a_pUnit, a_iCostume ) ) a_fnLog( "puppet hide not applied" );
}


// our parked bike next to where the remote player got on: it is the one they took
void TakeBike( Puppet& a_rPP, const TVector4& a_rPos )
{
	if ( a_rPP.pTakenBike || !*ppBikeManager ) return;
	void*  pBest = nullptr;
	TFLOAT fBest = 8.0f * 8.0f;
	for ( int i = 0; i < NumBikes() && i < 56; i++ )
	{
		void* pBike = Bike( i );
		if ( !pBike || !BikeParked( i ) || !Steer( pBike ) ) continue;
		bool bTaken = false;
		for ( auto& kv : g_oPuppets ) bTaken |= kv.second.pTakenBike == pBike;
		if ( bTaken ) continue;
		typedef const TVector4*( __thiscall * t_GetPos )( void* );
		const TVector4* p = ( *(t_GetPos*)( *(char**)Steer( pBike ) + 0xac ) )( Steer( pBike ) );
		if ( !p ) continue;
		const TFLOAT d = ( p->x - a_rPos.x ) * ( p->x - a_rPos.x ) + ( p->z - a_rPos.z ) * ( p->z - a_rPos.z );
		if ( d < fBest )
		{
			fBest = d;
			pBest = pBike;
		}
	}
	if ( !pBest ) return;
	SafeSetVisible( pBest, false );
	a_rPP.pTakenBike = pBest;
}

bool SafePlaceBike( void* a_pBike, const TVector4& a_rPos, const TVector4& a_rDir )
{
	__try
	{
		SteerCall( Steer( a_pBike ), VT_SETPOSITION, a_rPos );
		SteerCall( Steer( a_pBike ), VT_SETDIRECTION, a_rDir );
		return true;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return false;
	}
}

// got off: the cow comes back, the bike stays where the ride ended
void HideRide( Puppet& a_rPP, const std::function<void( const std::string& )>& a_fnLog )
{
	if ( !a_rPP.bRideShown ) return;
	a_rPP.bRideShown = false;
	SafeSetVisible( a_rPP.pRide, false );
	SafeMoveRide( a_rPP.pRide, TVector4( 0.0f, 0.0f, 0.0f, 1.0f ), TVector4( 0.0f, 0.0f, 1.0f, 0.0f ), 0.0f );
	if ( a_rPP.pTakenBike )
	{
		SafePlaceBike( a_rPP.pTakenBike, a_rPP.vRidePos, a_rPP.vRideDir );
		SafeSetVisible( a_rPP.pTakenBike, true );
		a_rPP.pTakenBike = nullptr;
	}
	a_fnLog( std::string( "puppet off the bike: " ) + a_rPP.szName );
}

void SpawnRide( int a_iSlot, Puppet& a_rPP, const coop::RemotePlayer& a_rRP, const TVector4& a_rPos, const TVector4& a_rDir, int a_iOwnGender, double a_fNow,
                const std::function<void( const std::string& )>& a_fnLog )
{
	const int iBikeType = LoadedBikeType();
	if ( iBikeType < 0 || !*ppUnitManager )
	{
		a_rPP.fRideFailed = a_fNow;
		return;
	}
	UnitTransform oT;
	oT.vPos = a_rPos;
	oT.vDir = a_rDir;
	char*      pProfiles     = *(char**)0x007b493c;
	const bool bWantOwnModel = a_rRP.look.gender == a_iOwnGender && a_rRP.look.hide != ( pProfiles ? *(TINT*)( pProfiles + 0x48 ) : -1 );
	g_iFallbackModel  = 0;
	g_iAltModelSlot   = bWantOwnModel ? a_iSlot : 0;
	g_bAltModelUsed   = false;
	g_bSpawningPuppet = true;
	void* pUnit = SafeSpawnRide( *ppUnitManager, a_rRP.look.gender == 1 ? "american_f_bike" : "american_m_bike", &oT, iBikeType );
	g_bSpawningPuppet = false;
	g_iAltModelSlot   = 0;
	if ( !pUnit )
	{
		a_rPP.fRideFailed = a_fNow;
		a_fnLog( std::string( "ride puppet spawn failed: " ) + a_rPP.szName );
		return;
	}
	ApplyHide( pUnit, a_rRP.look.gender, a_rRP.look.hide, a_iOwnGender, a_fnLog, g_bAltModelUsed );
	SafeSetVisible( pUnit, false );
	a_rPP.pRide      = pUnit;
	a_rPP.bRideShown = false;
	a_fnLog( std::string( "ride puppet spawned: " ) + a_rPP.szName + " bike" + std::to_string( iBikeType + 1 ) );
}

TVector4 PoseDir( const coop::Pose& p ) { return TVector4( std::sin( p.yaw ), 0.0f, std::cos( p.yaw ), 0.0f ); }
} // namespace

bool RemotePuppets::IsPuppet( void* a_pUnit )
{
	for ( auto& kv : g_oPuppets )
		if ( kv.second.pUnit == a_pUnit || kv.second.pRide == a_pUnit ) return true;
	return false;
}

bool RemotePuppets::IsRideSteer( void* a_pSteer )
{
	for ( auto& kv : g_oPuppets )
		if ( kv.second.pRide && Steer( kv.second.pRide ) == a_pSteer ) return true;
	return false;
}

bool RemotePuppets::HasPuppet( int a_iSlot )
{
	auto it = g_oPuppets.find( a_iSlot );
	return it != g_oPuppets.end() && it->second.bVisible;
}

uint32_t RemotePuppets::CurrentLevel() { return (uint32_t)*(TINT*)0x00772800 + 1; } // ATerrainInterface::ms_eCurrentLevel

void RemotePuppets::Update( void* a_pLocalUnit, uint32_t a_uiLocalLevel, const std::map<int, coop::RemotePlayer>& a_rRemotes, double a_fNow, double a_fDelay,
                            const std::function<void( const std::string& )>& a_fnLog )
{
	void* pMgr = *ppUnitManager;

	// Level changed or left: the game destroyed every unit, forget the stale pointers. (Not on a change
	// of our own unit: getting on a bike replaces it.)
	const uint32_t uiLevel = a_pLocalUnit ? a_uiLocalLevel : 0;
	if ( uiLevel != g_uiPuppetLevel )
	{
		if ( !g_oPuppets.empty() ) a_fnLog( "puppets: level changed, dropping " + std::to_string( g_oPuppets.size() ) );
		g_oPuppets.clear();
		Failed().clear();
		g_uiPuppetLevel = uiLevel;
		g_fLevelSince   = a_fNow;
	}
	if ( !a_pLocalUnit || !pMgr ) return;

	// Players who left: hide their cows (see SafeSetVisible)
	for ( auto& [ slot, pp ] : g_oPuppets )
		if ( !a_rRemotes.count( slot ) )
		{
			HideRide( pp, a_fnLog );
			if ( pp.bVisible )
			{
				SafeSetVisible( pp.pUnit, false );
				pp.bVisible = false;
				a_fnLog( std::string( "puppet hidden (left): " ) + pp.szName );
			}
		}

	char* pProfiles  = *(char**)0x007b493c;
	const int iOwnG  = pProfiles ? *(TINT*)( pProfiles + 0x10 ) : -1;

	// diagnostics every 5 s: why a remote cow is (not) there
	static double s_fNextDiag = 0;
	if ( a_fNow >= s_fNextDiag )
	{
		s_fNextDiag = a_fNow + 5.0;
		for ( auto& [ slot, rp ] : a_rRemotes )
		{
			coop::Pose p = rp.hasPose ? rp.Sample( a_fNow, a_fDelay ) : coop::Pose{};
			char       sz[ 200 ];
			snprintf( sz, sizeof( sz ), "diag: P%d %s pose=%d map %u/%u look g%d h%d loaded=%d own g%d puppet=%d", slot + 1, rp.look.name, rp.hasPose, p.levelHash, a_uiLocalLevel,
			          rp.look.gender, rp.look.hide, PlayerAssets::IsLoaded( rp.look.gender, rp.look.hide ), iOwnG, (int)g_oPuppets.count( slot ) );
			a_fnLog( sz );
		}
	}

	for ( auto& [ slot, rp ] : a_rRemotes )
	{
		if ( !rp.hasPose ) continue;
		coop::Pose p = rp.Sample( a_fNow, a_fDelay );
		TVector4   vPos( p.pos[ 0 ], p.pos[ 1 ], p.pos[ 2 ], 1.0f );
		TVector4   vDir = PoseDir( p );

		auto it = g_oPuppets.find( slot );
		// remote went to another map (or we did): its cow must not stand in ours
		if ( p.levelHash != a_uiLocalLevel )
		{
			if ( it != g_oPuppets.end() ) HideRide( it->second, a_fnLog );
			if ( it != g_oPuppets.end() && it->second.bVisible )
			{
				SafeSetVisible( it->second.pUnit, false );
				it->second.bVisible = false;
				a_fnLog( std::string( "puppet hidden (other map): " ) + it->second.szName );
			}
			continue;
		}
		if ( it == g_oPuppets.end() )
		{
			// wait until our own level has settled (the friend crashed spawning right after the creator)
			if ( a_fNow - g_fLevelSince < 0.5 ) continue;
			auto f = Failed().find( slot );
			if ( f != Failed().end() && a_fNow - f->second < 30.0 ) continue;

			// models/hides of other players are loaded by PlayerAssets (retail loads only ours)
			if ( rp.look.gender < 0 || rp.look.gender > 1 ) continue;
			if ( !PlayerAssets::IsLoaded( rp.look.gender, rp.look.hide ) ) continue; // model and hide
			UnitTransform oT;
			oT.vPos        = vPos;
			oT.vDir        = vDir;
			oT.iModelIndex = ModelIndex( rp.look.gender );
			g_iFallbackModel = ModelIndex( iOwnG );
			// same gender as ours with another hide: the puppet gets a copy of the model (the hide is a
			// texture of the model's material, shared by every cow using the model)
			const bool bWantOwnModel = rp.look.gender == iOwnG && rp.look.hide != ( pProfiles ? *(TINT*)( pProfiles + 0x48 ) : -1 );
			g_iAltModelSlot   = bWantOwnModel ? slot : 0;
			g_bAltModelUsed   = false;
			g_bSpawningPuppet = true;
			void* pUnit    = SafeSpawn( pMgr, &oT );
			g_bSpawningPuppet = false;
			g_iAltModelSlot   = 0;
			if ( !pUnit || !*(void**)( (char*)pUnit + OFF_UNIT_ANIMOBJ ) )
			{
				Failed()[ slot ] = a_fNow;
				if ( pUnit ) SafeDestroy( pMgr, pUnit ); // same frame, nothing refers to it yet
				a_fnLog( std::string( "puppet spawn failed: " ) + rp.look.name );
				continue;
			}
			ApplyHide( pUnit, rp.look.gender, rp.look.hide, iOwnG, a_fnLog, g_bAltModelUsed );
			Puppet& pp  = g_oPuppets[ slot ];
			pp.pUnit    = pUnit;
			pp.iGender  = rp.look.gender;
			pp.iCostume = rp.look.hide;
			pp.bOwnModel = g_bAltModelUsed;
			strncpy( pp.szName, rp.look.name, sizeof( pp.szName ) - 1 );
			a_fnLog( std::string( "puppet spawned: " ) + pp.szName + ( pp.iGender ? " female" : " male" ) + " hide " + std::to_string( pp.iCostume ) );
			continue;
		}
		Puppet& pp = it->second;
		// on a bike: the cow-on-a-bike unit instead of the cow
		if ( p.flags & 1 )
		{
			if ( !pp.pRide && a_fNow - pp.fRideFailed > 10.0 ) SpawnRide( slot, pp, rp, vPos, vDir, iOwnG, a_fNow, a_fnLog );
			if ( pp.pRide )
			{
				if ( !pp.bRideShown )
				{
					SafeSetVisible( pp.pRide, true );
					pp.bRideShown = true;
					TakeBike( pp, vPos );
					a_fnLog( std::string( "puppet on a bike: " ) + pp.szName );
				}
				if ( pp.bVisible )
				{
					SafeSetVisible( pp.pUnit, false );
					pp.bVisible = false;
				}
				pp.vRidePos = vPos;
				pp.vRideDir = vDir;
				if ( !SafeMoveRide( pp.pRide, vPos, vDir, p.speed ) )
				{
					a_fnLog( "ride puppet: move failed" );
					pp.pRide       = nullptr; // leave it, don't touch it again
					pp.bRideShown  = false;
					pp.fRideFailed = a_fNow;
				}
				// the hidden cow follows (name tag, interactions)
				void* pSteer = Steer( pp.pUnit );
				SteerCall( pSteer, VT_SETPOSITION, vPos );
				continue;
			}
		}
		else
			HideRide( pp, a_fnLog );

		if ( !pp.bVisible )
		{
			SafeSetVisible( pp.pUnit, true );
			pp.bVisible = true;
			a_fnLog( std::string( "puppet shown: " ) + pp.szName );
		}
		// the player changed the hide: retexture (a gender change needs another model: next level)
		if ( pp.iCostume != rp.look.hide && pp.iGender == rp.look.gender )
		{
			ApplyHide( pp.pUnit, rp.look.gender, rp.look.hide, iOwnG, a_fnLog, pp.bOwnModel );
			pp.iCostume = rp.look.hide;
		}

		void* pSteer = Steer( it->second.pUnit );
		SteerCall( pSteer, VT_SETPOSITION, vPos );
		SteerCall( pSteer, VT_SETDIRECTION, vDir );
		SetMeasuredSpeed( pSteer, p.speed );
	}
}

// AAnimatedUnit::CreateAnimatableObject: a unit whose model type isn't loaded is created with no
// animatable object, and the game later crashes on it (AAttachedUnitRequirement::Evaluate), even if
// it is destroyed right away. For a puppet, check the model first and fall back to our own model.
struct AAnimatedUnitStub
{};
MEMBER_HOOK( 0x00416670, AAnimatedUnitStub, AAnimatedUnit_CreateAnimatableObject, TBOOL, TINT a_iModelIndex )
{
	if ( g_bSpawningPuppet )
	{
		void* pPooled = nullptr; // TPString8 returned by AAnimatedUnit::GetModelName 0x4165c0
		CALL_THIS( 0x004165c0, void*, void*, this, void**, &pPooled, TINT, a_iModelIndex );
		void* pType = CALL_THIS( 0x0057d820, void*, void*, *(void**)0x007899b8, void**, &pPooled ); // AAnimatableObjectManager::FindType
		if ( pPooled && --*(TINT*)pPooled == 0 ) CALL_THIS( 0x006bc380, void*, void, pPooled );
		if ( !pType ) a_iModelIndex = g_iFallbackModel;
	}
	return CallOriginal( a_iModelIndex );
}

// AModelRepos::InstantiateModel: the first model of a puppet that needs its own hide is instantiated
// from a copy of the model, loaded under another path ("data\models\.\<name>", one "." per slot)
struct AModelReposStub
{};
MEMBER_HOOK( 0x006127f0, AModelReposStub, AModelRepos_InstantiateModel, void*, void* a_pModel )
{
	if ( g_bSpawningPuppet && g_iAltModelSlot > 0 && !g_bAltModelUsed && a_pModel )
	{
		g_bAltModelUsed       = true;
		void*       pName     = *(void**)( (char*)a_pModel + 0x14 ); // AModel::m_Name (TPString8)
		const char* szName    = pName ? *(const char**)( (char*)pName + 4 ) : nullptr;
		const char* szFile    = szName ? strrchr( szName, '\\' ) : nullptr;
		if ( szFile )
		{
			std::string strAlias( szName, szFile + 1 );
			for ( int i = 0; i < g_iAltModelSlot; i++ ) strAlias += ".\\";
			strAlias += szFile + 1;
			void* pPool  = **(void***)0x007ce230;
			void* pAlias = nullptr;
			CALL_THIS( 0x006bc3e0, void*, void, pPool, void**, &pAlias, const char*, strAlias.c_str(), bool*, nullptr ); // TPString8Pool::Get
			CALL_THIS( 0x00612c90, void*, void, this, void**, &pAlias, void*, nullptr );                       // LoadModel( alias, no TRB )
			void* pAltModel = CALL_THIS( 0x006123c0, void*, void*, this, void**, &pAlias );                   // GetModel( alias )
			if ( pAlias && --*(TINT*)pAlias == 0 ) CALL_THIS( 0x006bc380, void*, void, pAlias );
			if ( pAltModel ) return CallOriginal( pAltModel );
			g_bAltModelUsed = false;
		}
		else
			g_bAltModelUsed = false;
	}
	return CallOriginal( a_pModel );
}

// ABikeSteer::Update: a ride puppet only goes where the network says (no physics, input, sounds)
struct ABikeSteerStub
{};
MEMBER_HOOK( 0x0055c630, ABikeSteerStub, ABikeSteer_Update, TBOOL, TFLOAT a_fDeltaTime )
{
	if ( RemotePuppets::IsRideSteer( this ) ) return TTRUE;
	return CallOriginal( a_fDeltaTime );
}

void RemotePuppets::Install()
{
	InstallHook<ABikeSteer_Update>();
	InstallHook<AAnimatedUnit_CreateAnimatableObject>();
	InstallHook<AModelRepos_InstantiateModel>();
}
