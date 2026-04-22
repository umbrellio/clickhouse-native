require "clickhouse_native"

CH_HOST = ENV.fetch("CLICKHOUSE_HOST", "localhost")
CH_PORT = Integer(ENV.fetch("CLICKHOUSE_PORT", "9000"))

ClickhouseNative.configure do |c|
  c.host = CH_HOST
  c.port = CH_PORT
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
end
