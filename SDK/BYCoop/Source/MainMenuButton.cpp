// "MULTIPLAYER" button in the retail main menu (AFrontEndMainMenuState2), styled exactly like the
// game's own buttons: the retail per-button setup sequence (OnInsertion, 0x00409110..0x004091ec)
// is replayed for one extra button, inserted right under LOAD GAME (before OPTIONS). Activating it opens a game popup
// (AMessagePopupState) instead of the ImGui overlay.
// Addresses/offsets: barnyard.exe SHA-256 b243e121...af4f4d (retail PC, RU), see games/barnyard/NOTES.md.
#include "pch.h"
#include "MainMenuButton.h"
#include <HookHelpers.h>
#include <BYardSDK/AGameStateController.h>
#include <windows.h>

TOSHI_NAMESPACE_USING

namespace
{
// AFrontEndMainMenuState2 layout (retail)
constexpr TUINTPTR MAINMENU_VTABLE   = 0x00716d18;
constexpr TUINT    OFF_MENU          = 0x5b0;  // AGUI2Menu m_oMenu
constexpr TUINT    OFF_ROTATIONS     = 0x758;  // const TFLOAT* m_pButtonRotations
constexpr TUINT    OFF_BUTTONS       = 0x7d8;  // AGUI2Button m_aButtons[ 6 ]
constexpr TUINT    SIZEOF_BUTTON     = 0x204;
constexpr TUINT    OFF_ACTIVATED_ID  = 0x13f0; // TINT m_iActivatedButtonID
constexpr TINT     BUTTON_OPTIONS    = 2; // ours goes in front of it, i.e. under LOAD GAME
constexpr TINT     BUTTON_MULTIPLAYER = 6;

// AStartMenuState layout (retail, vtable 0x0071a440): buttons RESUME, RESTART, OPTIONS, HELP, QUIT
constexpr TUINTPTR PAUSEMENU_VTABLE     = 0x0071a440;
constexpr TUINT    OFF_PAUSE_BUTTONS    = 0x850;
constexpr TUINT    OFF_PAUSE_ACTIVATED  = 0x1264;
constexpr TINT     PAUSE_BUTTON_QUIT    = 4;
constexpr TINT     PAUSE_BUTTON_MULTIPLAYER = 5;

// AGUI2Button (retail)
constexpr TUINT OFF_BTN_ID            = 0x60;
constexpr TUINT OFF_BTN_SCALEONFOCUS  = 0x64;
constexpr TUINT OFF_BTN_TRANSFORM     = 0x10;

typedef void*( __stdcall* t_Malloc )( TUINT, TINT, TINT, TINT ); // ret 0x10
typedef void*( __stdcall* t_FindFont )( const char* ); // ret 4
typedef const wchar_t*( __thiscall* t_GetString )( void* a_pLocale, TINT a_iId );
typedef TFLOAT( __thiscall* t_GetTextWidth )( void* a_pFont, const wchar_t* a_wszText, TFLOAT a_fScale );
typedef void( __thiscall* t_BtnCtor )( void* );
typedef void( __thiscall* t_BtnCreate )( void*, void* a_pFont, TFLOAT a_fWidth, const wchar_t* a_wszText, TFLOAT a_fGap );
typedef void( __thiscall* t_BtnSetUInt )( void*, TUINT );
typedef void( __thiscall* t_BtnSetFloat )( void*, TFLOAT );
typedef void( __thiscall* t_BtnSetImage )( void*, const char*, TFLOAT, TFLOAT );
typedef void*( __thiscall* t_BtnGetText )( void* );
typedef void( __thiscall* t_Rotate )( void* a_pTransform, TFLOAT a_fAngle );
typedef void( __thiscall* t_VoidThis )( void* );
typedef void( __thiscall* t_SetAlphaV )( void*, TFLOAT );
typedef void*( __thiscall* t_PopupCtor )( void*, const wchar_t*, const wchar_t*, const void* a_pTPString8, TINT, TBOOL, TINT, TINT, TINT, TINT, TBOOL, TFLOAT, TUINT );
typedef void( __thiscall* t_ListenerConnect )( void* a_pListener, void* a_pEmitter, void* a_pCaller, void* a_pfnCallback, TINT a_iPriority );

const auto Malloc          = (t_Malloc)0x006b5540;
const auto FindFont        = (t_FindFont)0x006c44d0;
const auto GetString       = (t_GetString)0x006be7b0;
const auto GetTextWidth    = (t_GetTextWidth)0x006c3380;
const auto BtnCtor         = (t_BtnCtor)0x00640020;
const auto BtnCreate       = (t_BtnCreate)0x00640490;
const auto BtnSetFocusedCol = (t_BtnSetUInt)0x006401a0;
const auto BtnSetEnabledCol = (t_BtnSetUInt)0x006401c0;
const auto BtnSetDisabledCol = (t_BtnSetUInt)0x006401d0;
const auto BtnSetFocusedScale = (t_BtnSetFloat)0x00640190;
const auto BtnSetImage     = (t_BtnSetImage)0x00640380;
const auto BtnGetText      = (t_BtnGetText)0x00640220;
const auto Rotate          = (t_Rotate)0x006c2060;
const auto PopupCtor       = (t_PopupCtor)0x0042ead0;
const auto ListenerConnect = (t_ListenerConnect)0x006c4630;

void** const pLocaleManager = (void**)0x007ce6bc;
const char*  FONT_PRIMARY   = (const char*)0x00716444;
const char*  PANEL_IMAGE    = (const char*)0x00716d94; // "Panel_01"
const TFLOAT* pTextShadowAlpha = (const TFLOAT*)0x0071a654;
const TFLOAT* pShadowOffsetMul = (const TFLOAT*)0x0071ec9c;

constexpr TINT INPUT_YES = 10;
constexpr TINT INPUT_NO  = 11;


TBOOL IsRussian()
{
	const wchar_t* wsz = GetString( *pLocaleManager, 292 ); // "NEW GAME"
	return wsz && *wsz >= 0x400 && *wsz <= 0x4ff;
}

// Replays the retail button setup for one more button
void* CreateMultiplayerButton( char* a_pState, const char* a_szFont, TINT a_iId, TINT a_iRotation )
{
	void* pFont = FindFont( a_szFont );
	const wchar_t* wszText = IsRussian() ? L"\x041c\x0423\x041b\x042c\x0422\x0418\x041f\x041b\x0415\x0415\x0420" /* МУЛЬТИПЛЕЕР */ : L"MULTIPLAYER";

	char* pButton = (char*)Malloc( SIZEOF_BUTTON, 0, 0, -1 );
	memset( pButton, 0, SIZEOF_BUTTON );
	BtnCtor( pButton );
	BtnCreate( pButton, pFont, GetTextWidth( pFont, wszText, 1.0f ), wszText, 5.0f );
	BtnSetFocusedCol( pButton, 0xffffffff );
	BtnSetEnabledCol( pButton, 0xffffc800 );
	BtnSetDisabledCol( pButton, 0xff4b4b4b );
	BtnSetFocusedScale( pButton, 1.0f );
	( *(t_VoidThis**)pButton )[ 0x4c / 4 ]( pButton ); // OnFocusLost
	*(TINT*)( pButton + OFF_BTN_ID )            = a_iId;
	*(TBOOL*)( pButton + OFF_BTN_SCALEONFOCUS ) = TFALSE;

	// tilt like the neighbours (the retail table has 6 entries)
	const TFLOAT* pRotations = *(const TFLOAT**)( a_pState + OFF_ROTATIONS );
	Rotate( pButton + OFF_BTN_TRANSFORM, pRotations ? -pRotations[ a_iRotation ] : 0.0f ); // opposite tilt to the button above

	BtnSetImage( pButton, PANEL_IMAGE, 1.25f, 1.5f );

	char* pText                       = (char*)BtnGetText( pButton );
	*(TBOOL*)( pText + 0x54 )         = TTRUE;       // shadow on
	*(TUINT*)( pText + 0x60 )         = 0xa0000000;  // shadow colour
	( *(t_SetAlphaV**)pText )[ 0x3c / 4 ]( pText, *pTextShadowAlpha );
	const TFLOAT fOffset              = *(TFLOAT*)( pText + 0x6c ) * *pShadowOffsetMul;
	*(TFLOAT*)( pText + 0x58 )        = fOffset;
	*(TFLOAT*)( pText + 0x5c )        = fOffset;
	return pButton;
}

MainMenuButton::Opener g_fnOpen;
char*                  g_pMainMenu = nullptr; // the main menu the lobby was opened from
} // namespace

