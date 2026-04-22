# frozen_string_literal: true

require "time"
require "bigdecimal"
require "clickhouse_native/version"
require "clickhouse_native/errors"

# Precompiled gems ship the native extension under a Ruby-ABI subdir
# (e.g. lib/clickhouse_native/4.0/clickhouse_native.bundle). Source gems
# build into lib/clickhouse_native/clickhouse_native.{bundle,so}. Try the
# ABI-matched path first; fall back to the flat path.
begin
  RUBY_VERSION =~ /(\d+\.\d+)/
  require "clickhouse_native/#{Regexp.last_match(1)}/clickhouse_native"
rescue LoadError
  require "clickhouse_native/clickhouse_native"
end

require "clickhouse_native/client"
require "clickhouse_native/logging"
require "clickhouse_native/pool"

module ClickhouseNative
end
