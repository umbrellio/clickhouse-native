RSpec.describe ClickhouseNative do
  it "has a version number" do
    expect(ClickhouseNative::VERSION).to match(/\A\d+\.\d+\.\d+/)
  end

  describe ".hello", :clickhouse do
    it "round-trips SELECT toUInt64(42) via native protocol" do
      expect(ClickhouseNative.hello(CH_HOST, CH_PORT)).to eq(42)
    end
  end

  describe ".query", :clickhouse do
    def query(sql) = ClickhouseNative.query(sql, CH_HOST, CH_PORT)

    it "returns an array of symbol-keyed hashes" do
      rows = query("SELECT 1 AS a, 2 AS b UNION ALL SELECT 3 AS a, 4 AS b ORDER BY a")
      expect(rows).to eq([{a: 1, b: 2}, {a: 3, b: 4}])
    end

    it "handles no-row results" do
      expect(query("SELECT 1 WHERE 0")).to eq([])
    end

    context "type codec" do
      it "decodes String / LowCardinality(String) / FixedString" do
        row = query(<<~SQL).first
          SELECT
            'hello'                                AS s,
            CAST('cat' AS LowCardinality(String))  AS lc,
            CAST('abc' AS FixedString(5))          AS fs
        SQL
        expect(row[:s]).to eq("hello")
        expect(row[:s].encoding).to eq(Encoding::UTF_8)
        expect(row[:lc]).to eq("cat")
        expect(row[:fs]).to eq("abc\0\0")
      end

      it "decodes integer widths" do
        row = query(<<~SQL).first
          SELECT
            toInt8(-128)                           AS i8,
            toInt16(-32768)                        AS i16,
            toInt32(-2147483648)                   AS i32,
            toInt64(-9223372036854775808)          AS i64,
            toUInt8(255)                           AS u8,
            toUInt16(65535)                        AS u16,
            toUInt32(4294967295)                   AS u32,
            toUInt64(18446744073709551615)         AS u64
        SQL
        expect(row[:i8]).to eq(-128)
        expect(row[:i16]).to eq(-32_768)
        expect(row[:i32]).to eq(-2_147_483_648)
        expect(row[:i64]).to eq(-9_223_372_036_854_775_808)
        expect(row[:u8]).to eq(255)
        expect(row[:u16]).to eq(65_535)
        expect(row[:u32]).to eq(4_294_967_295)
        expect(row[:u64]).to eq(18_446_744_073_709_551_615)
      end

      it "decodes Float32 / Float64" do
        row = query("SELECT toFloat32(1.5) AS f32, toFloat64(3.141592653589793) AS f64").first
        expect(row[:f32]).to be_within(1e-6).of(1.5)
        expect(row[:f64]).to eq(3.141592653589793)
      end

      it "decodes DateTime64(6, 'UTC') at microsecond precision" do
        row = query("SELECT toDateTime64('2026-04-22 12:34:56.654321', 6, 'UTC') AS t").first
        expect(row[:t]).to be_a(Time)
        expect(row[:t].to_i).to eq(Time.utc(2026, 4, 22, 12, 34, 56).to_i)
        expect(row[:t].usec).to eq(654_321)
      end

      it "decodes Nullable(T) with both null and non-null values" do
        row = query("SELECT NULL::Nullable(String) AS a, toNullable('x') AS b").first
        expect(row[:a]).to be_nil
        expect(row[:b]).to eq("x")
      end

      it "decodes Array(T) recursively" do
        row = query("SELECT [1, 2, 3]::Array(UInt32) AS a, [['x'], ['y', 'z']]::Array(Array(String)) AS aa").first
        expect(row[:a]).to eq([1, 2, 3])
        expect(row[:aa]).to eq([["x"], ["y", "z"]])
      end

      it "decodes Map(K, V)" do
        row = query("SELECT map('a', 1, 'b', 2)::Map(String, Int32) AS m").first
        expect(row[:m]).to eq({"a" => 1, "b" => 2})
      end

      it "decodes Tuple(T1, T2, ...)" do
        row = query("SELECT (1, 'two', 3.0)::Tuple(UInt8, String, Float64) AS t").first
        expect(row[:t]).to eq([1, "two", 3.0])
      end

      it "decodes Enum8 as a Symbol name" do
        row = query("SELECT CAST('blue' AS Enum8('red' = 1, 'green' = 2, 'blue' = 3)) AS c").first
        expect(row[:c]).to eq(:blue)
      end
    end

    it "surfaces a server error as a Ruby exception" do
      expect { query("SELECT no_such_function()") }.to raise_error(RuntimeError, /clickhouse-native/)
    end
  end
end
