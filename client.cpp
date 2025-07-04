#include "message.hpp"
#include <iostream>
#include <boost/asio.hpp>
#include <thread>
#include <string>

using boost::asio::ip::tcp;

/*
 * ============================================================================
 * CHAT CLIENT - A Journey Through Network Programming Paradigms
 * ============================================================================
 *
 * 🤔 THE CENTRAL QUESTION: How does a client efficiently communicate with a server
 *    while handling user input simultaneously?
 *
 * 🧠 MY LEARNING JOURNEY:
 *    First attempt: "I'll just read from server and write user input sequentially"
 *    Reality check: User types → program blocks reading server → terrible UX
 *
 *    Insight: I need TWO concurrent flows:
 *      Flow 1: Server → Client (incoming messages)
 *      Flow 2: User → Client → Server (outgoing messages)
 *
 * 🎯 ARCHITECTURAL DECISIONS I HAD TO MAKE:
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │ DECISION 1: Threading Model                                             │
 * │                                                                         │
 * │ Option A: Single-threaded event loop (like server)                     │
 * │   + Consistent with server architecture                                 │
 * │   - Complex to handle stdin reading                                     │
 * │                                                                         │
 * │ Option B: Two threads (CHOSEN)                                          │
 * │   + Simple: Main thread = user input, IO thread = network              │
 * │   + Clear separation of concerns                                        │
 * │   - Need thread synchronization                                         │
 * │                                                                         │
 * │ 🧭 Why I chose B: User experience trumps architectural purity          │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │ DECISION 2: Message Protocol Integration                               │
 * │                                                                         │
 * │ 🤯 Realization: Server uses length-prefixed messages, not newlines!    │
 * │                                                                         │
 * │ Wrong approach: "I'll just send text with newlines"                    │
 * │   Result: Server gets confused, protocol mismatch                      │
 * │                                                                         │
 * │ Right approach: Embrace the Message class fully                        │
 * │   Send: Message(text) → [4-byte header][body]                          │
 * │   Recv: [4-byte header][body] → Message.getBody()                      │
 * │                                                                         │
 * │ 🧭 Lesson: Don't fight the protocol, learn to love it                  │
 * └─────────────────────────────────────────────────────────────────────────┘
 */

class ChatClient {
private:
    /*
     * 🏗️ ARCHITECTURE EMERGENCE:
     *
     * Initially I thought: "Just need a socket and send/receive functions"
     *
     * But then I realized I need to coordinate:
     *   - Connection management (resolver, connect, error handling)
     *   - Async message receiving (header + body protocol)
     *   - User input processing (blocking stdin reads)
     *   - Threading synchronization (io_context in background)
     *   - Graceful shutdown (cleanup on exit)
     *
     * Each responsibility became a method. The class evolved organically.
     */
    boost::asio::io_context io;          // The async event processor
    tcp::socket socket;                  // My lifeline to the server
    Message readMessage;                 // Reusable buffer for incoming data
    std::string serverHost;              // Where to connect
    std::string serverPort;              // Which port to connect to

public:
    ChatClient(const std::string& host, const std::string& port)
        : socket(io), serverHost(host), serverPort(port) {
        /*
         * 🤔 DESIGN QUESTION: Why pass host/port to constructor vs connect()?
         *
         * Alternative 1: client.connect("localhost", "8080")
         *   + Flexible, can connect multiple times
         *   - Easy to forget parameters
         *
         * Alternative 2: ChatClient client("localhost", "8080") (CHOSEN)
         *   + Impossible to forget connection details
         *   + Clear single-responsibility: one client = one server
         *   - Less flexible for reconnection scenarios
         *
         * 🧭 I chose immutability over flexibility. Chat clients typically
         *    connect once and stay connected.
         */
    }

