# clickhouse-native

A Ruby driver for [ClickHouse](https://clickhouse.com/) that speaks the native
TCP binary protocol via a C++ extension wrapping the official
[clickhouse-cpp](https://github.com/ClickHouse/clickhouse-cpp) client.

Compared to HTTP-based gems it's faster (binary blocks, columnar decode), it
releases the GVL during I/O (so a `Pool` with N connections actually scales
across N Ruby threads), and it preserves ClickHouse types end-to-end instead
of round-tripping through JSON strings.

## Installation

Add to your Gemfile:

```ruby
gem "clickhouse-native"
```

Precompiled native gems are published for:

- `x86_64-linux-gnu`, `aarch64-linux-gnu`
- `x86_64-darwin`, `arm64-darwin`

For Ruby ABIs 3.3, 3.4, 4.0. On those platforms `gem install` drops in a
prebuilt `.bundle` / `.so` with no compiler toolchain required.

On anything else, `gem install` falls back to compiling the vendored
clickhouse-cpp from source. You'll need:

- CMake 3.15+
- A C++17 compiler
- Ruby ≥ 3.3

```
# macOS
brew install cmake

# Debian/Ubuntu
sudo apt-get install -y cmake build-essential

# Alpine
apk add --no-cache cmake build-base linux-headers
```

## Quick start

```ruby
require "clickhouse_native"

client = ClickhouseNative::Client.new(
  host: "localhost",
  port: 9000,
  database: "default",
  user: "default",
  password: "",
)

client.ping                        # => true
client.server_version              # => "25.3.1"

client.query_value("SELECT 1 + 1") # => 2

client.query("SELECT number AS n FROM numbers(3)")
# => [{ n: 0 }, { n: 1 }, { n: 2 }]

client.execute(<<~SQL)
  CREATE TABLE events (
    id         UInt64,
    user       LowCardinality(String),
    tags       Array(String),
    happened   DateTime64(6, 'UTC')
  ) ENGINE = MergeTree ORDER BY id
SQL

client.insert("events", [
  { id: 1, user: "alice", tags: %w[login web],   happened: Time.now.utc },
  { id: 2, user: "bob",   tags: %w[signup],      happened: Time.now.utc },
])

client.query_each("SELECT * FROM events ORDER BY id") do |row|
  puts row.inspect
end

client.close
```

## Connection options

`ClickhouseNative::Client.new` accepts keyword arguments only:

| kwarg         | default       | notes                                               |
| ------------- | ------------- | --------------------------------------------------- |
| `host:`       | `"localhost"` |                                                     |
| `port:`       | `9000`        | native TCP port (not 8123)                          |
| `database:`   | `"default"`   |                                                     |
| `user:`       | `"default"`   |                                                     |
| `password:`   | `""`          |                                                     |
| `compression:`| `:none`       | `:none`, `:lz4`, or `:zstd`                         |
| `logger:`     | `nil`         | any `Logger`-compatible object (see [Logging](#logging)) |

`Pool.new` additionally accepts `pool_size:` (default 5) and `pool_timeout:`
(default 5, seconds).

## API

### `#execute(sql)`

Runs DDL/DML and discards any result. Returns `nil`. Releases the GVL for the
duration of the server round-trip.

```ruby
client.execute("CREATE TABLE t (id UInt64) ENGINE = Memory")
client.execute("INSERT INTO t VALUES (1), (2)")
```

### `#query(sql)`

Buffers the full result into an `Array<Hash>` with symbol keys:

```ruby
client.query("SELECT 1 AS a, 'x' AS b")
# => [{ a: 1, b: "x" }]
```

Returns `[]` for empty results. Use `#query_each` for large results or when
you need to release the GVL mid-iteration.

### `#query_value(sql)`

Returns the first cell of the first row, or `nil` if there are no rows.

```ruby
client.query_value("SELECT count() FROM events")   # => 1337
client.query_value("SELECT 1 WHERE 0")             # => nil
```

### `#query_each(sql, &block)`

Streams rows one block at a time, yielding each row hash to the block. Does
**not** materialise the full result in memory. The GVL is released while
ClickHouse is delivering bytes; it's reacquired only to run your block.

```ruby
client.query_each("SELECT number FROM numbers(10_000_000)") do |row|
  process(row[:number])
end
```

Raising from inside the block (or `throw`-ing past it) aborts the query
cleanly — the socket is reset and the client stays usable for the next call.

### `#insert(table, rows, columns: nil, db_name: nil, types: nil)`

Bulk block insert. `rows` can be either:

- `Array<Hash>` — keys are column names (symbols or strings). Defaults
  `columns:` to `rows.first.keys`.
- `Array<Array>` — positional. Defaults `columns:` to every column in the
  table's DDL order.

```ruby
client.insert("events", [
  { id: 1, user: "a", tags: [],    happened: Time.now.utc },
  { id: 2, user: "b", tags: %w[x], happened: Time.now.utc },
])

# Subset of columns (hash keys not in the schema would raise ArgumentError):
client.insert("events", [{ id: 1, user: "a" }],
              columns: %w[id user])

# Positional rows, cross-database:
client.insert("events", [[1, "a", [], Time.now.utc]], db_name: "analytics")
```

Without `types:`, `#insert` issues a `DESCRIBE TABLE` once to learn the
column types. If you already know them (e.g. you're inserting to the same
table in a tight loop), pass them to skip the round-trip:

```ruby
client.insert("events", rows,
              columns: %w[id user tags happened],
              types: %w[UInt64 LowCardinality(String) Array(String) DateTime64(6,'UTC')])
```

Returns the number of rows inserted. An empty `rows` array is a no-op and
returns `0` without touching the server.

### `#describe_table(table, db_name: nil)`

Shortcut for `DESCRIBE TABLE`. Returns the decoded rows (symbol-keyed).

```ruby
client.describe_table("events").map { |c| [c[:name], c[:type]] }
# => [["id", "UInt64"], ["user", "LowCardinality(String)"], ...]
```

### `#ping` / `#server_version`

```ruby
client.ping            # => true (raises ConnectionError on failure)
client.server_version  # => "25.3.1"
```

### `#reset_connection` / `#close`

`reset_connection` tears the TCP socket; clickhouse-cpp re-establishes it on
the next operation. `close` releases the underlying `Client` permanently —
further calls raise `ClickhouseNative::ConnectionError`.

## Connection pooling

`ClickhouseNative::Pool` wraps [`connection_pool`](https://github.com/mperham/connection_pool)
and exposes the same surface as `Client` (minus `close` / `reset_connection`).
Because the extension releases the GVL during I/O, N threads on a pool of
size N scale roughly linearly on I/O-bound work.

```ruby
pool = ClickhouseNative::Pool.new(
  host: "localhost", port: 9000,
  pool_size: 8, pool_timeout: 5,
)

pool.query_value("SELECT 1")   # checks out a client, runs, checks it back in

pool.with do |client|          # for multi-statement work on one connection
  client.execute("CREATE TEMPORARY TABLE t (n UInt64) ENGINE = Memory")
  client.insert("t", [{ n: 1 }, { n: 2 }])
  client.query("SELECT sum(n) FROM t")
end
```

## Type mapping

### Decoding (ClickHouse → Ruby)

| ClickHouse                          | Ruby                                                 |
| ----------------------------------- | ---------------------------------------------------- |
| `Int8`…`Int64`, `UInt8`…`UInt64`    | `Integer`                                            |
| `Float32`, `Float64`                | `Float`                                              |
| `Decimal`, `Decimal32/64/128`       | `BigDecimal` (scale preserved)                       |
| `String`                            | `String` (UTF-8)                                     |
| `FixedString(N)`                    | `String` (UTF-8, NUL-padded to N)                    |
| `Date`, `Date32`                    | `Date`                                               |
| `DateTime`                          | `Time` (UTC)                                         |
| `DateTime64(P, 'UTC')`              | `Time` (UTC, sub-second precision preserved up to µs)|
| `Array(T)`                          | `Array`                                              |
| `Nullable(T)`                       | `T` or `nil`                                         |
| `LowCardinality(String)`            | `String`                                             |
| `LowCardinality(Nullable(String))`  | `String` or `nil`                                    |
| `Map(K, V)`                         | `Hash`                                               |
| `Tuple(T1, T2, ...)`                | `Array`                                              |
| `Enum8`, `Enum16`                   | `Symbol` (the enum name)                             |

`Dynamic`, `Variant`, typed `JSON`, and other experimental CH 24.x+ types
raise `ClickhouseNative::UnsupportedTypeError` on decode.

### Encoding (Ruby → ClickHouse, for `#insert`)

- Integer/Float/BigDecimal/String are accepted for the matching numeric and
  string columns.
- `Symbol` is coerced to `String` (useful for `LowCardinality(String)`
  dictionaries like `:eur`, `:gbp`).
- `true` / `false` coerce to `1` / `0` for `Bool` (stored as `UInt8`).
- `Time`, `DateTime`, numeric epoch seconds, and ISO-8601 strings are all
  accepted for `DateTime` / `DateTime64`. Naked timestamp strings with no
  trailing timezone (e.g. `"2026-04-22 10:30:00"`) are interpreted as UTC.
- `Date` / `Time` / `String` / `Integer` epoch are accepted for `Date` /
  `Date32`; only the calendar day is stored.
- `nil` on a non-`Nullable` column is silently coerced to the column's
  default (zero / empty string / empty array) — mirrors the HTTP gem's
  `JSONEachRow` behaviour. For strict semantics, use a `Nullable(T)` column.
- `LowCardinality(String)` and `LowCardinality(Nullable(String))` inserts
  are supported. Numeric `LowCardinality` dictionaries are not.
- `Map`, arbitrary `Tuple`, and other structural types are not yet supported
  for `#insert` — they decode fine, but inserting raises `EncoderError`.

## Errors

All errors inherit from `ClickhouseNative::Error`.

| class                 | raised when                                                 |
| --------------------- | ----------------------------------------------------------- |
| `ConnectionError`     | socket errors, host unreachable, calling on a closed client |
| `TimeoutError`        | a network-level timeout (subclass of `ConnectionError`)     |
| `ProtocolError`       | malformed server frames                                     |
| `ServerError`         | server-side `DB::Exception` — carries `#server_code`, `#server_name`, `#server_stacktrace` |
| `EncoderError`        | a Ruby value can't be encoded for a given column type       |
| `DecoderError`        | malformed data from the server                              |
| `UnsupportedTypeError`| decoding a CH type we don't yet map (subclass of `DecoderError`) |

After a `ServerError` (or any decoder/encoder error), the client
auto-resets its socket before re-raising, so you can keep using it:

```ruby
begin
  client.query("SELECT no_such_function()")
rescue ClickhouseNative::ServerError => e
  e.server_code    # => 46
  e.server_name    # => "DB::Exception"
end
client.query_value("SELECT 1")  # => 1
```

## Logging

Pass any `Logger`-compatible object as `logger:` to `Client.new` or
`Pool.new`. Every SQL statement logs a Sequel-style line at `:debug` with
the elapsed time; errors log at `:error`.

```ruby
client = ClickhouseNative::Client.new(**opts, logger: Rails.logger)
client.query("SELECT 1")
# DEBUG -- : (0.421ms) SELECT 1
```

## Concurrency

The extension releases the GVL around every blocking `clickhouse-cpp` call
(`execute`, `query`, `query_value`, `query_each`, `insert_block`, `ping`),
so a `Pool` of size N genuinely runs N concurrent ClickHouse queries from N
Ruby threads.

`benchmark/threaded.rb` demonstrates this with a `SELECT sleep(0.1)`
workload. Example output:

```
serial (16 jobs):          1.612s
parallel ( 2 threads, 16 jobs): 0.815s  (1.98x vs serial)
parallel ( 4 threads, 16 jobs): 0.410s  (3.93x vs serial)
parallel ( 8 threads, 16 jobs): 0.208s  (7.75x vs serial)
parallel (16 threads, 16 jobs): 0.110s  (14.64x vs serial)
```

A single `Client` is **not** thread-safe — always go through a `Pool` for
concurrent work.

## Development

```
git clone --recursive https://github.com/tycooon/clickhouse-native
cd clickhouse-native
bundle install
docker compose up -d clickhouse
bundle exec rake compile
bundle exec rspec
```

Benchmarks (require the `clickhouse` and `click_house` gems from
`Gemfile.release`):

```
bundle exec ruby benchmark/compare.rb    # vs HTTP gem
bundle exec ruby benchmark/threaded.rb   # GVL release / scaling
```

## License

Apache-2.0. The vendored clickhouse-cpp and its transitive contribs
(absl, cityhash, lz4, zstd) ship under their own upstream licenses.
