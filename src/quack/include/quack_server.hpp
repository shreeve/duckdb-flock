#pragma once

// PR-2 refactor of upstream duckdb-quack's quack_server.hpp.
//
// Upstream had:
//   - struct QuackConnection            (per-session state)
//   - class QuackServer                 (base; session pool + auth + dispatch)
//   - class HttpQuackServer : QuackServer (concrete; owned httplib::Server)
//
// harbor has:
//   - struct HarborSession   (in src/include/harbor_session.hpp)
//   - class SessionManager  (in src/include/harbor_session.hpp)
//   - class AuthManager     (in src/include/harbor_auth.hpp)
//   - class HarborHttpServer (in src/include/harbor_http_server.hpp;
//                            owns the only duckdb_httplib::Server)
//   - class QuackHandlers   (declared here; registers /quack routes
//                            against the shared HarborHttpServer)
//
// QuackHandlers is the only class that survives from upstream's server
// hierarchy, and even it loses its httplib::Server ownership. The
// session pool moved to SessionManager; the auth state moved to
// AuthManager. QuackHandlers is now stateless w.r.t. transport — it
// borrows references to the shared subsystems via its ctor.
//
// The wire format on /quack is byte-identical to upstream Quack — only
// the C++ wiring around it changed. The PR-1.5 roundtrip test in
// test/sql/harbor.test verifies this end-to-end.

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"

// Match HarborHttpServer's openssl-enabled cpp-httplib (see header
// comment in harbor_http_server.hpp explaining the namespace migration).
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

namespace duckdb {

class DatabaseInstance;
class HarborHttpServer;
class SessionManager;
class AuthManager;
class QuackMessage;
class MemoryStream;
struct HarborSession;

// QuackHandlers — registers /quack and OPTIONS /quack routes against
// the shared HarborHttpServer. Owns no httplib::Server, no listener
// thread, no session pool, no auth state — those all live on
// HarborHttpServer / SessionManager / AuthManager.
class QuackHandlers {
public:
	static constexpr const idx_t QUACK_VERSION = 1;

	QuackHandlers(HarborHttpServer &server, SessionManager &sessions, AuthManager &auth,
	              weak_ptr<DatabaseInstance> db);
	~QuackHandlers();

	QuackHandlers(const QuackHandlers &) = delete;
	QuackHandlers &operator=(const QuackHandlers &) = delete;

	// Register OPTIONS /quack and POST /quack against the given
	// httplib server. Must be called between HarborHttpServer::Bind()
	// and HarborHttpServer::StartListening().
	//
	// Note: GET / (the upstream landing page) is NOT registered —
	// SPEC §4 reserves GET / for the future harbor login wrapper that
	// arrives in PR-4.
	void Register(duckdb_httplib_openssl::Server &server);

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
	                                               shared_ptr<HarborSession> session);

	// Borrowed references; lifetimes managed by HarborHttpServer (which
	// owns this object as well as the subsystems). HarborHttpServer's
	// dtor must drain in-flight requests before destroying handlers.
	HarborHttpServer &server;
	SessionManager &sessions;
	AuthManager &auth;
	weak_ptr<DatabaseInstance> db;
};

} // namespace duckdb
