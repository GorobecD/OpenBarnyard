#include "pch.h"
#include "RemotePuppets.h"
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

typedef TBOOL( __thiscall* t_SetVec )( void*, const TVector4& );

struct Puppet
{
	void* pUnit = nullptr;
	char  szName[ 24 ] = {};
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

TVector4 PoseDir( const coop::Pose& p ) { return TVector4( std::sin( p.yaw ), 0.0f, std::cos( p.yaw ), 0.0f ); }
} // namespace

bool RemotePuppets::IsPuppet( void* a_pUnit )
{
	for ( auto& kv : g_oPuppets )
		if ( kv.second.pUnit == a_pUnit ) return true;
	return false;
}

void RemotePuppets::Update( void* a_pLocalUnit, const std::map<int, coop::RemotePlayer>& a_rRemotes, double a_fNow, double a_fDelay,
                            const std::function<void( const std::string& )>& a_fnLog )
{
	void* pMgr = *ppUnitManager;

	// Level changed or left: the game destroyed every unit, forget the stale pointers
	if ( a_pLocalUnit != g_pLevelUnit )
	{
		if ( !g_oPuppets.empty() ) a_fnLog( "puppets: level changed, dropping " + std::to_string( g_oPuppets.size() ) );
		g_oPuppets.clear();
		g_pLevelUnit = a_pLocalUnit;
	}
	if ( !a_pLocalUnit || !pMgr ) return;

	// Remove puppets of players who left
	for ( auto it = g_oPuppets.begin(); it != g_oPuppets.end(); )
	{
		if ( !a_rRemotes.count( it->first ) )
		{
			CALL_THIS( 0x00410fe0, void*, void, pMgr, void*, it->second.pUnit ); // AUnitManager::DestroyUnit
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
		if ( it == g_oPuppets.end() )
		{
			void* pUnit = CALL_THIS( 0x00411970, void*, void*, pMgr, const char*, "player", const TVector4&, vPos, const TVector4&, vDir ); // SpawnUnit
			if ( !pUnit ) continue;
			Puppet& pp = g_oPuppets[ slot ];
			pp.pUnit   = pUnit;
			strncpy( pp.szName, rp.look.name, sizeof( pp.szName ) - 1 );
			a_fnLog( std::string( "puppet spawned: " ) + pp.szName );
			continue;
		}

		void* pSteer = Steer( it->second.pUnit );
		SteerCall( pSteer, VT_SETPOSITION, vPos );
		SteerCall( pSteer, VT_SETDIRECTION, vDir );
	}
}
