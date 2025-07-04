#include "chatRoom.hpp"
#include <iostream>

/*
 * ============================================================================
 * CHAT ROOM SERVER - The Journey from Sequential to Async Thinking
 * ============================================================================
 *
 * THE CENTRAL QUESTION: How do you coordinate multiple network clients
 *    in a single-threaded, async environment while maintaining data consistency
 *    and object safety?
 *
 * MY MENTAL MODEL EVOLUTION:
 *
 *    Stage 1: "I'll handle clients one at a time"
 *    └─ Reality: Second client can't connect while first is idle
 *
 *    Stage 2: "I'll use threads - one per client"
 *    └─ Reality: Race conditions, memory corruption, debugging nightmares
 *
 *    Stage 3: "I'll use async I/O with callbacks"
 *    └─ Reality: Object lifetime becomes the hardest problem
 *
 *    Stage 4: "I'll design around object lifetime from the start"
 *    └─ Success: Clean, scalable architecture emerges
 *
 *  THE THREE CORE REALIZATIONS:
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │ REALIZATION 1: Async Programming is Inverse Control                    │
 * │                                                                         │
 * │ Traditional: "Do A, then B, then C"                                    │
 * │     main() → accept() → read() → process() → write() → repeat          │
 * │                                                                         │
 * │ Async: "Start A, B, C; when any completes, do next step"              │
 * │     main() → start_accept() → return                                   │
 * │     [Later] callback_A() → start_next_A() → return                     │
 * │     [Later] callback_B() → start_next_B() → return                     │
 * │                                                                         │
 * │  Insight: Program flow becomes event-driven, not sequential          │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │ REALIZATION 2: Object Lifetime Determines Architecture                 │
 * │                                                                         │
 * │ Problem: Callbacks execute AFTER the function that started them        │
 * │                                                                         │
 * │ Dangerous pattern:                                                      │
 * │   Session session;                                                      │
 * │   async_read(&session.buffer, callback);  // callback captures &session│
 * │   return;  // session destroyed!                                       │
 * │   [Later] callback runs → accesses destroyed session → CRASH           │
 * │                                                                         │
 * │ Safe pattern:                                                           │
 * │   auto session = make_shared<Session>();                               │
 * │   async_read(session->buffer, [session](){ ... });  // session alive   │
 * │   return;  // session kept alive by callback                           │
 * │                                                                         │
 * │  Insight: shared_ptr becomes mandatory, not optional                 │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │ REALIZATION 3: Coordination Patterns Emerge Naturally                 │
 * │                                                                         │
 * │  How do I broadcast a message to multiple clients?                   │
 * │                                                                         │
 * │ Bad: Each Session knows about all other Sessions                       │
 * │   Problem: N×N dependencies, tight coupling                            │
 * │                                                                         │
 * │ Good: Central Room coordinates all Sessions                             │
 * │   Pattern: Mediator - Room mediates between Participants               │
 * │   Benefits: Loose coupling, single source of truth                     │
 * │                                                                         │
 * │  Insight: Design patterns aren't academic - they solve real problems │
 * └─────────────────────────────────────────────────────────────────────────┘
 */

// ============================================================================
// ROOM IMPLEMENTATION - The Mediator Pattern in Action
// ============================================================================

