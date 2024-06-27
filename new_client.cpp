#include <iostream>
#include <memory>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>
#include <sstream>
#include <chrono>
#include <ctime>
#include <getopt.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "utils.hpp"
#include "gpu_direct_rdma_access.h"

extern int debug;
extern int debug_fast_path;

#define DEBUG_LOG if (debug) std::cout
#define DEBUG_LOG_FAST_PATH if (debug_fast_path) std::cout
#define FDEBUG_LOG if (debug) std::cerr
#define FDEBUG_LOG_FAST_PATH if (debug_fast_path) std::cerr
#define ACK_MSG "rdma_task completed"

struct user_params {
    uint32_t  		    task;
    int             	port;
    unsigned long   	size;
    int             	iters;
    int             	use_cuda;
    std::string     	bdf;
    std::string     	servername;
    sockaddr        	hostaddr;
};

enum class payload_t { RDMA_BUF_DESC, TASK_ATTRS };

struct payload_attr {
    payload_t data_t;
    std::string payload_str;
};

int pack_payload_data(std::vector<uint8_t>& package, const payload_attr& attr) {
    uint8_t data_t = static_cast<uint8_t>(attr.data_t);
    uint16_t payload_size = attr.payload_str.length() + 1;

    size_t pos = 0;
    package.resize(sizeof(data_t) + sizeof(payload_size) + payload_size);

    std::memcpy(package.data() + pos, &data_t, sizeof(data_t));
    pos += sizeof(data_t);

    std::memcpy(package.data() + pos, &payload_size, sizeof(payload_size));
    pos += sizeof(payload_size);

    std::memcpy(package.data() + pos, attr.payload_str.c_str(), payload_size);

    return package.size();
}

constexpr size_t RDMA_TASK_ATTR_DESC_STRING_LENGTH = sizeof("12345678");

std::string rdma_task_attr_flags_get_desc_str(uint32_t flags) {
    if (RDMA_TASK_ATTR_DESC_STRING_LENGTH < (sizeof(uint32_t) * 2 + 1)) {
        std::ostringstream err;
        err << "desc string size (" << RDMA_TASK_ATTR_DESC_STRING_LENGTH 
            << ") is less than required (" << (sizeof(uint32_t) * 2 + 1) 
            << ") for sending rdma_task_attr_flags data";
        throw std::runtime_error(err.str());
    }
    
    char buffer[RDMA_TASK_ATTR_DESC_STRING_LENGTH];
    std::snprintf(buffer, sizeof(buffer), "%08x", flags);
    return std::string(buffer);
}

void usage(const std::string& argv0) {
    std::cout << "Usage:\n" << "  " << argv0 
              << " <host>     connect to server at <host>\n\nOptions:\n"
              << "  -t, --task_flags=<flags>  rdma task attrs bitmask: bit 0 - rdma operation type: 0 - \"WRITE\"(default), 1 - \"READ\"\n"
              << "  -a, --addr=<ipaddr>       ip address of the local host net device <ipaddr v4> (mandatory)\n"
              << "  -p, --port=<port>         listen on/connect to port <port> (default 18515)\n"
              << "  -s, --size=<size>         size of message to exchange (default 4096)\n"
              << "  -n, --iters=<iters>       number of exchanges (default 1000)\n"
              << "  -u, --use-cuda=<BDF>      use CUDA package (work with GPU memory),\n"
              << "                            BDF corresponding to CUDA device, for example, \"3e:02.0\"\n"
              << "  -D, --debug-mask=<mask>   debug bitmask: bit 0 - debug print enable,\n"
              << "                                           bit 1 - fast path debug print enable\n";
}

