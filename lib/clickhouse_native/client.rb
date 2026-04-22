module ClickhouseNative
  class Client
    attr_reader :host, :port, :database

    def describe_table(table, db_name: nil)
      fq = db_name ? "#{db_name}.#{table}" : table
      query("DESCRIBE TABLE #{fq}")
    end

    # insert(table, rows, columns: nil, db_name: nil, types: nil)
    #
    # rows may be Array<Hash{Symbol|String => Object}> or Array<Array>.
    # columns defaults to the first hash's keys (for Array<Hash>) or all table
    # columns in DDL order (for Array<Array>).
    # types may be supplied to skip the DESCRIBE lookup.
    #
    # Hash keys not present in the schema raise ArgumentError — if you need
    # to insert a subset, pass `columns:` explicitly.
    def insert(table, rows, columns: nil, db_name: nil, types: nil)
      return 0 if rows.empty?
      fq = db_name ? "#{db_name}.#{table}" : table

      if types && columns
        raise ArgumentError, "types and columns must have the same length" if columns.size != types.size
        col_pairs = columns.zip(types).map { |n, t| [n.to_s, t] }
      else
        schema = describe_table(table, db_name: db_name)
        type_by_name = schema.to_h { |c| [c[:name], c[:type]] }
        columns ||= rows.first.is_a?(Hash) ? rows.first.keys.map(&:to_s) : schema.map { |c| c[:name] }
        col_pairs = columns.map do |name|
          name_s = name.to_s
          t = type_by_name[name_s] or raise ArgumentError, "unknown column #{name_s.inspect} in #{fq}"
          [name_s, t]
        end
      end

      row_arrays =
        if rows.first.is_a?(Hash)
          col_pairs.map { |n, _| [n.to_sym, n] }.then do |lookup|
            rows.map { |h| lookup.map { |sym, str| h.fetch(sym) { h[str] } } }
          end
        else
          rows
        end

      insert_block(fq, col_pairs, row_arrays)
    end

    def inspect
      "#<#{self.class} #{host}:#{port}/#{database}>"
    end
  end
end
