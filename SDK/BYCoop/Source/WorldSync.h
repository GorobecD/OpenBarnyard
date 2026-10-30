#pragma once
// Stage 2 of the shared world (the host's world is the reference):
//  - game time and date follow the host
//  - regrowth objects (fruits, flowers, crates...): taking one takes it everywhere, the host's
//    growth state is mirrored to the joiners (their own random growth is off)
//  - fruits shaken out of a tree appear for everyone and disappear when anyone picks them up
//  - one inventory and one money count: the host's
#include "Net/Session.h"
#include <functional>
#include <mutex>
#include <string>

namespace WorldSync
{
void Install( coop::Session* a_pSession, std::mutex* a_pMutex, std::function<void( const std::string& )> a_fnLog );

// Game thread, every frame. a_bInWorld: our cow walks the host's world (host: its own world).
void Update( bool a_bInWorld );
} // namespace WorldSync
