// UDP transport for local tests: several game copies on one PC, no Steam.
// Membership: joiner sends HELLO to host; host replies with member list (ROSTER) and
// re-broadcasts it on every change. Keep-alive every 1 s, peer dropped after 10 s silence.
// Reliability: "reliable" packets are resent until ACKed (stop-and-wait per peer seq window
// is overkill on localhost; we resend unacked packets every 100 ms, dedup by seq on receive).
#include "Transport.h"
#ifdef _WIN32
#	include <winsock2.h>
#	include <ws2tcpip.h>
#	pragma comment( lib, "ws2_32.lib" )
typedef int socklen_t;
#else
#	include <arpa/inet.h>
#	include <netinet/in.h>
#	include <sys/socket.h>
#	include <unistd.h>
#	include <fcntl.h>
#	define closesocket close
typedef int SOCKET;
#	define INVALID_SOCKET ( -1 )
#endif
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <algorithm>

namespace coop
{
namespace
{

enum : uint8_t
{
	K_DATA_U = 1,
	K_DATA_R = 2,
	K_ACK    = 3,
	K_HELLO  = 4,
	K_ROSTER = 5,
	K_PING   = 6,
	K_BYE    = 7,
};

static double Now()
{
	using namespace std::chrono;
	return duration<double>( steady_clock::now().time_since_epoch() ).count();
}

static PeerId Pack( const sockaddr_in& a ) { return ( (uint64_t)ntohl( a.sin_addr.s_addr ) << 16 ) | ntohs( a.sin_port ); }
static sockaddr_in Unpack( PeerId p )
{
	sockaddr_in a{};
	a.sin_family      = AF_INET;
	a.sin_addr.s_addr = htonl( (uint32_t)( p >> 16 ) );
	a.sin_port        = htons( (uint16_t)( p & 0xffff ) );
	return a;
}

class UdpTransport : public ITransport
{
public:
	explicit UdpTransport( uint16_t port ) : m_port( port ) {}

	bool Init( std::string& err ) override
	{
#ifdef _WIN32
		WSADATA w;
		WSAStartup( MAKEWORD( 2, 2 ), &w );
#endif
		m_s = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
		sockaddr_in a{};
		a.sin_family      = AF_INET;
		a.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
		a.sin_port        = htons( m_port );
		if ( m_s == INVALID_SOCKET || bind( m_s, (sockaddr*)&a, sizeof( a ) ) != 0 )
		{
			err = "UDP bind failed on port " + std::to_string( m_port );
			return false;
		}
#ifdef _WIN32
		u_long nb = 1;
		ioctlsocket( m_s, FIONBIO, &nb );
#else
		fcntl( m_s, F_SETFL, O_NONBLOCK );
#endif
		m_self = Pack( a );
		return true;
	}

	void Shutdown() override
	{
		Leave();
		if ( m_s != INVALID_SOCKET ) closesocket( m_s );
		m_s = INVALID_SOCKET;
	}

	PeerId      LocalId() const override { return m_self; }
	std::string Describe() const override { return "UDP 127.0.0.1:" + std::to_string( m_port ); }

	bool Host( int maxPlayers ) override
	{
		m_max     = maxPlayers;
		m_host    = m_self;
		m_session = true;
		SetMembers( { m_self } );
		return true;
	}

	bool Join( const std::string& target ) override
	{
		// "port" or "127.0.0.1:port"
		auto     c    = target.rfind( ':' );
		uint16_t port = (uint16_t)atoi( target.c_str() + ( c == std::string::npos ? 0 : c + 1 ) );
		if ( !port ) return false;
		sockaddr_in a{};
		a.sin_family      = AF_INET;
		a.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
		a.sin_port        = htons( port );
		m_host            = Pack( a );
		m_session         = true;
		m_lastSeen[ m_host ] = Now();
		Ctrl( m_host, K_HELLO );
		return true;
	}

	void Leave() override
	{
		if ( !m_session ) return;
		for ( PeerId p : m_members )
			if ( p != m_self ) Ctrl( p, K_BYE );
		m_session = false;
		m_host    = 0;
		SetMembers( {} );
		m_out.clear();
	}

	bool                IsInSession() const override { return m_session && IsMember( m_self ); }
	bool                IsHost() const override { return m_session && m_host == m_self; }
	PeerId              HostId() const override { return m_host; }
	std::vector<PeerId> Members() const override { return m_members; }

	bool Send( PeerId to, const void* data, size_t size, bool reliable ) override
	{
		if ( !IsMember( to ) || size > 60000 ) return false;
		std::vector<uint8_t> pkt( 5 + size );
		pkt[ 0 ] = reliable ? K_DATA_R : K_DATA_U;
		uint32_t seq = reliable ? ++m_seqOut[ to ] : 0;
		memcpy( &pkt[ 1 ], &seq, 4 );
		memcpy( &pkt[ 5 ], data, size );
		Raw( to, pkt.data(), pkt.size() );
		if ( reliable ) m_out[ to ][ seq ] = { pkt, Now() };
		return true;
	}

	void Receive( std::vector<Packet>& out ) override
	{
		out.insert( out.end(), m_inbox.begin(), m_inbox.end() );
		m_inbox.clear();
	}

