#include <ruby.h>
#include <ruby/thread.h>

#include <clickhouse/client.h>
#include <clickhouse/columns/array.h>
#include <clickhouse/columns/date.h>
#include <clickhouse/columns/enum.h>
#include <clickhouse/columns/lowcardinality.h>
#include <clickhouse/columns/map.h>
#include <clickhouse/columns/numeric.h>
#include <clickhouse/columns/nullable.h>
#include <clickhouse/columns/string.h>
#include <clickhouse/columns/tuple.h>
#include <clickhouse/exceptions.h>
#include <clickhouse/types/types.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace clickhouse;

static VALUE rb_mClickhouseNative;
static VALUE rb_cClient;

static VALUE err_base, err_connection, err_timeout, err_protocol,
             err_server, err_encoder, err_decoder, err_unsupported;

// ------------------------------------------------------------------
// Error mapping
// ------------------------------------------------------------------

static void raise_mapped_ex(const std::exception& e) {
    if (auto* se = dynamic_cast<const ServerException*>(&e)) {
        const auto& exc = se->GetException();
        VALUE err = rb_exc_new_cstr(err_server, exc.display_text.c_str());
        rb_ivar_set(err, rb_intern("@server_code"), INT2NUM(exc.code));
        rb_ivar_set(err, rb_intern("@server_name"),
                    rb_utf8_str_new(exc.name.data(), exc.name.size()));
        rb_ivar_set(err, rb_intern("@server_stacktrace"),
                    rb_utf8_str_new(exc.stack_trace.data(), exc.stack_trace.size()));
        rb_exc_raise(err);
    }
    if (dynamic_cast<const ProtocolError*>(&e)) {
        rb_raise(err_protocol, "%s", e.what());
    }
    if (dynamic_cast<const UnimplementedError*>(&e)) {
        rb_raise(err_unsupported, "%s", e.what());
    }
    if (dynamic_cast<const ValidationError*>(&e)) {
        rb_raise(err_decoder, "%s", e.what());
    }
    if (auto* se = dynamic_cast<const std::system_error*>(&e)) {
        auto code = se->code();
        if (code == std::errc::timed_out || code == std::errc::connection_aborted) {
            rb_raise(err_timeout, "%s", e.what());
        }
        rb_raise(err_connection, "%s", e.what());
    }
    rb_raise(err_base, "%s", e.what());
}

// ------------------------------------------------------------------
// Type codec (value decoder)
// ------------------------------------------------------------------

static int64_t pow10_i64(size_t n) {
    int64_t r = 1;
    for (size_t i = 0; i < n; i++) r *= 10;
    return r;
}

