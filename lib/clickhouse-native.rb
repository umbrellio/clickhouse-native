# frozen_string_literal: true

# Shim so `require "clickhouse-native"` works (Bundler's auto-require for
# a hyphenated gem name falls through to this path).
require "clickhouse_native"
