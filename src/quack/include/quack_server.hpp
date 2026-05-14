#pragma once

// PR-2 refactor of upstream duckdb-quack's quack_server.hpp.
//
// Upstream had:
//   - struct QuackConnection            (per-session state)
//   - class QuackServer                 (base; session pool + auth + dispatch)
//   - class HttpQuackServer : QuackServer (concrete; owned httplib::Server)
//
// flock has:
//   - struct FlockSession   (in src/include/flock_session.hpp)
//   - class SessionManager  (in src/include/flock_session.hpp)
//   - class AuthManager     (in src/include/flock_auth.hpp)
//   - class FlockHttpServer (in src/include/flock_http_server.hpp;
//                            owns the only duckdb_httplib::Server)
//   - class QuackHandlers   (declared here; registers /quack routes
//                            against the shared FlockHttpServer)
//
// QuackHandlers is the only class that survives from upstream's server
// hierarchy, and even it loses its httplib::Server ownership. The
// session pool moved to SessionManager; the auth state moved to
// AuthManager. QuackHandlers is now stateless w.r.t. transport — it
// borrows references to the shared subsystems via its ctor.
//
// The wire format on /quack is byte-identical to upstream Quack — only
// the C++ wiring around it changed. The PR-1.5 roundtrip test in
// test/sql/flock.test verifies this end-to-end.

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"

#include "httplib.hpp"

namespace duckdb {

class DatabaseInstance;
class FlockHttpServer;
class SessionManager;
class AuthManager;
class QuackMessage;
class MemoryStream;
struct FlockSession;

// QuackHandlers — registers /quack and OPTIONS /quack routes against
// the shared FlockHttpServer. Owns no httplib::Server, no listener
// thread, no session pool, no auth state — those all live on
// FlockHttpServer / SessionManager / AuthManager.
class QuackHandlers {
public:
	static constexpr const idx_t QUACK_VERSION = 1;

	QuackHandlers(FlockHttpServer &server, SessionManager &sessions, AuthManager &auth,
	              weak_ptr<DatabaseInstance> db);
	~QuackHandlers();

	QuackHandlers(const QuackHandlers &) = delete;
	QuackHandlers &operator=(const QuackHandlers &) = delete;

	// Register OPTIONS /quack and POST /quack against the given
	// httplib server. Must be called between FlockHttpServer::Bind()
	// and FlockHttpServer::StartListening().
	//
	// Note: GET / (the upstream landing page) is NOT registered —
	// SPEC §4 reserves GET / for the future flock login wrapper that
	// arrives in PR-3.
	void Register(duckdb_httplib::Server &server);

private:
	// Top-level message dispatch. Reads the wire-format header,
	// validates the message type, looks up the session if needed,
	// then delegates to HandleMessageInternal. Mirrors upstream's
	// QuackServer::HandleMessage.
	unique_ptr<QuackMessage> HandleMessage(MemoryStream &read_stream);

	// Per-message-type handler. The session is passed by shared_ptr
	// (was optional_ptr<QuackConnection> upstream — change per
	// GPT-5.5 round 5 catch #2: keep the owning shared_ptr through
	// the request so a concurrent disconnect can't dangle the handler).
	unique_ptr<QuackMessage> HandleMessageInternal(DatabaseInstance &db, QuackMessage &received_message,
	                                               shared_ptr<FlockSession> session);

	// Borrowed references; lifetimes managed by FlockHttpServer (which
	// owns this object as well as the subsystems). FlockHttpServer's
	// dtor must drain in-flight requests before destroying handlers.
	FlockHttpServer &server;
	SessionManager &sessions;
	AuthManager &auth;
	weak_ptr<DatabaseInstance> db;
};

} // namespace duckdb