static VALUE value_at(const ColumnRef& col, size_t idx) {
    auto type = col->Type();
    switch (type->GetCode()) {
        case Type::Int8:    return INT2NUM(col->As<ColumnInt8>()->At(idx));
        case Type::Int16:   return INT2NUM(col->As<ColumnInt16>()->At(idx));
        case Type::Int32:   return INT2NUM(col->As<ColumnInt32>()->At(idx));
        case Type::Int64:   return LL2NUM(col->As<ColumnInt64>()->At(idx));
        case Type::UInt8:   return UINT2NUM(col->As<ColumnUInt8>()->At(idx));
        case Type::UInt16:  return UINT2NUM(col->As<ColumnUInt16>()->At(idx));
        case Type::UInt32:  return UINT2NUM(col->As<ColumnUInt32>()->At(idx));
        case Type::UInt64:  return ULL2NUM(col->As<ColumnUInt64>()->At(idx));
        case Type::Float32: return DBL2NUM(col->As<ColumnFloat32>()->At(idx));
        case Type::Float64: return DBL2NUM(col->As<ColumnFloat64>()->At(idx));

        case Type::String: {
            auto sv = col->As<ColumnString>()->At(idx);
            return rb_utf8_str_new(sv.data(), sv.size());
        }
        case Type::FixedString: {
            auto sv = col->As<ColumnFixedString>()->At(idx);
            return rb_utf8_str_new(sv.data(), sv.size());
        }

        case Type::Date: {
            auto t = col->As<ColumnDate>()->At(idx);
            return rb_time_new(t, 0);
        }
        case Type::Date32: {
            auto t = col->As<ColumnDate32>()->At(idx);
            return rb_time_new(t, 0);
        }
        case Type::DateTime: {
            auto t = col->As<ColumnDateTime>()->At(idx);
            return rb_time_new(t, 0);
        }
        case Type::DateTime64: {
            auto ct = col->As<ColumnDateTime64>();
            int64_t ticks = ct->At(idx);
            size_t prec = type->As<DateTime64Type>()->GetPrecision();
            int64_t denom = pow10_i64(prec);
            int64_t secs = ticks / denom;
            int64_t frac = ticks % denom;
            int64_t usec = (prec <= 6) ? frac * pow10_i64(6 - prec)
                                       : frac / pow10_i64(prec - 6);
            return rb_time_new(secs, usec);
        }

        case Type::Array: {
            auto arr = col->As<ColumnArray>();
            auto inner = arr->GetAsColumn(idx);
            size_t sz = inner->Size();
            VALUE ary = rb_ary_new_capa(sz);
            for (size_t i = 0; i < sz; i++) rb_ary_push(ary, value_at(inner, i));
            return ary;
        }

        case Type::Nullable: {
            auto n = col->As<ColumnNullable>();
            if (n->IsNull(idx)) return Qnil;
            return value_at(n->Nested(), idx);
        }

        case Type::LowCardinality: {
            auto lc = col->As<ColumnLowCardinality>();
            auto sv = lc->GetItem(idx).AsBinaryData();
            return rb_utf8_str_new(sv.data(), sv.size());
        }

        case Type::Map: {
            auto map_col = col->As<ColumnMap>();
            auto tuples = map_col->GetAsColumn(idx);
            auto tuple = tuples->As<ColumnTuple>();
            if (!tuple || tuple->TupleSize() != 2) {
                rb_raise(err_decoder, "clickhouse-native: malformed Map column");
            }
            auto keys = (*tuple)[0];
            auto vals = (*tuple)[1];
            size_t n = keys->Size();
            VALUE h = rb_hash_new();
            for (size_t i = 0; i < n; i++) {
                rb_hash_aset(h, value_at(keys, i), value_at(vals, i));
            }
            return h;
        }

        case Type::Tuple: {
            auto t = col->As<ColumnTuple>();
            size_t sz = t->TupleSize();
            VALUE ary = rb_ary_new_capa(sz);
            for (size_t i = 0; i < sz; i++) rb_ary_push(ary, value_at((*t)[i], idx));
            return ary;
        }

        case Type::Enum8: {
            auto sv = col->As<ColumnEnum8>()->NameAt(idx);
            return ID2SYM(rb_intern2(sv.data(), sv.size()));
        }
        case Type::Enum16: {
            auto sv = col->As<ColumnEnum16>()->NameAt(idx);
            return ID2SYM(rb_intern2(sv.data(), sv.size()));
        }

        default:
            rb_raise(err_unsupported,
                     "clickhouse-native: unsupported column type %s (code=%d)",
                     type->GetName().c_str(), static_cast<int>(type->GetCode()));
    }
    return Qnil;
}

// ------------------------------------------------------------------
// Client (TypedData-wrapped Client*)
// ------------------------------------------------------------------

struct CHClient {
    std::unique_ptr<Client> client;
};

static void ch_client_free(void* p) {
    delete static_cast<CHClient*>(p);
}

static size_t ch_client_size(const void* /*p*/) {
    return sizeof(CHClient);
}

static const rb_data_type_t ch_client_data_type = {
    "ClickhouseNative::Client",
    { NULL, ch_client_free, ch_client_size, },
    NULL, NULL, RUBY_TYPED_FREE_IMMEDIATELY,
};

