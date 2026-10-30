// See NativeMenu.h. Retail addresses: barnyard.exe b243e121... (PC RU), offsets from OpenBarnyard
// (GameInterface/ABYardMenuState, AStartMenuState) and the disassembly of ABYardMenuState::ABYardMenuState 0x00432390.
#include "pch.h"
#include "NativeMenu.h"
#include <BYardSDK/AGameStateController.h>
#include <windows.h>
#include <cstring>

TOSHI_NAMESPACE_USING

namespace
{
// ABYardMenuState (retail, 0x7d8 bytes)
constexpr TUINT    SIZEOF_BASE     = 0x7d8;
constexpr TUINTPTR BASE_VTABLE     = 0x0071a658;
constexpr int      NUM_VIRTUALS    = 30;
constexpr TUINT    OFF_MENU        = 0x5b0; // AGUI2Menu m_oMenu
constexpr TUINT    OFF_TITLE       = 0x6cc; // AGUI2TextBox m_oDialogTitle
constexpr TUINT    OFF_MENUSTATE   = 0x744;
// our part of the object
constexpr TUINT    OFF_OWNER       = SIZEOF_BASE;     // NativeMenu::Menu*
constexpr TUINT    OFF_BUTTONS     = SIZEOF_BASE + 8; // AGUI2Button[ MAX_ITEMS ]
constexpr TUINT    SIZEOF_BUTTON   = 0x204;
constexpr int      MAX_ITEMS       = 14;

// vtable slots (AGameState + ABYardMenuState, see OpenBarnyard)
enum
{
	VT_DTOR            = 2,
	VT_ONUPDATE        = 19,
	VT_ONINSERTION     = 20,
	VT_ONREMOVAL       = 21,
	VT_ONITEMACTIVATED = 27,
	VT_ONMENUCLOSE     = 29,
};

// AGUI2Button / AGUI2TextBox
constexpr TUINT OFF_BTN_ID           = 0x60;
constexpr TUINT OFF_BTN_SCALEONFOCUS = 0x64;
constexpr TUINT OFF_TEXT_STR         = 0x64; // AGUI2TextBox::m_wszText
constexpr TUINT OFF_TEXT_DIRTY       = 0x74; // AGUI2TextBox::m_bDimensionsDirty

typedef void*( __stdcall* t_Malloc )( TUINT, TINT, TINT, TINT );
typedef void( __stdcall* t_Free )( void* );
typedef void*( __stdcall* t_FindFont )( const char* );
typedef const wchar_t*( __thiscall* t_GetString )( void*, TINT );
typedef TFLOAT( __thiscall* t_GetTextWidth )( void*, const wchar_t*, TFLOAT );
typedef void( __thiscall* t_VoidThis )( void* );
typedef void( __thiscall* t_BtnCreate )( void*, void*, TFLOAT, const wchar_t*, TFLOAT );
typedef void( __thiscall* t_BtnSetUInt )( void*, TUINT );
typedef void( __thiscall* t_BtnSetFloat )( void*, TFLOAT );
typedef void( __thiscall* t_BtnSetImage )( void*, const char*, TFLOAT, TFLOAT );
typedef char*( __thiscall* t_BtnGetText )( void* );
typedef void( __thiscall* t_SetAlphaV )( void*, TFLOAT );
typedef void( __thiscall* t_MenuAdd )( void*, void* );
typedef TBOOL( __thiscall* t_UpdateF )( void*, TFLOAT );
typedef void( __thiscall* t_ItemThis )( void*, void* );
typedef void( __thiscall* t_Dtor )( void*, TUINT );
typedef void*( __thiscall* t_PopupCtor )( void*, const wchar_t*, const wchar_t*, const void*, TINT, TBOOL, TINT, TINT, TINT, TINT, TBOOL, TFLOAT, TUINT );
typedef void( __thiscall* t_ListenerConnect )( void*, void*, void*, void*, TINT );

const auto Malloc            = (t_Malloc)0x006b5540;
const auto Free              = (t_Free)0x006b4df0;
const auto FindFont          = (t_FindFont)0x006c44d0;
const auto GetString         = (t_GetString)0x006be7b0;
const auto GetTextWidth      = (t_GetTextWidth)0x006c3380;
const auto BaseCtor          = (t_VoidThis)0x00432390;
const auto BaseDtor          = (t_VoidThis)0x00432470;
const auto BtnCtor           = (t_VoidThis)0x00640020;
const auto BtnCreate         = (t_BtnCreate)0x00640490;
const auto BtnSetFocusedCol  = (t_BtnSetUInt)0x006401a0;
const auto BtnSetEnabledCol  = (t_BtnSetUInt)0x006401c0;
const auto BtnSetDisabledCol = (t_BtnSetUInt)0x006401d0;
const auto BtnSetFocusedScale = (t_BtnSetFloat)0x00640190;
const auto BtnSetImage       = (t_BtnSetImage)0x00640380;
const auto BtnGetText        = (t_BtnGetText)0x00640220;
const auto MenuAddItem       = (t_MenuAdd)0x0063e450;
const auto MenuSetFocusAt    = (t_MenuAdd)0x0063e4f0;
const auto BaseOnUpdate      = (t_UpdateF)0x004325b0;
const auto BaseOnInsertion   = (t_VoidThis)0x00432f30;
const auto BaseOnRemoval     = (t_VoidThis)0x00432530;
const auto OnButtonActivated = (t_ItemThis)0x00432a90;
const auto StateRemove       = (t_VoidThis)0x004282d0; // AGameState::Remove
const auto PopupCtor         = (t_PopupCtor)0x0042ead0;
const auto ListenerConnect   = (t_ListenerConnect)0x006c4630;

void** const pLocaleManager    = (void**)0x007ce6bc;
const char*  FONT_PRIMARY      = (const char*)0x00716444;
const char*  PANEL_IMAGE       = (const char*)0x00716d94; // "Panel_01"
const TFLOAT* pTextShadowAlpha = (const TFLOAT*)0x0071a654;
const TFLOAT* pShadowOffsetMul = (const TFLOAT*)0x0071ec9c;

void* g_aVtable[ NUM_VIRTUALS + 1 ]; // filled on first use

NativeMenu::Menu* Owner( void* a_pState ) { return *(NativeMenu::Menu**)( (char*)a_pState + OFF_OWNER ); }
char*             Button( void* a_pState, int i ) { return (char*)a_pState + OFF_BUTTONS + i * SIZEOF_BUTTON; }

void UnlinkElement( void* a_pElement )
{
	// T2GUIElementNode::Unlink (vptr, m_pNext, m_pPrev)
	void** pNode = (void**)a_pElement;
	void*  pNext = pNode[ 1 ];
	void*  pPrev = pNode[ 2 ];
	( (void**)pNext )[ 2 ] = pPrev;
	( (void**)pPrev )[ 1 ] = pNext;
	pNode[ 1 ]             = a_pElement;
	pNode[ 2 ]             = a_pElement;
}

void __fastcall Vt_OnMenuClose( void* a_pThis, void* );

// --- replaced virtuals (__fastcall with an unused edx == __thiscall) ---
void* __fastcall Vt_Dtor( void* a_pThis, void*, TUINT a_uiFlags )
{
	NativeMenu::Menu* pMenu = Owner( a_pThis );
	const int         n     = pMenu ? (int)pMenu->def.items.size() : 0;
	for ( int i = 0; i < n; i++ ) ( *(t_Dtor**)Button( a_pThis, i ) )[ 0 ]( Button( a_pThis, i ), 0 );
	BaseDtor( a_pThis );
	delete pMenu;
	if ( a_uiFlags & 1 ) Free( a_pThis );
	return a_pThis;
}

TBOOL __fastcall Vt_OnUpdate( void* a_pThis, void*, TFLOAT a_fDelta )
{
	NativeMenu::Menu* pMenu = Owner( a_pThis );
	if ( pMenu && pMenu->def.onUpdate && !pMenu->bClosing ) pMenu->def.onUpdate( *pMenu );
	if ( pMenu && pMenu->bWantClose && !pMenu->bClosing )
	{
		pMenu->iActivated = -1;
		Vt_OnMenuClose( a_pThis, nullptr ); // deletes the state: don't touch it after
		return TTRUE;
	}
	return BaseOnUpdate( a_pThis, a_fDelta );
}

void __fastcall Vt_OnInsertion( void* a_pThis, void* )
{
	NativeMenu::Menu* pMenu = Owner( a_pThis );
	void*             pFont = FindFont( FONT_PRIMARY );
	const int         n     = (int)pMenu->def.items.size();
	for ( int i = 0; i < n; i++ )
	{
		const auto& it      = pMenu->def.items[ i ];
		char*       pButton = Button( a_pThis, i );
		TFLOAT      fWidth  = GetTextWidth( pFont, it.text.c_str(), 1.0f );
		if ( !it.reserve.empty() ) { const TFLOAT f2 = GetTextWidth( pFont, it.reserve.c_str(), 1.0f ); if ( f2 > fWidth ) fWidth = f2; }
		BtnCreate( pButton, pFont, fWidth, pMenu->texts[ i ].c_str(), 5.0f );
		BtnSetFocusedCol( pButton, 0xffffffff );
		BtnSetEnabledCol( pButton, 0xffffc800 );
		BtnSetDisabledCol( pButton, 0xff4b4b4b );
		BtnSetFocusedScale( pButton, 1.0f );
		( *(t_VoidThis**)pButton )[ 0x4c / 4 ]( pButton ); // OnFocusLost
		*(TINT*)( pButton + OFF_BTN_ID )            = it.id;
		*(TBOOL*)( pButton + OFF_BTN_SCALEONFOCUS ) = TFALSE;
		BtnSetImage( pButton, PANEL_IMAGE, 1.25f, 1.5f );
		char* pText                = BtnGetText( pButton );
		*(TBOOL*)( pText + 0x54 )  = TTRUE;      // shadow on
		*(TUINT*)( pText + 0x60 )  = 0xa0000000; // shadow colour
		( *(t_SetAlphaV**)pText )[ 0x3c / 4 ]( pText, *pTextShadowAlpha );
		const TFLOAT fOffset       = *(TFLOAT*)( pText + 0x6c ) * *pShadowOffsetMul;
		*(TFLOAT*)( pText + 0x58 ) = fOffset;
		*(TFLOAT*)( pText + 0x5c ) = fOffset;
		MenuAddItem( (char*)a_pThis + OFF_MENU, pButton );
	}
	if ( n > 0 ) MenuSetFocusAt( (char*)a_pThis + OFF_MENU, Button( a_pThis, pMenu->def.focus >= 0 && pMenu->def.focus < n ? pMenu->def.focus : 0 ) );

	// title (the base sizes its panel from the text)
	*(const wchar_t**)( (char*)a_pThis + OFF_TITLE + OFF_TEXT_STR ) = pMenu->def.title.c_str();
	BaseOnInsertion( a_pThis );
}

void __fastcall Vt_OnRemoval( void* a_pThis, void* )
{
	NativeMenu::Menu* pMenu = Owner( a_pThis );
	const int         n     = pMenu ? (int)pMenu->def.items.size() : 0;
	for ( int i = 0; i < n; i++ ) UnlinkElement( Button( a_pThis, i ) );
	BaseOnRemoval( a_pThis );
}

void __fastcall Vt_OnItemActivated( void* a_pThis, void*, void* a_pItem )
{
	NativeMenu::Menu* pMenu = Owner( a_pThis );
	const int         iId   = *(TINT*)( (char*)a_pItem + OFF_BTN_ID );
	if ( pMenu->bClosing ) return;
	const bool bClose = !pMenu->def.onActivate || pMenu->def.onActivate( *pMenu, iId );
	if ( !bClose || pMenu->bClosing ) return;
	pMenu->iActivated = iId;
	OnButtonActivated( a_pThis, a_pItem ); // animation, then OnMenuClose
}

void __fastcall Vt_OnMenuClose( void* a_pThis, void* )
{
	NativeMenu::Menu* pMenu = Owner( a_pThis );
	if ( !pMenu->bClosing )
	{
		pMenu->bClosing = true;
		auto      fn    = pMenu->def.onClose;
		const int iId   = pMenu->iActivated;
		StateRemove( a_pThis ); // deletes the state and pMenu
		if ( fn ) fn( iId );
	}
}

void BuildVtable()
{
	if ( g_aVtable[ 0 ] ) return;
	memcpy( g_aVtable, (void*)BASE_VTABLE, sizeof( g_aVtable ) );
	g_aVtable[ VT_DTOR ]            = (void*)Vt_Dtor;
	g_aVtable[ VT_ONUPDATE ]        = (void*)Vt_OnUpdate;
	g_aVtable[ VT_ONINSERTION ]     = (void*)Vt_OnInsertion;
	g_aVtable[ VT_ONREMOVAL ]       = (void*)Vt_OnRemoval;
	g_aVtable[ VT_ONITEMACTIVATED ] = (void*)Vt_OnItemActivated;
	g_aVtable[ VT_ONMENUCLOSE ]     = (void*)Vt_OnMenuClose;
}

struct PopupCtx
{
	std::wstring              title, message;
	std::function<void( bool )> fn;
	TBYTE                     aListener[ 0x40 ] = {};
};
struct ResponseEvent
{
	TINT eButton;
};
// Same contract as retail OnQuitPopupResponse (0x00408ef0): __stdcall, close the popup, return FALSE
TBOOL __stdcall OnPopupResponse( PopupCtx* a_pCtx, void* a_pPopup, ResponseEvent* a_pEvent )
{
	const bool bYes = a_pEvent && ( a_pEvent->eButton == 10 /* Yes */ || a_pEvent->eButton == 1 /* OK */ );
	if ( a_pPopup ) StateRemove( a_pPopup );
	if ( a_pCtx->fn ) a_pCtx->fn( bYes );
	// the context (listener) stays allocated: the popup's emitter may still refer to it while it dies
	return TFALSE;
}

void ShowPopup( const std::wstring& a_wsTitle, const std::wstring& a_wsMessage, bool a_bYesNo, std::function<void( bool )> a_fn )
{
	PopupCtx* pCtx = new PopupCtx();
	pCtx->title    = a_wsTitle;
	pCtx->message  = a_wsMessage;
	pCtx->fn       = std::move( a_fn );
	TINT  aEmptyTPString8 = 0;
	void* pPopup          = Malloc( 0x19c8, 0, 0, -1 );
	if ( a_bYesNo )
		PopupCtor( pPopup, pCtx->title.c_str(), pCtx->message.c_str(), &aEmptyTPString8, 11, TFALSE, 10 /* Yes */, 11 /* No */, 0, 0, TFALSE, 60.0f, 0 );
	else
		PopupCtor( pPopup, pCtx->title.c_str(), pCtx->message.c_str(), &aEmptyTPString8, 1, TFALSE, 1 /* OK */, 0, 0, 0, TFALSE, 60.0f, 0 );
	ListenerConnect( pCtx->aListener, (char*)pPopup + 0x7d8, pCtx, (void*)OnPopupResponse, 0 );
	AGameStateController::GetSingleton()->PushState( (AGameState*)pPopup );
}
} // namespace

