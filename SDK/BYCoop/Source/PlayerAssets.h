#pragma once
// Every player picks a gender (model) and a hide (costume = material library) in the creator.
// Retail Barnyard loads the player assets of the local gender and costume only
// (AAssetLoader::LoadPlayerCharacter); spawning a cow of the other gender then crashed.
// This module also loads what the remote players need: at level load (hook) and, when a
// player joins or changes later, at runtime. Extra assets are released with the level.
#include <functional>
#include <string>

namespace PlayerAssets
{
void Install( const std::function<void( const std::string& )>& a_fnLog );

// Looks the remote players need (gender 0/1, costume 0..6). Main thread, every frame.
void SetWanted( const int* a_pGenders, const int* a_pCostumes, int a_iCount );

// Main thread: loads missing wanted assets while a level is loaded
void Update();

// True if a puppet of a_iGender/a_iCostume can be spawned and textured
bool IsLoaded( int a_iGender, int a_iCostume );
} // namespace PlayerAssets
