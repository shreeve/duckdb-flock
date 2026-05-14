// PR-2 refactor of upstream duckdb-quack's quack_server.cpp.
//
// The upstream QuackServer base class (session pool + RNG + auth
// state) and HttpQuackServer derived class (httplib::Server ownership
// + listener thread) are gone. QuackHandlers is what's left: a
// stateless-w.r.t.-transport route registrar that borrows references
// to SessionManager + AuthManager + FlockHttpServer.
//
// What stays from upstream:
//   - Wire-format dispatch logic (HandleMessage / HandleMessageInternal)
//   - The CreateBatch / ServerSupportsMessage / MessageRequiresConnection
//     / ExtractQuery helpers
//   - All references to QuackMessage, BinaryDeserializer, ConnectionRequestMessage,
//     PrepareRequestMessage, FetchRequestMessage, AppendRequestMessage,
//     DisconnectMessage, etc.
//
// What changes:
//   - QuackConnection → FlockSession (same fields; rename for SPEC §6 vocab)
//   - GenerateSessionId() → sessions.GenerateSessionId()
//   - GetConnection / CreateNewConnection / DisconnectConnection → sessions.*
//   - EvaluateAuthQuery + GetSettingString lookup → auth.RunAuthentication / RunAuthorization
//   - Token() → auth.ServerToken()
//   - HandleMessageInternal session arg: optional_ptr<QuackConnection> → shared_ptr<FlockSession>
//
// The wire format on /quack is byte-identical to upstream Quack —
// PR-1.5's roundtrip test verifies this.

#include "duckdb/common/render_tree.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/temporary_file_manager.hpp"

#include "flock_auth.hpp"
#include "flock_http_server.hpp"
#include "flock_session.hpp"

#include "quack_log.hpp"
#include "quack_message.hpp"
#include "quack_server.hpp"

namespace duckdb {

QuackHandlers::QuackHandlers(FlockHttpServer &server_p, SessionManager &sessions_p, AuthManager &auth_p,
                             weak_ptr<DatabaseInstance> db_p)
    : server(server_p), sessions(sessions_p), auth(auth_p), db(std::move(db_p)) {
}

QuackHandlers::~QuackHandlers() {
}

namespace {

bool ServerSupportsMessage(MessageType type) {
	switch (type) {
	case MessageType::CONNECTION_REQUEST:
	case MessageType::PREPARE_REQUEST:
	case MessageType::FETCH_REQUEST:
	case MessageType::APPEND_REQUEST:
	case MessageType::DISCONNECT_MESSAGE:
		return true;
	default:
		return false;
	}
}

bool MessageRequiresConnection(MessageType type) {
	switch (type) {
	case MessageType::CONNECTION_REQUEST:
		return false;
	default:
		return true;
	}
}

string ExtractQuery(QuackMessage &msg) {
	if (msg.Type() == MessageType::PREPARE_REQUEST) {
		return msg.Cast<PrepareRequestMessage>().Query();
	}
	return "";
}

vector<unique_ptr<DataChunkWrapper>> CreateBatch(Allocator &allocator, unique_ptr<QueryResult> &query_result,
                                                  idx_t max_chunks) {
	vector<unique_ptr<DataChunkWrapper>> results;

	while (results.size() < max_chunks) {
		auto result_chunk = query_result->Fetch();
		if (!result_chunk && query_result->HasError()) {
			results.clear();
			return results;
		}
		if (!result_chunk || result_chunk->size() == 0) {
			query_result.reset();
			break;
		}
		results.push_back(make_uniq<DataChunkWrapper>(*result_chunk));
	}
	return results;
}

} // namespace

unique_ptr<QuackMessage> QuackHandlers::HandleMessage(MemoryStream &read_stream) {
	auto db_locked = db.lock();
	if (!db_locked) {
		return make_uniq<ErrorResponse>("Database was closed");
	}
	auto &logger = Logger::Get(*db_locked);
	bool should_log = logger.ShouldLog(QuackLogType::NAME, QuackLogType::LEVEL);

	int64_t start_time = 0;
	if (should_log) {
		start_time = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now())
		                 .time_since_epoch()
		                 .count();
	}

	read_stream.Rewind();
	BinaryDeserializer deserializer(read_stream);
	auto header = QuackMessage::DeserializeHeader(deserializer);

	if (!ServerSupportsMessage(header.type)) {
		return make_uniq<ErrorResponse>("Unsupported message type for server");
	}

