// PR-3: UiHandlers — refactored from upstream duckdb-ui's HttpServer.
//
// What's preserved verbatim from upstream:
//   - HandleGetLocalEvents body (SSE via EventDispatcher)
//   - HandleGetLocalToken body (Referer + loopback gate)
//   - HandleInterrupt body
//   - HandleRun + DoHandleRun + HandleTokenize bodies (binary protocol)
//   - HandleProxyGet body (was HandleGet upstream — forwards to ui.duckdb.org)
//   - All helpers (ReadContent, SetResponseContent, SetResponseEmptyResult,
//     SetResponseErrorResult, InitClientFromParams)
//
// What's new / refactored:
//   - No singleton, no atexit, no Run() / Start() / Stop() lifecycle
//   - No embedded duckdb_httplib_openssl::Server (lives on HarborHttpServer)
//   - Constructor builds the allowed-origins set (per GPT-5.5 round 9
//     catch #4 — single-string Origin check breaks for non-loopback bind)
//   - SSE handler uses shared_ptr<ActiveRequestGuard> captured by the
//     chunked content provider closure (per GPT-5.5 round 9 catch on
//     ActiveRequestGuard lifetime — stack-local guard would die before
//     the provider runs)
//   - Shutdown() method called by HarborHttpServer::Close() before the
//     active-request drain (per GPT-5.5 round 9 catch #1)

#include "ui_handlers.hpp"

#include "event_dispatcher.hpp"
#include "settings.hpp"
#include "state.hpp"
#include "utils/encoding.hpp"
#include "utils/env.hpp"
#include "utils/helpers.hpp"
#include "utils/md_helpers.hpp"
#include "utils/serialization.hpp"
#include "version.hpp"
#include "watcher.hpp"

#include "harbor_auth.hpp"
#include "harbor_crypto.hpp"
#include "harbor_http_server.hpp"
#include "harbor_query_timeout.hpp"

#include "duckdb/main/config.hpp"

#include <duckdb/common/http_util.hpp>
#include <duckdb/common/serializer/binary_serializer.hpp>
#include <duckdb/common/serializer/memory_stream.hpp>
#include <duckdb/main/attached_database.hpp>
#include <duckdb/main/client_data.hpp>
#include <duckdb/parser/parsed_data/create_table_info.hpp>
#include <duckdb/parser/parser.hpp>
#if DUCKDB_VERSION_AT_LEAST(1, 5, 0)
#include <duckdb/common/enums/database_modification_type.hpp>
#include <duckdb/main/settings.hpp>
#endif

#include <climits>
#include <sstream>
#include <thread>

namespace httplib = duckdb_httplib_openssl;

