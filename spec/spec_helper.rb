# frozen_string_literal: true

require "bigdecimal"
require "logger"
require "stringio"
require "clickhouse_native"

CH_HOST = ENV.fetch("CLICKHOUSE_HOST", "localhost")
CH_PORT = Integer(ENV.fetch("CLICKHOUSE_PORT", "9000"))
CH_KWARGS = { host: CH_HOST, port: CH_PORT }.freeze

# Settings that allow the JSON type on the server. The names differ across
# versions (allow_experimental_json_type on 24.x, enable_json_type on 25.x);
# both are sent and the server silently ignores whichever it doesn't know.
CH_JSON_ENABLE = { allow_experimental_json_type: 1, enable_json_type: 1 }.freeze

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
