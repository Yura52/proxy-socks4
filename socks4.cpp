#include <socks4.h>
#include <arpa/inet.h>

// ================================================================================================
std::string streabuf_to_string(boost::asio::streambuf& buffer) {
    std::istream is{&buffer};
    std::string string;
    is >> string;
    return string;
}

boost::asio::ip::address BuildIPFromBytes(const std::string& bytes) {
    return boost::asio::ip::address::from_string(
        std::to_string(uint8_t(bytes[0])) + "." +
        std::to_string(uint8_t(bytes[1])) + "." +
        std::to_string(uint8_t(bytes[2])) + "." +
        std::to_string(uint8_t(bytes[3]))
    );
}

uint16_t BuildPortFromBytes(const std::string& bytes) {
    return ntohs(*(uint16_t*)bytes.data());
}

// ================================================================================================
Socks4Proxy::Connection::ConnectionParticipant::ConnectionParticipant(
    boost::asio::io_service& io_service) : socket{io_service} {}

char* Socks4Proxy::Connection::ConnectionParticipant::data() {
    return &buffer[0];
}

char const* Socks4Proxy::Connection::ConnectionParticipant::data() const {
    return &buffer[0];
}

Socks4Proxy::Connection::Client::Client(boost::asio::io_service& io_service)
    : ConnectionParticipant{io_service} {}

Socks4Proxy::Connection::Server::Server(boost::asio::io_service& io_service)
    : ConnectionParticipant{io_service} {}

// ================================================================================================
Socks4Proxy::Socks4Proxy(boost::asio::io_service& io_service,
                         boost::asio::ip::tcp::endpoint at,
                         boost::posix_time::time_duration timeout)
    : io_service_{io_service},
      endpoint_{at},
      acceptor_{io_service_, at},
      timeout_{timeout}
      {}

void Socks4Proxy::startAccept() {
    LOG(INFO) << ">>> [launched] " << __FUNCTION__;
    CreateConnection();
    // threads_.reserve(max_thread_count_);
    // for (int32_t i = 0; i < max_thread_count_; ++i) {
    //     threads_.emplace_back([this, self = shared_from_this()]() {
    //         CreateConnection();
    //         io_service_.run();
    //     });
    // }
    LOG(INFO) << "<<< [returned] " << __FUNCTION__;
}

void Socks4Proxy::CreateConnection() {
    LOG(INFO) << ">>> [launched] " << __FUNCTION__;
    auto connection = std::make_shared<Socks4Proxy::Connection>(shared_from_this());
    connection->Run();
    LOG(INFO) << "<<< [returned] " << __FUNCTION__;
}

std::vector<char> Socks4Proxy::BuildConnectRequest(const boost::asio::ip::tcp::endpoint& addr,
                                                   const std::string& userid) {
    LOG(INFO) << ">>> [launched] " << __FUNCTION__;
    std::vector<char> request(VERSION_CODE_PORT_IP_SIZE, 0);

    request[0] = SOCKS_PROTOCOL_VERSION;
    request[1] = CONNECT_COMMAND_CODE;
    *(uint16_t*)&request[2] = htons(addr.port());
    *(uint32_t*)&request[4] = htonl(
        boost::asio::ip::address_v4::from_string(addr.address().to_string()).to_ulong()
    );
    request.insert(request.end(), userid.begin(), userid.end());
    request.push_back(0);

    LOG(INFO) << "<<< [returned] " << __FUNCTION__;
    return request;
}

// ================================================================================================
Socks4Proxy::Connection::Connection(std::shared_ptr<Socks4Proxy> proxy)
    : proxy_{proxy},
      io_service_{proxy->io_service_},
      acceptor_{proxy->acceptor_},
      timeout_{proxy->timeout_},
      client_{io_service_},
      server_{io_service_},
      proxy_global_mutex_{proxy->global_mutex_}
      {}

