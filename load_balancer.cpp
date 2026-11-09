n// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================
#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include "helper.h"
using namespace std;

const char *SERVER_PATH = "./server";
const int LISTEN_BACKLOG = 16;
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;

template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *front;
    Node *rear;
    int count;
public:
    Queue()
    {
        front = nullptr;
        rear = nullptr;
        count = 0;
    }
    ~Queue()
    {
        while (!isEmpty())
        {
            dequeue();
        }
    }
    void enqueue(const T &val)
    {
        Node *new_node = new Node;
        new_node->data = val;
        new_node->next = nullptr;
        if (rear == nullptr)
        {
            front = new_node;
            rear = new_node;
        }
        else
        {
            rear->next = new_node;
            rear = new_node;
        }
        count++;
    }
    T dequeue()
    {
        if (isEmpty())
        {
            return T{};
        }
        Node *temp = front;
        T value = temp->data;
        front = front->next;
        if (front == nullptr)
        {
            rear = nullptr;
        }
        delete temp;
        count--;
        return value;
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int size()
    {
        return count;
    }
};
struct ClientInfo
{
    int sock;
    string address;
    int port;
    time_t arrivedAt;
};
Queue<ClientInfo> pendingClients;
mutex queueMutex;
volatile sig_atomic_t flag = 1;

bool sendAll(int sock, const char *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n)
    {
        ssize_t result = send(
            sock,
            buf + sent,
            n - sent,
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
bool sendError(int sock, const string &message)
{
    char status = 1;
    if (!sendAll(sock, &status, 1))
    {
        return false;
    }
    return sendAll(sock, message.c_str(), message.size());
}
