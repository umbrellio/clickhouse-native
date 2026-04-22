module ClickhouseNative
  class Config
    attr_accessor :host, :port, :database, :user, :password,
                  :pool_size, :pool_timeout

    def initialize
      @host = "localhost"
      @port = 9000
      @database = "default"
      @user = "default"
      @password = ""
      @pool_size = 5
      @pool_timeout = 5
    end

    def client_kwargs
      {
        host: @host, port: @port, database: @database,
        user: @user, password: @password,
      }
    end

    def pool_kwargs
      client_kwargs.merge(pool_size: @pool_size, pool_timeout: @pool_timeout)
    end
  end
end
