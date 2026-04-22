#include <ruby.h>

#include <clickhouse/client.h>
#include <clickhouse/columns/numeric.h>

#include <cstdint>
#include <exception>
#include <string>

using clickhouse::Block;
using clickhouse::Client;
using clickhouse::ClientOptions;
using clickhouse::ColumnUInt64;

static VALUE rb_mClickhouseNative;

// Spike 1 hello-world: connect to a ClickHouse server, run `SELECT toUInt64(42)`,
// return the scalar back to Ruby. Intentionally hard-codes the query and uses the
// simplest possible code path — no GVL handling, no pool, no type dispatcher.
// Arguments: optional host (String), optional port (Integer).
static VALUE ch_hello(int argc, VALUE* argv, VALUE /*self*/) {
    VALUE rb_host = Qnil;
    VALUE rb_port = Qnil;
    rb_scan_args(argc, argv, "02", &rb_host, &rb_port);

    std::string host = NIL_P(rb_host) ? std::string("localhost") : std::string(StringValueCStr(rb_host));
    uint16_t port = NIL_P(rb_port) ? 9000 : static_cast<uint16_t>(NUM2UINT(rb_port));

    try {
        Client client(ClientOptions().SetHost(host).SetPort(port));
        uint64_t result = 0;
        bool saw_row = false;
        client.Select("SELECT toUInt64(42)", [&](const Block& block) {
            if (block.GetRowCount() == 0) return;
            auto col = block[0]->As<ColumnUInt64>();
            if (col && col->Size() > 0) {
                result = col->At(0);
                saw_row = true;
            }
        });
        if (!saw_row) {
            rb_raise(rb_eRuntimeError, "clickhouse-native: server returned no rows");
        }
        return ULL2NUM(result);
    } catch (const std::exception& e) {
        rb_raise(rb_eRuntimeError, "clickhouse-native: %s", e.what());
    } catch (...) {
        rb_raise(rb_eRuntimeError, "clickhouse-native: unknown C++ exception");
    }
    return Qnil;  // unreachable
}

extern "C" void Init_clickhouse_native(void) {
    rb_mClickhouseNative = rb_define_module("ClickhouseNative");
    rb_define_singleton_method(rb_mClickhouseNative, "hello", reinterpret_cast<VALUE (*)(ANYARGS)>(ch_hello), -1);
}
