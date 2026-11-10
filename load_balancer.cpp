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
    CRC32 crc;
    crc.update(&status, 1);
    crc.update(message.data(), message.size());

    char trailer[4];
    checksum_to_bytes(crc.get_checksum(), trailer);
    if (!sendAll(sock, &status, 1))
    {
        return false;
    }
    if (!sendAll(sock, message.data(), message.size()))
    {
        return false;
    }
    return sendAll(sock, trailer, 4);
}

int createListenSocket(int port)
{
    int listenSock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listenSock < 0)
    {
        perror("Socket creation failed");
        exit(1);
    }
    int option = 1;
    setsockopt(
        listenSock,
        SOL_SOCKET,
        SO_REUSEADDR,
        &option,
        sizeof(option)
    );
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(
        listenSock,
        reinterpret_cast<sockaddr *>(&address),
        sizeof(address)
    ) < 0)
    {
        perror("Bind failed");
        close(listenSock);
        exit(1);
    }
    if (listen(listenSock, LISTEN_BACKLOG) < 0)
    {
        perror("Listen failed");
        close(listenSock);
        exit(1);
    }
    return listenSock;
}
int connectToServer(const char *host, int port)
{
    int serverSock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (serverSock < 0)
    {
        return -1;
    }
    sockaddr_in address{};

    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &address.sin_addr) <= 0)
    {
        close(serverSock);
        return -1;
    }
    if (connect(
        serverSock,
        reinterpret_cast<sockaddr *>(&address),
        sizeof(address)
    ) < 0)
    {
        close(serverSock);
        return -1;
    }
    return serverSock;
}

bool startServer(int port)
{
    string command = string(SERVER_PATH) + " " +
                     to_string(port) + " &";

    int res = system(command.c_str());
    if (res != 0)
    {
        return false;
    }
    return true;
}
int connectToServerWithWakeup()
{
    int serverSock = connectToServer(
        serverHost.c_str(),
        serverPort
    );
    if (serverSock >= 0)
    {
        return serverSock;
    }
    cout << "Server is not running. Starting server..." << endl;
    if (!startServer(serverPort))
    {
        return -1;
    }
    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++)
    {
        this_thread::sleep_for(
            chrono::milliseconds(STARTUP_POLL_INTERVAL_MS)
        );
        serverSock = connectToServer(
            serverHost.c_str(),
            serverPort
        );
        if (serverSock >= 0)
        {
            cout << "Server started successfully." << endl;
            return serverSock;
        }
    }
    cout << "Server failed to start." << endl;
    return -1;
}
void relayOneClient(ClientInfo &c)
{
    int serverSock = connectToServerWithWakeup();
    if (serverSock < 0)
    {
        sendError(c.sock, "backend server failed to start");
        close(c.sock);
        return;
    }
    char buffer[IO_BUFFER_SIZE];
    bool upload_ok = true;
    TrailerChecker uploadChecker;
    while (true)
    {
        ssize_t received = recv(c.sock, buffer, sizeof(buffer), 0);
        if (received == 0)
        {
            break;
        }
        if (received < 0)
        {
            upload_ok = false;
            break;
        }

        string forwarded;
        uploadChecker.process(
            buffer,
            static_cast<size_t>(received),
            forwarded
        );

        if (!forwarded.empty())
        {
            if (!sendAll(serverSock, forwarded.data(), forwarded.size()))
            {
                upload_ok = false;
                break;
            }
        }
    }
    if (!upload_ok || !uploadChecker.verify())
    {
        sendError(c.sock, "upload checksum mismatch");
        close(serverSock);
        close(c.sock);
        return;
    }
    char trailer[4];
    uploadChecker.get_trailer(trailer);
    if (!sendAll(serverSock, trailer, 4))
    {
        sendError(c.sock, "backend server communication failed");
        close(serverSock);
        close(c.sock);
        return;
    }
    shutdown(serverSock, SHUT_WR);
    TrailerChecker responseChecker;
    string response_body;
    bool response_ok = true;
    while (true)
    {
        ssize_t received = recv(serverSock, buffer, sizeof(buffer), 0);
        if (received == 0)
        {
            break;
        }
        if (received < 0)
        {
            response_ok = false;
            break;
        }

        string forwarded;
        responseChecker.process(
            buffer,
            static_cast<size_t>(received),
            forwarded
        );
        response_body += forwarded;
    }
    if (!response_ok || !responseChecker.verify())
    {
        sendError(c.sock, "backend response checksum mismatch");
    }
    else
    {
        char response_trailer[4];
        responseChecker.get_trailer(response_trailer);

        if (!sendAll(c.sock, response_body.data(), response_body.size()))
        {
            cout << "Client disconnected while receiving response." << endl;
        }
        else
        {
            sendAll(c.sock, response_trailer, 4);
        }
    }
    close(serverSock);
    close(c.sock);
}