static CHClient* as_client(VALUE self) {
    CHClient* c;
    TypedData_Get_Struct(self, CHClient, &ch_client_data_type, c);
    return c;
}

static VALUE ch_client_alloc(VALUE klass) {
    auto* c = new CHClient;
    return TypedData_Wrap_Struct(klass, &ch_client_data_type, c);
}

static std::string kwarg_str(VALUE kwargs, const char* key, const char* fallback) {
    ID id = rb_intern(key);
    VALUE v = rb_hash_lookup2(kwargs, ID2SYM(id), Qundef);
    if (v == Qundef || NIL_P(v)) return std::string(fallback);
    return std::string(StringValueCStr(v));
}

static uint16_t kwarg_uint16(VALUE kwargs, const char* key, uint16_t fallback) {
    ID id = rb_intern(key);
    VALUE v = rb_hash_lookup2(kwargs, ID2SYM(id), Qundef);
    if (v == Qundef || NIL_P(v)) return fallback;
    return static_cast<uint16_t>(NUM2UINT(v));
}

// Client.new(host:, port:, database:, user:, password:)
static VALUE ch_client_initialize(int argc, VALUE* argv, VALUE self) {
    VALUE kwargs = Qnil;
    rb_scan_args(argc, argv, "0:", &kwargs);
    if (NIL_P(kwargs)) kwargs = rb_hash_new();

    std::string host = kwarg_str(kwargs, "host", "localhost");
    uint16_t port = kwarg_uint16(kwargs, "port", 9000);
    std::string database = kwarg_str(kwargs, "database", "default");
    std::string user = kwarg_str(kwargs, "user", "default");
    std::string password = kwarg_str(kwargs, "password", "");

    CHClient* c = as_client(self);
    try {
        ClientOptions opts;
        opts.SetHost(host).SetPort(port)
            .SetDefaultDatabase(database).SetUser(user).SetPassword(password);
        c->client = std::make_unique<Client>(opts);
    } catch (const std::exception& e) {
        raise_mapped_ex(e);
    }

    rb_ivar_set(self, rb_intern("@host"), rb_utf8_str_new(host.data(), host.size()));
    rb_ivar_set(self, rb_intern("@port"), UINT2NUM(port));
    rb_ivar_set(self, rb_intern("@database"), rb_utf8_str_new(database.data(), database.size()));
    return self;
}

// ------------------------------------------------------------------
// GVL-released execute()
// ------------------------------------------------------------------

namespace {
struct ExecuteNoGVL {
    Client* client;
    std::string sql;
    std::exception_ptr err;
};
}  // namespace

static void* execute_no_gvl(void* data) {
    auto* a = static_cast<ExecuteNoGVL*>(data);
    try {
        a->client->Execute(Query(a->sql));
    } catch (...) {
        a->err = std::current_exception();
    }
    return nullptr;
}

static void execute_unblock(void* data) {
    // The only safe abort clickhouse-cpp exposes is tearing the connection.
    // On interrupt we kill the socket; the pool will discard this client.
    auto* a = static_cast<ExecuteNoGVL*>(data);
    try { a->client->ResetConnection(); } catch (...) {}
}

static VALUE ch_client_execute(VALUE self, VALUE rb_sql) {
    Check_Type(rb_sql, T_STRING);
    CHClient* c = as_client(self);
    if (!c->client) rb_raise(err_connection, "clickhouse-native: client is closed");

    ExecuteNoGVL args{c->client.get(), std::string(StringValueCStr(rb_sql)), nullptr};
    rb_thread_call_without_gvl(execute_no_gvl, &args, execute_unblock, &args);
    if (args.err) {
        // clickhouse-cpp may leave the read stream partially consumed when the
        // server exception or an unsupported-type error is thrown mid-block.
        // Reset so the next call on this Client starts from a clean protocol.
        try { c->client->ResetConnection(); } catch (...) {}
        try { std::rethrow_exception(args.err); }
        catch (const std::exception& e) { raise_mapped_ex(e); }
    }
    return Qnil;
}

