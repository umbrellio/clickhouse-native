# frozen_string_literal: true

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
    # The schema lookup is memoized per `(table, db_name)` on the client, so
    # repeated inserts into the same table don't re-run DESCRIBE. Call
    # `clear_schema_cache` after `ALTER TABLE` to invalidate.
    #
    # Hash keys not present in the schema raise ArgumentError — if you need
    # to insert a subset, pass `columns:` explicitly.
    def insert(table, rows, columns: nil, db_name: nil, types: nil)
      return 0 if rows.empty?

      fq = db_name ? "#{db_name}.#{table}" : table
      col_pairs =
        if types && columns
          zip_columns_and_types(columns, types)
        else
          columns_from_schema(table, rows, columns, db_name, fq)
        end
      row_arrays = rows.first.is_a?(Hash) ? hash_rows_to_arrays(rows, col_pairs) : rows

      insert_block(fq, col_pairs, row_arrays)
    end

    # Drop the memoized schema for a table (or all tables). Needed after
    # DDL that changes column set or types, since insert-time schema
    # lookups are cached per (table, db_name).
    def clear_schema_cache(table = nil, db_name: nil)
      return unless defined?(@schema_cache) && @schema_cache
      table.nil? ? @schema_cache.clear : @schema_cache.delete([db_name, table.to_s])
    end

    def inspect
      "#<#{self.class} #{host}:#{port}/#{database}>"
    end

    private

    def zip_columns_and_types(columns, types)
      if columns.size != types.size
        raise ArgumentError, "types and columns must have the same length"
      end
      columns.zip(types).map { |n, t| [n.to_s, t] }
    end

    def columns_from_schema(table, rows, columns, db_name, fqn)
      schema = cached_schema(table, db_name: db_name)
      type_by_name = schema.to_h { |c| [c[:name], c[:type]] }
      columns ||= rows.first.is_a?(Hash) ? rows.first.keys.map(&:to_s) : schema.map { |c| c[:name] }
      columns.map do |name|
        name_s = name.to_s
        t = type_by_name[name_s] or
          raise ArgumentError, "unknown column #{name_s.inspect} in #{fqn}"
        [name_s, t]
      end
    end

    def cached_schema(table, db_name:)
      @schema_cache ||= {}
      @schema_cache[[db_name, table.to_s]] ||=
        describe_table(table, db_name: db_name).freeze
    end

    def hash_rows_to_arrays(rows, col_pairs)
      lookup = col_pairs.map { |n, _| [n.to_sym, n] }
      rows.map { |h| lookup.map { |sym, str| h.fetch(sym) { h[str] } } }
    end
  end
end
