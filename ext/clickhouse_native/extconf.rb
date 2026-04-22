require "mkmf"
require "fileutils"

EXT_DIR = __dir__
VENDOR = File.expand_path("vendor/clickhouse-cpp", EXT_DIR)
BUILD_DIR = File.expand_path("build", EXT_DIR)

abort "clickhouse-cpp submodule missing at #{VENDOR}" unless File.exist?(File.join(VENDOR, "CMakeLists.txt"))

FileUtils.mkdir_p(BUILD_DIR)

unless File.exist?(File.join(BUILD_DIR, "CMakeCache.txt"))
  configure_args = [
    "cmake",
    "-S", VENDOR,
    "-B", BUILD_DIR,
    "-DBUILD_SHARED_LIBS=OFF",
    "-DBUILD_BENCHMARK=OFF",
    "-DBUILD_TESTS=OFF",
    "-DWITH_OPENSSL=OFF",
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
    "-DCMAKE_BUILD_TYPE=Release",
  ]
  system(*configure_args) or abort "cmake configure failed"
end

jobs = ENV["MAKE_JOBS"] || Etc.nprocessors.to_s rescue "4"
system("cmake", "--build", BUILD_DIR, "--parallel", jobs) or abort "cmake build failed"

inc_dirs = [
  VENDOR,
  File.join(VENDOR, "contrib"),
  File.join(VENDOR, "contrib", "absl"),
]

# Order matters: dependents before dependencies.
# clickhouse-cpp depends on cityhash, lz4, absl (and optionally zstd/openssl).
lib_files = Dir.glob(File.join(BUILD_DIR, "**", "*.a"))
main, contribs = lib_files.partition { |p| p.include?("libclickhouse-cpp") }
ordered_libs = main + contribs

$CXXFLAGS = "#{$CXXFLAGS} -std=c++17 #{inc_dirs.map { |d| "-I#{d}" }.join(' ')}"
$CPPFLAGS = "#{$CPPFLAGS} #{inc_dirs.map { |d| "-I#{d}" }.join(' ')}"
$LDFLAGS = "#{$LDFLAGS} #{ordered_libs.map { |p| p.shellescape }.join(' ')}"

# Explicit C++ standard library link.
have_library("c++") || have_library("stdc++")

# Tell mkmf to treat the source as C++ and name the compiled object properly.
$objs = ["client.o"]
$srcs = ["client.cpp"]

create_makefile("clickhouse_native/clickhouse_native")
