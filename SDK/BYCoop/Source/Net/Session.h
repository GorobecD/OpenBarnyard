#pragma once
// Game-agnostic co-op session: player roster, appearance, pose stream with interpolation.
// Toshi-free; the mod feeds it the local cow's pose and reads back remote cows.
#include "Transport.h"
#include <map>
#include <memory>
#include <cstring>

namespace coop
{

constexpr uint32_t PROTOCOL_VERSION = 3; // 2: Pose::levelHash = terrain id + 1, 3: world transfer
constexpr int      MAX_PLAYERS      = 4;

// What the player picked in the character creator (AFrontEndCharacterSelectState).
struct Appearance
{
	uint8_t gender = 0;       // 0 male, 1 female (as stored in APlayerSimProfile)
	uint8_t hide   = 0;       // index into the creator's hide list
	char    name[ 24 ] = {};  // display name (Steam persona or typed)
	bool operator==( const Appearance& o ) const { return gender == o.gender && hide == o.hide && !strncmp( name, o.name, sizeof( name ) ); }
};

struct Pose
{
	float    pos[ 3 ] = {};
	float    yaw      = 0;
	float    speed    = 0;
	uint16_t anim     = 0;   // engine animation id, 0 = idle
	uint16_t flags    = 0;   // bit0 riding, bit1 swimming, bit2 in minigame...
	uint32_t levelHash = 0;  // which map the player is in; remote cows only show on the same map
};

enum MsgType : uint8_t
{
	MSG_JOIN       = 1, // joiner -> host: version + appearance
	MSG_WELCOME    = 2, // host -> joiner: slot index, or rejection
	MSG_PLAYERINFO = 3, // host -> all: slot <-> peer <-> appearance (whole roster)
	MSG_POSE       = 4, // anyone -> host -> others (host relays): slot + tick + pose
	MSG_WORLD      = 5, // host -> all: world events (M3)
	MSG_WORLD_REQ  = 6, // joiner -> host: send me your world (save snapshot)
	MSG_WORLD_DATA = 7, // host -> joiner: status, level, position, save slot bytes
	MSG_WORLD_READY = 8, // host -> all: the host entered its world, waiting joiners ask again
};

// World snapshot sent to a joiner (stage 1: the host's save slot + where the host is)
struct WorldData
{
	bool                 ready = false; // false: the host is not in its world yet (menu, minigame)
	uint32_t             level = 0;
	float                pos[ 3 ] = {};
	std::vector<uint8_t> slot;          // APlayerProgress::SaveSlot (0xd170 bytes)
};

struct RemotePlayer
{
	PeerId     peer   = 0;
	int        slot   = -1;
	Appearance look;
	bool       hasPose = false;
	// two latest snapshots for interpolation
	Pose       a, b;
	double     ta = 0, tb = 0;
	Pose       Sample( double now, double delay ) const;
};

class Session
{
public:
	explicit Session( ITransport* t );
	~Session();

	bool Start( std::string& err );
	void SetLocalAppearance( const Appearance& a );
	void SetLocalPose( const Pose& p ) { m_localPose = p; m_hasLocalPose = true; }

	// call once per frame
	void Update( double now );

	bool Host() { return m_t->Host( MAX_PLAYERS ); }
	bool Join( const std::string& target ) { return m_t->Join( target ); }
	void Leave();

	// World transfer. Host: PopWorldRequest/SendWorld/BroadcastWorldReady from the game thread.
	// Joiner: RequestWorld, then PopWorld once it arrives (also re-request on WorldReady).
	void RequestWorld();
	bool PopWorldRequest( PeerId& a_rFrom );
	void SendWorld( PeerId a_to, const WorldData& a_rData );
	void BroadcastWorldReady();
	bool PopWorld( WorldData& a_rOut );
	bool PopWorldReady();

	ITransport*      Transport() { return m_t.get(); }
	int              LocalSlot() const { return m_localSlot; }
	const std::map<int, RemotePlayer>& Remotes() const { return m_remotes; }
	std::string      Status() const { return m_status; }

	double interpDelay = 0.1; // s
	double sendRate    = 20;  // poses/s

	std::function<void( const std::string& )> onLog;

private:
	void OnPacket( const Packet& p, double now );
	void SendRoster();
	void Log( const std::string& s ) { if ( onLog ) onLog( s ); }

	std::unique_ptr<ITransport> m_t;
	Appearance                  m_localLook;
	Pose                        m_localPose;
	bool                        m_hasLocalPose = false;
	int                         m_localSlot    = -1;
	bool                        m_sentJoin     = false;
	double                      m_lastSend     = 0, m_lastJoinTry = 0;
	uint32_t                    m_tick         = 0;
	std::map<int, RemotePlayer> m_remotes;                 // by slot, excludes local
	std::map<PeerId, int>       m_slotOf;                  // host only: peer -> slot
	std::map<PeerId, Appearance> m_looks;                  // host only
	std::map<int, uint32_t>     m_lastTick;                // drop stale/reordered poses
	std::string                 m_status = "offline";
	std::vector<PeerId>         m_worldRequests;           // host
	std::vector<WorldData>      m_worlds;                  // joiner
	bool                        m_worldReady = false;      // joiner: host announced its world
};

} // namespace coop
