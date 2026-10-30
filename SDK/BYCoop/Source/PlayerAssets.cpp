#include "pch.h"
#include "PlayerAssets.h"
#include <BYardSDK/SDKHooks.h>
#include <HookHelpers.h>
#include <cstdio>

// Retail addresses (barnyard.exe b243e121...), from AAssetLoader::LoadPlayerCharacter 0x424a20 /
// UnloadPlayerCharacter 0x4248a0 (both __stdcall( const PBPropertyValue* ), return bool)
namespace
{
char** const ppProfiles          = (char**)0x007b493c; // APlayerProfileManager: gender +0x10, costume +0x48
TINT* const  pLoadedCostume      = (TINT*)0x007727d4;  // AAssetLoader::ms_iPlayerCostume
const char** const ppCostumeName = (const char**)0x007727d8; // s_aCostumeNames[7]
void** const ppMatlibMgr         = (void**)0x007b4660; // AMaterialLibraryManager
void** const ppAssetPackTRB      = (void**)0x0077dd60; // AAssetLoader::ms_pTRBFiles[AssetPack]
void** const* const pppStrPool   = (void** const*)0x007ce230;

constexpr int NUM_COSTUMES = 7;

std::function<void( const std::string& )>* g_pLog = nullptr;
void Log( const std::string& s )
{
	if ( g_pLog ) ( *g_pLog )( s );
}

// No allocating globals in mod DLLs (see NOTES.md): plain data only
const void* g_pProp            = nullptr; // "playercharacter" property of the loaded level
int         g_iOwnGender       = -1, g_iOwnCostume = -1;
int         g_iOtherCostume    = -1; // costume the other gender's set was loaded with, -1 none
bool        g_aMatlib[ 2 ][ NUM_COSTUMES ] = {}; // extra material libraries we loaded
bool        g_bFailed          = false;
int         g_aWantG[ 8 ], g_aWantC[ 8 ], g_iWant = 0;

TINT& ProfGender() { return *(TINT*)( *ppProfiles + 0x10 ); }
TINT& ProfCostume() { return *(TINT*)( *ppProfiles + 0x48 ); }

typedef TBOOL( __stdcall* t_PlayerChar )( const void* );
t_PlayerChar g_pfnLoad = nullptr, g_pfnUnload = nullptr; // originals (set from the hooks)

bool SafeCall( t_PlayerChar a_pfn, const void* a_pProp )
{
	__try
	{
		return a_pfn( a_pProp ) != 0;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return false;
	}
}

// AMaterialLibraryManager::LoadLibrary 0x614a00 / UnrefLibrary 0x6142c0 with a pooled "p<g><costume>" name
bool Matlib( int a_iGender, int a_iCostume, bool a_bLoad )
{
	char sz[ 32 ];
	snprintf( sz, sizeof( sz ), "p%c%s", a_iGender ? 'f' : 'm', ppCostumeName[ a_iCostume ] );
	__try
	{
		void* pPool   = **pppStrPool;
		void* pPooled = nullptr;
		CALL_THIS( 0x006bc3e0, void*, void, pPool, void**, &pPooled, const char*, sz, bool*, nullptr ); // TPString8Pool::Get
		if ( a_bLoad )
			CALL_THIS( 0x00614a00, void*, void, *ppMatlibMgr, void**, &pPooled, void*, *ppAssetPackTRB, TBOOL, TTRUE );
		else
			CALL_THIS( 0x006142c0, void*, void, *ppMatlibMgr, void**, &pPooled, TBOOL, TTRUE );
		if ( pPooled && --*(TINT*)pPooled == 0 ) CALL_THIS( 0x006bc380, void*, void, pPooled ); // ~TPooledString8
		return true;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return false;
	}
}

// Replays the retail loader/unloader as if the profile had a_iGender/a_iCostume
bool ReplayAs( t_PlayerChar a_pfn, const void* a_pProp, int a_iGender, int a_iCostume )
{
	const TINT iG = ProfGender(), iC = ProfCostume(), iL = *pLoadedCostume;
	ProfGender()    = a_iGender;
	ProfCostume()   = a_iCostume;
	*pLoadedCostume = a_iCostume;
	const bool bOk  = SafeCall( a_pfn, a_pProp );
	ProfGender()    = iG;
	ProfCostume()   = iC;
	*pLoadedCostume = iL;
	return bOk;
}

// Runtime: hides of the other gender the remote players use (material libraries are plain files).
// The models/animations of the other gender are loaded with the level (PreloadOtherGender): the
// level's property tree is only valid during loading (a runtime replay of the loader crashed).
void LoadWanted()
{
	if ( !g_pProp || g_bFailed ) return;
	for ( int i = 0; i < g_iWant; i++ )
	{
		const int g = g_aWantG[ i ], c = g_aWantC[ i ];
		if ( g < 0 || g > 1 || c < 0 || c >= NUM_COSTUMES || PlayerAssets::IsLoaded( g, c ) ) continue; // (same gender: puppets get a model copy)
		const bool bOk = Matlib( g, c, true );
		if ( bOk ) g_aMatlib[ g ][ c ] = true;
		else g_bFailed = true; // never retry a crashing load in this level
		Log( "assets: loaded hide " + std::to_string( c ) + ( g ? " (female)" : " (male)" ) + ( bOk ? "" : " FAILED" ) );
	}
}

// Level load: models and animations of the other gender, always (a friend may join or pick the
// gender later), with the hide a remote player of that gender uses if known
void PreloadOtherGender( const void* a_pProp )
{
	if ( g_iOwnGender < 0 || g_iOwnGender > 1 ) return;
	const int g = 1 - g_iOwnGender;
	int       c = g_iOwnCostume >= 0 && g_iOwnCostume < NUM_COSTUMES ? g_iOwnCostume : 0;
	for ( int i = 0; i < g_iWant; i++ )
		if ( g_aWantG[ i ] == g && g_aWantC[ i ] >= 0 && g_aWantC[ i ] < NUM_COSTUMES ) c = g_aWantC[ i ];
	const bool bOk = ReplayAs( g_pfnLoad, a_pProp, g, c );
	if ( bOk ) g_iOtherCostume = c;
	else g_bFailed = true;
	Log( std::string( "assets: loaded " ) + ( g ? "female" : "male" ) + " cow, hide " + std::to_string( c ) + ( bOk ? "" : " FAILED" ) );
}

void UnloadExtras( const void* a_pProp )
{
	for ( int g = 0; g < 2; g++ )
		for ( int c = 0; c < NUM_COSTUMES; c++ )
			if ( g_aMatlib[ g ][ c ] ) Matlib( g, c, false );
	if ( g_iOtherCostume >= 0 ) ReplayAs( g_pfnUnload, a_pProp, 1 - g_iOwnGender, g_iOtherCostume );
	memset( g_aMatlib, 0, sizeof( g_aMatlib ) );
	g_iOtherCostume = -1;
	g_pProp         = nullptr;
	g_bFailed       = false;
	Log( "assets: extra player assets released" );
}
int g_iDepth = 0; // nested player-character loads (minigames) only use the outer level's extras
} // namespace

