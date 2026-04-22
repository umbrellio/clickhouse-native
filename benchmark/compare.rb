#!/usr/bin/env ruby
# Compare native TCP driver vs click_house HTTP gem on a few representative
# workloads. Assumes a ClickHouse server at localhost:9000 (native) and
# localhost:8123 (HTTP).
#
# Run: bundle exec ruby benchmark/compare.rb

require "bundler/setup"
require "clickhouse_native"
require "click_house"
require "benchmark/ips"

HOST = ENV.fetch("CLICKHOUSE_HOST", "localhost")
NATIVE_PORT = Integer(ENV.fetch("CLICKHOUSE_NATIVE_PORT", "9000"))
HTTP_PORT = Integer(ENV.fetch("CLICKHOUSE_HTTP_PORT", "8123"))

CHN = ClickhouseNative::Client.new(host: HOST, port: NATIVE_PORT)

ClickHouse.config do |c|
  c.adapter = :net_http
  c.url = "http://#{HOST}:#{HTTP_PORT}"
  c.database = "default"
  c.logger = Logger.new(IO::NULL)
end
CH_HTTP = ClickHouse.connection

TINY_SQL = "SELECT 1"

MEDIUM_SQL = <<~SQL.freeze
  SELECT
    toInt64(number)                              AS i,
    toString(number)                             AS s,
    CAST(toString(number % 100) AS LowCardinality(String)) AS lc,
    toFloat64(number) / 3.14                     AS f,
    toDateTime64(now64(6) + number, 6, 'UTC')    AS t,
    [number, number + 1, number + 2]             AS arr
  FROM numbers(1000)
SQL

LARGE_SQL = <<~SQL.freeze
  SELECT
    toInt64(number)                              AS i,
    toString(number)                             AS s,
    CAST(toString(number % 100) AS LowCardinality(String)) AS lc,
    toFloat64(number) / 3.14                     AS f
  FROM numbers(100000)
SQL

def bench_pair(label, sql)
  native_rows = CHN.query(sql)
  http_rows = CH_HTTP.select_all(sql).to_a

  puts "#{label}: native=#{native_rows.size} rows, http=#{http_rows.size} rows"

  Benchmark.ips do |x|
    x.warmup = 1
    x.time = 3
    x.report("native:#{label}") { CHN.query(sql) }
    x.report("http:#{label}")   { CH_HTTP.select_all(sql).to_a }
    x.compare!
  end
  puts
end

bench_pair("tiny", TINY_SQL)
bench_pair("medium(1k rows)", MEDIUM_SQL)
bench_pair("large(100k rows)", LARGE_SQL)

# ------------------------------------------------------------------
# Insert: native block insert vs HTTP JSONEachRow
# ------------------------------------------------------------------

CHN.execute("CREATE DATABASE IF NOT EXISTS chn_bench")
CHN.execute("DROP TABLE IF EXISTS chn_bench.t")
CHN.execute(<<~SQL)
  CREATE TABLE chn_bench.t (
    id UInt64, s String, f Float64, t DateTime64(6, 'UTC')
  ) ENGINE = Memory
SQL

ROWS_1K = Array.new(1000) do |i|
  {id: i, s: "row-#{i}", f: i / 3.14, t: Time.utc(2026, 4, 22, 10, 0, 0, 123_456)}
end

puts "insert(1k rows): warming up..."
CHN.insert("t", ROWS_1K, db_name: "chn_bench")
CHN.execute("TRUNCATE TABLE chn_bench.t")

Benchmark.ips do |x|
  x.warmup = 1
  x.time = 3
  x.report("native:insert(1k)") do
    CHN.insert("t", ROWS_1K, db_name: "chn_bench")
    CHN.execute("TRUNCATE TABLE chn_bench.t")
  end
  x.report("http:insert(1k)") do
    CH_HTTP.insert("chn_bench.t", ROWS_1K.map { |r| r.merge(t: r[:t].strftime("%Y-%m-%d %H:%M:%S.%6N")) })
    CH_HTTP.execute("TRUNCATE TABLE chn_bench.t")
  end
  x.compare!
end
CHN.execute("DROP DATABASE chn_bench")
