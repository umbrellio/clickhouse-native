require "connection_pool"

module ClickhouseNative
  class Pool
    def initialize(host:, port:, database: "default", user: "default", password: "",
                   pool_size: 5, pool_timeout: 5)
      client_kwargs = {host:, port:, database:, user:, password:}
      @pool = ConnectionPool.new(size: pool_size, timeout: pool_timeout) do
        Client.new(**client_kwargs)
      end
    end

    def with(&block)
      @pool.with(&block)
    end

    def execute(sql)
      @pool.with { |c| c.execute(sql) }
    end

    def query(sql)
      @pool.with { |c| c.query(sql) }
    end

    def query_value(sql)
      @pool.with { |c| c.query_value(sql) }
    end

    def ping
      @pool.with(&:ping)
    end

    def server_version
      @pool.with(&:server_version)
    end

    def describe_table(table, db_name: nil)
      @pool.with { |c| c.describe_table(table, db_name:) }
    end
  end
end