void Socks4Proxy::Connection::Run() {
    LOG(INFO) << ">>> [launched] " << __FUNCTION__;
    acceptor_.async_accept(client_.socket,
        [this, shared_this = shared_from_this()](const boost::system::error_code& error) {
            if (!error) {
                ReadVersionCodePortIp();
            } else {
                Finish();
            }
            proxy_->CreateConnection();
        }
    );
    LOG(INFO) << "<<< [returned] " << __FUNCTION__;
}

void Socks4Proxy::Connection::ReadVersionCodePortIp() {
    LOG(INFO) << ">>> [launched] " << __FUNCTION__;
    client_.buffer.resize(VERSION_CODE_PORT_IP_SIZE);
    boost::asio::async_read(client_.socket,
                            boost::asio::buffer(client_.data(), VERSION_CODE_PORT_IP_SIZE),
        [this, shared_this = shared_from_this()](const boost::system::error_code& error,
                                                 size_t byte_count) {
            if (error) {
                Finish();
                return;
            }
            const bool parsing_is_successful = ParseVersionCodePortIp();
            if (!parsing_is_successful) {
                Finish();
                return;
            }
            ReadUserId();
        }
    );
    LOG(INFO) << "<<< [returned] " << __FUNCTION__;
}

bool Socks4Proxy::Connection::ParseVersionCodePortIp() {
    LOG(INFO) << ">>> [launched] " << __FUNCTION__;
    if (client_.buffer[0] != SOCKS_PROTOCOL_VERSION) {
        LOG(INFO) << "<<< [returned] " << __FUNCTION__;
        return false;
    }
    if (client_.buffer[1] != CONNECT_COMMAND_CODE) {
        LOG(INFO) << "<<< [returned] " << __FUNCTION__;
        return false;
    }
    server_.endpoint = {BuildIPFromBytes(client_.buffer.substr(4, 4)),
                        BuildPortFromBytes(client_.buffer.substr(2, 2))};
    LOG(INFO) << "<<< [returned] " << __FUNCTION__;
    return true;
}

void Socks4Proxy::Connection::ReadUserId() {
    LOG(INFO) << ">>> [launched] " << __FUNCTION__;
    auto buffer = std::make_shared<boost::asio::streambuf>(MAX_USERID_SIZE);
    boost::asio::async_read_until(client_.socket, *buffer, 0,
        [this, shared_this = shared_from_this(), buffer](
            const boost::system::error_code& error, size_t byte_count) {
            if (error) {
                Finish();
                return;
            }
            client_.userid = streabuf_to_string(*buffer).substr(0, byte_count - 1);
            EstablishConnectionWithServer();
        }
    );
    LOG(INFO) << "<<< [returned] " << __FUNCTION__;
}

void Socks4Proxy::Connection::EstablishConnectionWithServer() {
    LOG(INFO) << ">>> [launched] " << __FUNCTION__;
    LOG(INFO) << "[ESTABLISH] " << server_.endpoint.address() << " " << server_.endpoint.port();
    server_.socket.async_connect(server_.endpoint,
        [this, shared_this = shared_from_this()](const boost::system::error_code& error) {
            if (error) {
                LOG(INFO) << "REQUEST REJECTED OR FAILED";
                SendConfirmationToClient(REQUEST_REJECTED_OR_FAILED);
            } else {
                LOG(INFO) << "REQUEST GRANTED";
                SendConfirmationToClient(REQUEST_GRANTED);
            }
        }
    );
    LOG(INFO) << "<<< [returned] " << __FUNCTION__;
}

void Socks4Proxy::Connection::SendConfirmationToClient(int8_t result_code) {
    LOG(INFO) << ">>> [launched] " << __FUNCTION__;
    // the current size of the buffer is just fine after ReadVersionCodePortIp
    client_.buffer[0] = REPLY_CODE_VERSION;
    client_.buffer[1] = result_code;
    // if (result_code != REQUEST_GRANTED) {
    //     return;
    // }
    boost::asio::async_write(client_.socket,
                             boost::asio::buffer(client_.data(), VERSION_CODE_PORT_IP_SIZE),
        [this, shared_this = shared_from_this(), result_code](
            const boost::system::error_code& error, size_t byte_count) {
            if (error) {
                Finish();
                return;
            }
            if (result_code != REQUEST_GRANTED) {
                Finish();
                return;
            }
            MainRoutine();
        }
    );
    LOG(INFO) << "<<< [returned] " << __FUNCTION__;
}