void Room::join(ParticipantPtr participant) {
    /*
     *  ROOM MEMBERSHIP - The Art of Managing Dynamic Collections
     *
     *  FUNDAMENTAL QUESTION: How do I safely manage a dynamic collection
     *    of network participants in an async environment?
     *
     *  MY DESIGN JOURNEY:
     *
     *    First attempt: vector<Session*>
     *    └─ Problem: How to remove without O(n) search?
     *
     *    Second attempt: unordered_set<Session*>
     *    └─ Problem: Hash function for pointers is implementation-defined
     *
     *    Final choice: set<shared_ptr<Participant>>
     *    └─ Benefits: Automatic ordering, fast insert/remove, deduplication
     *
     *  CONTAINER CHOICE ANALYSIS:
     *
     * ┌─────────────────────────────────────────────────────────────────────┐
     * │ Why std::set over other containers?                                 │
     * │                                                                     │
     * │ vector<ParticipantPtr>:                                             │
     * │   + Cache-friendly iteration                                        │
     * │   - O(n) removal                                                    │
     * │   - No automatic deduplication                                      │
     * │                                                                     │
     * │ unordered_set<ParticipantPtr>:                                      │
     * │   + O(1) average insert/remove                                      │
     * │   - Hash function for shared_ptr is pointer address                 │
     * │   - Hash collisions possible                                        │
     * │                                                                     │
     * │ set<ParticipantPtr> (CHOSEN):                                       │
     * │   + O(log n) insert/remove (good enough for chat rooms)            │
     * │   + Automatic deduplication                                         │
     * │   + Deterministic iteration order                                   │
     * │   + No hash function needed                                         │
     * │                                                                     │
     * │  Decision: Chat rooms rarely exceed 100 people.                  │
     * │    O(log n) vs O(1) doesn't matter, but reliability does.          │
     * └─────────────────────────────────────────────────────────────────────┘
     */
    participants.insert(participant);

    /*
     *  UX PSYCHOLOGY: The "Ghost Town" Problem
     *
     *  USER EXPERIENCE QUESTION: What happens when someone joins an empty room?
     *
     * Scenario visualization:
     *   Time 10:00: Alice joins empty room
     *               Alice sees: [nothing]
     *               Alice thinks: "Is this working? Anyone here?"
     *
     *   Time 10:05: Bob joins
     *               Bob sees: [nothing]
     *               Both Alice and Bob feel isolated
     *
     *   Time 10:10: Alice says "Hello?"
     *               Bob sees: "Hello?"
     *               Bob realizes Alice was here all along!
     *
     *  THE INSIGHT: Context matters more than real-time
     *
     * Solution: Show recent message history to new joiners
     *   - They see conversation context immediately
     *   - They know the room is active (or inactive)
     *   - They can jump into existing conversations
     *
     *  IMPLEMENTATION DECISIONS:
     *
     * ┌─────────────────────────────────────────────────────────────────────┐
     * │ How much history to show?                                           │
     * │                                                                     │
     * │ Option 1: All history                                               │
     * │   Problem: Could be thousands of messages, overwhelming             │
     * │                                                                     │
     * │ Option 2: Last N messages (CHOSEN)                                  │
     * │   Benefits: Manageable context, shows recent conversation flow      │
     * │   Implementation: MessageQueue.size() limited to 50                 │
     * │                                                                     │
     * │ Option 3: History from last X minutes                               │
     * │   Problem: Complexity of timestamp management                       │
     * │                                                                     │
     * │  Choice: Simple is better. 50 messages ≈ 5-10 minutes of chat    │
     * └─────────────────────────────────────────────────────────────────────┘
     *
     *  ASYNC SAFETY NOTE:
     * This loop is safe because:
     *   1. MessageQueue is owned by Room (single-threaded access)
     *   2. participant->deliver() might trigger async operations
     *   3. But those async ops can't modify MessageQueue (different object)
     *   4. Iterator stays valid throughout loop
     */
    for (const auto& msg : MessageQueue) {
        participant->deliver(msg);
    }
}

void Room::leave(ParticipantPtr participant) {
    /*
     *  DEPARTURE MANAGEMENT - Handling the Chaos of Network Disconnections
     *
     *  RELIABILITY QUESTION: How do I handle disconnections gracefully
     *    when networks are fundamentally unreliable?
     *
     *  THE REALITY OF NETWORK DISCONNECTIONS:
     *
     * Graceful departures (rare):
     *   - User clicks "disconnect" button
     *   - Client sends proper goodbye message
     *   - TCP connection closes cleanly
     *   - Everyone is happy
     *
     * Ungraceful departures (common):
     *   - Network cable unplugged
     *   - Laptop goes to sleep
     *   - WiFi drops out
     *   - Process killed with SIGKILL
     *   - Power outage
     *   - Phone runs out of battery
     *
     *  DESIGN INSIGHT: Plan for chaos, not perfection
     *
     *  ERROR HANDLING PHILOSOPHY:
     *
     * ┌─────────────────────────────────────────────────────────────────────┐
     * │ Defensive Programming in Network Code                               │
     * │                                                                     │
     * │ Principle 1: Assume operations might fail                          │
     * │   - Network calls can timeout                                       │
     * │   - Connections can break mid-operation                             │
     * │   - Objects might be destroyed during async operations              │
     * │                                                                     │
     * │ Principle 2: Make operations idempotent                            │
     * │   - Calling leave() multiple times should be safe                  │
     * │   - Removing non-existent participant should not crash              │
     * │   - Operations should clean up after themselves                     │
     * │                                                                     │
     * │ Principle 3: Fail gracefully                                       │
     * │   - Log errors, don't crash                                         │
     * │   - Partial failures shouldn't affect other participants           │
     * │   - System should continue operating                                │
     * │                                                                     │
     * │  std::set::erase() embodies these principles perfectly            │
     * └─────────────────────────────────────────────────────────────────────┘
     *
     * Why participants.erase(participant) is perfect:
     *
     *   1. Idempotent:
     *      - erase(non_existent_item) → no effect, no exception
     *      - erase(item); erase(item); → safe, second call does nothing
     *
     *   2. Exception safe:
     *      - std::set::erase() is noexcept for iterator/key versions
     *      - No memory allocation failures possible
     *
     *   3. Efficient:
     *      - O(log n) lookup and removal
     *      - No iterator invalidation for other elements
     *
     *   4. Automatic cleanup:
     *      - Removing shared_ptr decrements reference count
     *      - If this was the last reference, Session destructor runs
     *      - Socket closes, resources freed automatically
     *
     *  ASYNC SAFETY CONSIDERATION:
     *
     * What if participant has pending async operations when removed?
     *
     * Safe scenario:
     *   1. Session has pending async_read operation
     *   2. Room::leave() removes Session from participants set
     *   3. async_read callback still executes later (Session kept alive)
     *   4. Callback tries to call room.deliver() but Session no longer in set
     *   5. Message not broadcast (correct behavior for disconnected client)
     *   6. Session destructor eventually runs when all callbacks complete
     *
     * This is why shared_ptr + async design is so powerful!
     */
    participants.erase(participant);
}

