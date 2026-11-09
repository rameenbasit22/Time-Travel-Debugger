// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdlib>
#include <cstdio>
#include <cstddef>
#include "helper.h"
#include <cerrno>
#include <cstring>
#include <arpa/inet.h>
#include <netinet/in.h>
using namespace std;
// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top; 
    int32_t ct;

public:
    Stack()
{
    top = nullptr;
    ct = 0;
}
void push(const T &val)
{
    if (ct >= MAX_STACK_DEPTH)
    {
        return;
    }
    Node *temp = new Node;
    temp->data = val;
    temp->next = top;
    top = temp;
    ct++;
}
T pop()
{
    if (top == nullptr)
    {
        return T();
    }
    Node *temp = top;
    T value = temp->data;
    top = top->next;
    delete temp;
    ct--;
    return value;
}
T &peek()
{
    return top->data;
}
bool isEmpty()
{
    return top == nullptr;
}
int32_t depth()
{
    return ct;
}
int32_t snapshot_into(T out[], int32_t maxLen)
{
    Node *present = top;
    int32_t write_done = 0;
    while (present != nullptr && write_done < maxLen)
    {
        out[write_done] = present->data;
        write_done++;
        present = present->next;
    }
    return write_done;
}
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t ct;
public:
    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        ct = 0;
    }
    void record(Snapshot *s)
    {
        TimelineNode *temp = new TimelineNode;
        temp->data = s;
        temp->next = nullptr;
        temp->prev = tail;
        if (head == nullptr)
        {
            head = temp;
        }
        else
        {
            tail->next = temp;
        }
        tail = temp;
        ct++;
    }
    TimelineNode *begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return ct;
    }
};
// Core structs
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}
// resolve.bin - bookkeeping
void printSnapshots(Timeline &timeline)
{
    TimelineNode *node = timeline.begin();
    int count = 0;

    while (node != nullptr)
    {
        cout << "\nSnapshot " << count << endl;

        Snapshot *s = node->data;

        for (int32_t i = 0; i < s->stackDepth; i++)
        {
            Frame &frame = s->callStack[i];

            cout << "Function: " << frame.func_name << endl;

            for (int32_t j = 0; j < frame.localCount; j++)
            {
                cout << frame.locals[j].name
                     << " = " << frame.locals[j].value << endl;
            }
        }

        node = node->next;
        count++;
    }
}


// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    while (getline(in, out))
    {
        if (!out.empty())
        {
            return true;
        }
    }

    return false;
}
string firstWord(const string &line)
{
    string word = "";
    for (size_t i = 0; i < line.size(); i++)
    {
        if (line[i] == ' ')
        {
            break;
        }
        word = word + line[i];
    }
    return word;
}
string secondWord(const string &line)
{
    string word = "";
    bool found = false;
    for (size_t i = 0; i < line.size(); i++)
    {
        if (line[i] == ' ')
        {
            if (!found)
            {
                found = true;
            }
            else if (!word.empty())
            {
                break;
            }
        }
        else if (found)
        {
            word = word + line[i];
        }
    }
    return word;
}
bool validateProgram(const char *sourcePath)
{
    ifstream in(sourcePath);
    if (!in)
    {
        cout << "Error in opening the file !!!!" << endl;
        return false;
    }
    string line;
    bool in_func = false;
    while (readSourceLine(in, line))
    {
        string word = firstWord(line);
        if (word == "//")
        {
          continue;
        }

        if (word == "func")
        {
            if (in_func)
            {
                cout << "Error!!!Nested functions are not allowed." << endl;
                return false;
            }

            in_func = true;
        }
        else if (word == "func_end")
        {
            if (!in_func)
            {
                cout << "Error!! func_end without func." << endl;
                return false;
            }

            in_func = false;
        }
    }

    if (in_func)
    {
        cout << "Error!! Missing func_end." << endl;
        return false;
    }

    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    int64_t pos = ftell(f);
    int32_t size = text.size();
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&size, sizeof(int32_t), 1, f);
    fwrite(text.c_str(), sizeof(char), size, f);
    return pos;
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    int64_t offset;
    int32_t size;
    if (fread(&offset, sizeof(offset), 1, f) != 1)
    {
        return -1;
    }
    fread(&size, sizeof(size), 1, f);
    char buffer[1024];
    if (size >= 1024)
    {
        return -1;
    }
    fread(buffer, sizeof(char), size, f);
    buffer[size] = '\0';
    outText = buffer;
    return offset;
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t f_ct = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t p_ct = 0;
    FILE *srcFile = fopen(sourcePath, "r");
    FILE *resFile = fopen(resolveBinPath, "wb");
    if (srcFile == nullptr || resFile == nullptr)
    {
        cout << "Error in opening the file." << endl;
        return -1;
    }
    char buffer[1024];
    string line;
    int64_t pos = 0;
    int64_t pos2 = -1;
    while (fgets(buffer, sizeof(buffer), srcFile))
    {
        line = buffer;
        if (!line.empty() && line.back() == '\n')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }
        string word = firstWord(line);
        if (word == "func")
        {
            string name = secondWord(line);
            funcArray[f_ct].funcName = name;
            funcArray[f_ct].byteOffsetInResolveBin = pos;
            if (name == "main")
            {
                pos2 = pos;
            }
            f_ct++;
        }
        if (word == "call")
        {
            string name = secondWord(line);
            patches[p_ct].targetFuncName = name;
            patches[p_ct].byteOffsetOfOffsetField = pos;
            p_ct++;
        }
        writeResolveRecord(resFile, 0, line);
        pos = pos + 8 + 4 + line.size();
    }
    for (int32_t i = 0; i < p_ct; i++)
    {
        bool found = false;
        for (int32_t j = 0; j < f_ct; j++)
        {
            if (patches[i].targetFuncName == funcArray[j].funcName)
            {
                int64_t target_pos =funcArray[j].byteOffsetInResolveBin;
                fseek(resFile,patches[i].byteOffsetOfOffsetField,SEEK_SET);
                fwrite(&target_pos, sizeof(pos2), 1, resFile);
                found = true;
                break;
            }
        }
        if (!found)
        {
            cout << "Error!! Function " << patches[i].targetFuncName<< " not found." << endl;
            fclose(srcFile);
            fclose(resFile);
            return -1;
        }
    }
    fclose(srcFile);
    fclose(resFile);
    if (pos2 == -1)
    {
        cout << "Error!! main function not found." << endl;
        return -1;
    }
    return pos2;
}
// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    int32_t ct = 0;
    string word = "";
    for (size_t i = 0; i <= line.size(); i++)
    {
        if (i == line.size() || line[i] == ' ')
        {
            if (!word.empty())
            {
                if (ct == 0)
                {
                    tokens[ct].type = KEYWORD;
                }
                else if (ct == 1)
                {
                    tokens[ct].type = IDENTIFIER;
                }
                else
                {
                    tokens[ct].type = PARAM;
                }
                tokens[ct].text = word;
                ct++;
                word = "";
            }
        }
        else
        {
            word = word + line[i];
        }
        if (ct >= maxTokens)
        {
            break;
        }
    }
    return ct;
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    Snapshot *ss = new Snapshot;
    ss->stackDepth = callStack.depth();
    callStack.snapshot_into(ss->callStack, MAX_STACK_DEPTH);
    return ss;
}
void set_var(Frame &frame, string name, int32_t value)
{
    for (int32_t i = 0; i < frame.localCount; i++)
    {
        if (frame.locals[i].name == name)
        {
            frame.locals[i].value = value;
            return;
        }
    }

    if (frame.localCount >= MAX_VARS_PER_FRAME)
    {
        cout << "Error!! Maximum variables reached." << endl;
        return;
    }

    frame.locals[frame.localCount].name = name;
    frame.locals[frame.localCount].value = value;
    frame.localCount++;
}

