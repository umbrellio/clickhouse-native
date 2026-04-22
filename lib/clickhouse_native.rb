require "clickhouse_native/version"
require "clickhouse_native/errors"
require "clickhouse_native/config"
require "clickhouse_native/clickhouse_native"
require "clickhouse_native/client"
require "clickhouse_native/pool"

module ClickhouseNative
  class << self
    def configure
      @config ||= Config.new
      yield @config if block_given?
      @default_pool = nil
      @config
    end

    def config
      @config ||= Config.new
    end

    def default_pool
      @default_pool ||= Pool.new(**config.pool_kwargs)
    end

    def execute(sql) = default_pool.execute(sql)
    def query(sql) = default_pool.query(sql)
    def query_value(sql) = default_pool.query_value(sql)
    def ping = default_pool.ping
    def server_version = default_pool.server_version
    def describe_table(table, db_name: nil) = default_pool.describe_table(table, db_name:)
  end
end
