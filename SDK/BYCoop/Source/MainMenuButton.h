#pragma once
// "MULTIPLAYER" / "МУЛЬТИПЛЕЕР" button in the retail main menu, right under "Load game".
#include <functional>
#include <string>

namespace MainMenuButton
{
// Fills the popup title/text for the current co-op state (a_bRussian: game language)
using Describer = std::function<void( bool a_bRussian, std::wstring& a_rTitle, std::wstring& a_rMessage )>;
// Called with the player's answer (true = Yes)
using Handler = std::function<void( bool a_bYes )>;

void Install( Describer a_fnDescribe, Handler a_fnHandler );
} // namespace MainMenuButton