int parse_command_line(int argc, char* argv[], user_params& params) {
    params = {}; // Reset to default state
    // Set defaults
    params.port = 18515;
    params.size = 4096;
    params.iters = 1000;
    params.task = 0;

    struct option long_options[] = {
        { "task-flags", required_argument, nullptr, 't' },
        { "addr", required_argument, nullptr, 'a' },
        { "port", required_argument, nullptr, 'p' },
        { "size", required_argument, nullptr, 's' },
        { "iters", required_argument, nullptr, 'n' },
        { "use-cuda", required_argument, nullptr, 'u' },
        { "debug-mask", required_argument, nullptr, 'D' },
        { nullptr, 0, nullptr, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "t:a:p:s:n:u:D:", long_options, nullptr)) != -1) {
        switch (c) {
            case 't':
                params.task = static_cast<uint32_t>(std::strtol(optarg, nullptr, 0)) & 1u; // bit 0
                break;
            case 'a':
                get_addr(optarg, params.hostaddr);
                break;
            case 'p':
                params.port = static_cast<int>(std::strtol(optarg, nullptr, 0));
                if (params.port < 0 || params.port > 65535) {
                    usage(argv[0]);
                    return 1;
                }
                break;
            case 's':
                params.size = static_cast<unsigned long>(std::strtol(optarg, nullptr, 0));
                break;
            case 'n':
                params.iters = static_cast<int>(std::strtol(optarg, nullptr, 0));
                break;
            case 'u':
                params.use_cuda = 1;
                params.bdf = optarg;
                break;
            case 'D':
                debug = static_cast<int>(std::strtol(optarg, nullptr, 0)) & 1; // bit 0
                debug_fast_path = static_cast<int>(std::strtol(optarg, nullptr, 0)) >> 1 & 1; // bit 1
                break;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (optind < argc) {
        params.servername = argv[optind];
    } else {
        std::cerr << "FAILURE: Server name is missing in the command line.\n";
        usage(argv[0]);
        return 1;
    }

    return 0;
}

// Re-implementation of user_params, open_client_socket, unpack_payload_data and other auxiliary functions goes here.

class RDMAClient {
public:
    RDMAClient(const user_params& params)
        : params_(params), rdma_dev_(nullptr), rdma_buff_(nullptr) {
        std::srand(static_cast<unsigned int>(std::time(nullptr)) ^ getpid());

        std::cout << "Connecting to remote server \"" << params_.servername << ":" << params_.port << "\"\n";
        socket_ = new Socket(params_.servername, params_.port);

        std::cout << "Opening RDMA device\n";
        rdma_dev_ = rdma_open_device_client(&params_.hostaddr);
        if (!rdma_dev_) {
            throw std::runtime_error("Failed to open RDMA device.");
        }
    }

    ~RDMAClient() {
        if (rdma_buff_) {
            rdma_buffer_dereg(rdma_buff_);
        }
        if (rdma_dev_) {
            rdma_close_device(rdma_dev_);
        }
    }

    template <typename DType>
    int register_data(DType* data_ptr, size_t num_elements) {
        rdma_buff_ = rdma_buffer_reg(rdma_dev_, data_ptr, num_elements * sizeof(DType));
        if (!rdma_buff_) {
            throw std::runtime_error("Failed to register RDMA buffer.");
        }
        char desc_str[256], task_opt_str[16];

        int ret_desc_str_size = rdma_buffer_get_desc_str(rdma_buff_, desc_str, sizeof(desc_str));
        std::string ret_task_opt_str = rdma_task_attr_flags_get_desc_str(params_.task);
        int ret_task_opt_str_size = ret_task_opt_str.length() + 1;
     
        if (!ret_desc_str_size || !ret_task_opt_str_size) {
            throw std::runtime_error("Failed to get rdma_buffer_desc_str or rdma_task_attr_flags_desc_str");
        }

        /* Package memory allocation */
        std::vector<uint8_t> desc_package, task_package;

        /* Packing RDMA buff desc str */
        struct payload_attr pl_attr = { .data_t = payload_t::RDMA_BUF_DESC, .payload_str = desc_str };
        int buff_package_size = pack_payload_data(desc_package, pl_attr);
        if (!buff_package_size) {
            throw std::runtime_error("Failed to init data package");
        }
        
        /* Packing RDMA task attrs desc str */
        pl_attr.data_t = payload_t::TASK_ATTRS;
        pl_attr.payload_str = task_opt_str;
        buff_package_size += pack_payload_data(task_package, pl_attr);
        if (!buff_package_size) {
            throw std::runtime_error("Failed to init task package\n");
        }

        desc_package.insert(desc_package.end(), task_package.begin(), task_package.end());
        write(socket_->descriptor(), desc_package.data(), buff_package_size);
        return int(data_ptr);
    }

    void run(){
        auto start = std::chrono::high_resolution_clock::now();
        for (int cnt = 0; cnt < params_.iters; ++cnt) {
            char ackmsg[sizeof ACK_MSG];
            int  ret_size;
            int One = 1, Two = 2;
            
            // Sending RDMA data (address and rkey) by socket as a triger to start RDMA read/write operation
            ret_size = write(socket_->descriptor(), cnt % 2 == 0 ? &One : &Two, sizeof(int));
            if (ret_size != sizeof(int)) {
                fprintf(stderr, "FAILURE: Couldn't send RDMA data for iteration, write data size %d (errno=%d '%m')\n", ret_size, errno);
                throw std::runtime_error("ret_size != buff_package_size");
            }
            
            // Wating for confirmation message from the socket that rdma_read/write from the server has beed completed
            ret_size = recv(socket_->descriptor(), ackmsg, sizeof ackmsg, MSG_WAITALL);
            if (ret_size != sizeof ackmsg) {
                fprintf(stderr, "FAILURE: Couldn't read \"%s\" message, recv data size %d (errno=%d '%m')\n", ACK_MSG, ret_size, errno);
            }

            // Printing received data for debug purpose
            DEBUG_LOG_FAST_PATH << "Received ack N " << cnt << ": \"" << ackmsg << "\"\n";
        }
       print_run_time(start, params_.size, params_.iters);
    }

private:
    Socket* socket_;
    user_params params_;
    rdma_device* rdma_dev_;
    rdma_buffer* rdma_buff_;
    std::vector<int> task_ids;
};

int main(int argc, char *argv[]) {
    try {
        user_params params;
        int ret_val = parse_command_line(argc, argv, params);
        if (ret_val) {
            return 1;
        }

        char* data = new char[params.size];
        char* data2 = new char[params.size];
        
        RDMAClient client(params);
        client.register_data(data, params.size);
        client.register_data(data2, params.size);
        client.run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}