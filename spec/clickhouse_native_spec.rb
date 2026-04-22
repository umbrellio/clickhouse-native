RSpec.describe ClickhouseNative do
  it "has a version number" do
    expect(ClickhouseNative::VERSION).to match(/\A\d+\.\d+\.\d+/)
  end
end

RSpec.describe ClickhouseNative::Client, :clickhouse do
  subject(:client) { described_class.new(**CH_KWARGS) }
  after { client.close }

  describe "connection attributes" do
    it "exposes host/port/database" do
      expect(client.host).to eq(CH_HOST)
      expect(client.port).to eq(CH_PORT)
      expect(client.database).to eq("default")
    end

    it "inspect renders host:port/database" do
      expect(client.inspect).to include("#{CH_HOST}:#{CH_PORT}/default")
    end
  end

  describe "#ping" do
    it "returns true on a live connection" do
      expect(client.ping).to be(true)
    end
  end

  describe "#server_version" do
    it "returns a dotted version string" do
      expect(client.server_version).to match(/\A\d+\.\d+\.\d+\z/)
    end
  end

  describe "#execute" do
    it "runs DDL/DML without returning rows" do
      client.execute("CREATE DATABASE IF NOT EXISTS chn_test")
      client.execute("DROP TABLE IF EXISTS chn_test.x")
      client.execute("CREATE TABLE chn_test.x (id UInt64, s String) ENGINE = Memory")
      client.execute("INSERT INTO chn_test.x VALUES (1, 'a'), (2, 'b')")
      expect(client.query_value("SELECT count() FROM chn_test.x")).to eq(2)
      client.execute("DROP DATABASE chn_test")
    end
  end

  describe "#query_value" do
    it "returns the first cell or nil" do
      expect(client.query_value("SELECT 7 AS n")).to eq(7)
      expect(client.query_value("SELECT 1 WHERE 0")).to be_nil
    end
  end

  describe "#query" do
    it "returns an array of symbol-keyed hashes" do
      rows = client.query("SELECT 1 AS a, 2 AS b UNION ALL SELECT 3 AS a, 4 AS b ORDER BY a")
      expect(rows).to eq([{a: 1, b: 2}, {a: 3, b: 4}])
    end

    it "handles no-row results" do
      expect(client.query("SELECT 1 WHERE 0")).to eq([])
    end

    context "type codec" do
      it "decodes String / LowCardinality(String) / FixedString" do
        row = client.query(<<~SQL).first
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
        row = client.query(<<~SQL).first
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
        row = client.query("SELECT toFloat32(1.5) AS f32, toFloat64(3.141592653589793) AS f64").first
        expect(row[:f32]).to be_within(1e-6).of(1.5)
        expect(row[:f64]).to eq(3.141592653589793)
      end

      it "decodes DateTime64(6, 'UTC') at microsecond precision" do
        row = client.query("SELECT toDateTime64('2026-04-22 12:34:56.654321', 6, 'UTC') AS t").first
        expect(row[:t]).to be_a(Time)
        expect(row[:t].to_i).to eq(Time.utc(2026, 4, 22, 12, 34, 56).to_i)
        expect(row[:t].usec).to eq(654_321)
      end

      it "decodes Nullable(T)" do
        row = client.query("SELECT NULL::Nullable(String) AS a, toNullable('x') AS b").first
        expect(row[:a]).to be_nil
        expect(row[:b]).to eq("x")
      end

      it "decodes Array(T) recursively" do
        row = client.query("SELECT [1, 2, 3]::Array(UInt32) AS a, [['x'], ['y', 'z']]::Array(Array(String)) AS aa").first
        expect(row[:a]).to eq([1, 2, 3])
        expect(row[:aa]).to eq([["x"], ["y", "z"]])
      end

      it "decodes Map(K, V)" do
        row = client.query("SELECT map('a', 1, 'b', 2)::Map(String, Int32) AS m").first
        expect(row[:m]).to eq({"a" => 1, "b" => 2})
      end

      it "decodes Tuple(...)" do
        row = client.query("SELECT (1, 'two', 3.0)::Tuple(UInt8, String, Float64) AS t").first
        expect(row[:t]).to eq([1, "two", 3.0])
      end

      it "decodes Enum8 as a Symbol name" do
        row = client.query("SELECT CAST('blue' AS Enum8('red' = 1, 'green' = 2, 'blue' = 3)) AS c").first
        expect(row[:c]).to eq(:blue)
      end
    end

    context "advanced types not supported by clickhouse-cpp v2.6.1" do
      it "rejects Dynamic with UnsupportedTypeError" do
        expect { client.query("SELECT CAST(42 AS Dynamic) AS d") }
          .to raise_error(ClickhouseNative::UnsupportedTypeError, /Dynamic/)
      end

      it "rejects Variant with UnsupportedTypeError" do
        expect { client.query("SELECT CAST(toUInt64(42), 'Variant(UInt64, String)') AS v") }
          .to raise_error(ClickhouseNative::UnsupportedTypeError, /Variant/)
      end

      it "rejects typed JSON with UnsupportedTypeError" do
        expect { client.query("SELECT CAST('{\"a\":1}', 'JSON') AS j") }
          .to raise_error(ClickhouseNative::UnsupportedTypeError, /JSON/)
      end
    end
  end

  describe "#describe_table" do
    it "returns [{name:, type:, ...}]" do
      cols = client.describe_table("one", db_name: "system")
      expect(cols).to be_a(Array)
      expect(cols.first).to include(:name, :type)
      expect(cols.map { |c| c[:name] }).to include("dummy")
    end
  end

  describe "error mapping" do
    it "raises ServerError with code/name/message on server-side failures" do
      expect { client.query("SELECT no_such_function()") }.to raise_error do |err|
        expect(err).to be_a(ClickhouseNative::ServerError)
        expect(err.server_code).to be_a(Integer)
        expect(err.server_name).to be_a(String).and include("DB::Exception")
        expect(err.message).to include("no_such_function")
      end
    end

    it "raises ConnectionError when the host is unreachable" do
      expect { described_class.new(host: "127.0.0.1", port: 6553) }
        .to raise_error(ClickhouseNative::ConnectionError)
    end

    it "auto-resets after an error so the client stays usable" do
      expect { client.query("SELECT no_such_function()") }.to raise_error(ClickhouseNative::ServerError)
      expect(client.query_value("SELECT 1")).to eq(1)
    end
  end

  describe "#close" do
    it "releases the connection; subsequent calls raise ConnectionError" do
      client.close
      expect { client.query("SELECT 1") }.to raise_error(ClickhouseNative::ConnectionError, /closed/)
    end
  end

  describe "connection reuse" do
    it "serves many queries on one underlying socket" do
      30.times { expect(client.query_value("SELECT 1")).to eq(1) }
    end
  end
end

RSpec.describe ClickhouseNative::Pool, :clickhouse do
  subject(:pool) { described_class.new(**CH_KWARGS, pool_size: 4) }

  it "exposes the Client API" do
    expect(pool.query_value("SELECT 1")).to eq(1)
    expect(pool.ping).to be(true)
  end

  it "serves concurrent queries across its checked-out clients" do
    results = Array.new(8).map { Thread.new { pool.query_value("SELECT 1") } }.map(&:value)
    expect(results).to all(eq(1))
  end

  it "#with yields a Client handle" do
    pool.with { |c| expect(c).to be_a(ClickhouseNative::Client) }
  end
end
