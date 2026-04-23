# frozen_string_literal: true

require "connection_pool"

module ClickhouseNative
  class Pool
    STALE_IVAR = :@clickhouse_native_settings_stale

    attr_reader :host, :port, :database

    def initialize(host:, port:, database: "default", user: "default", password: "",
                   compression: :none, logger: nil, settings: {},
                   pool_size: 5, pool_timeout: 5)
      @host = host
      @port = port
      @database = database
      client_kwargs = { host:, port:, database:, user:, password:, compression:, logger: }
      @set_sql = settings_sql(settings)
      @pool = ConnectionPool.new(size: pool_size, timeout: pool_timeout) do
        client = Client.new(**client_kwargs)
        client.execute(@set_sql) if @set_sql
        client
      end
    end

    # Yields a client with the pool's session settings applied. If the
    # previous checkout raised (which, in this gem's C++ bindings, always
    # triggers a ResetConnection that wipes the session), re-apply the SET
    # before yielding. Exceptions re-raise after marking the client stale.
    def with
      @pool.with do |client|
        reapply_settings_if_stale(client)
        begin
          yield client
        rescue
          client.instance_variable_set(STALE_IVAR, true)
          raise
        end
      end
    end

    def execute(sql)
      with { |c| c.execute(sql) }
    end

    def query(sql)
      with { |c| c.query(sql) }
    end

    def query_each(sql, &block)
      with { |c| c.query_each(sql, &block) }
    end

    def query_value(sql)
      with { |c| c.query_value(sql) }
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

    private

    def reapply_settings_if_stale(client)
      return unless @set_sql
      return unless client.instance_variable_get(STALE_IVAR)
      client.execute(@set_sql)
      client.instance_variable_set(STALE_IVAR, false)
    end

    # Render a `SET key1 = val1, key2 = val2` statement once at pool setup
    # so every checked-out connection starts with the same session
    # settings. Matches how the HTTP driver injected global_params per
    # request. Values: Integer / Float render bare; anything else is
    # quoted as a SQL string literal.
    def settings_sql(settings)
      return nil if settings.nil? || settings.empty?
      parts = settings.map do |k, v|
        literal = v.is_a?(Numeric) ? v.to_s : "'#{v.to_s.gsub("'", "''")}'"
        "#{k} = #{literal}"
      end
      "SET #{parts.join(', ')}"
    end
  end
end
