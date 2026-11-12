#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <cerrno>
#include "helper.h"
using namespace std;

const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
string lb_host = "127.0.0.1";
int lb_port = 5000;

struct TdbgReader
{
    int file_fd;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;
    TdbgReader reader;
    int64_t current_step;
};
bool recvAll(int sock, char *buf, size_t n)
{
    size_t received = 0;
    while (received < n)
    {
        ssize_t res = recv(
            sock,
            buf + received,
            n - received,
            0
        );
        if (res <= 0)
        {
            return false;
        }
        received += static_cast<size_t>(res);
    }
    return true;
}
