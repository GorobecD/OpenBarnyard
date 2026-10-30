#pragma once
// "MULTIPLAYER" / "МУЛЬТИПЛЕЕР" button in the retail main menu (right under "Load game") and in the
// pause menu (above "Quit"), styled like the game's own buttons.
#include <functional>

namespace MainMenuButton
{
// Called when the button is chosen (a_bPauseMenu: from the in-game pause menu)
using Opener = std::function<void( bool a_bPauseMenu )>;

void Install( Opener a_fnOpen );

// Opens the game's "New game" / "Load game" screen from the main menu (the host starts its world)
void OpenNewGame();
void OpenLoadGame();
} // namespace MainMenuButton
