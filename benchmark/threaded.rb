#!/usr/bin/env ruby
# Demonstrates that the GVL is released during clickhouse-cpp I/O, so N
# worker threads on a pool of size N complete roughly in 1/N wall time
# of a serial loop.
#
# Each "job" does a CH-side sleep(0.1) so the work is almost entirely
# network-wait. If the GVL weren't released, N workers would serialise
# and wall time would not improve.
#
# Run: bundle exec ruby benchmark/threaded.rb

require "bundler/setup"
require "clickhouse_native"

HOST = ENV.fetch("CLICKHOUSE_HOST", "localhost")
PORT = Integer(ENV.fetch("CLICKHOUSE_PORT", "9000"))

SLEEP_SQL = "SELECT sleep(0.1)"
JOBS = 16

def run_serial
  c = ClickhouseNative::Client.new(host: HOST, port: PORT)
  t = Time.now
  JOBS.times { c.execute(SLEEP_SQL) }
  c.close
  Time.now - t
end

def run_parallel(threads:)
  pool = ClickhouseNative::Pool.new(host: HOST, port: PORT, pool_size: threads)
  t = Time.now
  workers = Array.new(threads) do
    Thread.new do
      (JOBS / threads).times { pool.execute(SLEEP_SQL) }
    end
  end
  workers.each(&:join)
  Time.now - t
end

serial_t = run_serial
puts format("serial (%2d jobs):          %.3fs", JOBS, serial_t)

[2, 4, 8, 16].each do |n|
  parallel_t = run_parallel(threads: n)
  speedup = serial_t / parallel_t
  puts format("parallel (%2d threads, %2d jobs): %.3fs  (%.2fx vs serial)", n, JOBS, parallel_t, speedup)
end
