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
#include <sys/stat.h>
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
bool sendAll(int sock, const char *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n)
    {
        ssize_t res = send(
            sock,
            buf + sent,
            n - sent,
            MSG_NOSIGNAL
        );
        if (res <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(res);
    }
    return true;
}
int connectToHost(const char *host, int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        return -1;
    }
    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);

    if (inet_pton(AF_INET,host,&server_address.sin_addr) <= 0)
    {
        close(sock);
        return -1;
    }
    if (connect(sock,(struct sockaddr *)&server_address,sizeof(server_address)
    ) < 0)
    {
        close(sock);
        return -1;
    }
    return sock;
}
bool uploadSource(int sock, const char *source_path)
{
    FILE *file = fopen(source_path, "rb");
    if (file == nullptr) {
        cout << "Could not open source file.\n";
        return false;
    }
    char status = 0;
    CRC32 crc;
    crc.update(&status, 1);
    if (!sendAll(sock, &status, 1)) {
        fclose(file);
        return false;
    }
    char buffer[IO_BUFFER_SIZE];
    while (true) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (count > 0) {
            crc.update(buffer, count);
            if (!sendAll(sock, buffer, count)) {
                fclose(file);
                return false;
            }
        }
        if (count < sizeof(buffer)) {
            if (ferror(file)) {
                fclose(file);
                return false;
            }
            break;
        }
    }
    fclose(file);

    // Send CRC trailer
    char trailer[4];
    checksum_to_bytes(crc.get_checksum(), trailer);

    if (!sendAll(sock, trailer, 4)) {
        return false;
    }
    if (shutdown(sock, SHUT_WR) < 0) {
        return false;
    }
    return true;
}
bool receiveResponse(int sock, const char *out_tdbg_path,string &out_error)
{
    TrailerChecker checker;
    char buffer[IO_BUFFER_SIZE];
    char output[IO_BUFFER_SIZE + 4];
    bool status_found = false;
    char status = 1;
    FILE *out_file = nullptr;

    while (true) {
        ssize_t received = recv(sock, buffer, sizeof(buffer), 0);
        if (received == 0) {
            break;
        }
        if (received < 0) {
            if (out_file != nullptr) {
                fclose(out_file);
            }
            remove(out_tdbg_path);
            out_error = "connection failed while receiving response";
            return false;
        }

        size_t output_size = checker.process(
            buffer, static_cast<size_t>(received), output
        );

        for (size_t i = 0; i < output_size; i++) {
            if (!status_found) {
                status = output[i];
                status_found = true;
                if (status == 0) {
                    out_file = fopen(out_tdbg_path, "wb");

                    if (out_file == nullptr) {
                        out_error = "could not create debug file";
                        return false;
                    }
                } else if (status != 1) {
                    out_error = "invalid response status";
                    return false;
                }
                continue;
            }
            if (status == 0) {
                if (fwrite(&output[i], 1, 1, out_file) != 1) {
                    fclose(out_file);
                    remove(out_tdbg_path);
                    out_error = "could not write debug file";
                    return false;
                }
            } else {
                out_error += output[i];
            }
        }
    }
    if (out_file != nullptr) {
        fclose(out_file);
    }
    if (!checker.verify() || !status_found) {
        remove(out_tdbg_path);
        out_error = "checksum mismatch, response corrupted";
        return false;
    }
    if (status == 1) {
        return false;
    }
    if (status != 0) {
        remove(out_tdbg_path);
        out_error = "invalid response status";
        return false;
    }
    return true;
}
bool openTdbg(const char *path, TdbgReader &reader)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    TTDBHeader header{};
    ssize_t count = pread(fd, &header, sizeof(header), 0);
    if (count != static_cast<ssize_t>(sizeof(header))) {
        close(fd);
        return false;
    }

    if (memcmp(header.magic, "TTDB", 4) != 0 ||
        header.version != 1 ||
        header.stepCount <= 0 ||
        header.indexOffset < static_cast<int64_t>(sizeof(header))) {
        close(fd);
        return false;
    }
    struct stat file_info{};
    if (fstat(fd, &file_info) < 0) {
        close(fd);
        return false;
    }
    int64_t file_size = file_info.st_size;
    int64_t index_size =
        static_cast<int64_t>(header.stepCount) * sizeof(int64_t);
    if (header.indexOffset > file_size ||
        index_size > file_size - header.indexOffset) {
        close(fd);
        return false;
    }
    vector<int64_t> offsets(header.stepCount);
    count = pread(fd, offsets.data(),
                  static_cast<size_t>(index_size),
                  static_cast<off_t>(header.indexOffset));

    if (count != index_size) {
        close(fd);
        return false;
    }

    // Validate snapshot offsets
    for (int32_t i = 0; i < header.stepCount; i++) {
        if (offsets[i] < static_cast<int64_t>(sizeof(header)) ||
            offsets[i] >= header.indexOffset) {
            close(fd);
            return false;
        }

        if (i > 0 && offsets[i] <= offsets[i - 1]) {
            close(fd);
            return false;
        }
    }
    // Save reader information
    reader.file_fd = fd;
    reader.header = header;
    reader.index = offsets;
    return true;
}
