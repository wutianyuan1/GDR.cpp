#include <string>
#include <iostream>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>
#include "utils.hpp"
#include "gpu_direct_rdma_access.h"

enum class payload_t { RDMA_BUF_DESC, TASK_ATTRS };

struct payload_attr {
    payload_t data_t;
    std::string payload_str;
};

struct client_params {
    uint32_t  		    task;
    int             	port;
    unsigned long   	size;
    int             	iters;
    int             	use_cuda;
    std::string     	bdf;
    std::string     	servername;
    sockaddr        	hostaddr;
};

class RDMAClient {
public:
    RDMAClient(std::string servername, int serverport);
    ~RDMAClient();
    template <class T>
    void RDMAClient::register_rdma_buff(T* buff, size_t num_elems);

private:
    Socket* socket_;
    rdma_device* rdma_dev_;
    std::vector<uint8_t*> buffs_;
    std::vector<rdma_buffer*> rdma_buffs_;
};