    void connect() {
        /*
         * 🌐 CONNECTION ESTABLISHMENT DEEP DIVE:
         *
         * Why this sequence? Let me trace through what happens:
         *
         * 1. tcp::resolver resolver(io)
         *    🤔 Question: What does a resolver actually DO?
         *
         *    Think about it: I have "localhost" and "8080" as strings.
         *    But TCP needs an IP address and port number.
         *
         *    Resolver = String → Network address translator
         *    "localhost" → 127.0.0.1
         *    "google.com" → 142.250.191.78 (or current IP)
         *
         *    🧠 Insight: Network programming is full of these translation layers
         *
         * 2. resolver.resolve(serverHost, serverPort)
         *    Returns multiple endpoints! Why?
         *
         *    Example: google.com might resolve to:
         *      - 142.250.191.78:80
         *      - 142.250.191.79:80
         *      - [IPv6 addresses]
         *
         *    Redundancy for reliability. If one fails, try the next.
         *
         * 3. boost::asio::connect(socket, endpoints)
         *    The magic function that tries each endpoint until one works.
         *
         *    🤯 Mind-bending realization: This function call might:
         *       - Try IPv4, fail → try IPv6, succeed
         *       - Try server 1, timeout → try server 2, succeed
         *       - Handle DNS resolution failures
         *       - Deal with network routing issues
         *
         *    All invisibly! That's why networking libraries are so valuable.
         */
        try {
            tcp::resolver resolver(io);
            auto endpoints = resolver.resolve(serverHost, serverPort);
            boost::asio::connect(socket, endpoints);

            std::cout << "✅ Connected to chat server!" << std::endl;
            std::cout << "Type messages and press Enter. Type 'quit' to exit.\n" << std::endl;

        } catch (std::exception& e) {
            /*
             * 🚨 ERROR HANDLING PHILOSOPHY:
             *
             * Question: Should I retry connection failures automatically?
             *
             * Pro-retry: Robust against temporary network issues
             * Anti-retry: User might want to fix hostname/port first
             *
             * 🧭 Decision: Fail fast, let user decide
             * Rationale: Chat clients need immediate feedback, not mysterious delays
             */
            std::cerr << "❌ Connection failed: " << e.what() << std::endl;
            throw;  // Re-throw to let main() handle final cleanup
        }
    }

