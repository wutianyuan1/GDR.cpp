#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <memory>
#include <cstdio>
#include <stdio.h>
#include <unistd.h>
#include "utils.hpp"


bool get_addr(const std::string& dst, sockaddr& addr) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC; // To allow for both IPv4 and IPv6

    struct addrinfo* res;
    int ret = getaddrinfo(dst.c_str(), nullptr, &hints, &res);
    if (ret) {
        std::cerr << "getaddrinfo failed (" << gai_strerror(ret) << ") - invalid hostname or IP address\n";
        throw std::system_error(ret, std::system_category(), "getaddrinfo failed");
    }

    // Assuming that sockaddr_storage is large enough to hold both sockaddr_in and sockaddr_in6
    if (res->ai_family == AF_INET)
        std::memcpy(&addr, res->ai_addr, sizeof(sockaddr_in));
    else if (res->ai_family == AF_INET6)
        std::memcpy(&addr, res->ai_addr, sizeof(sockaddr_in6));
    else
        throw std::runtime_error("Unsupported address family");

    freeaddrinfo(res);
    return true;
}

void print_run_time(const std::chrono::system_clock::time_point& start, unsigned long size, int iters) {
    auto end = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsed_seconds = end - start;
    double usec = std::chrono::duration_cast<std::chrono::microseconds>(elapsed_seconds).count();
    long long bytes = static_cast<long long>(size) * iters;

    std::cout << bytes << " bytes in " << elapsed_seconds.count() << " seconds = " 
              << (bytes * 8. / usec) << " Mbit/sec\n";
    std::cout << iters << " iters in " << elapsed_seconds.count() << " seconds = " 
              << (usec / iters) << " usec/iter\n";
}

Socket::Socket(const std::string& servername, int port) : sockfd(-1) {
    struct addrinfo hints {
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM
    };
    struct addrinfo* res;
    std::string service = std::to_string(port);

    int ret_val = getaddrinfo(servername.c_str(), service.c_str(), &hints, &res);
    if (ret_val != 0) {
        std::cerr << "FAILURE: " << gai_strerror(ret_val) << " for " << servername << ":" << port << std::endl;
        throw std::runtime_error("getaddrinfo failed");
    }

    auto addrinfo_deleter = [](struct addrinfo* ai) { freeaddrinfo(ai); };
    std::unique_ptr<struct addrinfo, decltype(addrinfo_deleter)> res_ptr(res, addrinfo_deleter);

    for (auto t = res_ptr.get(); t != nullptr; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            if (connect(sockfd, t->ai_addr, t->ai_addrlen) == 0) {
                return; // Connection successful
            }
            close(sockfd);
            sockfd = -1;
        }
    }

    std::cerr << "FAILURE: Couldn't connect to " << servername << ":" << port << std::endl;
    throw std::runtime_error("Could not open client socket");
}

Socket::~Socket() {
    if (sockfd >= 0) {
        close(sockfd);
    }
}

int Socket::descriptor() const {
    return sockfd;
}