void Room::deliver(ParticipantPtr sender, const Message& msg) {
    /*
     *  MESSAGE BROADCASTING - The Heart of Real-Time Communication
     *
     *  CENTRAL COORDINATION QUESTION: How do I efficiently distribute
     *    one message to N participants while maintaining consistency
     *    and user experience quality?
     *
     *  THE MEDIATOR PATTERN EMERGENCE:
     *
     * Why not peer-to-peer? (Session → Session directly)
     *   Problem 1: N×N connection complexity
     *   Problem 2: Each Session needs to know about all others
     *   Problem 3: No central message ordering
     *   Problem 4: Difficult to add features (logging, filtering, etc.)
     *
     * Why central coordination? (Session → Room → Sessions)
     *   Benefit 1: Single source of truth for message ordering
     *   Benefit 2: Loose coupling - Sessions only know Room
     *   Benefit 3: Easy to add features (history, moderation, etc.)
     *   Benefit 4: Simple broadcast logic
     *
     *  THREE-PHASE DELIVERY ALGORITHM:
     *
     * Phase 1: Archive for History
     * Phase 2: Memory Management
     * Phase 3: Real-time Broadcast
     */

    /*
     *  PHASE 1: MESSAGE ARCHIVAL
     *
     *  PERSISTENCE QUESTION: How long should messages live?
     *
     * ┌─────────────────────────────────────────────────────────────────────┐
     * │ Message Lifecycle Design Decisions                                  │
     * │                                                                     │
     * │ Option 1: Keep forever                                              │
     * │   + Complete history available                                      │
     * │   - Memory grows unbounded → eventual crash                         │
     * │   - Performance degrades as history grows                           │
     * │                                                                     │
     * │ Option 2: Time-based expiry (keep last N hours)                    │
     * │   + Predictable memory usage                                        │
     * │   - Complexity of timestamp management                              │
     * │   - What if room is quiet for hours?                                │
     * │                                                                     │
     * │ Option 3: Count-based sliding window (CHOSEN)                      │
     * │   + Simple to implement                                             │
     * │   + Bounded memory usage                                            │
     * │   + Always relevant recent context                                  │
     * │   - Busy rooms lose history faster                                  │
     * │                                                                     │
     * │  Decision: Simplicity and reliability > perfect history          │
     * └─────────────────────────────────────────────────────────────────────┘
     */
    MessageQueue.push_back(msg);

    /*
     *  PHASE 2: MEMORY MANAGEMENT
     *
     *  WHY 50 MESSAGES? The Psychology of Conversation Context
     *
     * Research insights:
     *   - Human working memory: ~7±2 items
     *   - Conversation context window: ~5-15 messages
     *   - "Overwhelming" threshold: >100 messages for newcomers
     *
     * Practical considerations:
     *   - Average message length: ~30 bytes
     *   - 50 messages ≈ 1.5KB memory per room
     *   - Typical conversation pace: 1-5 messages/minute
     *   - 50 messages ≈ 10-50 minutes of context
     *
     *  CONTAINER CHOICE ANALYSIS:
     *
     * ┌─────────────────────────────────────────────────────────────────────┐
     * │ Why std::deque for message storage?                                 │
     * │                                                                     │
     * │ vector<Message>:                                                    │
     * │   + Cache-friendly iteration                                        │
     * │   - O(n) pop_front() → expensive for sliding window                 │
     * │                                                                     │
     * │ list<Message>:                                                      │
     * │   + O(1) insert/remove anywhere                                     │
     * │   - Cache-unfriendly iteration                                      │
     * │   - Higher memory overhead per element                              │
     * │                                                                     │
     * │ deque<Message> (CHOSEN):                                            │
     * │   + O(1) push_back() and pop_front()                               │
     * │   + Cache-friendly iteration                                        │
     * │   + Perfect for sliding window pattern                              │
     * │   - Slightly more complex than vector                               │
     * │                                                                     │
     * │  deque is tailor-made for sliding window scenarios               │
     * └─────────────────────────────────────────────────────────────────────┘
     */
    if (MessageQueue.size() > 50) {
        MessageQueue.pop_front();  // O(1) sliding window operation
    }

    /*
     *  PHASE 3: REAL-TIME BROADCASTING
     *
     *  UX QUESTION: Should the sender receive their own message back?
     *
     *  USER EXPERIENCE SCENARIOS:
     *
     * Scenario A: Send message back to sender
     *   Time 0: User types "hello" and hits Enter
     *   Time 0: Client shows "hello" in chat (immediate feedback)
     *   Time 50ms: Server processes message, sends to all clients
     *   Time 100ms: Same client receives "hello" from server
     *   Time 100ms: Client shows "hello" again → DUPLICATE!
     *   User reaction: "Bug! My message appeared twice!"
     *
     * Scenario B: Don't send message back to sender (CHOSEN)
     *   Time 0: User types "hello" and hits Enter
     *   Time 0: Client shows "hello" in chat (immediate feedback)
     *   Time 50ms: Server processes message, sends to OTHER clients only
     *   Time 100ms: Other clients show "hello"
     *   User reaction: "My message appeared, others can see it. Perfect!"
     *
     *  INSIGHT: Client-side immediate feedback + server-side deduplication
     *            = optimal perceived responsiveness
     *
     *  ASYNC BROADCAST SAFETY:
     *
     * Key insight: This loop might trigger many async operations
     * (each participant->deliver() could start async_write), but
     * it's safe because:
     *
     *   1. We iterate over a snapshot of participants
     *   2. Each deliver() call is independent
     *   3. If a participant disconnects during iteration,
     *      their async operations complete safely
     *   4. Room state remains consistent
     *
     * The magic of async: starting many operations is fast,
     * even if completing them takes time.
     */
    for (auto participant : participants) {
        if (participant != sender) {
            participant->deliver(msg);  // Might start async_write operation
        }
    }
}

