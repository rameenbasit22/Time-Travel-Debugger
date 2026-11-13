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
void closeTdbg(TdbgReader &reader)
{
    if (reader.file_fd >= 0) 
    {
        close(reader.file_fd);
        reader.file_fd = -1;
    }
    reader.index.clear();
}
bool readBytes(int fd, void *data, size_t size,int64_t &offset, int64_t end_offset)
{
    if (offset < 0 || offset > end_offset ||
        size > static_cast<uint64_t>(end_offset - offset)) {
        return false;
    }

    size_t total_read = 0;
    while (total_read < size) {
        ssize_t count = pread(
            fd,
            static_cast<char *>(data) + total_read,
            size - total_read,
            static_cast<off_t>(offset)
        );
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        total_read += static_cast<size_t>(count);
        offset += count;
    }
    return true;
}
bool readString(int fd, string &value,int64_t &offset, int64_t end_offset)
{
    int32_t length = 0;
    if (!readBytes(fd, &length, sizeof(length), offset, end_offset)) {
        return false;
    }
    if (length < 0 || length > 4096) {
        return false;
    }
    value.clear();

    if (length == 0) {
        return true;
    }
    value.resize(length);
    return readBytes(
        fd, &value[0], static_cast<size_t>(length),
        offset, end_offset
    );
}
bool readVariable(int fd, Variable &var,int64_t &offset, int64_t end_offset)
{
    if (!readString(fd, var.name, offset, end_offset)) {
        return false;
    }
    if (!readBytes(
        fd, &var.value, sizeof(var.value), offset, end_offset
    )) 
    {
        return false;
    }
    return true;
}
Snapshot *readSnapshotAt(TdbgReader &reader, int64_t step)
{
    if (reader.file_fd < 0 ||
        step < 0 ||
        step >= reader.header.stepCount) 
    {
        return nullptr;
    }
    int64_t offset = reader.index[step];
    int64_t end_offset;
    if (step + 1 < reader.header.stepCount) {
        end_offset = reader.index[step + 1];
    } else 
    {
        end_offset = reader.header.indexOffset;
    }
    Snapshot *snapshot = new Snapshot{};
    if (!readBytes(
        reader.file_fd,
        &snapshot->stackDepth,
        sizeof(snapshot->stackDepth),
        offset,
        end_offset
    )) 
    {
        delete snapshot;
        return nullptr;
    }

    if (snapshot->stackDepth < 0 ||
        snapshot->stackDepth > MAX_STACK_DEPTH) {
        delete snapshot;
        return nullptr;
    }

    for (int32_t i = 0; i < snapshot->stackDepth; i++) {
        Frame &frame = snapshot->callStack[i];

        if (!readString(
            reader.file_fd, frame.func_name, offset, end_offset
        ))
        {
            delete snapshot;
            return nullptr;
        }
        if (!readBytes(
            reader.file_fd,
            &frame.argc,
            sizeof(frame.argc),
            offset,
            end_offset
        )) 
        {
            delete snapshot;
            return nullptr;
        }

        if (frame.argc < 0 || frame.argc > MAX_VARS_PER_FRAME) {
            delete snapshot;
            return nullptr;
        }

        for (int32_t j = 0; j < frame.argc; j++) {
            if (!readVariable(
                reader.file_fd, frame.argv[j], offset, end_offset
            ))
            {
                delete snapshot;
                return nullptr;
            }
        }
        if (!readBytes(
            reader.file_fd,
            &frame.returnLine,
            sizeof(frame.returnLine),
            offset,
            end_offset
        )) 
        {
            delete snapshot;
            return nullptr;
        }
        if (!readBytes(
            reader.file_fd,
            &frame.localCount,
            sizeof(frame.localCount),
            offset,
            end_offset
        )) 
        {
            delete snapshot;
            return nullptr;
        }
        if (frame.localCount < 0 ||
            frame.localCount > MAX_VARS_PER_FRAME) {
            delete snapshot;
            return nullptr;
        }

        for (int32_t j = 0; j < frame.localCount; j++) {
            if (!readVariable(
                reader.file_fd, frame.locals[j], offset, end_offset
            )) 
            {
                delete snapshot;
                return nullptr;
            }
        }
    }
    return snapshot;
}
void initSession(DebugSession &session)
{
    session.active_flag = 0;
    session.reader.file_fd = -1;
    session.reader.index.clear();
    session.current_step = 0;
}
void closeSession(DebugSession &session)
{
    if (session.active_flag) {
        closeTdbg(session.reader);
    }

    session.active_flag = 0;
    session.current_step = 0;
}
void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s);
void showCurrent(DebugSession &session)
{
    if (!session.active_flag) {
        cout << "No active debugging session.\n";
        return;
    }
    Snapshot *snapshot = readSnapshotAt(
        session.reader, session.current_step
    );
    if (snapshot == nullptr) {
        cout << "Could not read snapshot.\n";
        return;
    }
    printSnapshot(
        static_cast<int32_t>(session.current_step + 1),
        session.reader.header.stepCount,
        *snapshot
    );
    delete snapshot;
}
void moveBy(DebugSession &session, int32_t delta)
{
    if (!session.active_flag) {
        cout << "No active debugging session.\n";
        return;
    }
    int64_t new_step = session.current_step + delta;
    if (new_step < 0) {
        cout << "Already at the first step.\n";
        return;
    }
    if (new_step >= session.reader.header.stepCount) {
        cout << "Already at the last step.\n";
        return;
    }
    session.current_step = new_step;
    showCurrent(session);
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    string error;
    int sock = connectToHost(lb_host.c_str(), lb_port);
    if (sock < 0) 
    {
        cout << "Could not connect to load balancer.\n";
        return false;
    }
    if (!uploadSource(sock, sourcePath.c_str())) 
    {
        close(sock);
        cout << "Could not upload source file.\n";
        return false;
    }

    bool result = receiveResponse(sock, "ttdb_new.tdbg", error);
    close(sock);
    if (!result) 
    {
        remove("ttdb_new.tdbg");
        cout << error << "\n";
        return false;
    }

    TdbgReader new_reader{};
    new_reader.file_fd = -1;
    if (!openTdbg("ttdb_new.tdbg", new_reader)) 
    {
        remove("ttdb_new.tdbg");
        cout << "server sent a damaged .tdbg file\n";
        return false;
    }
    if (rename("ttdb_new.tdbg", TDBG_FILE) != 0) {
        closeTdbg(new_reader);
        remove("ttdb_new.tdbg");
        cout << "Could not save debug file.\n";
        return false;
    }
    if (s.active_flag) 
    {
        closeTdbg(s.reader);
    }

    s.reader = new_reader;
    s.current_step = 0;
    s.active_flag = 1;
    cout << "Debugging session started successfully.\n";
    showCurrent(s);
    return true;
}
bool cmdNext(DebugSession &s)
{
    moveBy(s, 1);
    return true;
}
bool cmdPrv(DebugSession &s)
{
    moveBy(s, -1);
    return true;
}
bool cmdJmpNext(DebugSession &s, int32_t n)
{
    if (n <= 0 || n > MAX_JUMP) 
    {
        cout << "Invalid jump value.\n";
        return false;
    }
    moveBy(s, n);
    return true;
}
bool cmdJmpPrv(DebugSession &s, int32_t n)
{
    if (n <= 0 || n > MAX_JUMP) 
    {
        cout << "Invalid jump value.\n";
        return false;
    }
    moveBy(s, -n);
    return true;
}
void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << "\n========== DEBUG SNAPSHOT ==========\n";
    cout << "Step: " << step << " of " << totalSteps << "\n";
    cout << "Call Stack Depth: " << s.stackDepth << "\n";
    for (int32_t i = 0; i < s.stackDepth; i++) {
        const Frame &frame = s.callStack[i];
        cout << "\nFunction: " << frame.func_name << "\n";
        cout << "Arguments:\n";
        if (frame.argc == 0) 
        {
            cout << "  No arguments\n";
        }
        for (int32_t j = 0; j < frame.argc; j++) {
            cout << "  " << frame.argv[j].name
                 << " = " << frame.argv[j].value << "\n";
        }

        cout << "Return Line: " << frame.returnLine << "\n";
        cout << "Local Variables:\n";
        if (frame.localCount == 0) 
        {
            cout << "  No local variables\n";
        }
        for (int32_t j = 0; j < frame.localCount; j++) 
        {
            cout << "  " << frame.locals[j].name
                 << " = " << frame.locals[j].value << "\n";
        }
    }
    cout << "====================================\n";
}
void printHelp()
{
    cout << "\n========== TTDB COMMANDS ==========\n";
    cout << "Start source.bin  - Start a new debugging session\n";
    cout << "Next              - Move to the next snapshot\n";
    cout << "Prv               - Move to the previous snapshot\n";
    cout << "Jmp next N        - Jump forward N snapshots\n";
    cout << "Jmp prv N         - Jump backward N snapshots\n";
    cout << "Help              - Display available commands\n";
    cout << "Quit              - Exit the debugger\n";
    cout << "Exit              - Exit the debugger\n";
    cout << "===================================\n";
}
