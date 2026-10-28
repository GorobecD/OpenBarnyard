#include "pch.h"
#include "DevTools.h"
#include <BYardSDK/SDKHooks.h>
#include <BYardSDK/APlayerManager.h>
#include <Math/TVector4.h>
#include <windows.h>
#include <cstdio>

TOSHI_NAMESPACE_USING

namespace
{
// Retail addresses (barnyard.exe b243e121...): see games/barnyard/NOTES.md
void** const   ppUnitManager = (void**)0x0077e344; // AUnitManager singleton (SpawnUnit callers load it)
constexpr TUINT OFF_UNITS    = 0x38;               // TNodeList<AUnit> m_oUnits (root node, then count)
constexpr TUINT OFF_UNIT_ID  = 0x24;
constexpr TUINT OFF_UNIT_TYPE = 0x28;
constexpr TUINT OFF_UNIT_STEER = 0x1c;
constexpr TUINT OFF_TYPE_NAME = 0x0c;              // TPString8 -> TPooledString8 { refcount; TString8 { char* } }

const char* TypeName( char* a_pUnit )
{
	char* pType = *(char**)( a_pUnit + OFF_UNIT_TYPE );
	if ( !pType ) return "?";
	char* pPooled = *(char**)( pType + OFF_TYPE_NAME );
	if ( !pPooled ) return "?";
	const char* sz = *(const char**)( pPooled + 4 );
	return sz ? sz : "?";
}

bool UnitPos( char* a_pUnit, TVector4& a_rOut )
{
	ASteer* pSteer = *(ASteer**)( a_pUnit + OFF_UNIT_STEER );
	if ( !pSteer ) return false;
	TVector4* p = pSteer->GetPosition();
	if ( !p ) return false;
	a_rOut = *p;
	return true;
}

bool KeyPressed( int a_iVk, bool& a_rWasDown )
{
	const bool bDown = ( GetAsyncKeyState( a_iVk ) & 0x8000 ) != 0;
	const bool bEdge = bDown && !a_rWasDown;
	a_rWasDown       = bDown;
	return bEdge && GetForegroundWindow() && ( GetWindowThreadProcessId( GetForegroundWindow(), NULL ) == GetCurrentThreadId() );
}
} // namespace

void DevTools::Update( const std::function<void( const std::string& )>& a_fnLog, const char* a_szSpawnType )
{
	static bool s_bF8 = false, s_bF9 = false;
	char*       pMgr = (char*)*ppUnitManager;

	if ( KeyPressed( VK_F8, s_bF8 ) && pMgr )
	{
		char* pRoot  = pMgr + OFF_UNITS;
		char* pEnd   = pRoot - 4; // TNode sits after the vtable pointer, list links point at the AUnit
		char* pUnit  = *(char**)( pRoot + 4 );
		int   iCount = 0;
		char  sz[ 256 ];
		while ( pUnit && pUnit != pEnd && iCount < 2000 )
		{
			TVector4 v;
			if ( UnitPos( pUnit, v ) )
				snprintf( sz, sizeof( sz ), "unit #%d %-32s (%.1f %.1f %.1f)", *(int*)( pUnit + OFF_UNIT_ID ), TypeName( pUnit ), v.x, v.y, v.z );
			else
				snprintf( sz, sizeof( sz ), "unit #%d %-32s (no steer)", *(int*)( pUnit + OFF_UNIT_ID ), TypeName( pUnit ) );
			a_fnLog( sz );
			pUnit = *(char**)( pUnit + 4 + 4 ); // TNode::m_pNext (node at +4: m_pList, m_pNext, m_pPrev)
			iCount++;
		}
		a_fnLog( "units dumped: " + std::to_string( iCount ) );
	}

	if ( KeyPressed( VK_F9, s_bF9 ) && pMgr && a_szSpawnType && *a_szSpawnType )
	{
		APlayerManager* pm = APlayerManager::GetSingleton();
		AUnitPlayer*    p  = pm ? pm->GetPlayer( 0 ) : TNULL;
		TVector4        vPos, vDir( 0, 0, 1, 0 );
		if ( !p || !UnitPos( (char*)p, vPos ) )
		{
			a_fnLog( "spawn: no player" );
			return;
		}
		vPos.x += 3.0f;
		void* pNew = CALL_THIS( 0x00411970, void*, void*, pMgr, const char*, a_szSpawnType, const TVector4&, vPos, const TVector4&, vDir );
		char sz[ 200 ];
		snprintf( sz, sizeof( sz ), "spawn '%s' at (%.1f %.1f %.1f) -> %p%s", a_szSpawnType, vPos.x, vPos.y, vPos.z, pNew, pNew ? ( std::string( " type " ) + TypeName( (char*)pNew ) ).c_str() : "" );
		a_fnLog( sz );
	}
}
