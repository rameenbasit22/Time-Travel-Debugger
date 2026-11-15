# Time-Travel Debugger (TTDB)

## Project Overview

The Time-Travel Debugger (TTDB) is a client-server based debugging system developed in C++.

It allows users to execute programs written in a custom C-- language, store their execution history, and move forward or backward through recorded execution snapshots.

The project is divided into two phases.

## Phase 01 - Core Debugging System

The server processes the source program through the following stages:

1. **Pass 0 - Validation:** Checks function declarations and program structure.
2. **Pass 1 - Resolve:** Resolves function calls and patches their offsets.
3. **Pass 2 - Execution:** Executes C-- instructions and records snapshots.
4. **Pass 3 - Serialization:** Saves the execution history in a `.tdbg` file.

The supported instructions include:

- `func`
- `func_end`
- `call`
- `set`
- `add`
- `mul`
- `sub`
- `div`

## Phase 02 - Client-Server System

Phase 02 extends the debugger with networking and an interactive client.

It includes:

### Server
- Receives source files from the load balancer.
- Validates uploaded data using CRC32 checksums.
- Executes the Phase 01 debugging pipeline.
- Sends the generated `.tdbg` file or an error response.

### Load Balancer
- Accepts client connections on port 5000.
- Maintains a linked-list waiting queue.
- Supports multiple clients using threads.
- Protects the queue using a mutex.
- Starts the backend server when it is not running.
- Handles queue limits, timeouts, and server startup failures.

### Client
- Uploads source files using the `start` command.
- Receives and locally stores debugging sessions.
- Displays execution snapshots.
- Supports forward and backward navigation.
- Supports jumping multiple snapshots.

## Available Client Commands

| Command | Description |
|---|---|
| `start source.bin` | Start a new debugging session |
| `next` | Move to the next snapshot |
| `prv` | Move to the previous snapshot |
| `jmp next N` | Jump forward N snapshots |
| `jmp prv N` | Jump backward N snapshots |
| `help` | Display available commands |
| `quit` / `exit` | Exit the debugger |

## Requirements

- Ubuntu Linux
- C++ compiler (g++)
- C++17 support
- POSIX threads and sockets

## Compilation

Run the following commands from the project directory:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -g server.cpp -o server -pthread

g++ -std=c++17 -Wall -Wextra -Werror -g load_balancer.cpp -o load_balancer -pthread

g++ -std=c++17 -Wall -Wextra -Werror -g client.cpp -o client
```

## Running the Project

### Step 1: Start the Load Balancer

Open the first terminal:

```bash
./load_balancer 127.0.0.1 5001
```

The load balancer listens for clients on port 5000 and uses port 5001 for the backend server.

### Step 2: Start the Client

Open a second terminal:

```bash
./client 127.0.0.1 5000
```

### Step 3: Start Debugging

Inside the client, enter:

```text
start source.bin
```

After the session starts, use `next`, `prv`, or the jump commands to navigate through execution snapshots.

## Project Files

- `helper.h` - Shared structures, constants, and helper functions.
- `server.cpp` - Backend debugging server.
- `load_balancer.cpp` - Load balancer and client queue management.
- `client.cpp` - Interactive debugging client.
- `source.bin` - Sample C-- source program.
- `resolve.bin` - Resolved program output.
- `session.tdbg` - Serialized execution history.
- `ttdb_last.tdbg` - Latest debugging session saved by the client.

## Technologies Used

- C++
- Linux sockets
- POSIX threads
- Mutex synchronization
- CRC32 checksum validation
- Binary file handling
- C++ vectors and linked lists