HOOK( 0x00424a20, AAssetLoader_LoadPlayerCharacter, TBOOL, const void* a_pProperty )
{
	g_pfnLoad  = (t_PlayerChar)AAssetLoader_LoadPlayerCharacter::_orig_func_ptr;
	TBOOL bRes = CallOriginal( a_pProperty );
	if ( !bRes || !a_pProperty || !*ppProfiles ) return bRes;
	if ( ++g_iDepth == 1 )
	{
		g_pProp       = a_pProperty;
		g_iOwnGender  = ProfGender();
		g_iOwnCostume = ProfCostume();
		PreloadOtherGender( a_pProperty );
		LoadWanted();
	}
	return bRes;
}

HOOK( 0x004248a0, AAssetLoader_UnloadPlayerCharacter, TBOOL, const void* a_pProperty )
{
	g_pfnUnload = (t_PlayerChar)AAssetLoader_UnloadPlayerCharacter::_orig_func_ptr;
	if ( a_pProperty && g_iDepth > 0 && --g_iDepth == 0 && g_pProp ) UnloadExtras( a_pProperty );
	return CallOriginal( a_pProperty );
}

void PlayerAssets::Install( const std::function<void( const std::string& )>& a_fnLog )
{
	g_pLog = new std::function<void( const std::string& )>( a_fnLog );
	InstallHook<AAssetLoader_LoadPlayerCharacter>();
	InstallHook<AAssetLoader_UnloadPlayerCharacter>();
	g_pfnLoad   = (t_PlayerChar)AAssetLoader_LoadPlayerCharacter::_orig_func_ptr;
	g_pfnUnload = (t_PlayerChar)AAssetLoader_UnloadPlayerCharacter::_orig_func_ptr;
}

void PlayerAssets::SetWanted( const int* a_pGenders, const int* a_pCostumes, int a_iCount )
{
	g_iWant = a_iCount < 8 ? a_iCount : 8;
	for ( int i = 0; i < g_iWant; i++ )
	{
		g_aWantG[ i ] = a_pGenders[ i ];
		g_aWantC[ i ] = a_pCostumes[ i ];
	}
}

void PlayerAssets::Update() { LoadWanted(); }

bool PlayerAssets::IsLoaded( int a_iGender, int a_iCostume )
{
	if ( !g_pProp || a_iGender < 0 || a_iGender > 1 || a_iCostume < 0 || a_iCostume >= NUM_COSTUMES ) return false;
	if ( a_iGender == g_iOwnGender ) return a_iCostume == g_iOwnCostume || g_aMatlib[ a_iGender ][ a_iCostume ];
	return g_iOtherCostume >= 0 && ( a_iCostume == g_iOtherCostume || g_aMatlib[ a_iGender ][ a_iCostume ] );
}