// ============================================================================
// SESSION IMPLEMENTATION - Where Async Programming Gets Mind-Bending
// ============================================================================

Session::Session(tcp::socket socket, Room& room)
    : clientSocket(std::move(socket)), room(room) {
    /*
     *  CONSTRUCTOR PHILOSOPHY - The Async Object Creation Dilemma
     *
     *  FUNDAMENTAL QUESTION: When should an object become "active"?
     *
     *  THE ASYNC CONSTRUCTOR PARADOX:
     *
     * Traditional objects: Constructor does everything
     *   class FileReader {
     *       FileReader(string filename) {
     *           file.open(filename);        // Immediate I/O
     *           content = file.read_all();  // Blocks until complete
     *       }
     *   };
     *
     * Async objects: Constructor CANNOT do everything
     *   class Session {
     *       Session(socket s) {
     *           async_read(s, callback);     // ❌ CRASHES!
     *           // shared_from_this() not available yet
     *           // Object not fully constructed
     *       }
     *   };
     *
     *  THE TWO-PHASE CONSTRUCTION PATTERN:
     *
     * ┌─────────────────────────────────────────────────────────────────────┐
     * │ Phase 1: Constructor (Initialization Only)                         │
     * │   - Initialize member variables                                     │
     * │   - Set up basic state                                              │
     * │   - NO async operations                                             │
     * │   - NO shared_from_this() calls                                     │
     * │                                                                     │
     * │ Phase 2: start() method (Activation)                               │
     * │   - Object fully constructed, shared_ptr available                 │
     * │   - Safe to call shared_from_this()                                │
     * │   - Begin async operations                                          │
     * │   - Join Room, start reading                                        │
     * │                                                                     │
     * │  This pattern separates "existence" from "activity"              │
     * └─────────────────────────────────────────────────────────────────────┘
     *
     *  RESOURCE OWNERSHIP ANALYSIS:
     *
     * clientSocket:
     *   - Moved from parameter (transfer ownership)
     *   - std::move() prevents copying (sockets aren't copyable anyway)
     *   - Session now owns the network connection
     *   - Destructor will automatically close socket
     *
     * room:
     *   - Stored as reference (no ownership)
     *   - Room outlives all Sessions (created in main(), destroyed at exit)
     *   - Sessions participate in Room, but don't control Room lifecycle
     *   - Reference semantics prevent accidental copying
     *
     *  INSIGHT: Constructor sets up ownership relationships
     *            but doesn't start any async operations
     */
}