void NativeMenu::Menu::SetText( int a_iId, const std::wstring& a_wsText )
{
	for ( size_t i = 0; i < def.items.size(); i++ )
		if ( def.items[ i ].id == a_iId )
		{
			texts[ i ]   = a_wsText;
			char* pText  = BtnGetText( Button( m_pState, (int)i ) );
			*(const wchar_t**)( pText + OFF_TEXT_STR ) = texts[ i ].c_str();
			*(TBOOL*)( pText + OFF_TEXT_DIRTY )        = TTRUE;
		}
}

NativeMenu::Menu* NativeMenu::Push( Def a_def )
{
	BuildVtable();
	if ( a_def.items.size() > MAX_ITEMS ) a_def.items.resize( MAX_ITEMS );
	Menu* pMenu = new Menu();
	pMenu->def  = std::move( a_def );
	for ( auto& it : pMenu->def.items ) pMenu->texts.push_back( it.text );

	const TUINT uiSize = OFF_BUTTONS + MAX_ITEMS * SIZEOF_BUTTON;
	char*       pState = (char*)Malloc( uiSize, 0, 0, -1 );
	memset( pState, 0, uiSize );
	BaseCtor( pState );
	*(void**)pState                    = g_aVtable;
	*(Menu**)( pState + OFF_OWNER )    = pMenu;
	pMenu->m_pState                    = pState;
	for ( size_t i = 0; i < pMenu->def.items.size(); i++ ) BtnCtor( Button( pState, (int)i ) );

	AGameStateController::GetSingleton()->PushState( (AGameState*)pState );
	return pMenu;
}

void NativeMenu::AskYesNo( const std::wstring& a_wsTitle, const std::wstring& a_wsMessage, std::function<void( bool )> a_fnResult )
{
	ShowPopup( a_wsTitle, a_wsMessage, true, std::move( a_fnResult ) );
}

void NativeMenu::Message( const std::wstring& a_wsTitle, const std::wstring& a_wsMessage )
{
	ShowPopup( a_wsTitle, a_wsMessage, false, nullptr );
}

bool NativeMenu::IsRussian()
{
	const wchar_t* wsz = GetString( *pLocaleManager, 292 ); // "NEW GAME"
	return wsz && *wsz >= 0x400 && *wsz <= 0x4ff;
}