namespace duckdb {
namespace ui {

const char *UiHandlers::UiExtensionVersion() {
	return UI_EXTENSION_VERSION;
}

UiHandlers::UiHandlers(HarborHttpServer &server_p, AuthManager &auth_p, weak_ptr<DatabaseInstance> db_p,
                       ClientContext &context)
    : server(server_p), auth(auth_p), ddb_instance(std::move(db_p)) {
	remote_url = GetRemoteUrl(context);
	allowed_origins = ComputeAllowedOrigins();
	local_url_prefix = ComputeLocalUrlPrefix();
	user_agent = StringUtil::Format("harbor-ui/%s-%s(%s)", DuckDB::LibraryVersion(), UI_EXTENSION_VERSION,
	                                DuckDB::Platform());
	polling_interval_ms = GetPollingInterval(context);

	auto &http_util = HTTPUtil::Get(*context.db);
	// FIXME upstream: https://github.com/duckdb/duckdb/pull/17655 will remove `unused`
	http_params = http_util.InitializeParameters(context, "unused");

	event_dispatcher = make_uniq<EventDispatcher>();
	watcher = make_uniq<Watcher>(ddb_instance, *event_dispatcher, polling_interval_ms);
}

UiHandlers::~UiHandlers() {
	// Defensive — Shutdown() should have been called by
	// HarborHttpServer::Close() already, but cover the path where
	// UiHandlers is destroyed without Close() being called (e.g.,
	// construction failure mid-RegisterBuiltinHandlers).
	Shutdown();
}

void UiHandlers::Shutdown() {
	if (shutdown_called) {
		return;
	}
	shutdown_called = true;
	// Order matters: stop the producer (Watcher) first so it doesn't
	// emit into a dispatcher that's about to close, then close the
	// dispatcher (which wakes any /localEvents WaitEvent calls so the
	// SSE handler lambda returns and releases its ActiveRequestGuard).
	if (watcher) {
		watcher->Stop();
	}
	if (event_dispatcher) {
		event_dispatcher->Close();
	}
}

std::vector<std::string> UiHandlers::ComputeAllowedOrigins() const {
	std::vector<std::string> origins;
	auto port = server.ListenUri().Port();
	// Always allow the loopback variants — these are what browsers
	// typically use for local dev.
	origins.push_back(StringUtil::Format("http://localhost:%d", port));
	origins.push_back(StringUtil::Format("http://127.0.0.1:%d", port));
	origins.push_back(StringUtil::Format("http://[::1]:%d", port));

	// If the bind host is concrete (not 0.0.0.0 or empty), include it
	// too. With 0.0.0.0 binding we can't validate against an arbitrary
	// remote Host header — that's PR-4's CORS allow-list territory.
	auto bind_host = server.ListenUri().Host();
	if (!bind_host.empty() && bind_host != "0.0.0.0" && bind_host != "localhost" && bind_host != "127.0.0.1" &&
	    bind_host != "::1") {
		// IPv6 hosts need brackets in URL-form
		auto host_form =
		    server.ListenUri().IPv6() ? StringUtil::Format("[%s]", bind_host) : bind_host;
		origins.push_back(StringUtil::Format("http://%s:%d", host_form, port));
	}
	return origins;
}

std::string UiHandlers::ComputeLocalUrlPrefix() const {
	// Per upstream UI: /localToken Referer check matches the START of
	// the local URL (because Referer includes the path). Use the
	// localhost variant — that's what browsers send for local dev.
	return StringUtil::Format("http://localhost:%d", server.ListenUri().Port());
}

bool UiHandlers::IsAllowedOrigin(const std::string &origin) const {
	if (origin.empty()) {
		return false;
	}
	for (const auto &allowed : allowed_origins) {
		if (origin == allowed) {
			return true;
		}
	}
	return false;
}

bool UiHandlers::IsBoundLocally() const {
	auto host = server.ListenUri().Host();
	return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

bool UiHandlers::LocalDevMode() const {
	// Snapshotted at server start (via `harbor_serve(..., token := NULL)`),
	// not read from a SQL setting — avoids mid-process mutation races
	// and keeps the auth posture immutable for the running server's
	// lifetime.
	return server.IsUnauthenticated();
}

namespace {

// The synthetic principal for unauthenticated mode lives on
// AuthManager (`AuthManager::LocalDevPrincipalId()`) so /sql, /quack,
// admin, and UI all reach the same constant.

// Minimal harbor login page. Inline HTML+CSS+JS so we don't need a
// separate asset pipeline. The page POSTs the user-pasted token to
// /auth/login as JSON; on 200 it does window.location.reload() so
// the cookie-bearing reload hits the cookie-gated catch-all and
// proxies through to the real UI.
//
// PR-7c (round-23) — CSP-enforced. The page is split into a prefix
// + suffix around the inline `<script>` tag so a per-request CSPRNG
// nonce can be interpolated as `<script nonce="...">`. The CSP
// header (set by HandleProxyGet) is `default-src 'none'`, with
// `script-src 'nonce-<nonce>'` allowing only the script tag bearing
// the matching nonce. Inline `<style>` keeps `'unsafe-inline'` for
// style-src; it's not the XSS surface and a per-style nonce would
// add complexity without a security benefit on this minimal page.
//
// Kept deliberately minimal: no external deps, no fonts, no
// frameworks. ~70 lines total. Substantive UX work (token rotation,
// multi-user, etc.) is post-v0.1 (SPEC §15 q4).
const char *kHarborLoginPagePrefix = R"HARBOR(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>harbor — Sign in</title>
<style>
  :root { color-scheme: light dark; }
  body { font-family: system-ui, -apple-system, Segoe UI, sans-serif;
         display: grid; place-items: center; min-height: 100vh; margin: 0;
         background: #fafafa; }
  @media (prefers-color-scheme: dark) {
    body { background: #18181b; color: #fafafa; }
    input { background: #27272a; color: #fafafa; border-color: #3f3f46; }
    .card { background: #27272a; }
  }
  .card { background: white; padding: 2rem; border-radius: 12px;
          box-shadow: 0 4px 20px rgba(0,0,0,0.08);
          width: min(420px, 90vw); }
  h1 { margin: 0 0 0.5rem; font-size: 1.5rem; }
  p { margin: 0.25rem 0 1.25rem; opacity: 0.75; font-size: 0.95rem; }
  label { display: block; margin-bottom: 0.5rem; font-weight: 500; }
  input[type="password"] { width: 100%; padding: 0.625rem 0.75rem;
                            border: 1px solid #d4d4d8; border-radius: 8px;
                            font-size: 1rem; box-sizing: border-box; }
  button { width: 100%; margin-top: 1rem; padding: 0.625rem;
           background: #18181b; color: white; border: 0; border-radius: 8px;
           font-size: 1rem; font-weight: 500; cursor: pointer; }
  button:hover { background: #27272a; }
  button:disabled { opacity: 0.5; cursor: not-allowed; }
  .err { color: #dc2626; font-size: 0.875rem; margin-top: 0.75rem;
         min-height: 1.2em; }
</style>
</head>
<body>
<form class="card" id="f">
  <h1>harbor</h1>
  <p>Paste the token printed by <code>harbor_serve()</code>.</p>
  <label for="t">Token</label>
  <input id="t" type="password" autocomplete="off" required autofocus>
  <button id="b" type="submit">Sign in</button>
  <div class="err" id="e"></div>
</form>
)HARBOR";

// PR-7c — closing chunk of the login page (everything from the
// nonce-bearing `<script>` tag onward). Concatenated by RenderLoginPage()
// with a per-request `<script nonce="...">` opening tag in between.
const char *kHarborLoginPageSuffix = R"HARBOR(
  const f=document.getElementById('f'),b=document.getElementById('b'),
        t=document.getElementById('t'),e=document.getElementById('e');
  f.addEventListener('submit', async (ev)=>{
    ev.preventDefault();
    e.textContent=''; b.disabled=true;
    try {
      const r=await fetch('/auth/login',{method:'POST',
        headers:{'Content-Type':'application/json'},
        credentials:'same-origin',
        body:JSON.stringify({token:t.value})});
      if (r.ok) { window.location.reload(); return; }
      const j=await r.json().catch(()=>({}));
      e.textContent=j.message||('Sign in failed ('+r.status+')');
    } catch(err) { e.textContent='Network error: '+err.message; }
    b.disabled=false;
  });
</script>
</body>
</html>
)HARBOR";

// PR-7c — generate a fresh CSPRNG-backed nonce for the login page's
// CSP header + inline `<script nonce="...">` attribute. Per round-23
// review (GPT-5.5): use STANDARD base64 (not base64url) for max CSP
// grammar compatibility — base64url is generally accepted by modern
// browsers but standard base64 is what the CSP spec examples use.
// Output of `harbor_crypto::Base64Encode` (or fallback to handcoded
// here if needed) for 16 random bytes is 24 characters with `=`
// padding; we keep the padding because CSP grammar accepts it.
//
// 16 bytes (128 bits) of entropy is more than the OWASP minimum
// (60+ bits) and is the same size as the cookie nonce in PR-4.
//
// Failure path: if RandomBytes throws (CSPRNG unavailable; broken
// kernel entropy on a misconfigured container), let the exception
// propagate. The HandleProxyGet caller catches and returns a 500
// rather than serve the login page WITHOUT CSP (round-23 catch).
std::string GenerateScriptNonce() {
	auto bytes = duckdb::harbor_crypto::RandomBytes(16);
	// Standard base64 alphabet, with `=` padding. The
	// harbor_crypto::Base64UrlEncode helper used elsewhere is
	// URL-safe (- _ in place of + /); for CSP we use the standard
	// alphabet.
	static const char kAlphabet[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((bytes.size() + 2) / 3) * 4);
	for (size_t i = 0; i + 2 < bytes.size(); i += 3) {
		uint32_t triplet = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | uint32_t(bytes[i + 2]);
		out.push_back(kAlphabet[(triplet >> 18) & 0x3F]);
		out.push_back(kAlphabet[(triplet >> 12) & 0x3F]);
		out.push_back(kAlphabet[(triplet >> 6) & 0x3F]);
		out.push_back(kAlphabet[triplet & 0x3F]);
	}
	const auto remaining = bytes.size() % 3;
	if (remaining == 1) {
		const uint32_t single = uint32_t(bytes[bytes.size() - 1]) << 16;
		out.push_back(kAlphabet[(single >> 18) & 0x3F]);
		out.push_back(kAlphabet[(single >> 12) & 0x3F]);
		out.push_back('=');
		out.push_back('=');
	} else if (remaining == 2) {
		const uint32_t pair = (uint32_t(bytes[bytes.size() - 2]) << 16) | (uint32_t(bytes[bytes.size() - 1]) << 8);
		out.push_back(kAlphabet[(pair >> 18) & 0x3F]);
		out.push_back(kAlphabet[(pair >> 12) & 0x3F]);
		out.push_back(kAlphabet[(pair >> 6) & 0x3F]);
		out.push_back('=');
	}
	return out;
}

// PR-7c — assemble the login page with a per-request nonce attribute
// on the inline `<script>` tag. Used by HandleProxyGet's no-cookie
// branch. The CSP header is set on the response separately by the
// caller; the nonce value is the SAME in both places.
std::string RenderLoginPage(const std::string &nonce) {
	std::string out;
	out.reserve(std::strlen(kHarborLoginPagePrefix) + std::strlen(kHarborLoginPageSuffix) + 64);
	out.append(kHarborLoginPagePrefix);
	// The script tag opens here. nonce is base64-only so safe to
	// interpolate into the HTML attribute without escaping.
	out.append("<script nonce=\"");
	out.append(nonce);
	out.append("\">");
	out.append(kHarborLoginPageSuffix);
	return out;
}

} // namespace

std::string UiHandlers::ScopedConnectionKey(const std::string &principal_id, const std::string &connection_name) {
	// NUL byte as separator: connection_name is user-controlled and
	// could contain '/', '_', etc., so we need a delimiter that
	// CAN'T appear in either part. principal_id is pure hex so it
	// definitely won't contain '\0'; an attacker would also have to
	// inject '\0' into an HTTP header value, which httplib rejects
	// at parse time (per HTTP/1.1 spec).
	std::string out;
	out.reserve(principal_id.size() + 1 + connection_name.size());
	out.append(principal_id);
	out.push_back('\0');
	out.append(connection_name);
	return out;
}

AuthResult UiHandlers::AuthorizeUiRequest(const httplib::Request &req, bool require_origin_allowed) {
	// Delegates to AuthManager. When the server was started with
	// `harbor_serve(uri, token := NULL)` on a loopback bind,
	// AuthenticateRequest short-circuits to the synthetic principal
	// before this function sees the request, so /sql, /quack,
	// /ddb/*, and admin all honor unauthenticated mode uniformly.
	//
	// `require_origin_allowed` is preserved as a parameter for future
	// use; Origin checks for browser-CSRF live in the route handlers
	// (HandleRun, HandleTokenize, HandleInterrupt, the /localEvents
	// route lambda).
	(void)require_origin_allowed;
	return auth.AuthenticateRequest(req, "__HARBOR_AUTH__:ddb");
}

shared_ptr<DatabaseInstance> UiHandlers::LockDatabaseInstance() {
	return ddb_instance.lock();
}

// Adapted from
// https://github.com/duckdb/duckdb/blob/1f8b6839ea7864c3e3fb020574f67384cb58124c/src/main/http/http_util.cpp#L129-L147
// (not currently exposed as a public DuckDB API).
void UiHandlers::InitClientFromParams(httplib::Client &client) {
	auto sec = static_cast<time_t>(http_params->timeout);
	auto usec = static_cast<time_t>(http_params->timeout_usec);
	client.set_keep_alive(true);
	client.set_write_timeout(sec, usec);
	client.set_read_timeout(sec, usec);
	client.set_connection_timeout(sec, usec);

	if (!http_params->http_proxy.empty()) {
		client.set_proxy(http_params->http_proxy, static_cast<int>(http_params->http_proxy_port));
		if (!http_params->http_proxy_username.empty()) {
			client.set_proxy_basic_auth(http_params->http_proxy_username, http_params->http_proxy_password);
		}
	}
}

std::string UiHandlers::ReadContent(const httplib::ContentReader &content_reader) {
	std::ostringstream oss;
	content_reader([&](const char *data, size_t data_length) {
		oss.write(data, data_length);
		return true;
	});
	return oss.str();
}

void UiHandlers::SetResponseContent(httplib::Response &res, const MemoryStream &content) {
	auto data = content.GetData();
	auto length = content.GetPosition();
	res.set_content(reinterpret_cast<const char *>(data), length, "application/octet-stream");
}

void UiHandlers::SetResponseEmptyResult(httplib::Response &res) {
	EmptyResult empty_result;
	MemoryStream response_content;
	BinarySerializer::Serialize(empty_result, response_content);
	SetResponseContent(res, response_content);
}

void UiHandlers::SetResponseErrorResult(httplib::Response &res, const std::string &error) {
	ErrorResult error_result;
	error_result.error = error;
	MemoryStream response_content;
	BinarySerializer::Serialize(error_result, response_content);
	SetResponseContent(res, response_content);
}

// ---------------- Route handlers ----------------

void UiHandlers::HandleGetLocalEvents(const httplib::Request &, httplib::Response &res) {
	// SSE: chunked content provider holds a shared_ptr<ActiveRequestGuard>
	// that lives for the duration of the streaming, not just the
	// route lambda. Set up at the call site (Register) — see comment
	// there. This method is called from inside the route lambda which
	// has already configured the chunked provider; nothing more to
	// do here at the synchronous handler entry point.
	(void)res;
}

void UiHandlers::HandleGetLocalToken(const httplib::Request &req, httplib::Response &res) {
	// /localToken is conditionally available only when bound locally
	// (per SPEC §7). This protects MotherDuck token disclosure on
	// remote-bound deployments.
	if (!IsBoundLocally()) {
		res.status = 404;
		return;
	}

	// GET requests don't include Origin, so use Referer instead.
	// Referer includes the path, so only compare the start.
	//
	// Accept any of the local URL variants we'd accept as Origin
	// elsewhere (allowed_origins already has http://localhost:<port>,
	// http://127.0.0.1:<port>, http://[::1]:<port>, and the configured
	// bind host if any). The previous code hardcoded only the
	// localhost variant; users connecting via 127.0.0.1 (the default
	// when copy-pasting from harbor_serve's output) saw 401 here
	// even though the browser was clearly same-origin.
	auto referer = req.get_header_value("Referer");
	bool referer_ok = false;
	for (const auto &prefix : allowed_origins) {
		if (referer.compare(0, prefix.size(), prefix) == 0) {
			referer_ok = true;
			break;
		}
	}
	if (!referer_ok) {
		res.status = 401;
		return;
	}

	auto db = LockDatabaseInstance();
	if (!db) {
		res.status = 500;
		res.set_content("Database was invalidated, UI needs to be restarted", "text/plain");
		return;
	}

	Connection connection {*db};
	try {
		auto token = GetMDToken(connection);
		res.status = 200;
		res.set_content(token, "text/plain");
	} catch (std::exception &ex) {
		res.status = 500;
		res.set_content("Could not get token: " + std::string(ex.what()), "text/plain");
	}
}

void UiHandlers::HandleProxyGet(const httplib::Request &req, httplib::Response &res) {
	// PR-4: cookie-gated catch-all. Without a valid credential:
	//   - GET /  → serve the harbor login page (200 text/html). The
	//     page POSTs to /auth/login and reloads on success.
	//   - GET /anything-else → 401 plain text. Never proxy assets
	//     for an unauthenticated client; that would let the upstream
	//     UI partially load and confuse the login flow.
	auto authn = AuthorizeUiRequest(req, /*require_origin_allowed=*/false);
	if (!authn.ok) {
		if (req.path == "/" || req.path.empty()) {
			// PR-7c — generate a fresh per-request CSPRNG nonce, set
			// the `Content-Security-Policy` header pinning script
			// execution to that nonce ONLY, then render the login
			// page with `<script nonce="...">`. If RandomBytes
			// throws (CSPRNG unavailable), do NOT fall back to
			// serving the login page without CSP — return 500 so
			// the operator notices.
			std::string nonce;
			try {
				nonce = GenerateScriptNonce();
			} catch (const std::exception &ex) {
				res.status = 500;
				res.set_content("Login page unavailable (csprng): " + std::string(ex.what()),
				                "text/plain; charset=utf-8");
				return;
			}
			std::string csp =
			    "default-src 'none'; "
			    "script-src 'nonce-" + nonce + "'; "
			    "style-src 'unsafe-inline'; "
			    "connect-src 'self'; "
			    "form-action 'self'; "
			    "base-uri 'none'; "
			    "frame-ancestors 'none'";
			res.set_header("Content-Security-Policy", csp);
			res.status = 200;
			res.set_content(RenderLoginPage(nonce), "text/html; charset=utf-8");
		} else {
			res.status = 401;
			res.set_content("Unauthorized — sign in at /\n", "text/plain; charset=utf-8");
		}
		return;
	}

	// Outbound HTTPS client to remote_url (default ui.duckdb.org).
	// TODO: Can this be created once and shared?
	httplib::Client client(remote_url);
	InitClientFromParams(client);

	if (IsEnvEnabled("ui_disable_server_certificate_verification")) {
		client.enable_server_certificate_verification(false);
	}

	// PR-8 (round-13 GPT-5.5 review catch): build the outbound headers
	// from a strict allow-list of safe asset-fetch headers. Pre-PR-4
	// this code forwarded the entire Cookie header so MotherDuck's
	// domain cookies could pass through to ui.duckdb.org. PR-4 added
	// our own harbor_session=v1.<principal_hex>... cookie under harbor's
	// origin; the browser sends it on every request to harbor for any
	// path, including /assets/*, and the old passthrough would forward
	// it verbatim to ui.duckdb.org — leaking harbor auth material to a
	// third party.
	//
	// Hard rule: we forward NO request header that could carry harbor
	// auth material. Specifically NEVER:
	//   - Cookie               (would leak harbor_session)
	//   - Authorization        (would leak Bearer token)
	//   - X-Harbor-Token        (would leak token via harbor-specific header)
	//   - X-Harbor-Session-Id   (future SQL session id)
	//   - Sec-* fetch metadata (browser-internal; not relevant upstream)
	//
	// The allow-list intentionally omits Origin too — sending it would
	// expose the user's local harbor URL (e.g. http://localhost:9494) to
	// upstream and could cause upstream to issue different content based
	// on the local host name, neither of which we want.
	//
	// If a future need to forward an upstream-set domain cookie back to
	// upstream emerges, the right shape is a positive filter that keeps
	// only cookies upstream itself set (tracked by inspecting Set-Cookie
	// in prior responses) — NOT a Cookie passthrough that includes
	// whatever the browser happens to send.
	// Force `Accept-Encoding: identity` upstream regardless of what the
	// browser asked. This sidesteps two cpp-httplib quirks:
	//
	//   1. Upstream's compressed body fails cpp-httplib client's
	//      decompression (its zlib/brotli support is conditional
	//      on compile flags that the vendored httplib build doesn't
	//      always have set), surfacing as `Error::Read`
	//      ("Failed to read connection") → 500 in the browser.
	//
	//   2. Even with `client.set_decompress(false)` to bypass the
	//      decompression attempt, cpp-httplib SERVER-side then emits
	//      both `Content-Length` (from upstream) and `Transfer-Encoding:
	//      chunked` (auto-applied for large bodies), which is invalid
	//      HTTP framing and breaks browser parsers.
	//
	// Forcing identity keeps the body uncompressed end-to-end, which
	// is the right merit for v0.1: harbor↔browser is localhost (or a
	// same-DC reverse proxy where compression is the proxy's job),
	// so the bandwidth savings on that hop are nil. Pure pass-through
	// of compressed bytes is the v0.2 plan via libcurl/HTTPUtil
	// (PR-10b in AGENTS.md), which has native gzip/br handling AND
	// doesn't have the chunked-vs-content-length framing conflict.
	httplib::Headers headers = {
	    {"User-Agent", user_agent},
	    {"Accept-Encoding", "identity"},
	};
	static const char *const kForwardableRequestHeaders[] = {
	    "Accept",            // content-type negotiation
	    "Accept-Language",   // localized assets if upstream supports it
	    "If-None-Match",     // ETag revalidation (304 path)
	    "If-Modified-Since", // legacy revalidation
	    "Range",             // partial-content for large assets
	};
	for (const auto *name : kForwardableRequestHeaders) {
		auto value = req.get_header_value(name);
		if (!value.empty()) {
			headers.emplace(name, value);
		}
	}

	// Defensive retry on transport-layer errors. cpp-httplib's HTTPS
	// client occasionally hits Error::Read / Error::Write / Error::Connection
	// on otherwise-fine upstream connections (transient TLS handshake
	// hiccups, edge-server connection-reset, etc.). A single retry
	// with a fresh client masks these for end users without changing
	// architecture. If the retry also fails, the original error
	// propagates.
	auto result = client.Get(req.path, req.params, headers);
	if (!result) {
		const auto e = result.error();
		if (e == httplib::Error::Read || e == httplib::Error::Write ||
		    e == httplib::Error::Connection) {
			httplib::Client retry(remote_url);
			InitClientFromParams(retry);
			if (IsEnvEnabled("ui_disable_server_certificate_verification")) {
				retry.enable_server_certificate_verification(false);
			}
			result = retry.Get(req.path, req.params, headers);
		}
	}
	if (!result) {
		res.status = 500;
		res.set_content("Could not fetch: '" + req.path + "' from '" + remote_url +
		                    "': " + to_string(result.error()),
		                "text/plain");
		return;
	}

	res = result.value();

	// If this is the config request, return additional information.
	if (req.path == "/config") {
		res.set_header("X-DuckDB-Version", DuckDB::LibraryVersion());
		res.set_header("X-DuckDB-Platform", DuckDB::Platform());
		// The UI looks for this to select the appropriate DuckDB mode (HTTP or
		// Wasm).
		res.set_header("X-DuckDB-UI-Extension-Version", UI_EXTENSION_VERSION);
	}

	// httplib will set Content-Length, remove it so it is not duplicated.
	res.headers.erase("Content-Length");
}

void UiHandlers::HandleInterrupt(const httplib::Request &req, httplib::Response &res) {
	// PR-4: Origin check (CSRF) AND auth (cookie/bearer/local-dev).
	// Both gates required — Origin alone is not authentication
	// (SPEC §7 line 861 "Browser-origin requests do NOT bypass auth").
	if (!IsAllowedOrigin(req.get_header_value("Origin"))) {
		res.status = 401;
		return;
	}
	auto authn = AuthorizeUiRequest(req, /*require_origin_allowed=*/true);
	if (!authn.ok) {
		res.status = 401;
		return;
	}

	auto raw_connection_name = req.get_header_value("X-DuckDB-UI-Connection-Name");

	auto db = LockDatabaseInstance();
	if (!db) {
		res.status = 404;
		return;
	}

	// Principal-scoped key (round-11 blocker fix). Connection lookups
	// must be isolated per-principal so user-controlled connection
	// names cannot collide.
	auto scoped_name = ScopedConnectionKey(authn.principal_id, raw_connection_name);
	auto connection = UIStorageExtensionInfo::GetState(*db).FindConnection(scoped_name);
	if (!connection) {
		res.status = 404;
		return;
	}

	connection->Interrupt();

	SetResponseEmptyResult(res);
}

void UiHandlers::HandleRun(const httplib::Request &req, httplib::Response &res,
                            const httplib::ContentReader &content_reader) {
	try {
		DoHandleRun(req, res, content_reader);
	} catch (const std::exception &ex) {
		SetResponseErrorResult(res, ex.what());
	}
}

void UiHandlers::DoHandleRun(const httplib::Request &req, httplib::Response &res,
                              const httplib::ContentReader &content_reader) {
	// PR-4: Origin (CSRF) + auth (cookie/bearer/local-dev). See
	// HandleInterrupt for the rationale.
	if (!IsAllowedOrigin(req.get_header_value("Origin"))) {
		res.status = 401;
		return;
	}
	auto authn = AuthorizeUiRequest(req, /*require_origin_allowed=*/true);
	if (!authn.ok) {
		res.status = 401;
		return;
	}

	auto raw_connection_name = req.get_header_value("X-DuckDB-UI-Connection-Name");
	auto connection_name = ScopedConnectionKey(authn.principal_id, raw_connection_name);
	auto database_name_option = DecodeBase64(req.get_header_value("X-DuckDB-UI-Database-Name"));
	auto schema_name_option = DecodeBase64(req.get_header_value("X-DuckDB-UI-Schema-Name"));

	std::vector<std::string> parameter_values;
	auto parameter_count_string = req.get_header_value("X-DuckDB-UI-Parameter-Count");
	if (!parameter_count_string.empty()) {
		auto parameter_count = std::stoi(parameter_count_string);
		for (auto i = 0; i < parameter_count; ++i) {
			auto parameter_value =
			    DecodeBase64(req.get_header_value(StringUtil::Format("X-DuckDB-UI-Parameter-Value-%d", i)));
			parameter_values.push_back(parameter_value);
		}
	}

	auto result_row_limit = INT_MAX;
	auto result_row_limit_string = req.get_header_value("X-DuckDB-UI-Result-Row-Limit");
	if (!result_row_limit_string.empty()) {
		result_row_limit = std::stoi(result_row_limit_string);
	}

	auto result_database_name_option = DecodeBase64(req.get_header_value("X-DuckDB-UI-Result-Database-Name"));
	auto result_schema_name_option = DecodeBase64(req.get_header_value("X-DuckDB-UI-Result-Schema-Name"));
	auto result_table_name = DecodeBase64(req.get_header_value("X-DuckDB-UI-Result-Table-Name"));

	auto result_table_row_limit = result_table_name.empty() ? 0 : INT_MAX;
	auto result_table_row_limit_string = req.get_header_value("X-DuckDB-UI-Result-Table-Row-Limit");
	if (!result_table_name.empty() && !result_table_row_limit_string.empty()) {
		result_table_row_limit = std::stoi(result_table_row_limit_string);
	}

	auto errors_as_json_string = req.get_header_value("X-DuckDB-UI-Errors-As-JSON");

	std::string content = ReadContent(content_reader);

	auto db = LockDatabaseInstance();
	if (!db) {
		SetResponseErrorResult(res, "Database was invalidated, UI needs to be restarted");
		return;
	}

	auto connection = UIStorageExtensionInfo::GetState(*db).FindOrCreateConnection(*db, connection_name);
	// PR-7b — wrap the UI-pool connection in a query-timeout watchdog
	// for the lifetime of this /ddb/run handler. UI connections are
	// in their own per-principal pool (UIStorageExtensionInfo) and
	// not visible to SessionManager's sweeper, so the per-request
	// RAII watchdog is the right enforcement vehicle. `timeout=0`
	// (the default) yields a no-op watchdog (no thread spawned).
	auto ui_query_timeout = ReadQueryTimeoutSeconds(*db);
	QueryTimeoutWatchdog ui_watchdog(*connection, ui_query_timeout);
	auto &context = *connection->context;
	if (!errors_as_json_string.empty()) {
#if DUCKDB_VERSION_AT_LEAST(1, 5, 0)
		auto &config = DBConfig::GetConfig(context);
		config.user_settings.SetUserSetting(ErrorsAsJSONSetting::SettingIndex, true);
#else
		auto &config = ClientConfig::GetConfig(context);
		config.errors_as_json = errors_as_json_string == "true";
#endif
	}

	if (!database_name_option.empty() || !schema_name_option.empty()) {
		auto schema_name = schema_name_option.empty() ? DEFAULT_SCHEMA : schema_name_option;
		context.RunFunctionInTransaction([&] {
			duckdb::ClientData::Get(context).catalog_search_path->Set(
			    {database_name_option, schema_name}, duckdb::CatalogSetPathType::SET_SCHEMA);
		});
	}

	vector<unique_ptr<SQLStatement>> statements;
	try {
		statements = connection->ExtractStatements(content);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		SetResponseErrorResult(res, error.RawMessage());
		return;
	}

	auto statement_count = statements.size();

	if (statement_count == 0) {
		SetResponseErrorResult(res, "No statements");
		return;
	}

	if (statement_count > 1) {
		for (size_t i = 0; i < statement_count - 1; ++i) {
			auto pending = connection->PendingQuery(std::move(statements[i]), true);
			if (pending->HasError()) {
				SetResponseErrorResult(res, pending->GetError());
				return;
			}
			auto exec_result = PendingExecutionResult::RESULT_NOT_READY;
			while (!PendingQueryResult::IsResultReady(exec_result)) {
				exec_result = pending->ExecuteTask();
				if (exec_result == PendingExecutionResult::BLOCKED ||
				    exec_result == PendingExecutionResult::NO_TASKS_AVAILABLE) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}
			switch (exec_result) {
			case PendingExecutionResult::EXECUTION_ERROR:
				SetResponseErrorResult(res, pending->GetError());
				return;
			case PendingExecutionResult::EXECUTION_FINISHED:
			case PendingExecutionResult::RESULT_READY:
				pending->Execute();
				break;
			default:
				SetResponseErrorResult(
				    res, StringUtil::Format("Unexpected PendingExecutionResult: %s", exec_result));
				return;
			}
		}
	}

	auto &statement_to_run = statements[statement_count - 1];

	unique_ptr<PendingQueryResult> pending;
	if (parameter_values.size() > 0) {
		auto prepared = connection->Prepare(std::move(statement_to_run));
		if (prepared->HasError()) {
			SetResponseErrorResult(res, prepared->GetError());
			return;
		}
		vector<Value> values;
		for (auto &parameter_value : parameter_values) {
			// TODO: support non-string parameters?
			values.push_back(Value(parameter_value));
		}
		pending = prepared->PendingQuery(values, true);
	} else {
		pending = connection->PendingQuery(std::move(statement_to_run), true);
	}

	if (pending->HasError()) {
		SetResponseErrorResult(res, pending->GetError());
		return;
	}

	auto exec_result = PendingExecutionResult::RESULT_NOT_READY;
	while (!PendingQueryResult::IsResultReady(exec_result)) {
		exec_result = pending->ExecuteTask();
		if (exec_result == PendingExecutionResult::BLOCKED ||
		    exec_result == PendingExecutionResult::NO_TASKS_AVAILABLE) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	switch (exec_result) {
	case PendingExecutionResult::EXECUTION_ERROR:
		SetResponseErrorResult(res, pending->GetError());
		break;
	case PendingExecutionResult::EXECUTION_FINISHED:
	case PendingExecutionResult::RESULT_READY: {
		auto result = pending->Execute();

		unique_ptr<duckdb::Connection> appender_connection;
		unique_ptr<duckdb::Appender> appender;

		if (!result_table_name.empty()) {
			auto result_database_name = result_database_name_option.empty() ? "memory" : result_database_name_option;
			auto result_schema_name = result_schema_name_option.empty() ? "main" : result_schema_name_option;

			auto result_table_info =
			    make_uniq<duckdb::CreateTableInfo>(result_database_name, result_schema_name, result_table_name);
			for (idx_t i = 0; i < result->names.size(); i++) {
				result_table_info->columns.AddColumn(ColumnDefinition(result->names[i], result->types[i]));
			}

			appender_connection = make_uniq<duckdb::Connection>(*db);
			auto appender_context = appender_connection->context;
			appender_context->RunFunctionInTransaction([&] {
				auto &catalog = duckdb::Catalog::GetCatalog(*appender_context, result_database_name);
#if DUCKDB_MAJOR_VERSION == 1 && DUCKDB_MINOR_VERSION < 5
				MetaTransaction::Get(*appender_context).ModifyDatabase(catalog.GetAttached());
#else
				MetaTransaction::Get(*appender_context)
				    .ModifyDatabase(catalog.GetAttached(), DatabaseModificationType::CREATE_CATALOG_ENTRY);
#endif
				catalog.CreateTable(*appender_context, std::move(result_table_info));
			});

			appender = make_uniq<duckdb::Appender>(*appender_connection, result_database_name, result_schema_name,
			                                       result_table_name);
		}

		SuccessResult success_result;
		success_result.column_names_and_types = {std::move(result->names), std::move(result->types)};

		auto row_limit = std::max(result_row_limit, result_table_row_limit);
		auto rows_fetched = 0;
		auto rows_appended = 0;
		auto rows_in_result = 0;
		unique_ptr<duckdb::DataChunk> chunk;
		while (rows_fetched < row_limit) {
			chunk = result->Fetch();
			if (!chunk) {
				break;
			}
			rows_fetched += chunk->size();
			if (appender && rows_appended < result_table_row_limit) {
				duckdb::DataChunk *chunk_to_append = chunk.get();
				duckdb::DataChunk chunk_prefix;
				const idx_t rows_left = result_table_row_limit - rows_appended;
				if (chunk->size() > rows_left) {
					chunk_prefix.InitializeEmpty(chunk->GetTypes());
					chunk_prefix.Reference(*chunk);
					chunk_prefix.Slice(0, rows_left);
					chunk_to_append = &chunk_prefix;
				}
				appender->AppendDataChunk(*chunk_to_append);
				rows_appended += chunk_to_append->size();
			}
			if (rows_in_result < result_row_limit) {
				duckdb::DataChunk *chunk_to_add = chunk.get();
				duckdb::DataChunk chunk_prefix;
				const idx_t rows_left = result_row_limit - rows_in_result;
				if (chunk->size() > rows_left) {
					chunk_prefix.InitializeEmpty(chunk->GetTypes());
					chunk_prefix.Reference(*chunk);
					chunk_prefix.Slice(0, rows_left);
					chunk_to_add = &chunk_prefix;
				}
				success_result.chunks.push_back(
				    {static_cast<uint16_t>(chunk_to_add->size()), std::move(chunk_to_add->data)});
				rows_in_result += chunk_to_add->size();
			}
		}

		if (appender) {
			appender->Close();
		}

		MemoryStream success_response_content;
		BinarySerializer::Serialize(success_result, success_response_content);
		SetResponseContent(res, success_response_content);
		break;
	}
	default:
		SetResponseErrorResult(res, StringUtil::Format("Unexpected PendingExecutionResult: %s", exec_result));
		break;
	}
}

void UiHandlers::HandleTokenize(const httplib::Request &req, httplib::Response &res,
                                 const httplib::ContentReader &content_reader) {
	// PR-4: Origin (CSRF) + auth. /ddb/tokenize returns no DB state
	// directly but it does invoke the parser on user input — gating
	// it the same way as /ddb/run prevents an unauth'd caller from
	// spinning the parser on arbitrary input or fingerprinting the
	// build via tokenizer behavior differences.
	if (!IsAllowedOrigin(req.get_header_value("Origin"))) {
		res.status = 401;
		return;
	}
	auto authn = AuthorizeUiRequest(req, /*require_origin_allowed=*/true);
	if (!authn.ok) {
		res.status = 401;
		return;
	}

	std::string content = ReadContent(content_reader);

	auto tokens = Parser::Tokenize(content);

	TokenizeResult result;
	result.offsets.reserve(tokens.size());
	result.types.reserve(tokens.size());
	for (auto token : tokens) {
		result.offsets.push_back(token.start);
		result.types.push_back(token.type);
	}

	MemoryStream response_content;
	BinarySerializer::Serialize(result, response_content);
	SetResponseContent(res, response_content);
}

// ---------------- Route registration ----------------

void UiHandlers::Register(duckdb_httplib_openssl::Server &http) {
	auto *self = this;

	// Start the catalog watcher (pushes "catalog changed" events to
	// /localEvents SSE clients).
	if (watcher) {
		watcher->Start();
	}

	// /localEvents — Server-Sent Events stream. PR-4 (round-11 catch):
	// auth-gated. An unauthenticated long-poll connection could
	// otherwise observe catalog-change timing without ever
	// authenticating; closing that observation channel.
	//
	// The chunked content provider runs LATER (after this lambda
	// returns), so the ActiveRequestGuard must outlive the lambda.
	// Use a shared_ptr captured by the provider closure.
	http.Get("/localEvents", [self](const httplib::Request &req, httplib::Response &res) {
		// CSRF defense for the SSE long-poll. Per Fetch spec, browsers
		// do NOT send Origin on same-origin "no-cors" requests like
		// `new EventSource('/localEvents')` — so an empty Origin is
		// implicitly same-origin and OK to proceed (auth-gated below).
		// A non-empty Origin that is NOT in the allow-list is a
		// cross-origin attempt and we reject pre-auth. Cross-origin
		// requests WITHOUT credentials would also fail the auth check
		// below; cross-origin WITH `withCredentials=true` is the actual
		// CSRF surface this guard closes.
		const auto origin = req.get_header_value("Origin");
		if (!origin.empty() && !self->IsAllowedOrigin(origin)) {
			res.status = 401;
			return;
		}
		auto authn = self->AuthorizeUiRequest(req, /*require_origin_allowed=*/true);
		if (!authn.ok) {
			res.status = 401;
			return;
		}
		auto guard = std::make_shared<HarborHttpServer::ActiveRequestGuard>(self->server);
		res.set_chunked_content_provider("text/event-stream",
		                                  [self, guard](size_t /* offset */, httplib::DataSink &sink) -> bool {
			                                  if (self->event_dispatcher && self->event_dispatcher->WaitEvent(&sink)) {
				                                  return true;
			                                  }
			                                  sink.done();
			                                  return false;
		                                  });
	});

	// /localToken — Referer + loopback gated.
	http.Get("/localToken", [self](const httplib::Request &req, httplib::Response &res) {
		HarborHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleGetLocalToken(req, res);
	});

	// /ddb/interrupt — Origin-checked.
	http.Post("/ddb/interrupt", [self](const httplib::Request &req, httplib::Response &res) {
		HarborHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleInterrupt(req, res);
	});

	// /ddb/run — Origin-checked. Binary protocol via BinarySerializer.
	http.Post("/ddb/run", [self](const httplib::Request &req, httplib::Response &res,
	                              const httplib::ContentReader &content_reader) {
		HarborHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleRun(req, res, content_reader);
	});

	// /ddb/tokenize — Origin-checked. Binary protocol.
	http.Post("/ddb/tokenize", [self](const httplib::Request &req, httplib::Response &res,
	                                   const httplib::ContentReader &content_reader) {
		HarborHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleTokenize(req, res, content_reader);
	});

	// GET /.* catch-all proxy. MUST be the LAST route registered (cpp-httplib
	// resolves in registration order — catch-all would shadow earlier
	// routes). Caller (HarborHttpServer::RegisterBuiltinHandlers)
	// invokes UiHandlers::Register AFTER QuackHandlers and AdminHandlers.
	http.Get("/.*", [self](const httplib::Request &req, httplib::Response &res) {
		HarborHttpServer::ActiveRequestGuard guard(self->server);
		self->HandleProxyGet(req, res);
	});
}

} // namespace ui
} // namespace duckdb