bool get_operand_value(Frame &frame, string operand, int32_t &value)
{
    char *end;
    errno = 0;

    long num = strtol(operand.c_str(), &end, 10);

    if (end != operand.c_str() && *end == '\0' &&
        errno != ERANGE && num >= -2147483648L &&
        num <= 2147483647L)
    {
        value = static_cast<int32_t>(num);
        return true;
    }

    for (int32_t i = 0; i < frame.localCount; i++)
    {
        if (frame.locals[i].name == operand)
        {
            value = frame.locals[i].value;
            return true;
        }
    }

    cout << "Error!! Variable or number " << operand
         << " not found." << endl;

    return false;
}

void add_var(Frame &frame, string name, int32_t value)
{
    for (int32_t i = 0; i < frame.localCount; i++)
    {
        if (frame.locals[i].name == name)
        {
            frame.locals[i].value += value;
            return;
        }
    }

    cout << "Error!! Variable " << name << " not found." << endl;
}

void sub_var(Frame &frame, string name, int32_t value)
{
    for (int32_t i = 0; i < frame.localCount; i++)
    {
        if (frame.locals[i].name == name)
        {
            frame.locals[i].value -= value;
            return;
        }
    }

    cout << "Error!! Variable " << name << " not found." << endl;
}

void mul_var(Frame &frame, string name, int32_t value)
{
    for (int32_t i = 0; i < frame.localCount; i++)
    {
        if (frame.locals[i].name == name)
        {
            frame.locals[i].value *= value;
            return;
        }
    }

    cout << "Error!! Variable " << name << " not found." << endl;
}