	void Pump() override
	{
		if ( m_s == INVALID_SOCKET ) return;
		uint8_t     buf[ 65536 ];
		sockaddr_in from{};
		socklen_t   fl = sizeof( from );
		int         n;
		double      now = Now();
		while ( ( n = recvfrom( m_s, (char*)buf, sizeof( buf ), 0, (sockaddr*)&from, &fl ) ) > 0 )
		{
			PeerId p = Pack( from );
			OnRaw( p, buf, n, now );
			fl = sizeof( from );
		}
		if ( !m_session ) return;
		// resend unacked reliable
		for ( auto& [ peer, q ] : m_out )
			for ( auto& [ seq, e ] : q )
				if ( now - e.second > 0.1 )
				{
					Raw( peer, e.first.data(), e.first.size() );
					e.second = now;
				}
		// keep-alive / timeout
		if ( now - m_lastPing > 1.0 )
		{
			m_lastPing = now;
			for ( PeerId q : m_members )
				if ( q != m_self ) Ctrl( q, K_PING );
			if ( !IsHost() && !IsMember( m_self ) ) Ctrl( m_host, K_HELLO ); // retry join
		}
		std::vector<PeerId> dead;
		for ( PeerId q : m_members )
			if ( q != m_self && now - m_lastSeen[ q ] > 10.0 ) dead.push_back( q );
		for ( PeerId q : dead )
		{
			if ( onLog ) onLog( "peer timed out" );
			Drop( q );
		}
	}

private:
	void Raw( PeerId to, const void* d, size_t n )
	{
		sockaddr_in a = Unpack( to );
		sendto( m_s, (const char*)d, (int)n, 0, (sockaddr*)&a, sizeof( a ) );
	}
	void Ctrl( PeerId to, uint8_t k, const void* extra = nullptr, size_t en = 0 )
	{
		std::vector<uint8_t> b( 1 + en );
		b[ 0 ] = k;
		if ( en ) memcpy( &b[ 1 ], extra, en );
		Raw( to, b.data(), b.size() );
	}
	bool IsMember( PeerId p ) const { return std::find( m_members.begin(), m_members.end(), p ) != m_members.end(); }

	void SetMembers( std::vector<PeerId> now )
	{
		auto old  = m_members;
		m_members = now;
		for ( PeerId p : now )
			if ( p != m_self && std::find( old.begin(), old.end(), p ) == old.end() )
			{
				m_lastSeen[ p ] = Now();
				if ( onPeerJoined ) onPeerJoined( p );
			}
		for ( PeerId p : old )
			if ( p != m_self && std::find( now.begin(), now.end(), p ) == now.end() )
			{
				m_out.erase( p );
				m_seqIn.erase( p );
				m_seqOut.erase( p );
				if ( onPeerLeft ) onPeerLeft( p );
			}
	}

	void BroadcastRoster()
	{
		for ( PeerId q : m_members )
			if ( q != m_self ) Ctrl( q, K_ROSTER, m_members.data(), m_members.size() * sizeof( PeerId ) );
	}

	void Drop( PeerId q )
	{
		if ( q == m_host && !IsHost() )
		{
			if ( onLog ) onLog( "lost host" );
			m_session = false;
			SetMembers( {} );
			return;
		}
		auto m = m_members;
		m.erase( std::remove( m.begin(), m.end(), q ), m.end() );
		SetMembers( m );
		if ( IsHost() ) BroadcastRoster();
	}

	void OnRaw( PeerId p, const uint8_t* b, int n, double now )
	{
		if ( !m_session || n < 1 ) return;
		const bool fromKnown = IsMember( p ) || p == m_host;
		switch ( b[ 0 ] )
		{
			case K_HELLO:
				if ( !IsHost() ) return;
				if ( !IsMember( p ) )
				{
					if ( (int)m_members.size() >= m_max ) return;
					auto m = m_members;
					m.push_back( p );
					SetMembers( m );
				}
				m_lastSeen[ p ] = now;
				BroadcastRoster();
				return;
			case K_ROSTER:
			{
				if ( p != m_host || IsHost() ) return;
				m_lastSeen[ p ] = now;
				std::vector<PeerId> m( ( n - 1 ) / sizeof( PeerId ) );
				memcpy( m.data(), b + 1, m.size() * sizeof( PeerId ) );
				SetMembers( m );
				return;
			}
			case K_PING:
				if ( fromKnown ) m_lastSeen[ p ] = now;
				return;
			case K_BYE:
				if ( IsMember( p ) ) Drop( p );
				return;
			case K_ACK:
				if ( n >= 5 && fromKnown )
				{
					uint32_t s;
					memcpy( &s, b + 1, 4 );
					m_out[ p ].erase( s );
				}
				return;
			case K_DATA_U:
			case K_DATA_R:
			{
				if ( !IsMember( p ) || n < 5 ) return;
				m_lastSeen[ p ] = now;
				uint32_t seq;
				memcpy( &seq, b + 1, 4 );
				if ( b[ 0 ] == K_DATA_R )
				{
					Ctrl( p, K_ACK, &seq, 4 );
					auto& seen = m_seqIn[ p ];
					if ( !seen.insert( seq ).second ) return; // duplicate
					// NOTE: delivery order of reliable packets is arrival order (fine on loopback;
					// tests with tools/chaos reordering must use the Steam mock instead).
				}
				m_inbox.push_back( { p, std::vector<uint8_t>( b + 5, b + n ) } );
				return;
			}
		}
	}

	uint16_t                                                                    m_port;
	SOCKET                                                                      m_s = INVALID_SOCKET;
	PeerId                                                                      m_self = 0, m_host = 0;
	bool                                                                        m_session = false;
	int                                                                         m_max     = 4;
	double                                                                      m_lastPing = 0;
	std::vector<PeerId>                                                         m_members;
	std::map<PeerId, double>                                                    m_lastSeen;
	std::map<PeerId, uint32_t>                                                  m_seqOut;
	std::map<PeerId, std::set<uint32_t>>                                        m_seqIn;
	std::map<PeerId, std::map<uint32_t, std::pair<std::vector<uint8_t>, double>>> m_out;
	std::vector<Packet>                                                         m_inbox;
};
} // namespace

ITransport* CreateUdpTransport( uint16_t localPort ) { return new UdpTransport( localPort ); }

} // namespace coop