    void startReceiving() {
        /*
         * 🎭 THE ASYNC RECEIVING DANCE - Act 1: Header Reading
         *
         * 🤔 FUNDAMENTAL QUESTION: Why read header separately from body?
         *
         * Alternative 1: Read everything at once
         *   Problem: How much to read? TCP is a STREAM, not packets.
         *
         *   Imagine server sends: "0005Hello0003Bye"
         *   If I read 10 bytes, I get: "0005Hello0"
         *   Now what? I'm in the middle of the second message!
         *
         * Alternative 2: Read header first, then body (CHOSEN)
         *   Step 1: Read exactly 4 bytes → "0005"
         *   Step 2: Parse 5 as body length
         *   Step 3: Read exactly 5 bytes → "Hello"
         *   Step 4: Message complete! Start over.
         *
         * 🧠 INSIGHT: Length-prefixed protocols REQUIRE two-phase reading
         *
         * 🎯 EXECUTION VISUALIZATION:
         *
         * Time 0: [Start async_read for 4 bytes]
         *   ↓
         * Time 5ms: [Header arrives: "0013"]
         *   ↓
         * Time 5ms: [Callback fires, decode: body_length = 13]
         *   ↓
         * Time 5ms: [Start async_read for 13 bytes]
         *   ↓
         * Time 12ms: [Body arrives: "Hello, world!"]
         *   ↓
         * Time 12ms: [Message complete, display to user]
         *   ↓
         * Time 12ms: [Recursively call startReceiving()]
         *
         * Notice: Each step is NON-BLOCKING. Main thread continues handling user input.
         */
        boost::asio::async_read(socket,
            boost::asio::buffer(readMessage.data, Message::header),
            [this](boost::system::error_code ec, std::size_t bytes_transferred) {
                /*
                 * 🧠 CALLBACK PSYCHOLOGY:
                 *
                 * This function runs in the FUTURE. When I wrote this code,
                 * the network data didn't exist yet. Now it does.
                 *
                 * Mental model shift required:
                 *   Sequential: "Do A, then B, then C"
                 *   Async: "Start A, when A completes someday, do B"
                 *
                 * 🤔 QUESTION: What if socket gets destroyed before callback runs?
                 * Answer: [this] capture keeps ChatClient alive via socket reference
                 *
                 * More elegant would be: [self = shared_from_this()]
                 * But ChatClient isn't designed for shared ownership currently.
                 */
                if (!ec) {
                    /*
                     * 🎉 SUCCESS PATH: Header arrived successfully
                     *
                     * Now the crucial question: Is this header VALID?
                     *
                     * What could go wrong?
                     *   - Corrupted data: "xyz3" instead of "0003"
                     *   - Network noise: Random bytes
                     *   - Protocol violation: Client sent wrong format
                     *   - Integer overflow: "9999999" (too big)
                     *
                     * decodeHeader() does the validation. If it fails,
                     * we COULD try to recover, but protocol corruption
                     * usually means something fundamental is wrong.
                     *
                     * 🧭 Philosophy: Fail fast on protocol errors
                     */
                    if (readMessage.decodeHeader()) {
                        readBodyData();  // Proceed to phase 2
                    } else {
                        std::cerr << "❌ Invalid message header received" << std::endl;
                        startReceiving(); // Try again - maybe just one bad message
                    }
                } else if (ec != boost::asio::error::operation_aborted) {
                    /*
                     * 💥 FAILURE PATH: Something went wrong
                     *
                     * 🤔 Why check for operation_aborted specifically?
                     *
                     * operation_aborted = Normal shutdown scenario
                     *   - User typed "quit"
                     *   - We called socket.close()
                     *   - Async operations get cancelled
                     *   - No need to alarm the user
                     *
                     * Other errors = Real problems:
                     *   - Server crashed
                     *   - Network cable unplugged
                     *   - Firewall blocked connection
                     *   - User should know about these
                     */
                    std::cerr << "❌ Connection lost: " << ec.message() << std::endl;
                }
            });
    }

private:
    void readBodyData() {
        /*
         * 🎭 THE ASYNC RECEIVING DANCE - Act 2: Body Reading
         *
         * 🎯 MEMORY LAYOUT AWARENESS:
         *
         * After header reading:
         *   readMessage.data = ['0', '0', '1', '3', ?, ?, ?, ...]
         *                       ↑_________________↑
         *                       Header (4 bytes)
         *
         * Now I need to read INTO positions 4-16:
         *   readMessage.data + Message::header = pointer to position 4
         *   getBodyLength() = 13 (from decoded header)
         *
         * After body reading:
         *   readMessage.data = ['0', '0', '1', '3', 'H', 'e', 'l', 'l', 'o', ...]
         *                       ↑_________________↑ ↑_________________________↑
         *                       Header (4 bytes)   Body (13 bytes)
         *
         * 🧠 POINTER ARITHMETIC INSIGHT:
         * Why not just use a separate buffer for body?
         *
         * Because Message class is designed for zero-copy operations:
         *   - Single buffer holds complete message
         *   - No need to concatenate header + body later
         *   - Efficient for sending (just pointer + length)
         *   - Memory layout matches wire format exactly
         */
        boost::asio::async_read(socket,
            boost::asio::buffer(readMessage.data + Message::header, readMessage.getBodyLength()),
            [this](boost::system::error_code ec, std::size_t bytes_transferred) {
                if (!ec) {
                    /*
                     * 🎉 COMPLETE MESSAGE RECEIVED!
                     *
                     * 🤔 PROCESSING QUESTION: What should I do with this message?
                     *
                     * Option 1: Just print it
                     *   Simple, but no room for growth
                     *
                     * Option 2: Parse it for commands (like "/who", "/quit")
                     *   More features, but client gets complex
                     *
                     * Option 3: Forward to a handler interface
                     *   Extensible, but over-engineered for simple chat
                     *
                     * 🧭 Choice: Keep it simple for now, but extract to method
                     *    for future extensibility
                     */
                    std::string messageBody = readMessage.getBody();
                    std::cout << "📩 " << messageBody << std::endl;

                    /*
                     * 🔄 THE ASYNC LOOP:
                     *
                     * This recursive call creates an infinite async loop:
                     *
                     *   startReceiving() → readBodyData() → startReceiving() → ...
                     *
                     * 🤔 Question: Is this safe? Won't it cause stack overflow?
                     *
                     * Answer: NO stack overflow because it's async!
                     *
                     * Call stack visualization:
                     *
                     * Traditional recursion (DANGEROUS):
                     *   main() → recv1() → recv2() → recv3() → ... [STACK GROWS]
                     *
                     * Async recursion (SAFE):
                     *   main() → startReceiving() → [callback scheduled] → main() returns
                     *   [Later...] callback() → startReceiving() → [new callback] → callback() returns
                     *
                     * Each "recursion" actually returns to the event loop!
                     * Stack depth stays constant.
                     */
                    startReceiving();
                } else if (ec != boost::asio::error::operation_aborted) {
                    std::cerr << "❌ Error reading message body: " << ec.message() << std::endl;
                }
            });
    }

public:
    void sendMessage(const std::string& messageText) {
        /*
         * 🚀 MESSAGE SENDING DEEP DIVE:
         *
         * 🤔 DESIGN QUESTION: Should this be async or sync?
         *
         * Option 1: Async sending
         *   + Consistent with receiving side
         *   + Won't block if network is slow
         *   - More complex error handling
         *   - Need to queue messages if user types fast
         *
         * Option 2: Sync sending (CHOSEN)
         *   + Simple: just write and done
         *   + Immediate error feedback
         *   - Could block if network is slow
         *   + But sending is usually fast
         *
         * 🧭 Decision rationale:
         *    Humans type slowly compared to network speed.
         *    If network is so slow that sending blocks noticeably,
         *    the user experience is already terrible anyway.
         *
         * 🎯 MESSAGE CONSTRUCTION ANALYSIS:
         *
         * Message msg(messageText) does:
         *   1. setBodyLength(messageText.size())  → bodyLength_ = 15
         *   2. encodeHeader()                     → data[0-3] = "  15"
         *   3. encodeBody(messageText)            → data[4-18] = "Hello, world!"
         *
         * Result: msg.data = "  15Hello, world!"
         *
         * 📊 WIRE FORMAT VISUALIZATION:
         *
         * User types: "Hello, world!"
         *     ↓
         * Message encoding: [' ', ' ', '1', '5', 'H', 'e', 'l', 'l', 'o', ',', ' ', 'w', 'o', 'r', 'l', 'd', '!']
         *     ↓
         * TCP transmission: These exact bytes sent to server
         *     ↓
         * Server receives: Same 17 bytes, decodes back to "Hello, world!"
         *
         * ✨ BEAUTIFUL SYMMETRY: Encoding and decoding are perfect inverses
         */
        try {
            Message msg(messageText);

            /*
             * 🎯 BUFFER MANAGEMENT INSIGHT:
             *
             * boost::asio::buffer(msg.data, Message::header + msg.getBodyLength())
             *
             * Why not just buffer(msg.data)?
             * Because msg.data is a FIXED SIZE array (516 bytes)
             * But my message might only be 17 bytes.
             *
             * Sending 516 bytes when I only need 17:
             *   - Wastes bandwidth
             *   - Server gets confused by extra garbage bytes
             *   - Protocol violation
             *
             * Sending exact length (header + body):
             *   - Efficient
             *   - Clean protocol compliance
             *   - Server gets exactly what it expects
             */
            boost::asio::write(socket, boost::asio::buffer(msg.data, Message::header + msg.getBodyLength()));

        } catch (std::exception& e) {
            /*
             * 🚨 ERROR HANDLING STRATEGY:
             *
             * What exceptions might happen here?
             *   1. Message constructor: length_error if message too long
             *   2. boost::asio::write: network_error if connection broken
             *
             * 🤔 Should I retry automatically?
             *
             * For message too long: NO, user needs to know
             * For network error: MAYBE, but complexity vs benefit?
             *
             * 🧭 Current choice: Inform user, keep running
             * Alternative: Exit program on network error
             *
             * Depends on user experience goals:
             *   - Robust app: Try to recover
             *   - Simple app: Fail gracefully with clear message
             */
            std::cerr << "❌ Failed to send message: " << e.what() << std::endl;
        }
    }

