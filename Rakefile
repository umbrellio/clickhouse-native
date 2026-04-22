require "bundler/gem_tasks"
require "rake/extensiontask"
require "rspec/core/rake_task"

Rake::ExtensionTask.new("clickhouse_native") do |ext|
  ext.lib_dir = "lib/clickhouse_native"
end

RSpec::Core::RakeTask.new(:spec)

task default: %i[compile spec]
