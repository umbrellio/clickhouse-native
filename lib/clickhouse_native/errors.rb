module ClickhouseNative
  class Error < StandardError; end

  class ConnectionError < Error; end
  class TimeoutError < ConnectionError; end

  class ProtocolError < Error; end

  class ServerError < Error
    attr_reader :server_code, :server_name, :server_stacktrace

    def initialize(message, code: nil, name: nil, stacktrace: nil)
      super(message)
      @server_code = code
      @server_name = name
      @server_stacktrace = stacktrace
    end
  end

  class EncoderError < Error; end
  class DecoderError < Error; end
  class UnsupportedTypeError < DecoderError; end
end