void Session::deliver(const Message& msg) {
    /*
     *  MESSAGE DELIVERY - The Async Write Coordination Problem
     *
     *  CONCURRENCY QUESTION: How do I safely send multiple messages
     *    to a client when async operations overlap in time?
     *
     *  THE ASYNC WRITE HAZARD:
     *
     * What I wanted to do (WRONG):
     *   void deliver(Message msg1) {
     *       async_write(socket, msg1, callback1);
     *   }
     *   void deliver(Message msg2) {
     *       async_write(socket, msg2, callback2);  // ❌ DISASTER!
     *   }
     *
     * What actually happens:
     *   Time 0: Start writing msg1 ("Hello")
     *   Time 5: Start writing msg2 ("World")
     *   Time 10: Both writes complete simultaneously
     *   Client receives: "HeWorlldlo" or "WoHellorld" or any interleaving!
     *
     *  THE FUNDAMENTAL INSIGHT: TCP sockets are NOT thread-safe for writes
     *
     * ┌─────────────────────────────────────────────────────────────────────┐
     * │ Why Serial Write Queue Pattern is Essential                        │
     * │                                                                     │
     * │ Problem: Multiple async_write() calls on same socket               │
     * │   - Bytes from different writes can interleave                     │
     * │   - Protocol corruption (headers mixed with bodies)                │
     * │   - Impossible to debug (race conditions)                          │
     * │                                                                     │
     * │ Solution: Serialize writes through a queue                         │
     * │   - Only ONE async_write active at a time                          │
     * │   - Queue holds pending messages                                    │
     * │   - When write completes, start next write from queue              │
     * │                                                                     │
     * │ Benefits:                                                           │
     * │   + Message integrity guaranteed                                    │
     * │   + Protocol compliance maintained                                  │
     * │   + Backpressure handling (slow clients don't break fast ones)     │
     * │                                                                     │
     * │  Pattern: Producer-Consumer with Single Consumer                 │
     * └─────────────────────────────────────────────────────────────────────┘
     *
     *  MESSAGE QUEUE STATE MACHINE:
     *
     * State 1: Queue Empty, No Active Write
     *   deliver() called → add to queue → start async_write()
     *   Queue: [msg] → Writing: msg
     *
     * State 2: Queue Has Items, Write Active
     *   deliver() called → add to queue → do nothing (write in progress)
     *   Queue: [msg1, msg2] → Writing: msg1
     *
     * State 3: Write Completes, Queue Has More Items
     *   callback() → remove completed → start next async_write()
     *   Queue: [msg2] → Writing: msg2
     *
     * State 4: Write Completes, Queue Empty
     *   callback() → remove completed → idle state
     *   Queue: [] → Writing: none
     */
    outgoingMessages.push_back(msg);

    /*
     *  QUEUE LENGTH ANALYSIS - The Write State Detection Pattern
     *
     *  HOW DO I KNOW if an async_write() is currently active?
     *
     * Traditional approach: boolean flag
     *   bool writing_active = false;
     *   if (!writing_active) {
     *       writing_active = true;
     *       async_write(...);
     *   }
     *   // In callback: writing_active = false;
     *
     * Elegant approach: Queue length analysis (CHOSEN)
     *   - Queue length == 1 → Was empty, now has 1 → Start writing
     *   - Queue length > 1 → Was not empty → Write already active
     *
     *  INSIGHT: The queue length IMPLICITLY encodes the write state!
     *
     * Benefits of queue-length approach:
     *   + One less state variable to manage
     *   + Impossible to get flag out of sync with reality
     *   + Self-documenting: queue size tells the whole story
     *   + Thread-safe by design (single-threaded async context)
     *
     *  EXECUTION VISUALIZATION:
     *
     * Call 1: deliver(msg_A)
     *   outgoingMessages = [msg_A]  (size == 1)
     *   No write active → start async_write(msg_A)
     *
     * Call 2: deliver(msg_B) [while msg_A still writing]
     *   outgoingMessages = [msg_A, msg_B]  (size == 2)
     *   Write active → do nothing, just queue
     *
     * Callback: msg_A write completes
     *   outgoingMessages = [msg_B]  (after pop_front)
     *   Queue not empty → start async_write(msg_B)
     */
    if (outgoingMessages.size() == 1) {
        async_write();  // Begin the write state machine
    }
}

