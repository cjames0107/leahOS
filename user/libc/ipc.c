#include <ipc.h>
#include <sys/syscall.h>

int port_create(unsigned name)
{
    return (int)__syscall(SYS_portcreate, (long)name, 0, 0, 0, 0);
}

int port_open(unsigned name)
{
    return (int)__syscall(SYS_portopen, (long)name, 0, 0, 0, 0);
}

int port_destroy(int port)
{
    return (int)__syscall(SYS_portdestroy, port, 0, 0, 0, 0);
}

int ipc_call(int port, const struct ipc_message* request,
             struct ipc_message* reply)
{
    return (int)__syscall(SYS_ipccall, port, (long)request, (long)reply, 0, 0);
}

int ipc_recv(int port, struct ipc_message* out, unsigned* caller_pid)
{
    return (int)__syscall(SYS_ipcrecv, port, (long)out, (long)caller_pid, 0, 0);
}

int ipc_reply(int handle, const struct ipc_message* msg)
{
    return (int)__syscall(SYS_ipcreply, handle, (long)msg, 0, 0, 0);
}
