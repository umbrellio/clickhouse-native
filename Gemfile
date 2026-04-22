# frozen_string_literal: true

source "https://rubygems.org"

gemspec

group :development, :test do
  gem "rake", "~> 13.2"
  gem "rake-compiler", "~> 1.2"
  gem "rake-compiler-dock", "~> 1.9"
  gem "rspec", "~> 3.13"
  gem "rubocop-config-umbrellio"
end

group :benchmark do
  gem "benchmark-ips", "~> 2.14"
  gem "click_house", github: "umbrellio/click_house", branch: "master"
  gem "csv"
  gem "logger"
end