void Session::write(Message& msg) {
    /*
     *  OUTBOUND MESSAGE FLOW - From Client to the World
     *
     *  DIRECTION QUESTION: How does data flow in a bidirectional system?
     *
     *  THE TWO DATA FLOWS IN CHAT SYSTEMS:
     *
     * ┌─────────────────────────────────────────────────────────────────────┐
     * │ INBOUND FLOW: World → Client                                        │
     * │                                                                     │
     * │ Other Session → Room → deliver() → this Session → async_write()    │
     * │                                          ↓                          │
     * │                                   Client Socket                     │
     * │                                          ↓                          │
     * │                                   Client receives                   │
     * │                                                                     │
     * │  This Session acts as RECEIVER                                    │
     * └─────────────────────────────────────────────────────────────────────┘
     *
     * ┌─────────────────────────────────────────────────────────────────────┐
     * │ OUTBOUND FLOW: Client → World (THIS METHOD)                        │
     * │                                                                     │
     * │ Client sends → Client Socket → async_read() → this Session         │
     * │                                          ↓                          │
     * │                                     write() ← You are here         │
     * │                                          ↓                          │
     * │                                    Room.deliver()                   │
     * │                                          ↓                          │
     * │                              Other Sessions receive                 │
     * │                                                                     │
     * │  This Session acts as SENDER                                      │
     * └─────────────────────────────────────────────────────────────────────┘
     *
     *  ARCHITECTURAL INSIGHT: Session is a bidirectional adapter
     *
     * Session responsibilities:
     *   1. Translate between network protocol and Room interface
     *   2. Handle one client's connection lifecycle
     *   3. Queue outbound messages for reliable delivery
     *   4. Parse inbound messages from raw bytes
     *
     * Room responsibilities:
     *   1. Coordinate message distribution
     *   2. Maintain participant list
     *   3. Store message history
     *   4. Apply business logic (filtering, moderation, etc.)
     *
     *  THE DELEGATION PATTERN:
     *
     * This method is beautifully simple because it follows the
     * Single Responsibility Principle:
     *
     * Session's job: "I represent one client connection"
     * Room's job: "I coordinate all clients"
     *
     * Session doesn't need to know:
     *   - How many other clients exist
     *   - Where to send the message
     *   - Message history management
     *   - Broadcast algorithms
     *
     * Session just says: "Room, here's a message from my client.
     * You figure out what to do with it."
     *
     *  SHARED_PTR NECESSITY:
     *
     * shared_from_this() is critical here because:
     *   1. Room.deliver() might trigger async operations on other Sessions
     *   2. Those operations might complete AFTER this method returns
     *   3. Room needs to identify which Session sent the message
     *   4. shared_ptr ensures this Session stays alive during Room operations
     *   5. Even if client disconnects, Room can safely exclude sender
     */
    room.deliver(shared_from_this(), msg);
}

void Session::start() {
    /*
     *  SESSION ACTIVATION - The Two-Phase Construction Pattern
     *
     *  CONSTRUCTION QUESTION: Why separate object creation from activation?
     *
     *  THE SHARED_PTR CONSTRUCTION PARADOX:
     *
     * What I wanted to do (BROKEN):
     *   Session::Session(socket, room) {
     *       room.join(shared_from_this());  // ❌ CRASH!
     *       async_read();                   // ❌ CRASH!
     *   }
     *
     * Why it crashes:
     *   1. shared_from_this() only works if object is owned by shared_ptr
     *   2. During constructor, shared_ptr isn't fully constructed yet
     *   3. enable_shared_from_this checks if weak_ptr is valid
     *   4. weak_ptr is only valid AFTER shared_ptr construction completes
     *   5. Constructor runs DURING shared_ptr construction → weak_ptr invalid
     *
     *  THE TWO-PHASE CONSTRUCTION SOLUTION:
     *
     * ┌─────────────────────────────────────────────────────────────────────┐
     * │ Phase 1: Object Construction (Constructor)                          │
     * │   auto session = make_shared<Session>(socket, room);                │
     * │   ↑ shared_ptr is being constructed                                 │
     * │   ↑ Constructor runs, initializes members                           │
     * │   ↑ NO async operations, NO shared_from_this()                      │
     * │   ↑ Object exists but is INACTIVE                                   │
     * │                                                                     │
     * │ Phase 2: Object Activation (start() method)                        │
     * │   session->start();                                                 │
     * │   ↑ shared_ptr fully constructed, weak_ptr valid                    │
     * │   ↑ shared_from_this() safe to call                                 │
     * │   ↑ Begin async operations                                          │
     * │   ↑ Object becomes ACTIVE                                           │
     * │                                                                     │
     * │  This pattern is ubiquitous in async C++ programming             │
     * └─────────────────────────────────────────────────────────────────────┘
     *
     *  WHY THIS PATTERN IS NECESSARY IN ASYNC CODE:
     *
     * Async operations need object lifetime guarantees:
     *   1. async_read() callback might execute MUCH later
     *   2. Callback needs Session object to still exist
     *   3. Only shared_ptr can provide this guarantee
     *   4. But shared_ptr isn't available during construction
     *   5. Therefore: construct first, activate second
     *
     *  ALTERNATIVE PATTERNS CONSIDERED:
     *
     * Alternative 1: Static factory method
     *   static shared_ptr<Session> create(socket, room) {
     *       auto session = make_shared<Session>(socket, room);
     *       session->start();  // Activate immediately
     *       return session;
     *   }
     *   Problem: Less flexible, hides two-phase nature
     *
     * Alternative 2: Promise/Future pattern
     *   Complex, overkill for this use case
     *
     * Alternative 3: Raw pointers (DANGEROUS)
     *   Lifetime management becomes manual and error-prone
     *
     * Current pattern: Simple, explicit, safe
     */

    /*
     *  PHASE 1: JOIN THE ROOM COMMUNITY
     *
     * Why join the room first?
     *   1. Room.join() delivers message history immediately
     *   2. async_read() will soon deliver incoming messages
     *   3. Want history BEFORE new messages for proper ordering
     *   4. User sees context, then real-time flow
     *
     *  SHARED_PTR SAFETY:
     * shared_from_this() is now safe because:
     *   - Constructor has completed
     *   - shared_ptr is fully constructed
     *   - weak_ptr inside enable_shared_from_this is valid
     *   - Room can safely store the shared_ptr
     */
    room.join(shared_from_this());

    /*
     *  PHASE 2: START LISTENING FOR CLIENT MESSAGES
     *
     * This begins the async message reading state machine:
     *   async_read() → readMessageBody() → async_read() → ...
     *
     *  INSIGHT: This is where the Session becomes "alive"
     *
     * Before this call:
     *   - Session exists but is dormant
     *   - No network activity
     *   - Room knows about us but we're not processing messages
     *
     * After this call:
     *   - Session is actively processing incoming data
     *   - Async callbacks keep Session alive via shared_ptr
     *   - Message flow begins: network → Session → Room → other Sessions
     *
        *  THE ASYNC LOOP BEGINS:
     * This single call starts an infinite async loop that only ends when:
     *   1. Client disconnects (network error)
     *   2. Server shuts down (io_context stops)
     *   3. Unrecoverable protocol error occurs
     */
    async_read();
}

