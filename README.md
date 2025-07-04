# Chat Room System

A high-performance multi-client chat room built with C++ and Boost.Asio.

## Features

- **Multi-client support** - Multiple users can chat simultaneously
- **Message history** - New users see recent messages when joining
- **Async I/O** - Non-blocking server architecture for optimal performance
- **Length-prefixed protocol** - Reliable message delivery
- **Graceful disconnection** - Clean handling of client departures

## Prerequisites

Install required dependencies:
```bash
sudo apt-get install libboost-system-dev libboost-thread-dev
```

## Building

```bash
make all
```

This creates two executables:
- `chatApp` - The server
- `clientApp` - The client

## Usage

### 1. Start the Server
```bash
./chatApp <port>
```
Example:
```bash
./chatApp 8080
```

### 2. Connect Clients
Open new terminals and run:
```bash
./clientApp <host> <port>
```
Example:
```bash
./clientApp localhost 8080
```

### 3. Chat
- Type messages and press Enter to send
- Type `quit` or `exit` to disconnect
- New clients automatically see recent message history

## Quick Test

Run the automated test:
```bash
chmod +x test_system.sh
./test_system.sh
```

## Clean Build

```bash
make clean
```

## Example Session

**Terminal 1 (Server):**
```bash
./chatApp 8080
```

**Terminal 2 (Client 1):**
```bash
./clientApp localhost 8080
> Hello everyone!
```

**Terminal 3 (Client 2):**
```bash
./clientApp localhost 8080
[Message History]
Hello everyone!
> Hi there!
```

## Architecture

- **Server**: Async event-driven architecture using Boost.Asio
- **Client**: Multi-threaded (network I/O + user input)
- **Protocol**: Length-prefixed messages for reliable delivery
- **Memory Management**: Smart pointers for safe async operations
