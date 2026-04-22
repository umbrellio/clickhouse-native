# frozen_string_literal: true

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

    def execute(sql)
      log_sql(sql) { super }
    end

    def query(sql)
      log_sql(sql) { super }
    end

    def query_value(sql)
      log_sql(sql) { super }
    end

    def query_each(sql, &)
      log_sql(sql) { super }
    end

    def insert_block(table, columns, rows)
      col_list = columns.map(&:first).join(", ")
      log_sql("INSERT INTO #{table} (#{col_list}) VALUES (#{rows.size} rows)") { super }
    end

    private

    def log_sql(sql)
      return yield unless @logger

      t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      begin
        result = yield
        elapsed_ms = (Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0) * 1000
        @logger.public_send(LEVEL, format("(%<ms>.3fms) %<sql>s", ms: elapsed_ms, sql: sql))
        result
      rescue => error
        elapsed_ms = (Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0) * 1000
        first_line = error.message.to_s.lines.first.to_s.strip
        @logger.error(format(
                        "(%<ms>.3fms) %<class>s: %<msg>s -- %<sql>s",
                        ms: elapsed_ms, class: error.class, msg: first_line, sql: sql,
                      ))
        raise
      end
    end
  end

  class Client
    attr_accessor :logger

    prepend Logging
  end
end