// ------------------------------------------------------------------
// query() — synchronous, GVL held (streaming query_each comes in Week 5)
// ------------------------------------------------------------------

static VALUE ch_client_query(VALUE self, VALUE rb_sql) {
    Check_Type(rb_sql, T_STRING);
    CHClient* c = as_client(self);
    if (!c->client) rb_raise(err_connection, "clickhouse-native: client is closed");

    std::string sql(StringValueCStr(rb_sql));
    VALUE rows = rb_ary_new();
    try {
        std::vector<ID> col_ids;
        c->client->Select(sql, [&](const Block& block) {
            size_t ncols = block.GetColumnCount();
            size_t nrows = block.GetRowCount();
            if (nrows == 0) return;
            if (col_ids.empty()) {
                col_ids.reserve(ncols);
                for (size_t i = 0; i < ncols; i++) {
                    const std::string& name = block.GetColumnName(i);
                    col_ids.push_back(rb_intern2(name.data(), name.size()));
                }
            }
            for (size_t r = 0; r < nrows; r++) {
                VALUE h = rb_hash_new();
                for (size_t cc = 0; cc < ncols; cc++) {
                    rb_hash_aset(h, ID2SYM(col_ids[cc]), value_at(block[cc], r));
                }
                rb_ary_push(rows, h);
            }
        });
        return rows;
    } catch (const std::exception& e) {
        try { c->client->ResetConnection(); } catch (...) {}
        raise_mapped_ex(e);
    }
    return Qnil;
}

// ------------------------------------------------------------------
// query_value — returns the first cell of the first row, or nil
// ------------------------------------------------------------------

static VALUE ch_client_query_value(VALUE self, VALUE rb_sql) {
    Check_Type(rb_sql, T_STRING);
    CHClient* c = as_client(self);
    if (!c->client) rb_raise(err_connection, "clickhouse-native: client is closed");

    std::string sql(StringValueCStr(rb_sql));
    try {
        VALUE out = Qnil;
        bool seen = false;
        c->client->Select(sql, [&](const Block& block) {
            if (seen) return;
            if (block.GetRowCount() == 0 || block.GetColumnCount() == 0) return;
            out = value_at(block[0], 0);
            seen = true;
        });
        return out;
    } catch (const std::exception& e) {
        try { c->client->ResetConnection(); } catch (...) {}
        raise_mapped_ex(e);
    }
    return Qnil;
}

// ------------------------------------------------------------------
// ping / server_version / reset_connection / close
// ------------------------------------------------------------------

namespace {
struct PingNoGVL {
    Client* client;
    std::exception_ptr err;
};
}  // namespace

static void* ping_no_gvl(void* data) {
    auto* a = static_cast<PingNoGVL*>(data);
    try { a->client->Ping(); } catch (...) { a->err = std::current_exception(); }
    return nullptr;
}

static VALUE ch_client_ping(VALUE self) {
    CHClient* c = as_client(self);
    if (!c->client) rb_raise(err_connection, "clickhouse-native: client is closed");
    PingNoGVL args{c->client.get(), nullptr};
    rb_thread_call_without_gvl(ping_no_gvl, &args, nullptr, nullptr);
    if (args.err) {
        try { std::rethrow_exception(args.err); }
        catch (const std::exception& e) { raise_mapped_ex(e); }
    }
    return Qtrue;
}

static VALUE ch_client_server_version(VALUE self) {
    CHClient* c = as_client(self);
    if (!c->client) rb_raise(err_connection, "clickhouse-native: client is closed");
    try {
        const ServerInfo& info = c->client->GetServerInfo();
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "%llu.%llu.%llu",
                         static_cast<unsigned long long>(info.version_major),
                         static_cast<unsigned long long>(info.version_minor),
                         static_cast<unsigned long long>(info.version_patch));
        return rb_utf8_str_new(buf, n);
    } catch (const std::exception& e) {
        raise_mapped_ex(e);
    }
    return Qnil;
}