struct AGUI2MenuStub
{};
MEMBER_HOOK( 0x0063e450, AGUI2MenuStub, AGUI2Menu_AddMenuItem, void, void* a_pItem )
{
	char* pState = (char*)this - OFF_MENU;
	if ( a_pItem == pState + OFF_BUTTONS + BUTTON_OPTIONS * SIZEOF_BUTTON && *(TUINTPTR*)pState == MAINMENU_VTABLE )
		CallOriginal( CreateMultiplayerButton( pState, FONT_PRIMARY, BUTTON_MULTIPLAYER, 1 ) ); // goes right under LOAD GAME
	if ( a_pItem == pState + OFF_PAUSE_BUTTONS + PAUSE_BUTTON_QUIT * SIZEOF_BUTTON && *(TUINTPTR*)pState == PAUSEMENU_VTABLE )
		CallOriginal( CreateMultiplayerButton( pState, "Rekord26", PAUSE_BUTTON_MULTIPLAYER, 3 ) ); // above QUIT

	CallOriginal( a_pItem );
}

struct AMainMenuStub
{};
MEMBER_HOOK( 0x004099f0, AMainMenuStub, AFrontEndMainMenuState2_OnMenuClose, void )
{
	char* pState = (char*)this;
	if ( *(TINT*)( pState + OFF_ACTIVATED_ID ) != BUTTON_MULTIPLAYER )
	{
		CallOriginal();
		return;
	}

	*(TINT*)( pState + OFF_ACTIVATED_ID ) = -1;
	g_pMainMenu                           = pState;
	if ( g_fnOpen ) g_fnOpen( false );
}

