#include "Session.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace coop
{
namespace
{
struct W
{
	std::vector<uint8_t> b;
	template <class T> W& operator<<( const T& v )
	{
		const uint8_t* p = (const uint8_t*)&v;
		b.insert( b.end(), p, p + sizeof( T ) );
		return *this;
	}
};
struct R
{
	const uint8_t* p;
	size_t         n, i = 0;
	bool           ok = true;
	template <class T> R& operator>>( T& v )
	{
		if ( i + sizeof( T ) > n ) { ok = false; return *this; }
		memcpy( &v, p + i, sizeof( T ) );
		i += sizeof( T );
		return *this;
	}
};
static float LerpAngle( float a, float b, float t )
{
	float d = std::fmod( b - a + 3.14159265f * 3, 3.14159265f * 2 ) - 3.14159265f;
	return a + d * t;
}
} // namespace

Pose RemotePlayer::Sample( double now, double delay ) const
{
	double t = now - delay;
	if ( tb <= ta || t >= tb ) return b;
	if ( t <= ta ) return a;
	float k = (float)( ( t - ta ) / ( tb - ta ) );
	Pose  r = b;
	for ( int i = 0; i < 3; ++i ) r.pos[ i ] = a.pos[ i ] + ( b.pos[ i ] - a.pos[ i ] ) * k;
	r.yaw = LerpAngle( a.yaw, b.yaw, k );
	return r;
}

Session::Session( ITransport* t ) : m_t( t )
{
	m_t->onLog = [ this ]( const std::string& s ) { Log( s ); };
	m_t->onPeerLeft = [ this ]( PeerId p ) {
		for ( auto it = m_remotes.begin(); it != m_remotes.end(); )
			if ( it->second.peer == p ) it = m_remotes.erase( it );
			else ++it;
		if ( m_t->IsHost() )
		{
			m_slotOf.erase( p );
			m_looks.erase( p );
			SendRoster();
		}
		Log( "player left" );
	};
}

Session::~Session() { m_t->Shutdown(); }

bool Session::Start( std::string& err ) { return m_t->Init( err ); }

void Session::SetLocalAppearance( const Appearance& a )
{
	bool changed = !( a == m_localLook );
	m_localLook  = a;
	if ( !changed || !m_t->IsInSession() ) return;
	if ( m_t->IsHost() ) SendRoster();
	else m_sentJoin = false; // resend JOIN with the new look
}

void Session::Leave()
{
	m_t->Leave();
	m_remotes.clear();
	m_slotOf.clear();
	m_looks.clear();
	m_lastTick.clear();
	m_localSlot = -1;
	m_sentJoin  = false;
	m_worldRequests.clear();
	m_worlds.clear();
	m_worldReady = false;
	m_wantWorld  = false;
}

void Session::RequestWorld()
{
	if ( !m_t->IsInSession() || m_t->IsHost() ) return;
	m_wantWorld    = true;
	m_lastWorldReq = -100;
}

bool Session::PopWorldRequest( PeerId& a_rFrom ) // (one entry per joiner)
{
	if ( m_worldRequests.empty() ) return false;
	a_rFrom = m_worldRequests.front();
	m_worldRequests.erase( m_worldRequests.begin() );
	return true;
}

void Session::SendWorld( PeerId a_to, const WorldData& a_rData )
{
	W w;
	w << (uint8_t)MSG_WORLD_DATA << (uint8_t)( a_rData.ready ? 1 : 0 ) << a_rData.level << a_rData.pos[ 0 ] << a_rData.pos[ 1 ] << a_rData.pos[ 2 ]
	  << (uint32_t)a_rData.slot.size();
	w.b.insert( w.b.end(), a_rData.slot.begin(), a_rData.slot.end() );
	m_t->Send( a_to, w.b.data(), w.b.size(), true );
	if ( a_rData.ready ) Log( "world sent (" + std::to_string( a_rData.slot.size() ) + " bytes)" );
}

void Session::BroadcastWorldReady()
{
	uint8_t b = MSG_WORLD_READY;
	for ( auto& [ peer, slot ] : m_slotOf ) m_t->Send( peer, &b, 1, true );
}

bool Session::PopWorld( WorldData& a_rOut )
{
	if ( m_worlds.empty() ) return false;
	a_rOut = std::move( m_worlds.front() );
	m_worlds.erase( m_worlds.begin() );
	return true;
}

bool Session::PopWorldReady()
{
	bool b       = m_worldReady;
	m_worldReady = false;
	return b;
}

void Session::SendRoster()
{
	W w;
	w << (uint8_t)MSG_PLAYERINFO << (uint8_t)( m_slotOf.size() + 1 );
	w << m_t->LocalId() << (int8_t)0 << m_localLook;
	for ( auto& [ peer, slot ] : m_slotOf ) w << peer << (int8_t)slot << m_looks[ peer ];
	for ( auto& [ peer, slot ] : m_slotOf ) m_t->Send( peer, w.b.data(), w.b.size(), true );
	// host's own view of remotes
	for ( auto& [ peer, slot ] : m_slotOf )
	{
		auto& r = m_remotes[ slot ];
		r.peer  = peer;
		r.slot  = slot;
		r.look  = m_looks[ peer ];
	}
}

void Session::Update( double now )
{
	m_t->Pump();

	if ( !m_t->IsInSession() )
	{
		m_status    = "offline";
		m_localSlot = -1;
		m_sentJoin  = false;
		m_remotes.clear();
	}
	else if ( m_t->IsHost() )
	{
		m_localSlot = 0;
		m_status    = "hosting, " + std::to_string( m_slotOf.size() + 1 ) + " player(s)";
	}
	else
	{
		// JOIN until welcomed (handshake may race the transport's own membership)
		if ( m_localSlot < 0 && ( !m_sentJoin || now - m_lastJoinTry > 1.0 ) )
		{
			W w;
			w << (uint8_t)MSG_JOIN << PROTOCOL_VERSION << m_localLook;
			m_sentJoin    = m_t->Send( m_t->HostId(), w.b.data(), w.b.size(), true );
			m_lastJoinTry = now;
		}
		else if ( m_localSlot >= 0 && !m_sentJoin )
		{
			W w;
			w << (uint8_t)MSG_JOIN << PROTOCOL_VERSION << m_localLook;
			m_sentJoin = m_t->Send( m_t->HostId(), w.b.data(), w.b.size(), true );
		}
		m_status = m_localSlot < 0 ? "joining..." : "joined as player " + std::to_string( m_localSlot + 1 );
	}

	// joiner: keep asking for the host's world (the game thread may be paused in an inactive window)
	if ( m_wantWorld && m_localSlot >= 0 && !m_t->IsHost() && now - m_lastWorldReq >= 2.0 )
	{
		m_lastWorldReq = now;
		uint8_t b      = MSG_WORLD_REQ;
		m_t->Send( m_t->HostId(), &b, 1, true );
	}

	std::vector<Packet> in;
	m_t->Receive( in );
	for ( auto& p : in ) OnPacket( p, now );

	// pose stream
	if ( m_localSlot >= 0 && m_hasLocalPose && now - m_lastSend >= 1.0 / sendRate )
	{
		m_lastSend = now;
		W w;
		w << (uint8_t)MSG_POSE << (int8_t)m_localSlot << ++m_tick << m_localPose;
		if ( m_t->IsHost() )
			for ( auto& [ peer, slot ] : m_slotOf ) m_t->Send( peer, w.b.data(), w.b.size(), false );
		else
			m_t->Send( m_t->HostId(), w.b.data(), w.b.size(), false );
	}
}

void Session::OnPacket( const Packet& p, double now )
{
	if ( p.data.empty() ) return;
	R       r{ p.data.data(), p.data.size() };
	uint8_t type = 0;
	r >> type;
	const bool host = m_t->IsHost();

	switch ( type )
	{
		case MSG_JOIN:
		{
			if ( !host ) return;
			uint32_t   ver = 0;
			Appearance look;
			r >> ver >> look;
			if ( !r.ok ) return;
			look.name[ sizeof( look.name ) - 1 ] = 0;
			W w;
			if ( ver != PROTOCOL_VERSION )
			{
				w << (uint8_t)MSG_WELCOME << (int8_t)-1;
				m_t->Send( p.from, w.b.data(), w.b.size(), true );
				return;
			}
			int slot = -1;
			if ( m_slotOf.count( p.from ) ) slot = m_slotOf[ p.from ];
			else
				for ( int s = 1; s < MAX_PLAYERS && slot < 0; ++s )
				{
					bool used = false;
					for ( auto& kv : m_slotOf ) used |= kv.second == s;
					if ( !used ) slot = s;
				}
			if ( slot < 0 )
			{
				w << (uint8_t)MSG_WELCOME << (int8_t)-2; // full
				m_t->Send( p.from, w.b.data(), w.b.size(), true );
				return;
			}
			m_slotOf[ p.from ] = slot;
			m_looks[ p.from ]  = look;
			w << (uint8_t)MSG_WELCOME << (int8_t)slot;
			m_t->Send( p.from, w.b.data(), w.b.size(), true );
			SendRoster();
			Log( std::string( "player joined: " ) + look.name );
			return;
		}
		case MSG_WELCOME:
		{
			if ( host || p.from != m_t->HostId() ) return;
			int8_t slot = -1;
			r >> slot;
			if ( !r.ok ) return;
			if ( slot < 0 )
			{
				Log( slot == -1 ? "host runs a different BYCoop version" : "session is full" );
				Leave();
				return;
			}
			m_localSlot = slot;
			return;
		}
		case MSG_PLAYERINFO:
		{
			if ( host || p.from != m_t->HostId() ) return;
			uint8_t n = 0;
			r >> n;
			std::map<int, RemotePlayer> next;
			for ( int i = 0; i < n && r.ok; ++i )
			{
				PeerId     peer = 0;
				int8_t     slot = -1;
				Appearance look;
				r >> peer >> slot >> look;
				if ( !r.ok || slot < 0 || slot >= MAX_PLAYERS || slot == m_localSlot ) continue;
				look.name[ sizeof( look.name ) - 1 ] = 0;
				RemotePlayer rp = m_remotes.count( slot ) ? m_remotes[ slot ] : RemotePlayer{};
				rp.peer = peer;
				rp.slot = slot;
				rp.look = look;
				next[ slot ] = rp;
			}
			if ( r.ok ) m_remotes.swap( next );
			return;
		}
		case MSG_WORLD_REQ:
			if ( host && m_slotOf.count( p.from ) && std::find( m_worldRequests.begin(), m_worldRequests.end(), p.from ) == m_worldRequests.end() )
				m_worldRequests.push_back( p.from );
			return;
		case MSG_WORLD_DATA:
		{
			if ( host || p.from != m_t->HostId() ) return;
			WorldData d;
			uint8_t   ready = 0;
			uint32_t  n     = 0;
			r >> ready >> d.level >> d.pos[ 0 ] >> d.pos[ 1 ] >> d.pos[ 2 ] >> n;
			if ( !r.ok || n > r.n - r.i ) return;
			d.ready = ready != 0;
			if ( !d.ready ) return; // the host isn't in its world yet: keep asking
			d.slot.assign( r.p + r.i, r.p + r.i + n );
			m_wantWorld = false;
			m_worlds.clear();
			m_worlds.push_back( std::move( d ) );
			Log( "world received (" + std::to_string( n ) + " bytes)" );
			return;
		}
		case MSG_WORLD_READY:
			if ( !host && p.from == m_t->HostId() ) m_worldReady = true;
			return;
		case MSG_POSE:
		{
			int8_t   slot = -1;
			uint32_t tick = 0;
			Pose     pose;
			r >> slot >> tick >> pose;
			if ( !r.ok || slot < 0 || slot >= MAX_PLAYERS || slot == m_localSlot ) return;
			if ( host )
			{
				// only accept a client's pose for its own slot, then relay to others
				auto it = m_slotOf.find( p.from );
				if ( it == m_slotOf.end() || it->second != slot ) return;
				for ( auto& [ peer, s ] : m_slotOf )
					if ( peer != p.from ) m_t->Send( peer, p.data.data(), p.data.size(), false );
			}
			else if ( p.from != m_t->HostId() )
				return;
			auto lt = m_lastTick.find( slot );
			if ( lt != m_lastTick.end() && (int32_t)( tick - lt->second ) <= 0 ) return; // stale
			m_lastTick[ slot ] = tick;
			auto rit = m_remotes.find( slot );
			if ( rit == m_remotes.end() ) return; // roster not known yet
			RemotePlayer& rp = rit->second;
			if ( !rp.hasPose ) { rp.a = rp.b = pose; rp.ta = rp.tb = now; rp.hasPose = true; }
			else { rp.a = rp.b; rp.ta = rp.tb; rp.b = pose; rp.tb = now; }
			return;
		}
	}
}

} // namespace coop
