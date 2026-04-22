RSpec.describe ClickhouseNative do
  it "has a version number" do
    expect(ClickhouseNative::VERSION).to match(/\A\d+\.\d+\.\d+/)
  end

  describe ".hello", :clickhouse do
    it "round-trips SELECT toUInt64(42) via native protocol" do
      expect(ClickhouseNative.hello(CH_HOST, CH_PORT)).to eq(42)
    end
  end
end
