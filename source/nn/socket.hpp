#pragma once

#include <nn/nn_common.hpp>
#include <poll.h>
#include <sys/socket.h>

namespace nn::socket {

    struct InAddr {
        u32 addr;
    };

    Result Initialize(void* pool, ulong poolSize, ulong allocPoolSize, int concurLimit);
    Result Finalize();

    s32 SetSockOpt(s32 socket, s32 socketLevel, s32 option, void const*, u32 len);
    ssize_t Send(s32 socket, void const* buffer, u64 bufferLength, s32 flags);
    ssize_t Recv(s32 desc, void *buffer, size_t bufferLength, s32 flags);
    s32 Close(s32 desc);

    s32 Socket(s32 domain, s32 type, s32 proto);
    u16 InetHtons(u16);
    u32 InetAton(const char* str, InAddr*);
    u32 Connect(s32 socket, const sockaddr* addr, u32 addrLen);
    s32 Bind(s32 socket, const sockaddr* addr, u32 addrLen);
    s32 Listen(s32 socket, s32 backlog);
    s32 Accept(s32 socket, sockaddr* addrOut, u32* addrLenOut);
    s32 Shutdown(s32 desc, s32 how);

    // Bridge networking additions — Poll arrived via the original
    // Ryujinx-fix patch (we kept it for parity); SendTo / RecvFrom /
    // GetLastErrno are needed by discovery.cpp.
    s32 Poll(pollfd* fds, ulong nfds, s32 timeoutMs);
    ssize_t SendTo(s32 socket, const void* buffer, u64 bufferLength,
                   s32 flags, const sockaddr* addr, u32 addrLen);
    ssize_t RecvFrom(s32 desc, void* buffer, size_t bufferLength,
                     s32 flags, sockaddr* addrOut, u32* addrLenOut);
    s32 GetLastErrno();
};
