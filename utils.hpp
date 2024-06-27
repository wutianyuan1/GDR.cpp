#pragma once
#include <string>
#include <iostream>
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <string>
#include <system_error>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

class Socket {
public:
    Socket(const std::string& servername, int port);
    ~Socket();
    // Deleted copy constructor and assignment operator
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int descriptor() const;
private:
    int sockfd;
};


/*
 * Convert IP address from string to sockaddr
 *
 * returns: true on success or false on error
 */
bool get_addr(const std::string& dst, sockaddr& addr);

/*
 * Print program run time.
 *
 * returns: 0 on success or 1 on error
 */
void print_run_time(const std::chrono::system_clock::time_point& start, unsigned long size, int iters);
