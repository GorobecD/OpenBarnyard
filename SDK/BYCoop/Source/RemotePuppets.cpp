#include "pch.h"
#include "RemotePuppets.h"
#include "PlayerAssets.h"
#include <BYardSDK/SDKHooks.h>
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
};
// by slot. Allocated on first use: a global std::map allocates in its constructor, which runs
// before TMemory is initialised and made LoadLibrary fail (error 1114) once already.
std::map<int, Puppet>& Puppets()
{
	static std::map<int, Puppet>* s_p = new std::map<int, Puppet>();
	return *s_p;
}
#define g_oPuppets Puppets()
void*                 g_pLevelUnit = nullptr; // local unit the puppets were spawned next to

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

TVector4 PoseDir( const coop::Pose& p ) { return TVector4( std::sin( p.yaw ), 0.0f, std::cos( p.yaw ), 0.0f ); }
} // namespace

bool RemotePuppets::IsPuppet( void* a_pUnit )
{
	for ( auto& kv : g_oPuppets )
		if ( kv.second.pUnit == a_pUnit ) return true;
	return false;
}

bool RemotePuppets::HasPuppet( int a_iSlot ) { return g_oPuppets.count( a_iSlot ) != 0; }

uint32_t RemotePuppets::CurrentLevel() { return (uint32_t)*(TINT*)0x00772800 + 1; } // ATerrainInterface::ms_eCurrentLevel

void RemotePuppets::Update( void* a_pLocalUnit, uint32_t a_uiLocalLevel, const std::map<int, coop::RemotePlayer>& a_rRemotes, double a_fNow, double a_fDelay,
                            const std::function<void( const std::string& )>& a_fnLog )
{
	void* pMgr = *ppUnitManager;

	// Level changed or left: the game destroyed every unit, forget the stale pointers
	if ( a_pLocalUnit != g_pLevelUnit )
	{
		if ( !g_oPuppets.empty() ) a_fnLog( "puppets: level changed, dropping " + std::to_string( g_oPuppets.size() ) );
		g_oPuppets.clear();
		Failed().clear();
		g_pLevelUnit  = a_pLocalUnit;
		g_fLevelSince = a_fNow;
	}
	if ( !a_pLocalUnit || !pMgr ) return;

	// Remove puppets of players who left
	for ( auto it = g_oPuppets.begin(); it != g_oPuppets.end(); )
	{
		if ( !a_rRemotes.count( it->first ) )
		{
			SafeDestroy( pMgr, it->second.pUnit );
			a_fnLog( std::string( "puppet removed: " ) + it->second.szName );
			it = g_oPuppets.erase( it );
		}
		else
			++it;
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
			if ( it != g_oPuppets.end() )
			{
				SafeDestroy( pMgr, it->second.pUnit );
				a_fnLog( std::string( "puppet hidden (other map): " ) + it->second.szName );
				g_oPuppets.erase( it );
			}
			continue;
		}
		if ( it == g_oPuppets.end() )
		{
			// wait until our own level has settled (the friend crashed spawning right after the creator)
			if ( a_fNow - g_fLevelSince < 3.0 ) continue;
			auto f = Failed().find( slot );
			if ( f != Failed().end() && a_fNow - f->second < 30.0 ) continue;

			// models/hides of other players are loaded by PlayerAssets (retail loads only ours)
			if ( !PlayerAssets::IsLoaded( rp.look.gender, rp.look.hide ) ) continue;
			UnitTransform oT;
			oT.vPos        = vPos;
			oT.vDir        = vDir;
			oT.iModelIndex = rp.look.gender;
			void* pUnit    = SafeSpawn( pMgr, &oT );
			if ( !pUnit || !*(void**)( (char*)pUnit + OFF_UNIT_ANIMOBJ ) )
			{
				Failed()[ slot ] = a_fNow;
				if ( pUnit ) SafeDestroy( pMgr, pUnit );
				a_fnLog( std::string( "puppet spawn failed: " ) + rp.look.name );
				continue;
			}
			if ( !SafeApplyMaterial( pUnit, rp.look.hide ) ) a_fnLog( "puppet hide not applied" );
			Puppet& pp  = g_oPuppets[ slot ];
			pp.pUnit    = pUnit;
			pp.iGender  = rp.look.gender;
			pp.iCostume = rp.look.hide;
			strncpy( pp.szName, rp.look.name, sizeof( pp.szName ) - 1 );
			a_fnLog( std::string( "puppet spawned: " ) + pp.szName + ( pp.iGender ? " female" : " male" ) + " hide " + std::to_string( pp.iCostume ) );
			continue;
		}
		// the player changed the cow in the creator: build it again
		if ( it->second.iGender != rp.look.gender || it->second.iCostume != rp.look.hide )
		{
			SafeDestroy( pMgr, it->second.pUnit );
			a_fnLog( std::string( "puppet look changed: " ) + it->second.szName );
			g_oPuppets.erase( it );
			continue;
		}

		void* pSteer = Steer( it->second.pUnit );
		SteerCall( pSteer, VT_SETPOSITION, vPos );
		SteerCall( pSteer, VT_SETDIRECTION, vDir );
		SetMeasuredSpeed( pSteer, p.speed );
	}
}
