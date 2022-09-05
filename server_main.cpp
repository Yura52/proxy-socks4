#include <socks4.h>

#include <glog/logging.h>

int main(int argc, char* argv[]) {
    google::InitGoogleLogging(argv[0]);

    boost::asio::io_service io_service;
    const boost::asio::ip::tcp::endpoint endpoint =
        {boost::asio::ip::address::from_string("127.0.0.1"), 12346};
    const boost::posix_time::time_duration timeout = boost::posix_time::seconds(10);
    auto proxy = std::make_shared<Socks4Proxy>(io_service, endpoint, timeout);
    proxy->startAccept();
    io_service.run();

    return 0;
}
