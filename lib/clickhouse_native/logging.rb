module ClickhouseNative
  # Sequel-style logging wrapper. Prepended onto Client so it fires for the
  # raw C-level execute/query/query_value/query_each/insert_block calls.
  #
  #   client = Client.new(..., logger: Rails.logger)
  #   client.query("SELECT 1")
  #   # => DEBUG  -- : (0.421ms) SELECT 1
  #
  # Errors are logged at ERROR with the elapsed time and exception class.
  module Logging
    LEVEL = :debug

    def execute(sql, *a, **kw)
      log_sql(sql) { super }
    end

    def query(sql, *a, **kw)
      log_sql(sql) { super }
    end

    def query_value(sql, *a, **kw)
      log_sql(sql) { super }
    end

    def query_each(sql, *a, **kw, &block)
      log_sql(sql) { super }
    end

    def insert_block(table, columns, rows)
      log_sql("INSERT INTO #{table} (#{columns.map(&:first).join(', ')}) VALUES (#{rows.size} rows)") { super }
    end

    private

    def log_sql(sql)
      return yield unless @logger

      t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      begin
        result = yield
        elapsed_ms = (Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0) * 1000
        @logger.public_send(LEVEL, format("(%.3fms) %s", elapsed_ms, sql))
        result
      rescue => e
        elapsed_ms = (Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0) * 1000
        @logger.error(format("(%.3fms) %s: %s -- %s", elapsed_ms, e.class, e.message.to_s.lines.first.to_s.strip, sql))
        raise
      end
    end
  end

  class Client
    attr_accessor :logger
    prepend Logging
  end
end
