#include "pch.h"
#include "AHooks.h"
#include "ModLoader.h"
#include "HookHelpers.h"
#include "AModLoaderTask.h"
#include "ACoreSettings.h"
#include "AImGUI.h"

#include "BYardSDK/SDKHooks.h"

#include "BYardSDK/AGUI2.h"
#include "BYardSDK/SDK_T2GUIElement.h"
#include "BYardSDK/SDK_T2GUIRectangle.h"
#include "BYardSDK/AGUI2TextBox.h"
#include "BYardSDK/SDK_T2GUIFontManager.h"
#include "BYardSDK/AGUISlideshow.h"
#include "BYardSDK/THookedRenderD3DInterface.h"

#include <Toshi/THPTimer.h>
#include <Toshi/TApplication.h>
#include <Toshi/TUtil.h>

#include <cstdio>
#include <windows.h>

TOSHI_NAMESPACE_USING

static T2CommandLine g_CommandLine;
const T2CommandLine* g_pCommandLine = &g_CommandLine;

// Unbuffered breadcrumbs (Toshi's log file is buffered and may never flush if the game hangs)
void RawLog( const char* a_szFormat, ... )
{
	FILE* f = fopen( "Logs\\modcore_raw.txt", "a" );
	if ( !f ) return;
	va_list args;
	va_start( args, a_szFormat );
	vfprintf( f, a_szFormat, args );
	va_end( args );
	fputc( '\n', f );
	fclose( f );
}

const char* GetModsDirectory()
{
	return "Mods\\";
}

// Game bug: WM_MOUSEMOVE before the DirectInput mouse exists calls
// TInputDXDeviceMouse::SetCurrentPosition on a null device and crashes at startup
// (barnyard.exe+0x2d4179 writing [0x48]) if the cursor moves over the window while loading.
struct AMouseDeviceStub
{};
MEMBER_HOOK( 0x006d4140, AMouseDeviceStub, Mouse_SetCurrentPosition, void, TINT a_iX, TINT a_iY )
{
	void* volatile pThis = this;
	if ( pThis == TNULL ) return;
	CallOriginal( a_iX, a_iY );
}

MEMBER_HOOK( 0x006c17f0, TApplication, TApplication_Create, TBOOL, const TCHAR* a_pchName, TINT a_iArgc, TCHAR** a_ppArgv )
{
	RawLog( "TApplication::Create hook fired" );
	// Installed here (game main thread) rather than in DllMain: from DllMain this hook never took
	// effect on the user's PC (target bytes unchanged in crash.dmp, no InstallHook log line).
	InstallHook<Mouse_SetCurrentPosition>();
	// Create the modloader task
	AGlobalModLoaderTask::CreateSingleton()->Create();
	RawLog( "mod loader task created, %u mod(s)", AGlobalModLoaderTask::Get()->GetMods().Size() );

	return CallOriginal( a_pchName, a_iArgc, a_ppArgv );
}

MEMBER_HOOK( 0x006c1760, TApplication, TApplication_Destroy, void )
{
	AGlobalModLoaderTask::Get()->UnloadMods();
	CallOriginal();
}

HOOK( 0x004042c0, ToshiDestroy, void )
{
	AGlobalModLoaderTask::Get()->UnloadMods();
	CallOriginal();

	// Temporary solution
	ExitProcess( 0 );
}

DWORD WINAPI MainThread( HMODULE hModule )
{
	TINFO( "BYModCore thread has been started!\n" );
	TINFO( "Waiting for Toshi systems to be loaded...\n" );

	RawLog( "main thread started" );
	// Wait until AGUI2 is ready to use
	for ( TINT i = 0; !AGUI2::IsSingletonCreated() || !AGUI2::GetSingleton()->GetRootElement(); i++ )
	{
		if ( i == 600 ) // 30 s without AGUI2: record where every thread is
		{
			extern void create_hang_dump( const char* );
			create_hang_dump( "Logs\\hang.dmp" );
			RawLog( "wrote Logs\\hang.dmp" );
		}
		if ( i % 100 == 0 )
		{
			void* pGui = *(void**)0x007b4ff4;
			RawLog( "waiting for AGUI2: singleton=%p raw+0x20=%p sdk root=%p sizeof(TTask)=%u", pGui,
			        pGui ? *(void**)( (char*)pGui + 0x20 ) : TNULL,
			        AGUI2::IsSingletonCreated() ? (void*)AGUI2::GetSingleton()->GetRootElement() : TNULL, (unsigned)sizeof( Toshi::TTask ) );
		}
		ThreadSleep( 50 );
	}
	RawLog( "AGUI2 ready" );

	// Log info about AGUI2
	TFLOAT fWidth, fHeight;
	AGUI2::GetSingleton()->GetDimensions( fWidth, fHeight );
	TINFO( "AGUI2 is ready! (Dimensions: %.0fx%.0f)\n", fWidth, fHeight );

	TUtil::SetTPStringPool( **(TPString8Pool***)0x007ce230 );

	AGlobalModLoaderTask::Get()->OnAGUI2Ready();
	return TTRUE;
}

BOOL WINAPI exit_handler( DWORD dwCtrlType )
{
	switch ( dwCtrlType )
	{
		case CTRL_C_EVENT:
			return TRUE;
		case CTRL_BREAK_EVENT:
			return TRUE;
		case CTRL_CLOSE_EVENT:
			return TRUE;
		default:
			return FALSE;
	}

	return TRUE;
}

LONG WINAPI unhandled_handler( struct _EXCEPTION_POINTERS* apExceptionInfo );

DWORD APIENTRY DllMain( HMODULE hModule, DWORD reason, LPVOID reserved )
{
	switch ( reason )
	{
		case DLL_PROCESS_ATTACH:
		{
			SetUnhandledExceptionFilter( unhandled_handler );
			TMemory::Initialise( 64 * 1024 * 1024, 0 );

#ifdef TOSHI_DEBUG
			AllocConsole();
			FILE* fDummy;
			freopen_s( &fDummy, "CONOUT$", "w", stdout );
#endif

			TUtil::TOSHIParams toshiParams;
			toshiParams.szCommandLine = "";
			toshiParams.szLogFileName = "modcore";
			toshiParams.szLogAppName  = "BYModCore";

			TUtil::ToshiCreate( toshiParams );

			g_oSettings.Load();
			g_CommandLine.Create( GetCommandLineA() );
			RawLog( "command line: %s | -windowed=%d -width=%s", GetCommandLineA(), g_CommandLine.HasParameter( "-windowed" ), g_CommandLine.GetParameterValue( "-width", "-" ).GetString() );

			// Fix necessary regedit records missing
			HKEY    hKey;
			LSTATUS nStatus = RegCreateKeyExA( HKEY_CURRENT_USER, "Software\\THQ\\Barnyard", NULL, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hKey, NULL );
			if ( nStatus == ERROR_SUCCESS ) RegCloseKey( hKey );

			// Initialise hooks
			RawLog( "DllMain: installing hooks" );
			AHooks::Initialise();
			InstallHook<TApplication_Create>();
			InstallHook<TApplication_Destroy>();
			InstallHook<ToshiDestroy>();

			SetConsoleCtrlHandler( exit_handler, TRUE );

			// Create thread
			TINFO( "Log system was successfully initialised!\n" );
			TINFO( "Starting BYModCore thread...\n" );

			HANDLE hThread = CreateThread( 0, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, 0 );

			if ( hThread == NULL )
			{
				TERROR( "Couldn't start the main thread\n" );
				return TFALSE;
			}

			CloseHandle( hThread );
			return TTRUE;
		}
		default:
			return TFALSE;
	}
}
