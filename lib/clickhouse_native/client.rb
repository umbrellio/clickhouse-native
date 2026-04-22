module ClickhouseNative
  class Client
    attr_reader :host, :port, :database

    def describe_table(table, db_name: nil)
      fq = db_name ? "#{db_name}.#{table}" : table
      query("DESCRIBE TABLE #{fq}")
    end

    def inspect
      "#<#{self.class} #{host}:#{port}/#{database}>"
    end
  end
end