	// Resolve the owning session shared_ptr now and hold it for the
	// remainder of the request — keeps the session alive even if a
	// concurrent DISCONNECT erases the map mid-request (per GPT-5.5
	// round 5 catch #2).
	shared_ptr<FlockSession> session;
	if (MessageRequiresConnection(header.type)) {
		session = sessions.GetConnection(header.connection_id);
		if (!session) {
			return make_uniq<ErrorResponse>("Invalid connection id");
		}
	}

	auto received_message = QuackMessage::DeserializeMessage(deserializer, header);
	auto response = HandleMessageInternal(*db_locked, *received_message, session);

	if (should_log) {
		int64_t end_time = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now())
		                       .time_since_epoch()
		                       .count();
		string error;
		if (response->Type() == MessageType::ERROR_RESPONSE) {
			error = response->Cast<ErrorResponse>().ErrorMessage();
		}
		auto msg = QuackLogType::ConstructLogMessage(header.type, header.connection_id, header.client_query_id,
		                                             ExtractQuery(*received_message), "", end_time - start_time,
		                                             response->Type(), error);
		logger.WriteLog(QuackLogType::NAME, QuackLogType::LEVEL, msg);
	}

	return response;
}

unique_ptr<QuackMessage> QuackHandlers::HandleMessageInternal(DatabaseInstance &db_inst, QuackMessage &received_message,
                                                               shared_ptr<FlockSession> session) {
	switch (received_message.Type()) {
	case MessageType::CONNECTION_REQUEST: {
		auto &connection_request_message = received_message.Cast<ConnectionRequestMessage>();
		if (connection_request_message.MinimumSupportedQuackVersion() > QUACK_VERSION) {
			return make_uniq<ErrorResponse>("Unsupported Quack version - server only supports version 1 of quack");
		}
		auto session_id = sessions.GenerateSessionId();
		if (!auth.RunAuthentication(session_id, connection_request_message.AuthString())) {
			return make_uniq<ErrorResponse>("Authentication failed");
		}
		return make_uniq<ConnectionResponseMessage>(sessions.CreateNewConnection(session_id));
	}
	case MessageType::DISCONNECT_MESSAGE: {
		auto &s = *session;
		if (!sessions.DisconnectConnection(s.session_id)) {
			return make_uniq<ErrorResponse>("Connection does not exist / already disconnected");
		}
		return make_uniq<SuccessResponse>();
	}
	case MessageType::PREPARE_REQUEST: {
		auto &prepare_request_message = received_message.Cast<PrepareRequestMessage>();
		auto &s = *session;

		if (!auth.RunAuthorization(prepare_request_message.ConnectionId(), prepare_request_message.Query())) {
			return make_uniq<ErrorResponse>("Authorization failed");
		}

		std::unique_lock<std::mutex> lock(s.lock);
		s.duckdb_query_result.reset();

		{
			auto query_result = s.duckdb_connection->SendQuery(prepare_request_message.Query());
			if (query_result->HasError()) {
				return make_uniq<ErrorResponse>(query_result->GetErrorObject());
			}
			if (query_result->names.empty()) {
				return make_uniq<ErrorResponse>("Query did not return any columns");
			}
			s.duckdb_query_result = std::move(query_result);
		}
		s.next_batch_index = 1;
		s.result_uuid = UUID::GenerateRandomUUID();

		Value max_chunks_val;
		DBConfig::GetConfig(db_inst).TryGetCurrentSetting("quack_fetch_batch_chunks", max_chunks_val);
		auto max_chunks_per_batch = max_chunks_val.GetValue<uint64_t>();

		auto names = s.duckdb_query_result->names;
		auto types = s.duckdb_query_result->types;

		auto results = CreateBatch(Allocator::Get(db_inst), s.duckdb_query_result, max_chunks_per_batch);
		if (s.duckdb_query_result && s.duckdb_query_result->HasError()) {
			D_ASSERT(results.empty());
			auto error_message = s.duckdb_query_result->GetErrorObject();
			s.duckdb_query_result.reset();
			return make_uniq<ErrorResponse>(std::move(error_message));
		}
		auto needs_more_fetch = results.size() == max_chunks_per_batch;
		return make_uniq<PrepareResponseMessage>(types, names, std::move(results), needs_more_fetch, s.result_uuid);
	}

	case MessageType::FETCH_REQUEST: {
		auto &fetch_request_message = received_message.Cast<FetchRequestMessage>();
		auto &s = *session;
		std::unique_lock<std::mutex> lock(s.lock);

		if (s.result_uuid != fetch_request_message.uuid) {
			return make_uniq<ErrorResponse>("Result has been closed");
		}
		if (!s.duckdb_query_result) {
			return make_uniq<FetchResponseMessage>();
		}
		if (s.duckdb_query_result->HasError()) {
			return make_uniq<ErrorResponse>(s.duckdb_query_result->GetErrorObject());
		}

		Value max_chunks_val;
		DBConfig::GetConfig(db_inst).TryGetCurrentSetting("quack_fetch_batch_chunks", max_chunks_val);
		auto max_chunks_per_batch = max_chunks_val.GetValue<uint64_t>();

		auto results = CreateBatch(Allocator::Get(db_inst), s.duckdb_query_result, max_chunks_per_batch);
		if (s.duckdb_query_result && s.duckdb_query_result->HasError()) {
			D_ASSERT(results.empty());
			auto error_message = s.duckdb_query_result->GetErrorObject();
			s.duckdb_query_result.reset();
			return make_uniq<ErrorResponse>(std::move(error_message));
		}
		auto assigned_batch_index = s.next_batch_index++;
		return make_uniq<FetchResponseMessage>(std::move(results), optional_idx(assigned_batch_index));
	}

	case MessageType::APPEND_REQUEST: {
		auto &append_request_message = received_message.Cast<AppendRequestMessage>();
		auto &s = *session;

		// Synthesize an INSERT for the authorization callback to inspect.
		// We never execute this query — it's just so the policy can decide
		// whether this principal gets to insert into this table.
		auto dummy_insert_query =
		    StringUtil::Format("INSERT INTO %s.%s VALUES (NULL)", SQLIdentifier(append_request_message.SchemaName()),
		                       SQLIdentifier(append_request_message.TableName()));

		if (!auth.RunAuthorization(append_request_message.ConnectionId(), dummy_insert_query)) {
			return make_uniq<ErrorResponse>("Authorization failed");
		}

		std::unique_lock<std::mutex> lock(s.lock);
		auto &context = *s.duckdb_connection->context;
		auto table_info = context.TableInfo(append_request_message.SchemaName(), append_request_message.TableName());
		if (!table_info) {
			return make_uniq<ErrorResponse>("Table %s.%s does not exist",
			                                SQLIdentifier(append_request_message.SchemaName()),
			                                SQLIdentifier(append_request_message.TableName()));
		}
		try {
			ColumnDataCollection collection(Allocator::Get(context), append_request_message.AppendChunk().GetTypes());
			collection.Append(append_request_message.AppendChunk());
			s.duckdb_connection->Append(*table_info, collection);
		} catch (std::exception &ex) {
			return make_uniq<ErrorResponse>(ErrorData(ex));
		}
		return make_uniq<SuccessResponse>();
	}
	default: {
		return make_uniq<ErrorResponse>(
		    StringUtil::Format("Unimplemented message type %s", MessageTypeToString(received_message.Type())));
	}
	}
}