static VALUE ch_client_reset_connection(VALUE self) {
    CHClient* c = as_client(self);
    if (!c->client) return Qnil;
    try { c->client->ResetConnection(); } catch (...) {}
    return Qtrue;
}

static VALUE ch_client_close(VALUE self) {
    CHClient* c = as_client(self);
    c->client.reset();
    return Qnil;
}

// ------------------------------------------------------------------
// Backward-compat: ClickhouseNative.hello(host, port) — Spike 1 smoke test
// ------------------------------------------------------------------

static VALUE ch_hello(int argc, VALUE* argv, VALUE /*self*/) {
    VALUE rb_host = Qnil, rb_port = Qnil;
    rb_scan_args(argc, argv, "02", &rb_host, &rb_port);
    std::string host = NIL_P(rb_host) ? "localhost" : StringValueCStr(rb_host);
    uint16_t port = NIL_P(rb_port) ? 9000 : static_cast<uint16_t>(NUM2UINT(rb_port));

    try {
        Client client(ClientOptions().SetHost(host).SetPort(port));
        uint64_t result = 0;
        bool saw_row = false;
        client.Select("SELECT toUInt64(42)", [&](const Block& block) {
            if (block.GetRowCount() == 0) return;
            auto col = block[0]->As<ColumnUInt64>();
            if (col && col->Size() > 0) { result = col->At(0); saw_row = true; }
        });
        if (!saw_row) rb_raise(err_base, "clickhouse-native: no rows");
        return ULL2NUM(result);
    } catch (const std::exception& e) {
        raise_mapped_ex(e);
    }
    return Qnil;
}

// ------------------------------------------------------------------
// Init
// ------------------------------------------------------------------

extern "C" void Init_clickhouse_native(void) {
    rb_mClickhouseNative = rb_define_module("ClickhouseNative");

    err_base        = rb_const_get(rb_mClickhouseNative, rb_intern("Error"));
    err_connection  = rb_const_get(rb_mClickhouseNative, rb_intern("ConnectionError"));
    err_timeout     = rb_const_get(rb_mClickhouseNative, rb_intern("TimeoutError"));
    err_protocol    = rb_const_get(rb_mClickhouseNative, rb_intern("ProtocolError"));
    err_server      = rb_const_get(rb_mClickhouseNative, rb_intern("ServerError"));
    err_encoder     = rb_const_get(rb_mClickhouseNative, rb_intern("EncoderError"));
    err_decoder     = rb_const_get(rb_mClickhouseNative, rb_intern("DecoderError"));
    err_unsupported = rb_const_get(rb_mClickhouseNative, rb_intern("UnsupportedTypeError"));
    rb_global_variable(&err_base);
    rb_global_variable(&err_connection);
    rb_global_variable(&err_timeout);
    rb_global_variable(&err_protocol);
    rb_global_variable(&err_server);
    rb_global_variable(&err_encoder);
    rb_global_variable(&err_decoder);
    rb_global_variable(&err_unsupported);

    rb_cClient = rb_define_class_under(rb_mClickhouseNative, "Client", rb_cObject);
    rb_global_variable(&rb_cClient);
    rb_define_alloc_func(rb_cClient, ch_client_alloc);
    rb_define_method(rb_cClient, "initialize",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_client_initialize), -1);
    rb_define_method(rb_cClient, "execute",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_client_execute), 1);
    rb_define_method(rb_cClient, "query",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_client_query), 1);
    rb_define_method(rb_cClient, "query_value",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_client_query_value), 1);
    rb_define_method(rb_cClient, "ping",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_client_ping), 0);
    rb_define_method(rb_cClient, "server_version",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_client_server_version), 0);
    rb_define_method(rb_cClient, "reset_connection",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_client_reset_connection), 0);
    rb_define_method(rb_cClient, "close",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_client_close), 0);

    rb_define_singleton_method(rb_mClickhouseNative, "hello",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_hello), -1);
}
