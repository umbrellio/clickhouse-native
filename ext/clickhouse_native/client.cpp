#include <ruby.h>

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
#include <clickhouse/types/types.h>

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

using namespace clickhouse;

static VALUE rb_mClickhouseNative;

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
            // Ruby Time holds microsecond precision. Scale to usec, truncating sub-μs.
            int64_t usec = (prec <= 6) ? frac * pow10_i64(6 - prec)
                                       : frac / pow10_i64(prec - 6);
            return rb_time_new(secs, usec);
        }

        case Type::Array: {
            auto arr = col->As<ColumnArray>();
            auto inner = arr->GetAsColumn(idx);
            size_t sz = inner->Size();
            VALUE ary = rb_ary_new_capa(sz);
            for (size_t i = 0; i < sz; i++) {
                rb_ary_push(ary, value_at(inner, i));
            }
            return ary;
        }

        case Type::Nullable: {
            auto n = col->As<ColumnNullable>();
            if (n->IsNull(idx)) return Qnil;
            return value_at(n->Nested(), idx);
        }

        case Type::LowCardinality: {
            auto lc = col->As<ColumnLowCardinality>();
            ItemView item = lc->GetItem(idx);
            auto sv = item.AsBinaryData();
            return rb_utf8_str_new(sv.data(), sv.size());
        }

        case Type::Map: {
            auto map_col = col->As<ColumnMap>();
            auto tuples = map_col->GetAsColumn(idx);
            auto tuple = tuples->As<ColumnTuple>();
            if (!tuple || tuple->TupleSize() != 2) {
                rb_raise(rb_eRuntimeError, "clickhouse-native: malformed Map column");
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
            for (size_t i = 0; i < sz; i++) {
                rb_ary_push(ary, value_at((*t)[i], idx));
            }
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
            rb_raise(rb_eRuntimeError,
                     "clickhouse-native: unsupported column type %s (code=%d)",
                     type->GetName().c_str(), static_cast<int>(type->GetCode()));
    }
    return Qnil;  // unreachable
}

static std::string rb_host_or_default(VALUE v) {
    return NIL_P(v) ? std::string("localhost") : std::string(StringValueCStr(v));
}

static uint16_t rb_port_or_default(VALUE v) {
    return NIL_P(v) ? 9000 : static_cast<uint16_t>(NUM2UINT(v));
}

// ClickhouseNative.hello(host="localhost", port=9000) -> 42
static VALUE ch_hello(int argc, VALUE* argv, VALUE /*self*/) {
    VALUE rb_host = Qnil, rb_port = Qnil;
    rb_scan_args(argc, argv, "02", &rb_host, &rb_port);
    std::string host = rb_host_or_default(rb_host);
    uint16_t port = rb_port_or_default(rb_port);

    try {
        Client client(ClientOptions().SetHost(host).SetPort(port));
        uint64_t result = 0;
        bool saw_row = false;
        client.Select("SELECT toUInt64(42)", [&](const Block& block) {
            if (block.GetRowCount() == 0) return;
            auto col = block[0]->As<ColumnUInt64>();
            if (col && col->Size() > 0) { result = col->At(0); saw_row = true; }
        });
        if (!saw_row) rb_raise(rb_eRuntimeError, "clickhouse-native: no rows");
        return ULL2NUM(result);
    } catch (const std::exception& e) {
        rb_raise(rb_eRuntimeError, "clickhouse-native: %s", e.what());
    }
    return Qnil;
}

// ClickhouseNative.query(sql, host="localhost", port=9000) -> Array<Hash{Symbol => typed}>
static VALUE ch_query(int argc, VALUE* argv, VALUE /*self*/) {
    VALUE rb_sql, rb_host = Qnil, rb_port = Qnil;
    rb_scan_args(argc, argv, "12", &rb_sql, &rb_host, &rb_port);
    Check_Type(rb_sql, T_STRING);
    std::string sql = StringValueCStr(rb_sql);
    std::string host = rb_host_or_default(rb_host);
    uint16_t port = rb_port_or_default(rb_port);

    try {
        Client client(ClientOptions().SetHost(host).SetPort(port));
        VALUE rows = rb_ary_new();
        std::vector<ID> col_ids;
        client.Select(sql, [&](const Block& block) {
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
                for (size_t c = 0; c < ncols; c++) {
                    rb_hash_aset(h, ID2SYM(col_ids[c]), value_at(block[c], r));
                }
                rb_ary_push(rows, h);
            }
        });
        return rows;
    } catch (const std::exception& e) {
        rb_raise(rb_eRuntimeError, "clickhouse-native: %s", e.what());
    }
    return Qnil;
}

extern "C" void Init_clickhouse_native(void) {
    rb_mClickhouseNative = rb_define_module("ClickhouseNative");
    rb_define_singleton_method(rb_mClickhouseNative, "hello",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_hello), -1);
    rb_define_singleton_method(rb_mClickhouseNative, "query",
        reinterpret_cast<VALUE (*)(ANYARGS)>(ch_query), -1);
}
