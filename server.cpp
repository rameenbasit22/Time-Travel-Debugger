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
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

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
    int32_t count;

public:
    Stack()
{
    top = nullptr;
    count = 0;
}

void push(const T &val)
{
    if (count >= MAX_STACK_DEPTH)
    {
        return;
    }

    Node *temp = new Node;
    temp->data = val;
    temp->next = top;
    top = temp;
    count++;
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
    count--;
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
    return count;
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
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



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
    for (int i = 0; i < line.size(); i++)
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
    for (int i = 0; i < line.size(); i++)
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
    for (int i = 0; i <= line.size(); i++)
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
    // build the snapshot based on the callStack given
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{
    Token tokens[16];

    string line = "call addNum x";

    int32_t count = tokenizeLine(line, tokens, 16);

    cout << "Token count: " << count << endl;

    for (int32_t i = 0; i < count; i++)
    {
        cout << tokens[i].text << endl;
    }

    return 0;
}
