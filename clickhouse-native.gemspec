# frozen_string_literal: true

require_relative "lib/clickhouse_native/version"

Gem::Specification.new do |spec|
  spec.name = "clickhouse-native"
  spec.version = ClickhouseNative::VERSION
  spec.authors = ["Yuri Smirnov"]
  spec.email = ["tycoooon@gmail.com"]

  spec.summary = "ClickHouse Ruby driver over the native TCP protocol"
  spec.description = "A high-performance Ruby client for ClickHouse using the " \
                     "native binary protocol via a C++ extension wrapping clickhouse-cpp."
  spec.license = "Apache-2.0"
  spec.required_ruby_version = ">= 3.3.0"

  vendored_cpp = %r{/vendor/clickhouse-cpp/(?:\.git|bench|tests|ut)/}
  spec.files = Dir.glob("{lib,ext}/**/*", File::FNM_DOTMATCH).reject do |path|
    File.directory?(path) || path.include?("/build/") || path.match?(vendored_cpp)
  end
  spec.require_paths = ["lib"]
  spec.extensions = ["ext/clickhouse_native/extconf.rb"]

  spec.add_dependency "connection_pool", ">= 2.4"
end
