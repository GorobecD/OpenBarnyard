#pragma once
// Milestone 2: each remote player is shown as a "player" unit (the same unit type as the local cow,
// spawned through AUnitManager::SpawnUnit) and moved every frame to the interpolated network pose.
#include "Net/Session.h"
#include <functional>
#include <string>

namespace RemotePuppets
{
// a_pLocalUnit: the local player's AUnit (TNULL outside a level, e.g. in the menus)
void Update( void* a_pLocalUnit, const std::map<int, coop::RemotePlayer>& a_rRemotes, double a_fNow, double a_fDelay,
             const std::function<void( const std::string& )>& a_fnLog );
// True if a_pUnit is one of our puppets (so it is never mistaken for the local player)
bool IsPuppet( void* a_pUnit );
} // namespace RemotePuppets
