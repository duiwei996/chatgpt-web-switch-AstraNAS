// SPDX-License-Identifier: GPL-3.0-or-later
#include "smb_client.hpp"
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <algorithm>
#include <climits>
#include <sys/socket.h>

void SmbRemoteClient::set_benchmark_socket_recv_buffer(std::uint32_t bytes) {
    benchmark_recv_buffer_ = bytes;
    if (!ctx_ || bytes == 0) return;
    const int fd = smb2_get_fd(ctx_);
    if (fd < 0) return;
    const int requested = static_cast<int>(
        std::min<std::uint32_t>(bytes, static_cast<std::uint32_t>(INT_MAX)));
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &requested, sizeof(requested));
}