    void run() {
        /*
         * 🎭 THE GRAND FINALE: Orchestrating Concurrent Operations
         *
         * 🧠 THREADING MODEL DEEP DIVE:
         *
         * Here's where my dual-flow architecture comes together:
         *
         * ┌─────────────────────────────────────────────────────────────┐
         * │                    MAIN THREAD                              │
         * │                                                             │
         * │  while (getline(cin, input))                                │
         * │    ↓                                                        │
         * │  if (input == "quit") break;                                │
         * │    ↓                                                        │
         * │  sendMessage(input);                                        │
         * │    ↓                                                        │
         * │  [BLOCKS waiting for next user input]                      │
         * └─────────────────────────────────────────────────────────────┘
         *
         * ┌─────────────────────────────────────────────────────────────┐
         * │                    IO THREAD                                │
         * │                                                             │
         * │  io.run() → [event loop running]                           │
         * │    ↓                                                        │
         * │  async_read completes → callback fires                     │
         * │    ↓                                                        │
         * │  cout << "📩 " << message << endl;                         │
         * │    ↓                                                        │
         * │  startReceiving() → next async_read                        │
         * └─────────────────────────────────────────────────────────────┘
         *
         * 🤔 CONCURRENCY QUESTION: What about shared data?
         *
         * Shared between threads:
         *   - std::cout (for printing messages)
         *   - socket (IO thread reads, main thread writes)
         *
         * 🛡️ THREAD SAFETY ANALYSIS:
         *   - std::cout: Thread-safe for individual << operations
         *   - socket: boost::asio sockets are thread-safe for
         *     simultaneous read/write (different directions)
         *
         * So no explicit synchronization needed! Clean design.
         *
         * 🧭 ALTERNATIVE ARCHITECTURES CONSIDERED:
         *
         * Alternative 1: Single-threaded with async stdin
         *   Problem: No standard way to do async console input
         *   Platform-specific, complex
         *
         * Alternative 2: Both flows in IO thread
         *   Problem: How to get user input into event loop?
         *   Requires timer polling or complex input handling
         *
         * Alternative 3: Message queue between threads
         *   Overkill for simple chat client
         *   Adds complexity without clear benefit
         *
         * Current choice balances simplicity with functionality perfectly.
         */

        // Flow 1: Start async message receiving in background
        startReceiving();

        // Flow 2: Start IO event loop in separate thread
        std::thread ioThread([this]() {
            /*
             * 🎯 IO THREAD RESPONSIBILITY:
             *
             * This thread OWNS the async event processing.
             * It sits in io.run() and processes:
             *   - Completed async_read operations
             *   - Network connection events
             *   - Timer callbacks (if we had any)
             *   - Socket error conditions
             *
             * 🧠 LIFETIME INSIGHT:
             * [this] capture is safe because:
             *   1. ChatClient object lives until main() returns
             *   2. ioThread.join() ensures thread completes before destruction
             *   3. No dangling references possible
             */
            io.run();
        });

        // Flow 3: Main thread handles user input (blocking)
        std::string input;
        while (std::getline(std::cin, input)) {
            /*
             * 🎯 USER INPUT PROCESSING:
             *
             * This is the synchronous part of my program.
             * getline() BLOCKS until user presses Enter.
             *
             * 🤔 Why is blocking OK here?
             * Because this IS the user interaction thread!
             * It should wait for user input - that's its job.
             *
             * Meanwhile, server messages continue flowing
             * through the IO thread completely independently.
             */
            if (input == "quit" || input == "exit") {
                std::cout << "👋 Disconnecting..." << std::endl;
                break;
            }

            if (!input.empty()) {
                sendMessage(input);
            }
        }

        /*
         * 🧹 GRACEFUL SHUTDOWN SEQUENCE:
         *
         * 1. socket.close() → Cancels pending async operations
         * 2. io.stop() → Tells io.run() to exit event loop
         * 3. ioThread.join() → Wait for IO thread to finish cleanup
         *
         * Order matters! If I join() before stop(), thread might hang.
         * If I stop() before close(), callbacks might access closed socket.
         *
         * This sequence ensures clean shutdown without resource leaks.
         */
        socket.close();  // Cancel async operations
        io.stop();       // Exit event loop
        ioThread.join(); // Wait for IO thread completion
    }
};

