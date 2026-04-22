require "mkmf"
require "fileutils"
require "etc"
require "shellwords"

EXT_DIR = __dir__
VENDOR = File.expand_path("vendor/clickhouse-cpp", EXT_DIR)
# Scope the cmake build dir by target arch so host and cross-compile
# builds don't share a CMakeCache.txt (which burns in absolute paths
# like the host cmake binary).
ARCH = RbConfig::CONFIG["arch"] || "unknown"
BUILD_DIR = File.expand_path("../../tmp/cpp-build-#{ARCH}", EXT_DIR)

def fatal(msg)
  warn
  warn "=== clickhouse-native build failed ==="
  warn msg.chomp
  warn "===================================="
  warn
  abort "clickhouse-native: cannot build extension"
end

unless File.exist?(File.join(VENDOR, "CMakeLists.txt"))
  fatal <<~MSG
    clickhouse-cpp submodule not found at:
      #{VENDOR}

    If you cloned this gem's repo, run:
      git submodule update --init --recursive

    If you see this during `gem install`, please report a bug — the
    gemspec should have bundled the submodule tree.
  MSG
end

unless find_executable("cmake")
  fatal <<~MSG
    CMake is required to build the vendored clickhouse-cpp library
    but was not found on PATH.

    macOS:   brew install cmake
    Ubuntu:  sudo apt-get install -y cmake build-essential
    Alpine:  apk add --no-cache cmake build-base linux-headers

    A precompiled gem for your platform may be available on rubygems;
    if so, just `gem install clickhouse-native` (without a compiler
    tool chain) should pull it. If you're seeing this message, no
    precompiled gem matched your platform and we fell back to source.
  MSG
end

cxx = ENV["CXX"] || RbConfig::CONFIG["CXX"] || "c++"
cc = ENV["CC"] || RbConfig::CONFIG["CC"] || "cc"
unless find_executable(cxx.split.first)
  fatal <<~MSG
    A C++17-capable compiler is required.
    Tried:  #{cxx}

    macOS:   xcode-select --install
    Ubuntu:  sudo apt-get install -y g++
    Alpine:  apk add --no-cache g++
  MSG
end

FileUtils.mkdir_p(BUILD_DIR)

unless File.exist?(File.join(BUILD_DIR, "CMakeCache.txt"))
  configure_args = [
    "cmake",
    "-S", VENDOR,
    "-B", BUILD_DIR,
    "-DCMAKE_C_COMPILER=#{cc.split.first}",
    "-DCMAKE_CXX_COMPILER=#{cxx.split.first}",
    "-DBUILD_SHARED_LIBS=OFF",
    "-DBUILD_BENCHMARK=OFF",
    "-DBUILD_TESTS=OFF",
    "-DWITH_OPENSSL=OFF",
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
    "-DCMAKE_BUILD_TYPE=Release",
  ]
  system(*configure_args) or fatal("cmake configure failed. See #{BUILD_DIR}/CMakeFiles/CMakeOutput.log")
end

jobs = ENV.fetch("MAKE_JOBS") { Etc.nprocessors.to_s }
system("cmake", "--build", BUILD_DIR, "--parallel", jobs) or fatal("cmake build failed")

inc_dirs = [
  VENDOR,
  File.join(VENDOR, "contrib"),
  File.join(VENDOR, "contrib", "absl"),
]

# Order matters: dependents before dependencies.
# clickhouse-cpp depends on cityhash, lz4, absl, zstd.
lib_files = Dir.glob(File.join(BUILD_DIR, "**", "*.a"))
main, contribs = lib_files.partition { |p| p.include?("libclickhouse-cpp") }
ordered_libs = main + contribs

$CXXFLAGS = "#{$CXXFLAGS} -std=c++17 #{inc_dirs.map { |d| "-I#{d}" }.join(' ')}"
$CPPFLAGS = "#{$CPPFLAGS} #{inc_dirs.map { |d| "-I#{d}" }.join(' ')}"
$LDFLAGS = "#{$LDFLAGS} #{ordered_libs.map(&:shellescape).join(' ')}"

have_library("c++") || have_library("stdc++")

$objs = ["client.o"]
$srcs = ["client.cpp"]

create_makefile("clickhouse_native/clickhouse_native")