void Socks4Proxy::Connection::MainRoutine() {
    LOG(INFO) << ">>> [launched] " << __FUNCTION__;
    client_.buffer.resize(PARTICIPANT_BUFFER_SIZE_);
    server_.buffer.resize(PARTICIPANT_BUFFER_SIZE_);
    RecieveData(client_, server_);
    RecieveData(server_, client_);
    LOG(INFO) << "<<< [returned] " << __FUNCTION__;
}

void Socks4Proxy::Connection::RecieveData(ConnectionParticipant& from,
                                          ConnectionParticipant& to) {
    LOG(INFO) << ">>> [launched] " << __FUNCTION__
              << " from " << from.socket.remote_endpoint().port()
              << " to "   <<   to.socket.remote_endpoint().port() << std::endl;
    from.socket.async_read_some(boost::asio::buffer(from.data(), PARTICIPANT_BUFFER_SIZE_),
        [this, shared_this = shared_from_this(), &from, &to](
            const boost::system::error_code& error, size_t byte_count) {
            if (error) {
                Finish();
                return;
            }
            SendData(from, to, byte_count);
        }
    );
}

void Socks4Proxy::Connection::SendData(ConnectionParticipant& from,
                                       ConnectionParticipant& to,
                                       size_t byte_count) {
    LOG(INFO) << ">>> [launched] " << __FUNCTION__
              << " from " << from.socket.remote_endpoint().port()
              << " to "   <<   to.socket.remote_endpoint().port() << std::endl;
    boost::asio::async_write(to.socket, boost::asio::buffer(from.data(), byte_count),
        [this, shared_this = shared_from_this(), &from, &to](
            const boost::system::error_code& error, size_t byte_count) {
            if (error) {
                Finish();
                return;
            }
            RecieveData(from, to);
        }
    );
}

void Socks4Proxy::Connection::Finish() {
    LOG(INFO) << ">>> [launched] " << __FUNCTION__;
    std::lock_guard<std::mutex> guard(global_mutex_);
    if (finished_) {
        return;
    }
    finished_ = true;
    try {
        client_.socket.close();
    } catch (...) {

    }
    try {
        server_.socket.close();
    } catch (...) {
        
    }
    LOG(INFO) << "<<< [returned] " << __FUNCTION__;
}

// ================================================================================================
void Socks4Handshake(boost::asio::ip::tcp::socket& conn,
                     const boost::asio::ip::tcp::endpoint& addr,
                     const std::string& user) {
    LOG(INFO) << ">>> [launched] " << __FUNCTION__;
    std::vector<char> buffer = Socks4Proxy::BuildConnectRequest(addr, user);
    boost::asio::write(conn, boost::asio::buffer(buffer));
    boost::asio::read(conn, boost::asio::buffer(buffer.data(), Socks4Proxy::VERSION_CODE_PORT_IP_SIZE));
    LOG(INFO) << "<<< [returned] " << __FUNCTION__;
    if (buffer[0] != Socks4Proxy::REPLY_CODE_VERSION) {
        return;
    }
    if (buffer[1] != Socks4Proxy::REQUEST_GRANTED) {
        return;
    }
}
                     
void ConnectProxyChain(boost::asio::ip::tcp::socket& socket,
                       const std::vector<ProxyParams>& proxy_chain,
                       boost::asio::ip::tcp::endpoint destination) {
    LOG(INFO) << ">>> [launched] " << __FUNCTION__;
    const int32_t chain_size = proxy_chain.size();
    if (chain_size == 0) {
        socket.connect(destination);
        return;
    }
    socket.connect(proxy_chain.front().endpoint);
    for (int32_t i = 1; i < chain_size; ++i) {
        Socks4Handshake(socket, proxy_chain[i].endpoint, proxy_chain[i - 1].user);
    }
    Socks4Handshake(socket, destination, proxy_chain.back().user);
    LOG(INFO) << "<<< [returned] " << __FUNCTION__;
}
