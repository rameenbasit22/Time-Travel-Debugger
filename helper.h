#pragma once
#include <iostream>
#include <string>
#include <cstdint>
#include <cstddef>
using namespace std;

// Shared constants
const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;
const int IO_BUFFER_SIZE = 64 * 1024;
const int MAX_PENDING_CLIENTS = 8;
const int STARTUP_MAX_ATTEMPTS = 50;
const int STARTUP_POLL_INTERVAL_MS = 100;
const int MAX_JUMP = 1000000000;
const char TDBG_FILE[] = "ttdb_last.tdbg";
const int MAX_FUNCS = 128;
const int MAX_TOKENS = MAX_VARS_PER_FRAME + 2;
const int MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024;
const int SOCKET_TIMEOUT_SEC = 5;
// Shared Phase 01 structures
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4];
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin;
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField;
    string targetFuncName;
};
// CRC-32 checksum
class CRC32
{
private:
    uint32_t crc;
public:
    CRC32()
    {
        crc = 0xFFFFFFFF;
    }
    void update(const char *data, size_t size)
    {
        for (size_t i = 0; i < size; i++)
        {
            crc ^= static_cast<unsigned char>(data[i]);
            for (int j = 0; j < 8; j++)
            {
                if (crc & 1)
                {
                    crc = (crc >> 1) ^ 0xEDB88320;
                }
                else
                {
                    crc >>= 1;
                }
            }
        }
    }
    uint32_t get_checksum() const
    {
        return crc ^ 0xFFFFFFFF;
    }
};

// Converts checksum into 4 bytes
inline void checksum_to_bytes(uint32_t checksum, char bytes[4])
{
    bytes[0] = static_cast<char>(checksum & 0xFF);
    bytes[1] = static_cast<char>((checksum >> 8) & 0xFF);
    bytes[2] = static_cast<char>((checksum >> 16) & 0xFF);
    bytes[3] = static_cast<char>((checksum >> 24) & 0xFF);
}
// Checks a streamed message while keeping its last 4 bytes
class TrailerChecker
{
private:
    CRC32 crc;
    char last_bytes[4];
    size_t last_count;
public:
    TrailerChecker()
    {
        last_count = 0;
    }
    size_t process(const char *data, size_t size, char *output)
    {
        size_t output_count = 0;
        for (size_t i = 0; i < size; i++)
        {
            if (last_count < 4)
            {
                last_bytes[last_count] = data[i];
                last_count++;
            }
            else
            {
                output[output_count] = last_bytes[0];
                output_count++;
                crc.update(&last_bytes[0], 1);
                last_bytes[0] = last_bytes[1];
                last_bytes[1] = last_bytes[2];
                last_bytes[2] = last_bytes[3];
                last_bytes[3] = data[i];
            }
        }
        return output_count;
    }
    bool verify() const
    {
        if (last_count != 4)
        {
            return false;
        }
        uint32_t received =
            static_cast<unsigned char>(last_bytes[0]) |
            (static_cast<uint32_t>(
                static_cast<unsigned char>(last_bytes[1])) << 8) |
            (static_cast<uint32_t>(
                static_cast<unsigned char>(last_bytes[2])) << 16) |
            (static_cast<uint32_t>(
                static_cast<unsigned char>(last_bytes[3])) << 24);
        return received == crc.get_checksum();
    }
    void get_trailer(char output[4]) const
{
    for (int i = 0; i < 4; i++)
    {
        output[i] = last_bytes[i];
    }
}
};
