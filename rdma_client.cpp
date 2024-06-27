#include "rdma_client.hpp"
#include <sstream>
#include <stdexcept>
static constexpr size_t RDMA_TASK_ATTR_DESC_STRING_LENGTH = sizeof("12345678");


static std::string rdma_task_attr_flags_get_desc_str(uint32_t flags) {
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

RDMAClient::RDMAClient(std::string servername, int serverport): rdma_dev_(nullptr) {
    std::srand(static_cast<unsigned int>(std::time(nullptr)) ^ getpid());

    std::cout << "Connecting to remote server \"" << servername << ":" << serverport << "\"\n";
    socket_ = new Socket(servername, serverport);

    std::cout << "Opening RDMA device\n";
    sockaddr hostaddr;
    get_addr(servername, hostaddr);
    rdma_dev_ = rdma_open_device_client(&hostaddr);
    if (!rdma_dev_) {
        throw std::runtime_error("Failed to open RDMA device.");
    }
}

RDMAClient::~RDMAClient() {
    if (rdma_buff_) {
        rdma_buffer_dereg(rdma_buff_);
    }
    if (buff_) {
        delete[] buff_;
    }
    if (rdma_dev_) {
        rdma_close_device(rdma_dev_);
    }
}


template <class T>
void RDMAClient::register_rdma_buff(T* buff, size_t num_elems)
{
    buff_ = new uint8_t[params_.size];
    if (!buff_) {
        throw std::runtime_error("Failed to allocate buffer.");
    }
    rdma_buff_ = rdma_buffer_reg(rdma_dev_, buff_, params_.size);
    if (!rdma_buff_) {
        throw std::runtime_error("Failed to register RDMA buffer.");
    }
}

void RDMAClient::run() {
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

        auto start = std::chrono::high_resolution_clock::now();
        for (int cnt = 0; cnt < params_.iters; ++cnt) {
            char ackmsg[sizeof ACK_MSG];
            int  ret_size;
            
            // Sending RDMA data (address and rkey) by socket as a triger to start RDMA read/write operation
            ret_size = write(socket_->descriptor(), desc_package.data(), buff_package_size);
            if (ret_size != buff_package_size) {
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
            if (!params_.use_cuda) {
                DEBUG_LOG_FAST_PATH << "Written data \"" << buff_ << "\"\n";
        }
        }
       print_run_time(start, params_.size, params_.iters);
    }
