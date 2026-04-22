#include <ruby.h>
#include <ruby/thread.h>

#include <clickhouse/client.h>
#include <clickhouse/columns/array.h>
#include <clickhouse/columns/date.h>
#include <clickhouse/columns/enum.h>
#include <clickhouse/columns/factory.h>
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

// Internal exception used to tag encoder failures and drive them through
// raise_mapped_ex -> err_encoder without rb_raising from inside a try block.
namespace chn {
class EncoderFailure : public clickhouse::Error {
    using clickhouse::Error::Error;
};
}  // namespace chn

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
    if (dynamic_cast<const chn::EncoderFailure*>(&e)) {
        rb_raise(err_encoder, "%s", e.what());
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
// Write codec (value encoder for inserts)
// ------------------------------------------------------------------

static void append_value(const ColumnRef& col, VALUE value);

// Append a zero/default value; used for the nested column of Nullable when
// the flag is set to null. We never expose these bytes to the caller.
static void append_default(const ColumnRef& col) {
    auto type = col->Type();
    switch (type->GetCode()) {
        case Type::Int8:    col->As<ColumnInt8>()->Append(0); return;
        case Type::Int16:   col->As<ColumnInt16>()->Append(0); return;
        case Type::Int32:   col->As<ColumnInt32>()->Append(0); return;
        case Type::Int64:   col->As<ColumnInt64>()->Append(0); return;
        case Type::UInt8:   col->As<ColumnUInt8>()->Append(0); return;
        case Type::UInt16:  col->As<ColumnUInt16>()->Append(0); return;
        case Type::UInt32:  col->As<ColumnUInt32>()->Append(0); return;
        case Type::UInt64:  col->As<ColumnUInt64>()->Append(0); return;
        case Type::Float32: col->As<ColumnFloat32>()->Append(0); return;
        case Type::Float64: col->As<ColumnFloat64>()->Append(0); return;
        case Type::String:      col->As<ColumnString>()->Append(std::string_view()); return;
        case Type::FixedString: col->As<ColumnFixedString>()->Append(std::string_view()); return;
        case Type::Date:        col->As<ColumnDate>()->Append(0); return;
        case Type::Date32:      col->As<ColumnDate32>()->Append(0); return;
        case Type::DateTime:    col->As<ColumnDateTime>()->Append(0); return;
        case Type::DateTime64:  col->As<ColumnDateTime64>()->Append(0); return;
        case Type::Array: {
            auto inner_type = type->As<ArrayType>()->GetItemType();
            auto inner_col = CreateColumnByType(inner_type->GetName());
            col->As<ColumnArray>()->AppendAsColumn(inner_col);
            return;
        }
        default:
            throw chn::EncoderFailure(
                "no default value for Nullable(" + type->GetName() + ")");
    }
}

static int64_t time_to_datetime64_ticks(VALUE value, size_t prec) {
    VALUE to_i = rb_funcall(value, rb_intern("to_i"), 0);
    VALUE nsec = rb_funcall(value, rb_intern("nsec"), 0);
    int64_t sec = NUM2LL(to_i);
    int64_t ns = NUM2LL(nsec);
    int64_t total_ns = sec * 1'000'000'000LL + ns;
    if (prec <= 9) return total_ns / pow10_i64(9 - prec);
    return total_ns * pow10_i64(prec - 9);
}

static void append_value(const ColumnRef& col, VALUE value) {
    auto type = col->Type();
    switch (type->GetCode()) {
        case Type::Int8:    col->As<ColumnInt8>()->Append(NUM2INT(value)); return;
        case Type::Int16:   col->As<ColumnInt16>()->Append(NUM2INT(value)); return;
        case Type::Int32:   col->As<ColumnInt32>()->Append(NUM2INT(value)); return;
        case Type::Int64:   col->As<ColumnInt64>()->Append(NUM2LL(value)); return;
        case Type::UInt8:   col->As<ColumnUInt8>()->Append(NUM2UINT(value)); return;
        case Type::UInt16:  col->As<ColumnUInt16>()->Append(NUM2UINT(value)); return;
        case Type::UInt32:  col->As<ColumnUInt32>()->Append(NUM2UINT(value)); return;
        case Type::UInt64:  col->As<ColumnUInt64>()->Append(NUM2ULL(value)); return;
        case Type::Float32: col->As<ColumnFloat32>()->Append(static_cast<float>(NUM2DBL(value))); return;
        case Type::Float64: col->As<ColumnFloat64>()->Append(NUM2DBL(value)); return;

        case Type::String: {
            StringValue(value);
            col->As<ColumnString>()->Append(
                std::string_view(RSTRING_PTR(value), RSTRING_LEN(value)));
            return;
        }
        case Type::FixedString: {
            StringValue(value);
            col->As<ColumnFixedString>()->Append(
                std::string_view(RSTRING_PTR(value), RSTRING_LEN(value)));
            return;
        }

        case Type::Date: {
            VALUE to_i = rb_funcall(value, rb_intern("to_i"), 0);
            col->As<ColumnDate>()->Append(static_cast<std::time_t>(NUM2LL(to_i)));
            return;
        }
        case Type::Date32: {
            VALUE to_i = rb_funcall(value, rb_intern("to_i"), 0);
            col->As<ColumnDate32>()->Append(static_cast<std::time_t>(NUM2LL(to_i)));
            return;
        }
        case Type::DateTime: {
            VALUE to_i = rb_funcall(value, rb_intern("to_i"), 0);
            col->As<ColumnDateTime>()->Append(static_cast<std::time_t>(NUM2LL(to_i)));
            return;
        }
        case Type::DateTime64: {
            size_t prec = type->As<DateTime64Type>()->GetPrecision();
            col->As<ColumnDateTime64>()->Append(time_to_datetime64_ticks(value, prec));
            return;
        }

        case Type::Nullable: {
            auto nul = col->As<ColumnNullable>();
            auto nested = nul->Nested();
            if (NIL_P(value)) {
                nul->Append(true);
                append_default(nested);
            } else {
                nul->Append(false);
                append_value(nested, value);
            }
            return;
        }

        case Type::Array: {
            Check_Type(value, T_ARRAY);
            auto inner_type = type->As<ArrayType>()->GetItemType();
            auto inner_col = CreateColumnByType(inner_type->GetName());
            if (!inner_col) {
                throw chn::EncoderFailure(
                    "cannot create column for Array inner type " + inner_type->GetName());
            }
            long n = RARRAY_LEN(value);
            for (long i = 0; i < n; i++) {
                append_value(inner_col, rb_ary_entry(value, i));
            }
            col->As<ColumnArray>()->AppendAsColumn(inner_col);
            return;
        }

        default:
            throw chn::EncoderFailure(
                "cannot insert into column of type " + type->GetName());
    }
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
// insert_block(table, [[name, type], ...], [[v, v, ...], ...])
// ------------------------------------------------------------------

namespace {
struct InsertNoGVL {
    Client* client;
    const std::string* table;
    const Block* block;
    std::exception_ptr err;
};
}  // namespace

static void* insert_no_gvl(void* data) {
    auto* a = static_cast<InsertNoGVL*>(data);
    try {
        a->client->Insert(*a->table, *a->block);
    } catch (...) {
        a->err = std::current_exception();
    }
    return nullptr;
}

static void insert_unblock(void* data) {
    auto* a = static_cast<InsertNoGVL*>(data);
    try { a->client->ResetConnection(); } catch (...) {}
}

static VALUE ch_client_insert_block(VALUE self, VALUE rb_table, VALUE rb_columns, VALUE rb_rows) {
    Check_Type(rb_table, T_STRING);
    Check_Type(rb_columns, T_ARRAY);
    Check_Type(rb_rows, T_ARRAY);
    CHClient* c = as_client(self);
    if (!c->client) rb_raise(err_connection, "clickhouse-native: client is closed");

    long ncols = RARRAY_LEN(rb_columns);
    long nrows = RARRAY_LEN(rb_rows);
    if (ncols == 0) rb_raise(err_encoder, "clickhouse-native: insert requires at least one column");

    try {
        std::string table(RSTRING_PTR(rb_table), RSTRING_LEN(rb_table));
        std::vector<std::string> names;
        std::vector<ColumnRef> cols;
        names.reserve(ncols);
        cols.reserve(ncols);
        for (long i = 0; i < ncols; i++) {
            VALUE pair = rb_ary_entry(rb_columns, i);
            Check_Type(pair, T_ARRAY);
            VALUE rb_name = rb_ary_entry(pair, 0);
            VALUE rb_type = rb_ary_entry(pair, 1);
            StringValue(rb_name);
            StringValue(rb_type);
            names.emplace_back(RSTRING_PTR(rb_name), RSTRING_LEN(rb_name));
            std::string type_str(RSTRING_PTR(rb_type), RSTRING_LEN(rb_type));
            auto ch_col = CreateColumnByType(type_str);
            if (!ch_col) {
                throw chn::EncoderFailure("unknown column type " + type_str + " for " + names.back());
            }
            ch_col->Reserve(static_cast<size_t>(nrows));
            cols.push_back(ch_col);
        }

        for (long r = 0; r < nrows; r++) {
            VALUE row = rb_ary_entry(rb_rows, r);
            Check_Type(row, T_ARRAY);
            if (RARRAY_LEN(row) != ncols) {
                char msg[96];
                std::snprintf(msg, sizeof(msg),
                    "row %ld has %ld values, expected %ld",
                    r, static_cast<long>(RARRAY_LEN(row)), ncols);
                throw chn::EncoderFailure(msg);
            }
            for (long cc = 0; cc < ncols; cc++) {
                append_value(cols[cc], rb_ary_entry(row, cc));
            }
        }

        Block block;
        for (long i = 0; i < ncols; i++) {
            block.AppendColumn(names[i], cols[i]);
        }

        InsertNoGVL args{c->client.get(), &table, &block, nullptr};
        rb_thread_call_without_gvl(insert_no_gvl, &args, insert_unblock, &args);
        if (args.err) {
            try { c->client->ResetConnection(); } catch (...) {}
            try { std::rethrow_exception(args.err); }
            catch (const std::exception& e) { raise_mapped_ex(e); }
        }
    } catch (const std::exception& e) {
        try { c->client->ResetConnection(); } catch (...) {}
        raise_mapped_ex(e);
    }
    return LONG2NUM(nrows);
}

// ------------------------------------------------------------------
// query_each(sql) { |row_hash| ... }
// ------------------------------------------------------------------

namespace {
struct QueryEachState {
    VALUE user_proc;
    std::vector<ID> col_ids;
    int exc_tag;
    bool aborted;
};

struct YieldBlockArgs {
    const Block* block;
    QueryEachState* state;
};

struct QueryEachNoGVL {
    Client* client;
    std::string sql;
    QueryEachState* state;
    std::exception_ptr err;
};
}  // namespace

static VALUE yield_rows_body(VALUE arg) {
    auto* args = reinterpret_cast<YieldBlockArgs*>(arg);
    const Block& block = *args->block;
    auto* state = args->state;
    size_t ncols = block.GetColumnCount();
    size_t nrows = block.GetRowCount();
    if (nrows == 0) return Qnil;
    if (state->col_ids.empty() && ncols > 0) {
        state->col_ids.reserve(ncols);
        for (size_t i = 0; i < ncols; i++) {
            const std::string& name = block.GetColumnName(i);
            state->col_ids.push_back(rb_intern2(name.data(), name.size()));
        }
    }
    for (size_t r = 0; r < nrows; r++) {
        VALUE h = rb_hash_new();
        for (size_t cc = 0; cc < ncols; cc++) {
            rb_hash_aset(h, ID2SYM(state->col_ids[cc]), value_at(block[cc], r));
        }
        rb_funcall(state->user_proc, rb_intern("call"), 1, h);
    }
    return Qnil;
}

static void* with_gvl_yield(void* data) {
    auto* args = static_cast<YieldBlockArgs*>(data);
    int tag = 0;
    rb_protect(yield_rows_body, reinterpret_cast<VALUE>(args), &tag);
    if (tag != 0) {
        args->state->exc_tag = tag;
        args->state->aborted = true;
    }
    return nullptr;
}

static void* query_each_no_gvl(void* data) {
    auto* a = static_cast<QueryEachNoGVL*>(data);
    try {
        a->client->SelectCancelable(a->sql, [&](const Block& block) -> bool {
            if (a->state->aborted) return false;
            YieldBlockArgs ya{&block, a->state};
            rb_thread_call_with_gvl(with_gvl_yield, &ya);
            return !a->state->aborted;
        });
    } catch (...) {
        a->err = std::current_exception();
    }
    return nullptr;
}

static void query_each_unblock(void* data) {
    auto* a = static_cast<QueryEachNoGVL*>(data);
    a->state->aborted = true;
    try { a->client->ResetConnection(); } catch (...) {}
}

static VALUE ch_client_query_each(VALUE self, VALUE rb_sql) {
    rb_need_block();
    Check_Type(rb_sql, T_STRING);
    CHClient* c = as_client(self);
    if (!c->client) rb_raise(err_connection, "clickhouse-native: client is closed");

    QueryEachState state{rb_block_proc(), {}, 0, false};
    QueryEachNoGVL args{
        c->client.get(),
        std::string(RSTRING_PTR(rb_sql), RSTRING_LEN(rb_sql)),
        &state,
        nullptr,
    };

    rb_thread_call_without_gvl(query_each_no_gvl, &args, query_each_unblock, &args);

    if (args.err) {
        try { c->client->ResetConnection(); } catch (...) {}
        if (state.exc_tag) rb_jump_tag(state.exc_tag);
        try { std::rethrow_exception(args.err); }
        catch (const std::exception& e) { raise_mapped_ex(e); }
    }
    if (state.exc_tag) {
        try { c->client->ResetConnection(); } catch (...) {}
        rb_jump_tag(state.exc_tag);
    }
    return self;
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
    rb_define_method(rb_cClient, "query_each",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_client_query_each), 1);
    rb_define_method(rb_cClient, "insert_block",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_client_insert_block), 3);
    rb_define_method(rb_cClient, "ping",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_client_ping), 0);
    rb_define_method(rb_cClient, "server_version",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_client_server_version), 0);
    rb_define_method(rb_cClient, "reset_connection",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_client_reset_connection), 0);
    rb_define_method(rb_cClient, "close",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_client_close), 0);
}
