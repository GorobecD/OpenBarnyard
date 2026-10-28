#pragma once
// BYCoop transport layer. Toshi-free so it can be unit tested outside the game.
// Two backends behind one interface:
//  - Steam: AppID 480 (Spacewar) lobbies + ISteamNetworkingMessages via the flat C API,
//    loaded dynamically from steam_api.dll (no SDK headers). Relayed, no port forwarding.
//  - UDP:   plain sockets, for local tests (several game copies on one PC, bots).
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>

namespace coop
{

using PeerId = uint64_t; // SteamID64, or "127.0.0.1:port" packed for UDP

enum Channel : int
{
	CH_RELIABLE   = 0,
	CH_UNRELIABLE = 1,
};

struct Packet
{
	PeerId               from;
	std::vector<uint8_t> data;
};

class ITransport
{
public:
	virtual ~ITransport() = default;

	virtual bool        Init( std::string& err ) = 0;
	virtual void        Shutdown()               = 0;
	virtual void        Pump()                   = 0; // once per frame
	virtual PeerId      LocalId() const          = 0;
	virtual std::string Describe() const         = 0;

	// Session
	virtual bool Host( int maxPlayers ) = 0;
	virtual bool Join( const std::string& target ) = 0; // lobby id (Steam) or host:port (UDP)
	virtual void Leave()                           = 0;
	virtual bool IsInSession() const               = 0;
	virtual bool IsHost() const                    = 0;
	virtual PeerId HostId() const                  = 0;
	virtual std::vector<PeerId> Members() const    = 0;
	virtual void OpenInviteDialog() {}
	// Online friends (for an in-game invite list; the Steam overlay does not hook D3D8 games)
	struct Friend
	{
		PeerId      id;
		std::string name;
		bool        inGame; // currently running the same app (Spacewar 480)
	};
	virtual std::vector<Friend> OnlineFriends() { return {}; }
	virtual bool                Invite( PeerId ) { return false; }

	// Messaging
	virtual bool Send( PeerId to, const void* data, size_t size, bool reliable ) = 0;
	virtual void Receive( std::vector<Packet>& out )                             = 0;

	std::function<void( PeerId )> onPeerJoined;
	std::function<void( PeerId )> onPeerLeft;
	std::function<void( const std::string& )> onLog;
};

ITransport* CreateSteamTransport();
ITransport* CreateUdpTransport( uint16_t localPort );

} // namespace coop
