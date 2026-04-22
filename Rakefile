require "bundler/gem_tasks"
require "rake/extensiontask"
require "rake_compiler_dock"
require "rspec/core/rake_task"

GEMSPEC = Gem::Specification.load("clickhouse-native.gemspec")

# Cross-compile targets. For local dev you only care about your host
# platform, but pre-built gems published to rubygems cover the matrix
# below. Everything outside `ruby` platform is produced via
# rake-compiler-dock (Docker-based cross-compile).
CROSS_PLATFORMS = %w[
  x86_64-linux-gnu
  aarch64-linux-gnu
  x86_64-darwin
  arm64-darwin
].freeze

# Ruby ABIs to build pre-compiled gems for. Patch versions must match
# what rake-compiler-dock ships pre-installed (see
# /usr/local/rake-compiler/ruby/<platform> inside the image).
CROSS_RUBIES = %w[3.3.11 3.4.9 4.0.2].freeze

Rake::ExtensionTask.new("clickhouse_native", GEMSPEC) do |ext|
  ext.lib_dir = "lib/clickhouse_native"
  ext.cross_compile = true
  ext.cross_platform = CROSS_PLATFORMS
  ext.cross_compiling do |spec|
    # Pre-compiled gems carry the .bundle/.so plus Ruby sources only.
    # Drop ext/ (C++ sources, vendored clickhouse-cpp submodule, build
    # artifacts) so the gem stays small and `gem install` doesn't
    # re-invoke extconf.
    spec.files.reject! { |f| f.start_with?("ext/") }
    spec.extensions = []
    spec.metadata["precompiled"] = "true"
  end
end

RSpec::Core::RakeTask.new(:spec)

task default: %i[compile spec]

# rake gem:cross:<platform>
#
# Cross-compile a precompiled native gem for one target. Requires a
# running Docker daemon and the rake-compiler-dock images (pulled on
# first use; ~1GB per variant).
namespace :gem do
  namespace :cross do
    CROSS_PLATFORMS.each do |platform|
      desc "Cross-compile native gem for #{platform}"
      task platform do
        RakeCompilerDock.sh(
          <<~SH,
            export BUNDLE_GEMFILE=$(pwd)/Gemfile.release
            bundle install
            bundle exec rake native:#{platform} gem RUBY_CC_VERSION=#{CROSS_RUBIES.join(':')}
          SH
          platform: platform,
        )
      end
    end

    desc "Cross-compile native gems for all supported platforms"
    task all: CROSS_PLATFORMS.map { |p| "gem:cross:#{p}" }
  end
end