void QuackHandlers::Register(duckdb_httplib_openssl::Server &http) {
	auto *self = this;

	// CORS preflight. Public — no auth, no body. Identical to upstream.
	//
	// TODO PR-4 (auth-cookie): wildcard origin is incompatible with
	// credentialed requests per the W3C CORS spec, and SPEC §7
	// explicitly forbids it once cookies are involved. PR-2 keeps
	// the upstream wildcard for /quack-only compatibility (no cookies
	// flow through /quack); PR-4 will refactor this when cookie auth
	// arrives, replacing `*` with the configured `flock_cors_origins`
	// allow-list.
	http.Options("/quack", [](const duckdb_httplib_openssl::Request &, duckdb_httplib_openssl::Response &res) {
		res.set_header("Access-Control-Allow-Origin", "*");
		res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
		res.set_header("Access-Control-Allow-Headers", "*");
		res.status = 204;
	});

	// The protocol. Each request increments the active-request counter
	// via the ActiveRequestGuard so FlockHttpServer::Close() can drain.
	http.Post("/quack", [self](const duckdb_httplib_openssl::Request &, duckdb_httplib_openssl::Response &res,
	                           const duckdb_httplib_openssl::ContentReader &content_reader) {
		FlockHttpServer::ActiveRequestGuard guard(self->server);
		res.set_header("Access-Control-Allow-Origin", "*");
		MemoryStream stream;
		content_reader([&](const char *data, size_t data_length) {
			stream.WriteData((data_ptr_t)data, data_length);
			return true;
		});
		auto response = self->HandleMessage(stream);
		response->ToMemoryStream(stream);
		res.set_content((const char *)stream.GetData(), stream.GetPosition(), "application/vnd.duckdb");
	});
}

} // namespace duckdb
