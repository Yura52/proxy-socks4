#include <glog/logging.h>

#include <iostream>
#include <sstream>
#include <memory>

#include <cstdint>
#include <vector>
#include <set>
#include <string>
#include <sstream>

#include <thread>
#include <mutex>

#define BOOST_ASIO_ENABLE_HANDLER_TRACKING
#include <boost/asio.hpp>

void Socks4Handshake(boost::asio::ip::tcp::socket& conn,
                     const boost::asio::ip::tcp::endpoint& addr,
                     const std::string& user);

struct ProxyParams {
    boost::asio::ip::tcp::endpoint endpoint;
    std::string user;
};
                     
void ConnectProxyChain(boost::asio::ip::tcp::socket& socket,
                       const std::vector<ProxyParams>& proxy_chain,
                       boost::asio::ip::tcp::endpoint destination);

// ================================================================================================
class A : public std::enable_shared_from_this<A> {

};

class Socks4Proxy : public std::enable_shared_from_this<Socks4Proxy> {
public:
    static const int8_t SOCKS_PROTOCOL_VERSION = 4;
    static const int8_t CONNECT_COMMAND_CODE = 1;
    static const int32_t MAX_USERID_SIZE = 1024;
    static const int32_t VERSION_CODE_PORT_IP_SIZE = 1 + 1 + 2 + 4;
    static const int8_t REPLY_CODE_VERSION = 0;
    enum ResultCodes {
        REQUEST_GRANTED = 90,
        REQUEST_REJECTED_OR_FAILED = 91,
        IDENTD_IS_NOT_REACHABLE = 92,
        CLIENT_AND_IDENTD_REPORTED_DIFFERENT_USERIDS = 93
    };

    Socks4Proxy(boost::asio::io_service& io_service,
                boost::asio::ip::tcp::endpoint at,
                boost::posix_time::time_duration timeout);

    void startAccept();
    static std::vector<char> BuildConnectRequest(const boost::asio::ip::tcp::endpoint& addr,
                                                 const std::string& user);

private:
    class Connection;

    void CreateConnection();

    // === network ===
    boost::asio::io_service& io_service_;
    const boost::asio::ip::tcp::endpoint endpoint_;
    boost::asio::ip::tcp::acceptor acceptor_;
    const boost::posix_time::time_duration timeout_;

    // === threads ===
    const int32_t max_thread_count_ = 4;
    std::vector<std::thread> threads_;

    std::mutex global_mutex_;
};

class Socks4Proxy::Connection : public std::enable_shared_from_this<Socks4Proxy::Connection> {
public:
    explicit Connection(std::shared_ptr<Socks4Proxy> proxy);
    void Run();

private:
    struct ConnectionParticipant {
        explicit ConnectionParticipant(boost::asio::io_service& io_service);
        char* data();
        char const* data() const;

        boost::asio::ip::tcp::socket socket;
        std::string buffer;
    };

    struct Client : public ConnectionParticipant {
        explicit Client(boost::asio::io_service& io_service);

        std::string userid;
    };

    struct Server : public ConnectionParticipant {
        explicit Server(boost::asio::io_service& io_service);

        boost::asio::ip::tcp::endpoint endpoint;
    };

    static const int32_t PARTICIPANT_BUFFER_SIZE_ = 1024 * 1024;

    void ReadVersionCodePortIp();
    bool ParseVersionCodePortIp();
    void ReadUserId();
    void EstablishConnectionWithServer();
    void SendConfirmationToClient(int8_t result_code);
    void MainRoutine();
    void RecieveData(ConnectionParticipant& from, ConnectionParticipant& to);
    void SendData(ConnectionParticipant& from, ConnectionParticipant& to, size_t byte_count);
    void Finish();

    std::shared_ptr<Socks4Proxy> proxy_;
    boost::asio::io_service& io_service_;
    boost::asio::ip::tcp::acceptor& acceptor_;
    const boost::posix_time::time_duration timeout_;
    // std::set<std::shared_ptr<Connection>> connections_;

    Client client_;
    Server server_;
    std::atomic<bool> finished_{false};

    std::mutex& proxy_global_mutex_;
    std::mutex global_mutex_;
};
