# frozen_string_literal: true

require "bigdecimal"
require "logger"
require "socket"
require "stringio"
require "clickhouse_native"

CH_HOST = ENV.fetch("CLICKHOUSE_HOST", "localhost")
CH_PORT = Integer(ENV.fetch("CLICKHOUSE_PORT", "9000"))
CH_KWARGS = { host: CH_HOST, port: CH_PORT }.freeze

# Settings that allow the JSON type on the server. The names differ across
# versions (allow_experimental_json_type on 24.x, enable_json_type on 25.x);
# both are sent and the server silently ignores whichever it doesn't know.
CH_JSON_ENABLE = { allow_experimental_json_type: 1, enable_json_type: 1 }.freeze

# Minimal TCP pass-through proxy used to simulate a server / load balancer
# dropping an established connection (idle_connection_timeout, LB recycle).
# A client points at #port; #sever_all tears down every live tunnel so the
# client's next use of that socket hits recv()==0, exactly like production.
# The accept loop keeps running, so reconnects succeed against a fresh tunnel.
#
# The accept/tunnel loop runs in a CHILD PROCESS: Client#initialize does its
# connect+handshake without releasing the GVL, so an in-process accept thread
# could never run and the very first connect would deadlock. A separate process
# has its own GVL. #sever_all signals the child (SIGUSR1) to drop live tunnels
# and blocks on a pipe ack until the child has actually closed them — otherwise
# the caller could race ahead and query the still-live socket, so a reconnect
# would never be exercised.
class TcpProxy
  attr_reader :port

  def initialize(upstream_host:, upstream_port:)
    @upstream_host = upstream_host
    @upstream_port = upstream_port
    @server = TCPServer.new("127.0.0.1", 0)
    @port = @server.addr[1]
    @ack_read, @ack_write = IO.pipe
  end

  def start
    @pid = fork do
      @ack_read.close
      run_child
    rescue Exception # rubocop:disable Lint/RescueException
      nil
    ensure
      exit!(0) # never fall back into the RSpec process
    end
    @server.close
    @ack_write.close # only the child writes acks
    self
  end

  def sever_all
    Process.kill("USR1", @pid)
    @ack_read.read(1) # block until the child confirms the tunnels are closed
  end

  def stop
    Process.kill("KILL", @pid)
    Process.wait(@pid)
  rescue Errno::ESRCH, Errno::ECHILD
    nil
  ensure
    @ack_read.close
  end

  private

  def run_child
    conns = []
    # The trap fires on this same (accept-loop) thread, so no lock is needed —
    # and Mutex/Thread ops are illegal in a trap context anyway.
    trap("USR1") do
      live = conns
      conns = []
      live.each do |a, b|
        close(a)
        close(b)
      end
      @ack_write.write(".") # tell sever_all the tunnels are down
    end
    loop do
      downstream = @server.accept
      upstream = TCPSocket.new(@upstream_host, @upstream_port)
      conns << [downstream, upstream]
      Thread.new { pump(downstream, upstream) }
      Thread.new { pump(upstream, downstream) }
    end
  rescue IOError, Errno::EBADF
    nil
  end

  def pump(src, dst)
    IO.copy_stream(src, dst)
  rescue IOError, Errno::EBADF, Errno::ECONNRESET, Errno::EPIPE
    nil
  ensure
    close(src)
    close(dst)
  end

  def close(io)
    io&.close
  rescue IOError
    nil
  end
end

RSpec.configure do |config|
  config.expect_with :rspec do |c|
    c.syntax = :expect
  end

  config.mock_with :rspec do |mocks|
    mocks.verify_partial_doubles = true
  end

  config.disable_monkey_patching!
  config.order = :random
  Kernel.srand(config.seed)

  # Gate examples tagged `min_ch_major:` to a minimum ClickHouse major version.
  # The server version is fetched once and memoized. Used for features like the
  # JSON type, whose string-backed wire format requires CH 25.x+.
  ch_major = nil
  config.before do |example|
    min = example.metadata[:min_ch_major]
    next unless min

    ch_major ||= begin
      conn = ClickhouseNative::Client.new(**CH_KWARGS)
      conn.server_version.split(".").first.to_i
    ensure
      conn&.close
    end
    skip "requires ClickHouse >= #{min}.x (server is #{ch_major}.x)" if ch_major < min
  end
end