void Session::async_read() {
    /*
     * This is where async programming gets mind-bending. I'm not "reading" -
     * I'm "starting a read" that will complete later.
     *
     * The function returns immediately, but the read happens in the background.
     * When data arrives, the callback function runs.
     */

    auto self = shared_from_this();
    /*
     * Critical insight: The callback will run LATER, maybe much later.
     * What if this Session object gets deleted before then? The callback
     * would access garbage memory.
     *
     * By capturing 'self', the callback holds a reference to this Session,
     * keeping it alive until the callback completes. This is the fundamental
     * pattern for safe async programming.
     */

    // Step 1: Read the 4-byte header first
    boost::asio::async_read(clientSocket,
        boost::asio::buffer(incomingMessage.data, Message::header),
        [this, self](boost::system::error_code ec, std::size_t bytes_transferred) {
            if (!ec) {
                /*
                 * Success! Header arrived. Now decode it to get body length.
                 * The Message protocol uses length-prefixed messages.
                 */
                if (incomingMessage.decodeHeader()) {
                    // Header is valid, now read the body
                    readMessageBody();
                } else {
                    // Invalid header - disconnect this client
                    std::cout << "Invalid message header from client" << std::endl;
                    room.leave(shared_from_this());
                }
            } else {
                /*
                 * Something went wrong. Either the client disconnected cleanly
                 * (EOF) or there was a network error.
                 *
                 * Either way, I need to leave the room and clean up.
                 */
                room.leave(shared_from_this());
                if (ec == boost::asio::error::eof) {
                    std::cout << "Client disconnected" << std::endl;
                } else {
                    std::cout << "Read error: " << ec.message() << std::endl;
                }
            }
        });
}

void Session::readMessageBody() {
    /*
     * Step 2: Read the message body based on the length from header
     */
    auto self = shared_from_this();

    boost::asio::async_read(clientSocket,
        boost::asio::buffer(incomingMessage.data + Message::header, incomingMessage.getBodyLength()),
        [this, self](boost::system::error_code ec, std::size_t bytes_transferred) {
            if (!ec) {
                /*
                 * Success! Complete message received. Extract the body text
                 * and send it to the room for broadcasting.
                 */
                write(incomingMessage);

                /*
                 * Keep listening for more messages. This recursive call creates
                 * an "async loop" - each completion triggers the next read.
                 */
                async_read();
            } else {
                /*
                 * Read failed. Client probably disconnected.
                 */
                room.leave(shared_from_this());
                std::cout << "Read body error: " << ec.message() << std::endl;
            }
        });
}

void Session::async_write() {
    /*
     * Time to send a message to my client. But first, do I have anything to send?
     */
    if (outgoingMessages.empty()) {
        return;  // Queue is empty, nothing to do
    }

    auto self = shared_from_this();
    const Message& msg = outgoingMessages.front();

    /*
     * Send the complete Message protocol data: [4-byte header][body]
     * No newlines needed - the length prefix handles message boundaries.
     */
    size_t totalLength = Message::header + msg.getBodyLength();

    boost::asio::async_write(clientSocket,
        boost::asio::buffer(msg.data, totalLength),
        [this, self](boost::system::error_code ec, std::size_t bytes_transferred) {
            if (!ec) {
                /*
                 * Message sent successfully. Remove it from the queue.
                 */
                outgoingMessages.pop_front();

                /*
                 * Are there more messages waiting? If so, send the next one.
                 * This creates a chain of writes that processes the entire
                 * queue without blocking.
                 */
                if (!outgoingMessages.empty()) {
                    async_write();
                }
            } else {
                /*
                 * Write failed. Client probably disconnected.
                 * Clean up and leave the room.
                 */
                std::cout << "Write error: " << ec.message() << std::endl;
                room.leave(shared_from_this());
            }
        });
}

