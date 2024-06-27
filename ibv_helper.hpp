#include <string>
#include <fstream>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <system_error>

#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>


static std::string ibv_read_sysfs_file(const std::string& dir, const std::string& file) {
    std::string path = dir + "/" + file;
    std::ifstream file_stream(path.c_str(), std::ios::in | std::ios::binary);
    std::ostringstream file_content_stream;

    if (!file_stream) {
        throw std::runtime_error("Failed to open " + path);
    }

    file_content_stream << file_stream.rdbuf();
    file_stream.close();

    std::string file_content = file_content_stream.str();
    // Remove a possible newline character at the end of the file content
    if (!file_content.empty() && file_content.back() == '\n') {
        file_content.pop_back();
    }
    
    return file_content;
}


/* GID types as appear in sysfs, no change is expected as of ABI
 * compatibility.
 */
static int ibv_query_gid_type(struct ibv_context* context, uint8_t port_num, unsigned int index, ibv_gid_type* type) {
    if (!context || !context->device || !context->device->ibdev_path || !type) {
        throw std::invalid_argument("Invalid argument provided to ibv_query_gid_type");
    }

    std::string name = "ports/" + std::to_string(port_num) + "/gid_attrs/types/" + std::to_string(index);

    try {
        std::string buff = ibv_read_sysfs_file(context->device->ibdev_path, name);

        if (buff == "IB/RoCE v1") {
            *type = IBV_GID_TYPE_ROCE_V1;
        } else if (buff == "RoCE v2") {
            *type = IBV_GID_TYPE_ROCE_V2;
        } else {
            throw std::runtime_error("Unsupported GID type found in sysfs");
        }
    } catch (const std::runtime_error& e) {
        // If file doesn't exist (e.g., the kernel behaves differently for IB), assume v1
        if (e.what() == std::string("Failed to open /sys/class/infiniband/<device>/ports/x/gid_attrs/types/y")) {
            *type = IBV_GID_TYPE_ROCE_V1; // default to V1 if file doesn't exist
            return 0;
        }

        // Check if 'gid_attrs' directory exists - if not, we assume v1
        std::string dir_path = std::string(context->device->ibdev_path) + "/ports/" + std::to_string(port_num) + "/gid_attrs";
        if (!std::filesystem::exists(dir_path)) {
            *type = IBV_GID_TYPE_ROCE_V1; // default to V1 if directory doesn't exist
            return 0;
        }

        throw; // re-throw the catched exception if it's not a handling case
    }

    return 0;
}
int ibv_find_sgid_type(struct ibv_context *context, uint8_t port_num,
		enum ibv_gid_type gid_type, int gid_family)
{
        enum ibv_gid_type sgid_type = (enum ibv_gid_type)0;
        union ibv_gid sgid;
        int sgid_family = -1;
        int idx = 0;

        do {
                if (ibv_query_gid(context, port_num, idx, &sgid)) {
                        errno = EFAULT;
                        return -1;
                }
                if (ibv_query_gid_type(context, port_num, idx, &sgid_type)) {
                        errno = EFAULT;
                        return -1;
                }
                if (sgid.raw[0] == 0 && sgid.raw[1] == 0) {
                        sgid_family = AF_INET;
                }

                if (gid_type == sgid_type && gid_family == sgid_family) {
                        return idx;
                }

                idx++;
        } while (gid_type != sgid_type || gid_family != sgid_family);

        return idx;
}


