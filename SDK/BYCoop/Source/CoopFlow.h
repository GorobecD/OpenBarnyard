#pragma once
// Stage 1 of the shared world: lobby and invites in game-styled menus, the in-game invite notice,
// the cow creator of a joining player and entering the host's world (the host's save, loaded
// without touching the joiner's own saves).
#include "Net/Session.h"
#include <functional>
#include <mutex>
#include <string>

namespace CoopFlow
{
struct Context
{
	coop::Session*                             pSession = nullptr;
	std::mutex*                                pMutex   = nullptr; // guards pSession (network thread)
	coop::Appearance*                          pLook    = nullptr; // our name; gender/hide come from the profile
	std::function<void( const std::string& )> fnLog;
};

void Install( const Context& a_rCtx );

// Main menu / pause menu MULTIPLAYER button
void OpenLobby( bool a_bFromPauseMenu );

// Game thread, every frame. a_bInLevel: our cow is in the world; a_pPos its position.
void Update( bool a_bInLevel, const float* a_pPos );

// ImGui overlay (the invite notice in the corner)
void DrawOverlay();

// True while this player plays in the host's world (its own saves are protected)
bool InHostWorld();
} // namespace CoopFlow