struct APauseMenuStub
{};
MEMBER_HOOK( 0x0042fa40, APauseMenuStub, AStartMenuState_OnMenuClose, void )
{
	char* pState = (char*)this;
	if ( *(TINT*)( pState + OFF_PAUSE_ACTIVATED ) != PAUSE_BUTTON_MULTIPLAYER )
	{
		CallOriginal();
		return;
	}
	*(TINT*)( pState + OFF_PAUSE_ACTIVATED ) = -1;
	if ( g_fnOpen ) g_fnOpen( true );
}

static void OpenMainMenuItem( TINT a_iButton )
{
	if ( !g_pMainMenu || *(TUINTPTR*)g_pMainMenu != MAINMENU_VTABLE ) return;
	*(TINT*)( g_pMainMenu + OFF_ACTIVATED_ID ) = a_iButton;
	reinterpret_cast<AFrontEndMainMenuState2_OnMenuClose::_hook_obj*>( g_pMainMenu )->CallOriginal();
}

void MainMenuButton::OpenNewGame() { OpenMainMenuItem( 0 ); }  // FRONTENDBUTTON_NEWGAME
void MainMenuButton::OpenLoadGame() { OpenMainMenuItem( 1 ); } // FRONTENDBUTTON_LOADGAME

void MainMenuButton::Install( Opener a_fnOpen )
{
	g_fnOpen = a_fnOpen;
	InstallHook<AGUI2Menu_AddMenuItem>();
	InstallHook<AFrontEndMainMenuState2_OnMenuClose>();
	InstallHook<AStartMenuState_OnMenuClose>();
}
