# frozen_string_literal: true

require "connection_pool"

module ClickhouseNative
  class Pool
    attr_reader :host, :port, :database

    def initialize(host:, port:, database: "default", user: "default", password: "",
                   compression: :none, logger: nil, settings: {},
                   pool_size: 5, pool_timeout: 5,
                   ping_before_query: true, tcp_keepalive: true, retry_timeout: 1)
      @host = host
      @port = port
      @database = database
      client_kwargs = {
        host:, port:, database:, user:, password:, compression:, logger:, settings:,
        ping_before_query:, tcp_keepalive:, retry_timeout:
      }
      @pool = ConnectionPool.new(size: pool_size, timeout: pool_timeout) do
        Client.new(**client_kwargs)
      end
    end

    # On any non-local exit, discard the client rather than reuse it: an
    # aborted operation leaves the socket in an unknown state. The C++
    # binding issues ResetConnection, but a subsequent send can still surface buffered
    # protocol errors from the prior aborted operation — those get
    # attributed to whatever SQL we tried next, producing misleading log
    # lines and re-raises in unrelated code. A fresh socket + handshake is
    # cheap relative to debugging that.
    #
    # ConnectionError gets one automatic retry: pooled connections that
    # have been idle long enough for the server / an LB to FIN them
    # surface as "closed" on the very next recv (errno is whatever stale
    # value was left in the thread — "closed: Success", "closed: Operation
    # now in progress", etc. all mean the same recv()==0). Discarding and
    # re-checking out lands a fresh socket and the operation succeeds. The
    # retry only triggers when the dead-connection error fired before any
    # data was sent, so write operations don't risk double-execution.
    #
    # This is the backstop: with ping_before_query on (the default) the
    # driver already pings and transparently reconnects a dead socket
    # before running the query, so most stale connections never surface as
    # a ConnectionError here at all. This retry still covers the residual
    # race (socket dies between the ping and the query).
    #
    # The discard hangs off `ensure`, not `rescue`, because the ways a
    # query gets abandoned mid-flight mostly are not StandardError.
    # Thread#kill raises nothing at all (Parallel.in_threads kills every
    # sibling worker when one of them fails), and Timeout / Sidekiq
    # shutdown raise off Exception. A `rescue` misses all of those and
    # checks a connection back in with the server still streaming a
    # response at it — the next checkout then reads that leftover as its
    # own, which surfaces as a bogus packet type far from here.
    def with
      attempts = 0
      begin
        @pool.with do |client|
          finished = false
          begin
            result = yield client
            finished = true
            result
          ensure
            @pool.discard_current_connection(&:close) unless finished
          end
        end
      rescue ConnectionError
        attempts += 1
        retry if attempts == 1
        raise
      end
    end

    def execute(sql, **opts)
      with { |c| c.execute(sql, **opts) }
    end

    def query(sql, **opts)
      with { |c| c.query(sql, **opts) }
    end

    def query_each(sql, **opts, &block)
      with { |c| c.query_each(sql, **opts, &block) }
    end

    def query_value(sql, **opts)
      with { |c| c.query_value(sql, **opts) }
    end

    def insert(table, rows, **opts)
      with { |c| c.insert(table, rows, **opts) }
    end

    def ping
      with(&:ping)
    end

    def server_version
      with(&:server_version)
    end

    def describe_table(table, db_name: nil)
      with { |c| c.describe_table(table, db_name:) }
    end
  end
end