// ============================================================================
// SERVER INFRASTRUCTURE
// ============================================================================

void start_accept(tcp::acceptor& acceptor, Room& room) {
    /*
     * I need to continuously accept new connections. But accept() is blocking -
     * it waits until someone connects.
     *
     * async_accept() is non-blocking. It says "when someone connects, call
     * this function" and returns immediately.
     */

    /*
     * Create a socket for the incoming connection. async_accept needs somewhere
     * to put the new connection when it arrives.
     */
    auto socket = std::make_shared<tcp::socket>(acceptor.get_executor());

    acceptor.async_accept(*socket,
        [&acceptor, &room, socket](boost::system::error_code ec) {
            if (!ec) {
                /*
                 * Someone connected! Wrap their socket in a Session object
                 * and start participating in the chat.
                 */
                auto session = std::make_shared<Session>(std::move(*socket), room);
                session->start();

                std::cout << "New client connected" << std::endl;
            } else {
                std::cout << "Accept error: " << ec.message() << std::endl;
            }

            /*
             * Keep accepting more connections. This recursive call creates
             * an endless loop of accepts. Each success triggers another
             * async_accept().
             */
            start_accept(acceptor, room);
        });
}

int main(int argc, char* argv[]) {
    /*
     * Server startup. The pattern:
     * 1. Create the core objects (Room, io_context, acceptor)
     * 2. Start accepting connections
     * 3. Run the event loop
     *
     * Everything happens inside io.run() - that's where the async magic lives.
     */

    try {
        if (argc != 2) {
            std::cerr << "Usage: " << argv[0] << " <port>\n";
            return 1;
        }

        /*
         * The foundation objects:
         * - Room: coordinates all the chat participants
         * - io_context: the async event processing engine
         * - endpoint: defines where to listen (IP + port)
         * - acceptor: listens for incoming connections
         */
        Room room;
        boost::asio::io_context io;
        tcp::endpoint endpoint(tcp::v4(), std::atoi(argv[1]));
        tcp::acceptor acceptor(io, endpoint);

        std::cout << "Chat server listening on port " << argv[1] << std::endl;

        /*
         * Start the accept loop. Once this starts, the server will
         * continuously accept new connections.
         */
        start_accept(acceptor, room);

        /*
         * Run the event loop. This is where the server "lives".
         * io.run() processes async events until the program exits:
         *
         * - New connections arrive → create Sessions
         * - Data arrives from clients → parse into Messages
         * - Messages need broadcasting → send to all participants
         * - Clients disconnect → clean up Sessions
         *
         * All of this happens in a single thread through the magic of
         * async I/O. No thread synchronization needed.
         */
        io.run();

    } catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

/*
 * ============================================================================
 * WHAT I LEARNED BUILDING THIS
 * ============================================================================
 *
 * Async programming flipped my mental model upside down:
 *
 * SYNCHRONOUS THINKING:
 * "Do this, then do that, then do the other thing"
 * Linear, sequential, predictable
 *
 * ASYNCHRONOUS REALITY:
 * "Start this, start that, start the other thing"
 * "When any of them finish, do something else"
 * Event-driven, callback-based, emergent behavior
 *
 * The key insights:
 *
 * 1. OBJECT LIFETIME IS TRICKY
 *    Objects must outlive all their async operations
 *    shared_ptr + enable_shared_from_this() solve this automatically
 *
 * 2. STATE MACHINES EVERYWHERE
 *    async_write queue processing is a state machine
 *    Connection acceptance is a state machine
 *    Message parsing is a state machine
 *
 * 3. SINGLE THREAD, MANY CONNECTIONS
 *    No thread synchronization needed
 *    Event loop handles hundreds of connections efficiently
 *    CPU never blocks waiting for slow network I/O
 *
 * 4. ERROR HANDLING IS DIFFERENT
 *    Errors happen in callbacks, not where operations start
 *    Must handle partial failures gracefully
 *    Network connections can vanish at any moment
 *
 * 5. COMPOSITION OVER INHERITANCE WINS
 *    Room HAS participants (composition)
 *    Session IS-A participant (inheritance)
 *    Session HAS-A socket (composition)
 *    Mix and match as needed
 *
 * Building this taught me that async isn't just faster - it's a fundamentally
 * different way of thinking about program structure. Once you see it, you can't
 * go back to the blocking, sequential mindset.
 * ============================================================================
 */

