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
    it "returns an array of symbol-keyed hashes" do
      rows = ClickhouseNative.query("SELECT 1 AS a, 2 AS b UNION ALL SELECT 3 AS a, 4 AS b ORDER BY a")
      expect(rows).to eq([{a: 1, b: 2}, {a: 3, b: 4}])
    end

    it "handles no-row results" do
      expect(ClickhouseNative.query("SELECT 1 WHERE 0")).to eq([])
    end

    context "type codec" do
      def query(sql) = ClickhouseNative.query(sql)

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
            toInt8(-128) AS i8, toInt16(-32768) AS i16,
            toInt32(-2147483648) AS i32, toInt64(-9223372036854775808) AS i64,
            toUInt8(255) AS u8, toUInt16(65535) AS u16,
            toUInt32(4294967295) AS u32, toUInt64(18446744073709551615) AS u64
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

      it "decodes Nullable(T)" do
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

      it "decodes Tuple(...)" do
        row = query("SELECT (1, 'two', 3.0)::Tuple(UInt8, String, Float64) AS t").first
        expect(row[:t]).to eq([1, "two", 3.0])
      end

      it "decodes Enum8 as a Symbol name" do
        row = query("SELECT CAST('blue' AS Enum8('red' = 1, 'green' = 2, 'blue' = 3)) AS c").first
        expect(row[:c]).to eq(:blue)
      end
    end

    context "advanced types not supported by clickhouse-cpp v2.6.1" do
      it "rejects Dynamic with UnsupportedTypeError" do
        expect { ClickhouseNative.query("SELECT CAST(42 AS Dynamic) AS d") }
          .to raise_error(ClickhouseNative::UnsupportedTypeError, /Dynamic/)
      end

      it "rejects Variant with UnsupportedTypeError" do
        expect { ClickhouseNative.query("SELECT CAST(toUInt64(42), 'Variant(UInt64, String)') AS v") }
          .to raise_error(ClickhouseNative::UnsupportedTypeError, /Variant/)
      end

      it "rejects typed JSON with UnsupportedTypeError" do
        expect { ClickhouseNative.query("SELECT CAST('{\"a\":1}', 'JSON') AS j") }
          .to raise_error(ClickhouseNative::UnsupportedTypeError, /JSON/)
      end
    end
  end

  describe "error mapping", :clickhouse do
    it "raises ServerError with code/name/message on server-side failures" do
      expect { ClickhouseNative.query("SELECT no_such_function()") }.to raise_error do |err|
        expect(err).to be_a(ClickhouseNative::ServerError)
        expect(err.server_code).to be_a(Integer)
        expect(err.server_name).to be_a(String).and include("DB::Exception")
        expect(err.message).to include("no_such_function")
      end
    end

    it "raises ConnectionError when the host is unreachable" do
      expect { ClickhouseNative::Client.new(host: "127.0.0.1", port: 6553) }
        .to raise_error(ClickhouseNative::ConnectionError)
    end
  end

  describe ClickhouseNative::Client, :clickhouse do
    subject(:client) { described_class.new(host: CH_HOST, port: CH_PORT) }
    after { client.close }

    it "exposes host/port/database" do
      expect(client.host).to eq(CH_HOST)
      expect(client.port).to eq(CH_PORT)
      expect(client.database).to eq("default")
    end

    it "#ping returns true" do
      expect(client.ping).to be(true)
    end

    it "#server_version returns a version string" do
      expect(client.server_version).to match(/\A\d+\.\d+\.\d+\z/)
    end

    it "#execute runs DDL/DML without returning rows" do
      client.execute("CREATE DATABASE IF NOT EXISTS chn_test")
      client.execute("DROP TABLE IF EXISTS chn_test.x")
      client.execute("CREATE TABLE chn_test.x (id UInt64, s String) ENGINE = Memory")
      client.execute("INSERT INTO chn_test.x VALUES (1, 'a'), (2, 'b')")
      expect(client.query_value("SELECT count() FROM chn_test.x")).to eq(2)
      client.execute("DROP DATABASE chn_test")
    end

    it "#query_value returns the first cell" do
      expect(client.query_value("SELECT 7 AS n")).to eq(7)
      expect(client.query_value("SELECT 1 WHERE 0")).to be_nil
    end

    it "#describe_table returns [{name:, type:, ...}]" do
      cols = client.describe_table("one", db_name: "system")
      expect(cols).to be_a(Array)
      expect(cols.first).to include(:name, :type)
      expect(cols.map { |c| c[:name] }).to include("dummy")
    end

    it "reuses the same connection across many calls" do
      threads = 30
      threads.times { expect(client.query_value("SELECT 1")).to eq(1) }
    end

    it "#close releases the connection and subsequent calls raise" do
      client.close
      expect { client.query("SELECT 1") }.to raise_error(ClickhouseNative::ConnectionError, /closed/)
    end
  end

  describe ClickhouseNative::Pool, :clickhouse do
    let(:pool) { described_class.new(host: CH_HOST, port: CH_PORT, pool_size: 4) }

    it "exposes the Client API" do
      expect(pool.query_value("SELECT 1")).to eq(1)
      expect(pool.ping).to be(true)
    end

    it "serves concurrent queries across its checked-out clients" do
      results = Array.new(8).map do
        Thread.new { pool.query_value("SELECT 1") }
      end.map(&:value)
      expect(results).to all(eq(1))
    end
  end
end
