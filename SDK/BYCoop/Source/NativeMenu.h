#pragma once
// Game-styled menus built from the retail ABYardMenuState (the base of the options/pause menus):
// same dialog, title panel, buttons, sounds, keyboard/mouse navigation. The state object is allocated
// like a retail state, constructed with the retail base constructor and given a copy of the base
// vtable in which the virtuals we need are replaced (no C++ mirror of the retail class).
#include <functional>
#include <string>
#include <vector>

namespace NativeMenu
{
struct Item
{
	int          id = 0;
	std::wstring text;
	std::wstring reserve; // the button is sized for the longer of text/reserve (its text can change)
};

class Menu;

struct Def
{
	std::wstring      title;
	std::vector<Item> items;
	int               focus = 0; // index of the item focused first
	// Activation: return true to close the menu (with the retail button animation), false to stay
	std::function<bool( Menu&, int a_iId )> onActivate;
	// The menu has closed: a_iId of the activated item, -1 if cancelled (Esc). The state is removed after.
	std::function<void( int a_iId )> onClose; // (the menu object is gone by then)
	// Every frame while the menu is on top
	std::function<void( Menu& )> onUpdate;
};

class Menu
{
public:
	// Changes the text of an item in place (the menu stays open)
	void SetText( int a_iId, const std::wstring& a_wsText );
	// Removes the menu on its next update (e.g. the session ended); onClose is called with -1
	void Close() { bWantClose = true; }
	void* State() const { return m_pState; }

	Def                       def;
	std::vector<std::wstring> texts; // stable buffers the buttons point at
	void*                     m_pState = nullptr;
	int                       iActivated = -1;
	bool                      bClosing   = false;
	bool                      bWantClose = false;
};

// Pushes a new menu on top of the game state stack
Menu* Push( Def a_def );

// Game-styled Yes/No popup (retail AMessagePopupState, as the QUIT question)
void AskYesNo( const std::wstring& a_wsTitle, const std::wstring& a_wsMessage, std::function<void( bool a_bYes )> a_fnResult );

// Game-styled message with OK
void Message( const std::wstring& a_wsTitle, const std::wstring& a_wsMessage );

// True if the game shows Russian text (the menus follow the game's language)
bool IsRussian();
} // namespace NativeMenu