void div_var(Frame &frame, string name, int32_t value)
{
    if (value == 0)
    {
        cout << "Error!! Cannot divide by zero." << endl;
        return;
    }

    for (int32_t i = 0; i < frame.localCount; i++)
    {
        if (frame.locals[i].name == name)
        {
            frame.locals[i].value /= value;
            return;
        }
    }

    cout << "Error!! Variable " << name << " not found." << endl;
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    Stack<Frame> callStack;
    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.returnLine = -1;
    mainFrame.localCount = 0;
    callStack.push(mainFrame);

    FILE *file = fopen(resolveBinPath, "rb");
    if (file == nullptr)
    {
        cout << "Error in opening resolve.bin." << endl;
        return;
    }
    fseek(file, mainOffset, SEEK_SET);
    int64_t return_pos[MAX_STACK_DEPTH];
    int32_t return_ct = 0;
    string line;

    while (true)
    {
        int64_t offset = readResolveRecord(file, line);
        if (offset == -1)
        {
            break;
        }

        Token tokens[MAX_TOKENS];
        int32_t ct = tokenizeLine(line, tokens, MAX_TOKENS);

        if (ct == 0)
        {
            continue;
        }
        string word = tokens[0].text;
        cout << "Executing: " << line << endl;
        if (word == "set")
        {
            if (ct >= 3)
            {
                Frame &frame = callStack.peek();
                string name = tokens[1].text;
                int32_t value = atoi(tokens[2].text.c_str());
                set_var(frame, name, value);
            }
            Snapshot *ss = buildSnapshot(callStack);
            timeline.record(ss);
        }
        else if (word == "add")
        {
            if (ct >= 3)
            {
                Frame &frame = callStack.peek();
                string name = tokens[1].text;
                int32_t value;
                if (get_operand_value(frame, tokens[2].text, value))
                {
                    add_var(frame, name, value);
                }
            }
            Snapshot *ss = buildSnapshot(callStack);
            timeline.record(ss);
        }
        else if (word == "sub")
        {
            if (ct >= 3)
            {
                Frame &frame = callStack.peek();
                string name = tokens[1].text;
                int32_t value;
                if (get_operand_value(frame, tokens[2].text, value))
                {
                    sub_var(frame, name, value);
                }
            }
            Snapshot *ss = buildSnapshot(callStack);
            timeline.record(ss);
        }
        else if (word == "mul")
        {
            if (ct >= 3)
            {
                Frame &frame = callStack.peek();
                string name = tokens[1].text;
                int32_t value;
                if (get_operand_value(frame, tokens[2].text, value))
                {
                    mul_var(frame, name, value);
                }
            }
            Snapshot *ss = buildSnapshot(callStack);
            timeline.record(ss);
        }
        else if (word == "div")
        {
            if (ct >= 3)
            {
                Frame &frame = callStack.peek();
                string name = tokens[1].text;
                int32_t value;
                if (get_operand_value(frame, tokens[2].text, value))
                {
                    div_var(frame, name, value);
                }
            }
            Snapshot *ss = buildSnapshot(callStack);
            timeline.record(ss);
        }
        else if (word == "call")
        {
            if (ct >= 2)
            {
                if (callStack.depth() >= MAX_STACK_DEPTH ||
                    return_ct >= MAX_STACK_DEPTH)
                {
                    cout << "Error!! Maximum call stack depth reached." << endl;
                    fclose(file);
                    return;
                }
                int64_t current_pos = ftell(file);
                string headerLine;
                fseek(file, offset, SEEK_SET);

                int64_t headerOffset = readResolveRecord(file, headerLine);
                if (headerOffset == -1)
                {
                    cout << "Error!! Cannot read function header." << endl;
                    fclose(file);
                    return;
                }
                Token headerTokens[MAX_TOKENS];
                int32_t headerCount = tokenizeLine(
                    headerLine, headerTokens, MAX_TOKENS);
                if (headerCount < 2 ||
                    headerTokens[0].text != "func")
                {
                    cout << "Error!! Invalid function header." << endl;
                    fclose(file);
                    return;
                }
                int32_t argCount = headerCount - 2;
                if (argCount != ct - 2)
                {
                    cout << "Error!! Incorrect number of arguments." << endl;
                    fclose(file);
                    return;
                }

                Frame old_frame = callStack.peek();
                Frame new_frame;
                new_frame.func_name = headerTokens[1].text;
                new_frame.argc = argCount;
                new_frame.returnLine = -1;
                new_frame.localCount = 0;
                for (int32_t i = 0; i < argCount; i++)
                {
                    string actualName = tokens[i + 2].text;
                    string formalName = headerTokens[i + 2].text;
                    bool found = false;
                    for (int32_t j = 0; j < old_frame.localCount; j++)
                    {
                        if (old_frame.locals[j].name == actualName)
                        {
                            new_frame.argv[i].name = actualName;
                            new_frame.argv[i].value =
                                old_frame.locals[j].value;

                            new_frame.locals[i].name = formalName;
                            new_frame.locals[i].value =
                                old_frame.locals[j].value;

                            new_frame.localCount++;

                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        cout << "Error!! Argument variable "
                             << actualName << " not found." << endl;
                        fclose(file);
                        return;
                    }
                }
                return_pos[return_ct] = current_pos;
                return_ct++;

                callStack.push(new_frame);
                fseek(file, offset, SEEK_SET);
            }
            Snapshot *ss = buildSnapshot(callStack);
            timeline.record(ss);
        }
        else if (word == "func_end")
        {
            if (callStack.depth() > 1)
            {
                Frame finishedFrame = callStack.pop();
                Frame &caller = callStack.peek();
                for (int32_t i = 0; i < finishedFrame.argc; i++)
                {
                    string actualName = finishedFrame.argv[i].name;

                    int32_t updatedValue =
                        finishedFrame.locals[i].value;

                    for (int32_t j = 0; j < caller.localCount; j++)
                    {
                        if (caller.locals[j].name == actualName)
                        {
                            caller.locals[j].value = updatedValue;
                            break;
                        }
                    }
                }
                return_ct--;
                fseek(file, return_pos[return_ct], SEEK_SET);
            }
            else
            {
                Snapshot *ss = buildSnapshot(callStack);
                timeline.record(ss);
                break;
            }
            Snapshot *ss = buildSnapshot(callStack);
            timeline.record(ss);
        }
    }
    fclose(file);
}
    
// PASS 0x3: SERIALIZE TIMELINE
void write_str(FILE *f, const string &text)
{
    int32_t size = text.size();
    fwrite(&size, sizeof(int32_t), 1, f);
    fwrite(text.c_str(), sizeof(char), size, f);
}
void write_var(FILE *f, const Variable &var)
{
    write_str(f, var.name);
    fwrite(&var.value, sizeof(int32_t), 1, f);
}
void write_ss(FILE *f, const Snapshot &s)
{
    fwrite(&s.stackDepth, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < s.stackDepth; i++)
    {
        const Frame &frame = s.callStack[i];
        write_str(f, frame.func_name);
        fwrite(&frame.argc, sizeof(int32_t), 1, f);
        for (int32_t j = 0; j < frame.argc; j++)
        {
            write_var(f, frame.argv[j]);
        }
        fwrite(&frame.returnLine, sizeof(int32_t), 1, f);
        fwrite(&frame.localCount, sizeof(int32_t), 1, f);
        for (int32_t j = 0; j < frame.localCount; j++)
        {
            write_var(f, frame.locals[j]);
        }
    }
}
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    FILE *f = fopen(tdbgPath, "wb");
    if (f == nullptr)
    {
        cout << "Error in creating session.tdbg." << endl;
        return;
    }
    TTDBHeader h;
    h.magic[0] = 'T';
    h.magic[1] = 'T';
    h.magic[2] = 'D';
    h.magic[3] = 'B';
    h.version = 1;
    h.stepCount = timeline.getStepCount();
    h.indexOffset = 0;
    writeHeader(f, h);

    int64_t *idx = new int64_t[h.stepCount];
    TimelineNode *node = timeline.begin();
    for (int32_t i = 0; i < h.stepCount; i++)
    {
        idx[i] = ftell(f);
        write_ss(f, *node->data);
        node = node->next;
    }
    h.indexOffset = ftell(f);
    for (int32_t i = 0; i < h.stepCount; i++)
    {
        fwrite(&idx[i], sizeof(int64_t), 1, f);
    }
    fseek(f, 0, SEEK_SET);
    writeHeader(f, h);
    delete[] idx;
    fclose(f);
    cout << "Pass 3 successful!!" << endl;
}
bool send_all(int sock, const char *data, size_t size)
{
    size_t sent = 0;
    while (sent < size)
    {
        ssize_t result = send(sock,
            data + sent,
            size - sent,
            MSG_NOSIGNAL
        );
        if (result <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}
bool receive_upload(int sock)
{
    FILE *file = fopen("source.bin", "wb");
    if (file == nullptr)
    {
        return false;
    }
    TrailerChecker checker;
    char buffer[IO_BUFFER_SIZE];
    char output[IO_BUFFER_SIZE + 4];
    bool status_found = false;
    bool too_large = false;
    char status = 1;
    uint64_t source_size = 0;
    while (true)
    {
        ssize_t received = recv(sock, buffer, sizeof(buffer), 0);
        if (received == 0)
        {
            break;
        }
        if (received < 0)
        {
            fclose(file);
            remove("source.bin");
            return false;
        }
        size_t output_size = checker.process(
            buffer,
            static_cast<size_t>(received),
            output
        );
        for (size_t i = 0; i < output_size; i++)
        {
            if (!status_found)
            {
                status = output[i];
                status_found = true;
                continue;
            }
            if (source_size >= MAX_SOURCE_BYTES)
            {
                too_large = true;
                continue;
            }
            if (fwrite(&output[i], 1, 1, file) != 1)
            {
                fclose(file);
                remove("source.bin");
                return false;
            }
            source_size++;
        }
    }
    fclose(file);
    if (!checker.verify() || !status_found ||
        status != 0 || too_large || source_size == 0)
    {
        remove("source.bin");
        return false;
    }
    return true;
}
bool send_response(int sock, char status, const char *file_path,
                   const string &error_message)
{
    CRC32 crc;
    crc.update(&status, 1);
    if (!send_all(sock, &status, 1))
    {
        return false;
    }
    if (status == 0)
    {
        FILE *file = fopen(file_path, "rb");
        if (file == nullptr)
        {
            return false;
        }
        char buffer[IO_BUFFER_SIZE];
        while (true)
        {
            size_t count = fread(buffer, 1, sizeof(buffer), file);
            if (count > 0)
            {
                crc.update(buffer, count);
                if (!send_all(sock, buffer, count))
                {
                    fclose(file);
                    return false;
                }
            }
            if (count < sizeof(buffer))
            {
                break;
            }
        }
        fclose(file);
    }
    else
    {
        crc.update(error_message.c_str(), error_message.size());
        if (!send_all(sock, error_message.c_str(),
                      error_message.size()))
        {
            return false;
        }
    }
    char trailer[4];
    checksum_to_bytes(crc.get_checksum(), trailer);
    return send_all(sock, trailer, 4);
}
void handle_client(int client_sock)
{
    struct timeval timeout;
    timeout.tv_sec = SOCKET_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    setsockopt(
        client_sock,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &timeout,
        sizeof(timeout)
    );
    if (!receive_upload(client_sock))
    {
        send_response(
            client_sock,
            1,
            "",
            "upload failed: invalid source or checksum"
        );
        return;
    }
    cout << "Source file received successfully." << endl;
    if (!validateProgram("source.bin"))
    {
        send_response(
            client_sock,
            1,
            "",
            "program validation failed"
        );
        return;
    }
    cout << "Pass 0 successful" << endl;
    int64_t main_offset =
        resolveProgram("source.bin", "resolve.bin");
    if (main_offset == -1)
    {
        send_response(
            client_sock,
            1,
            "",
            "program resolution failed"
        );
        return;
    }
    cout << "Pass 1 successful" << endl;
    Timeline timeline;
    executeProgram("resolve.bin", main_offset, timeline);
    cout << "Total snapshots: "
         << timeline.getStepCount() << endl;
    if (timeline.getStepCount() <= 0)
    {
        send_response(
            client_sock,
            1,
            "",
            "program execution failed"
        );
        return;
    }
    writeTdbg(timeline, "session.tdbg");
    ifstream check_file("session.tdbg", ios::binary);

    if (!check_file)
    {
        send_response(
            client_sock,
            1,
            "",
            "could not create session file"
        );
        return;
    }
    check_file.close();
    cout << "Sending session.tdbg..." << endl;
    send_response(
        client_sock,
        0,
        "session.tdbg",
        ""
    );
}
// main section
int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        cout << "Usage: ./server <port>" << endl;
        return 1;
    }
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535)
    {
        cout << "Invalid port number." << endl;
        return 1;
    }

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0)
    {
        perror("Socket error");
        return 1;
    }
    int option = 1;

    setsockopt(
        server_sock,
        SOL_SOCKET,
        SO_REUSEADDR,
        &option,
        sizeof(option)
    );

    sockaddr_in server_address{};

    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(port);

    if (bind(
        server_sock,
        (struct sockaddr *)&server_address,
        sizeof(server_address)
    ) < 0)
    {
        perror("Bind error");
        close(server_sock);
        return 1;
    }

    if (listen(server_sock, 10) < 0)
    {
        perror("Listen error");
        close(server_sock);
        return 1;
    }
    cout << "Server listening on port " << port << endl;
    while (true)
    {
        int client_sock = accept(server_sock, nullptr, nullptr);
        if (client_sock < 0)
        {
            continue;
        }

        cout << "\nNew load balancer connection." << endl;

        handle_client(client_sock);
        close(client_sock);
        cout << "Connection closed." << endl;
    }
    close(server_sock);
    return 0;
}