/*
 * 🎯 MAIN FUNCTION: The Entry Point
 *
 * 🤔 DESIGN PHILOSOPHY: Keep main() as simple as possible
 *
 * main() responsibilities:
 *   1. Validate command line arguments
 *   2. Create and configure ChatClient
 *   3. Handle top-level exceptions
 *   4. Return appropriate exit codes
 *
 * Everything else delegated to ChatClient methods.
 * This separation makes testing and reuse easier.
 */
int main(int argc, char* argv[]) {
    /*
     * 📝 ARGUMENT VALIDATION:
     *
     * Why require exactly 3 arguments?
     * argv[0] = program name ("./client")
     * argv[1] = host ("localhost" or "192.168.1.5")
     * argv[2] = port ("8080")
     *
     * Total: argc = 3
     *
     * 🧭 Design choice: Fail fast with clear usage message
     * Better than trying to guess defaults and confusing user.
     */
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <host> <port>" << std::endl;
        std::cerr << "Example: " << argv[0] << " localhost 8080" << std::endl;
        return 1;
    }

    try {
        /*
         * 🎭 THE HAPPY PATH:
         *
         * If everything goes right:
         *   1. ChatClient constructor succeeds
         *   2. connect() establishes TCP connection
         *   3. run() starts dual-threaded operation
         *   4. User chats happily
         *   5. User types "quit"
         *   6. run() returns, destructor cleans up
         *   7. main() returns 0 (success)
         */
        ChatClient client(argv[1], argv[2]);
        client.connect();
        client.run();

    } catch (std::exception& e) {
        /*
         * 🚨 THE ERROR PATH:
         *
         * What exceptions might reach here?
         *   - DNS resolution failure (bad hostname)
         *   - Connection refused (server not running)
         *   - Network unreachable (firewall, routing)
         *   - Permission denied (trying to use privileged port)
         *
         * 🧭 Philosophy: Don't try to handle network errors in main()
         * Let connect() throw, catch here, show user-friendly message.
         *
         * Return code 1 = error (shell scripting convention)
         */
        std::cerr << "❌ Client error: " << e.what() << std::endl;
        return 1;
    }

    return 0;  // Success!
}

