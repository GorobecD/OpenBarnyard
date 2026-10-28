// Two real processes on one machine over UDP, like two game copies with bycoop.ini mode=auto.
// Usage: test_two_process <seconds>   (start twice; first hosts on 27300, second joins)
// Prints the status transitions and exits 0 if the final state is the expected one.
#include "../Source/Net/Session.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>
#ifdef _WIN32
#  include <process.h>
#  define getpid _getpid
#else
#  include <unistd.h>
#endif
using namespace coop;
static double T() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int main(int argc, char** argv)
{
	std::unique_ptr<Session> s;
	std::string              err;
	int                      i = 0;
	int                      pid = (int)getpid();
	for ( ; i < 4; i++ )
	{
		s.reset( new Session( CreateUdpTransport( (uint16_t)( 27300 + i ) ) ) );
		s->onLog = [ pid ]( const std::string& m ) { printf( "[%d] %s\n", pid, m.c_str() ); fflush( stdout ); };
		if ( s->Start( err ) ) break;
	}
	Appearance a;
	snprintf( a.name, sizeof a.name, "Cow%d", i );
	s->SetLocalAppearance( a );
	if ( i == 0 ) s->Host(); else s->Join( "127.0.0.1:27300" );
	double      end = T() + atof( argc > 1 ? argv[ 1 ] : "6" );
	std::string last;
	int         changes = 0;
	size_t      maxRemotes = 0;
	while ( T() < end )
	{
		Pose p;
		p.pos[ 0 ] = (float)i;
		s->SetLocalPose( p );
		s->Update( T() );
		if ( s->Remotes().size() > maxRemotes ) maxRemotes = s->Remotes().size();
		if ( s->Status() != last ) { last = s->Status(); changes++; printf( "[%d] status %s\n", pid, last.c_str() ); fflush( stdout ); }
		std::this_thread::sleep_for( std::chrono::milliseconds( 16 ) );
	}
	bool ok = maxRemotes == 1 && changes <= 4; // offline/joining/joined (+ host back to 1 after the joiner quits)
	printf( "[%d] final %s remotes=%zu changes=%d -> %s\n", pid, s->Status().c_str(), s->Remotes().size(), changes, ok ? "OK" : "FAIL" );
	return ok ? 0 : 1;
}
