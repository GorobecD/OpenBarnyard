#pragma once
// Developer helpers for Milestone 2 research (remote player cows). Keys work in-game:
//   F8 - write every live unit (id, type, position) to Logs\bycoop.txt
//   F9 - spawn bycoop.ini [dev] spawn=<unit type> 3 m in front of the player and log the result
#include <functional>
#include <string>

namespace DevTools
{
void Update( const std::function<void( const std::string& )>& a_fnLog, const char* a_szSpawnType );
}