/*
 * ============================================================================
 * REFLECTION: What I Learned Building This Client
 * ============================================================================
 *
 * 🧠 BIGGEST INSIGHTS:
 *
 * 1. **Async is viral**: Once you go async for networking, everything else
 *    needs to be designed around it. You can't just "add async later."
 *
 * 2. **Threading models matter**: The "IO thread + UI thread" pattern is
 *    incredibly powerful and simple. Each thread has a clear responsibility.
 *
 * 3. **Protocol design affects everything**: The choice to use length-prefixed
 *    messages (instead of newline-delimited) rippled through the entire
 *    architecture. Good protocols make client code clean.
 *
 * 4. **Error handling is hard**: Network programming has so many failure modes.
 *    The key is deciding which errors to recover from vs. which to report.
 *
 * 🎯 WHAT I'D DO DIFFERENTLY:
 *
 * - Add automatic reconnection for network failures
 * - Support for message history scrollback
 * - Better handling of large messages (progress indicators)
 * - Configuration file for default host/port
 * - Unit tests for the Message protocol handling
 *
 * 🚀 WHAT I'M PROUD OF:
 *
 * - Clean separation between networking and UI concerns
 * - Robust error handling without over-engineering
 * - Efficient protocol implementation (zero-copy where possible)
 * - Threading model that's simple but effective
 *
 * This client went from "just send messages" to a thoughtful piece of
 * software that handles the real complexities of network programming.
 * ============================================================================
 */
