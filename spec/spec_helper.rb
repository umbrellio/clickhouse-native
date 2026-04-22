# frozen_string_literal: true

require "bigdecimal"
require "logger"
require "stringio"
require "clickhouse_native"

CH_HOST = ENV.fetch("CLICKHOUSE_HOST", "localhost")
CH_PORT = Integer(ENV.fetch("CLICKHOUSE_PORT", "9000"))
CH_KWARGS = { host: CH_HOST, port: CH_PORT }.freeze

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
end
