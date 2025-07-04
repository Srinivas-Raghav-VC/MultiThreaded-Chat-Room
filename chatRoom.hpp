#include "message.hpp"
#include <iostream>
#include <set>
#include <memory>
#include <deque>
#include <boost/asio.hpp>

#ifndef CHATROOM_HPP
#define CHATROOM_HPP

/*
 * ============================================================================
 * MY CHAT SERVER DESIGN JOURNEY
 * ============================================================================
 *
 * So I'm building a chat server. The goal is simple: multiple people should be
 * able to connect and chat in real-time. Sounds easy, right?
 *
 * Well, let me tell you what I learned...
 *
 * First attempt: "I'll just handle one client at a time"
 *   → Disaster! User A types slowly, everyone else waits
 *   → User goes to bathroom, server frozen
 *   → Completely unusable for actual chat
 *
 * Realization: I need ASYNCHRONOUS handling
 *   → All clients can send/receive simultaneously
 *   → No blocking, no waiting
 *   → Much more complex, but actually works
 *
 * Big challenge: Object lifetime in async world
 *   → Operations complete LATER
 *   → Objects might be deleted while still in use
 *   → Need smart pointers to keep things alive
 *
 * Architecture that emerged:
 *   - Room: Central coordinator (knows about all participants)
 *   - Session: Handles one client connection (socket + async operations)
 *   - Participant: Abstract interface (Room doesn't care about details)
 *
 * The tricky parts I had to figure out:
 *   - enable_shared_from_this (for async callbacks)
 *   - Message queuing (for slow clients)
 *   - Two-phase construction (shared_ptr in constructor = crash)
 *   - Virtual destructors (learned this the hard way)
 * ============================================================================
 */

using boost::asio::ip::tcp;

/*
 * ============================================================================
 * THE SYNC vs ASYNC REALIZATION
 * ============================================================================
 *
 * I started with the obvious approach - handle clients one by one:
 *
 *   while (true) {
 *       auto client = acceptConnection();
 *       while (client.connected()) {
 *           auto message = client.read();  // This BLOCKS!
 *           broadcast(message);
 *       }
 *   }
 *
 * Seemed logical. Then I tested with 2 friends:
 *   - Friend A connects, starts typing...
 *   - Friend B tries to connect → NOTHING HAPPENS
 *   - I'm confused, server looks fine...
 *   - Oh wait, server is stuck waiting for Friend A to finish typing!
 *
 * That's when it hit me: TCP read() BLOCKS until data arrives. If someone
 * is thinking about what to type, EVERYONE else is locked out. Totally broken.
 *
 * So I needed async I/O. The concept:
 *   - Start read operation, but don't wait for it
 *   - Accept more clients immediately
 *   - When data arrives LATER, a callback handles it
 *
 * Much more complex, but now 100 people can chat simultaneously. Even if
 * one person has terrible internet, others aren't affected.
 *
 * The price: Object lifetime becomes a nightmare...
 *
 * ┌─────────────────────────────────────────────────────────┐
 * │                SYNCHRONOUS (BROKEN)                     │
 * │                                                         │
 * │  ┌─────────┐    ┌─────────┐    ┌─────────┐             │
 * │  │Client A │    │Client B │    │Client C │             │
 * │  └─────────┘    └─────────┘    └─────────┘             │
 * │       │              │              │                  │
 * │       ▼              ▼              ▼                  │
 * │  ┌─────────────────────────────────────────────────────┐│
 * │  │                  Server                            ││
 * │  │                                                     ││
 * │  │  while(true) {                                      ││
 * │  │    client = accept();                               ││
 * │  │    while(client.connected()) {                      ││
 * │  │      msg = client.read(); ◄─── BLOCKS HERE!        ││
 * │  │      broadcast(msg);                                ││
 * │  │    }                                                ││
 * │  │  }                                                  ││
 * │  └─────────────────────────────────────────────────────┘│
 * └─────────────────────────────────────────────────────────┘
 *
 * ┌─────────────────────────────────────────────────────────┐
 * │                ASYNCHRONOUS (WORKS!)                    │
 * │                                                         │
 * │  ┌─────────┐    ┌─────────┐    ┌─────────┐             │
 * │  │Client A │    │Client B │    │Client C │             │
 * │  └─────────┘    └─────────┘    └─────────┘             │
 * │       │              │              │                  │
 * │       ▼              ▼              ▼                  │
 * │  ┌─────────────────────────────────────────────────────┐│
 * │  │                  Server                            ││
 * │  │                                                     ││
 * │  │  ┌─────────┐  ┌─────────┐  ┌─────────┐             ││
 * │  │  │Session A│  │Session B│  │Session C│             ││
 * │  │  │         │  │         │  │         │             ││
 * │  │  │async_   │  │async_   │  │async_   │             ││
 * │  │  │read()   │  │read()   │  │read()   │             ││
 * │  │  └─────────┘  └─────────┘  └─────────┘             ││
 * │  │       │              │              │              ││
 * │  │       └──────────────┼──────────────┘              ││
 * │  │                      ▼                             ││
 * │  │              ┌─────────────┐                       ││
 * │  │              │    Room     │ ◄─── Central coordinator││
 * │  │              │ (broadcast) │                       ││
 * │  │              └─────────────┘                       ││
 * │  └─────────────────────────────────────────────────────┘│
 * └─────────────────────────────────────────────────────────┘
 */

/*
 * ============================================================================
 * THINKING THROUGH THE PARTICIPANT INTERFACE
 * ============================================================================
 *
 * At first, I was going to make Room directly manage Session objects:
 *
 *   class Room {
 *       std::set<Session*> sessions;
 *   };
 *
 * But then I thought... what if I want to add:
 *   - A bot that responds to commands?
 *   - A logging system that records all messages?
 *   - An admin interface that can moderate?
 *
 * These aren't "sessions" with network sockets. They're just... participants.
 *
 * That's when I realized I needed an abstraction. Room shouldn't care HOW
 * a participant works, just that it can:
 *   - Receive messages (deliver)
 *   - Send messages (write)
 *
 * This way Room can treat everyone the same - humans, bots, loggers, whatever.
 * The implementation details are hidden behind the interface.
 *
 * ┌─────────────────────────────────────────────────────────┐
 * │                   My Design Evolution                   │
 * │                                                         │
 * │ FIRST ATTEMPT (Concrete):                              │
 * │  ┌─────────────┐                                       │
 * │  │    Room     │                                       │
 * │  │             │                                       │
 * │  │ set<Session*>                                       │
 * │  │ sessions;   │ ◄─── Only handles network clients     │
 * │  └─────────────┘                                       │
 * │                                                         │
 * │ FINAL DESIGN (Abstract):                               │
 * │  ┌─────────────┐     ┌─────────────────┐               │
 * │  │    Room     │────▶│   Participant   │ ◄─── Abstract │
 * │  │             │     │   (interface)   │     interface │
 * │  │ set<Participant*> └─────────────────┘               │
 * │  │ participants;           △                           │
 * │  └─────────────┘           │                           │
 * │                            │ implements                │
 * │       ┌────────────────────┼────────────────────┐      │
 * │       │                    │                    │      │
 * │  ┌────▼────┐         ┌─────▼─────┐       ┌─────▼─────┐ │
 * │  │ Session │         │    Bot    │       │  Logger   │ │
 * │  │(network)│         │   (AI)    │       │  (file)   │ │
 * │  └─────────┘         └───────────┘       └───────────┘ │
 * └─────────────────────────────────────────────────────────┘
 *
 * Subtle insight: I made deliver() const because participants shouldn't
 * modify the message they're receiving (it might go to other participants too).
 * But write() is non-const because we might want to add timestamps or
 * sender info as the message flows through the system.
 */

class Participant {
    public:
    /*
     * deliver() - "Hey, here's a message for you"
     *
     * My thought process: When Room broadcasts a message, it needs to tell
     * each participant "here's what someone else said".
     *
     * const Message& because:
     *   - I'm broadcasting the SAME message to everyone
     *   - Don't want 100 copies floating around (memory waste)
     *   - Participant shouldn't modify what they're receiving
     *   - const makes the contract clear: "read-only access"
     *
     * Nuance: This is the "push" direction - Room pushing messages TO participants
     */
        virtual void deliver(const Message& msg) = 0;

    /*
     * write() - "I want to send a message"
     *
     * This is the "pull" direction - participant pulling Room's attention
     * to send a message.
     *
     * Message& (non-const) because:
     *   - Might want to add metadata (timestamp, sender ID)
     *   - Could validate/sanitize content
     *   - Message might get modified as it flows through system
     *
     * Initially I thought about making this const, but realized I'd need
     * to copy the message to modify it. Better to modify in-place.
     */
        virtual void write(Message& msg) = 0;

    /*
     * Destructor story: I forgot this initially...
     *
     * Then I had this bug where Sessions weren't cleaning up properly.
     * Spent hours debugging. Finally realized:
     *
     *   Participant* p = new Session();  // Socket opens
     *   delete p;  // Called ~Participant(), NOT ~Session()!
     *              // Socket never closed, file descriptor leak!
     *
     * Virtual destructor ensures the right destructor gets called.
     * Learned this lesson the hard way after running out of file descriptors.
     */
    virtual ~Participant() = default;
};

/*
 * Why shared_ptr? Let me tell you about my pointer journey...
 *
 * First attempt: Raw pointers
 *   Room* room = new Room();
 *   Participant* p = new Session();
 *
 *   Who deletes what? When? Total mess. Memory leaks everywhere.
 *
 * Second attempt: unique_ptr
 *   auto session = std::make_unique<Session>();
 *   session->async_read();  // Starts background operation
 *   room.join(std::move(session));  // session is now null!
 *
 *   But the async operation still needs the Session! How does it access it?
 *   Crashes when async callback tries to use moved-away Session.
 *
 * Final realization: shared_ptr
 *   - Multiple owners: Room + any active async operations
 *   - Reference counting: Object stays alive as long as ANYONE needs it
 *   - Automatic cleanup: When last reference goes away, object deleted
 *
 * The "aha!" moment: In async programming, you don't know who needs what when.
 * shared_ptr lets the system figure it out automatically.
 *
 * ┌─────────────────────────────────────────────────────────┐
 * │               My Pointer Evolution Journey              │
 * │                                                         │
 * │ RAW POINTERS (Disaster):                               │
 * │  Room* room = new Room();                              │
 * │  Session* session = new Session();                     │
 * │  ┌─────────┐     ┌─────────┐                           │
 * │  │  Room   │────▶│ Session │                           │
 * │  └─────────┘     └─────────┘                           │
 * │       │               │                                │
 * │       └─── Who deletes what? When? ◄─── Memory leaks!  │
 * │                                                         │
 * │ UNIQUE_PTR (Better, but...):                           │
 * │  auto session = make_unique<Session>();                │
 * │  session->async_read();  // Starts background op       │
 * │  room.join(move(session)); // session now null!       │
 * │  ┌─────────┐     ┌─────────┐                           │
 * │  │  Room   │────▶│ Session │                           │
 * │  └─────────┘     └─────────┘                           │
 * │                       ▲                                │
 * │  ┌─────────────────────┼─────────────────────────────┐ │
 * │  │ async_read callback │ tries to access Session... │ │
 * │  │ CRASH! Session moved away!                       │ │
 * │  └───────────────────────────────────────────────────┘ │
 * │                                                         │
 * │ SHARED_PTR (Perfect!):                                 │
 * │  auto session = make_shared<Session>();                │
 * │  session->async_read();  // async op holds copy       │
 * │  room.join(session);     // room holds copy           │
 * │  ┌─────────┐     ┌─────────┐                           │
 * │  │  Room   │────▶│ Session │◄─── ref_count: 2          │
 * │  └─────────┘     └─────────┘                           │
 * │                       ▲                                │
 * │  ┌─────────────────────┼─────────────────────────────┐ │
 * │  │ async callback also holds copy (ref_count: 3)    │ │
 * │  │ Session stays alive until EVERYONE done with it! │ │
 * │  └───────────────────────────────────────────────────┘ │
 * └─────────────────────────────────────────────────────────┘
 */
typedef std::shared_ptr<Participant> ParticipantPtr;

/*
 * ============================================================================
 * DESIGNING THE ROOM - THE CENTRAL COORDINATOR
 * ============================================================================
 *
 * Room is my "message broker". Everyone talks to Room, Room talks to everyone.
 *
 * Alternative I considered: Direct participant-to-participant communication
 *   - Each Session keeps list of all other Sessions
 *   - When sending message, iterate and call deliver() on each
 *
 * Problems I foresaw:
 *   - What happens when someone joins/leaves? Everyone updates their lists?
 *   - How do I add features like message logging or content filtering?
 *   - Tightly coupled - Sessions know too much about each other
 *
 * Room as mediator solves this:
 *   - Sessions only know about Room
 *   - Room knows about everyone
 *   - Features get added in Room (logging, filtering, etc.)
 *   - Clean separation of concerns
 *
 * The data structure choices I had to make...
 */

class Room {
    public:
    /*
     * Hmm, how should I store the participants?
     *
     * My first instinct: vector<ParticipantPtr> participants;
     *   - Simple, I know vectors well
     *   - But wait... what if the same user's connection hiccups?
     *   - They might reconnect before the first connection times out
     *   - Now I have the same person twice → they get duplicate messages!
     *   - To prevent this, I'd need to check "if (find(participants, newUser) == end)"
     *   - That's O(n) search on every join. Gets slow with lots of users.
     *
     * Hmm, maybe unordered_set<ParticipantPtr>?
     *   - Hash table = O(1) average case operations. Fast!
     *   - But... how does it hash a shared_ptr?
     *   - Probably hashes the pointer address, not the object content
     *   - Actually, that's what I want - prevent same Session* twice
     *   - Wait, but what if I need to iterate in predictable order for testing?
     *   - Also, for a chat room, how many users? 50? 100? O(log n) is fine.
     *
     * You know what, I'll go with set<ParticipantPtr>:
     *   - Automatic uniqueness (main goal)
     *   - O(log n) is plenty fast for chat room sizes
     *   - Ordered iteration makes debugging easier
     *   - No hash function edge cases to worry about
     *
     * Subtle detail: std::set compares shared_ptr objects by their stored pointer
     * address, not by comparing what they point to. So two shared_ptrs pointing
     * to the same Session will compare equal. Perfect - exactly what I want.
     *
     * ┌─────────────────────────────────────────────────────────┐
     * │            Container Choice Analysis                    │
     * │                                                         │
     * │ OPTION 1: vector<ParticipantPtr>                       │
     * │  ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐     │
     * │  │ P1  │ P2  │ P3  │ P4  │ P5  │ P6  │ P7  │ P8  │     │
     * │  └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘     │
     * │  join(): push_back() = O(1) ✓                          │
     * │  leave(): find() + erase() = O(n) ✗                    │
     * │  Problem: Duplicate users if connection hiccups!        │
     * │                                                         │
     * │ OPTION 2: unordered_set<ParticipantPtr>               │
     * │  ┌───────────────────────────────────────────────────┐ │
     * │  │ [hash buckets with collision chains]             │ │
     * │  │ P1 → P5 → null                                   │ │
     * │  │ P2 → null                                        │ │
     * │  │ P3 → P7 → P4 → null                              │ │
     * │  │ P6 → null                                        │ │
     * │  └───────────────────────────────────────────────────┘ │
     * │  join(): hash + insert = O(1) avg ✓                   │
     * │  leave(): hash + erase = O(1) avg ✓                   │
     * │  Problem: Hash edge cases, unordered iteration         │
     * │                                                         │
     * │ CHOSEN: set<ParticipantPtr>                           │
     * │              ┌─────────┐                               │
     * │              │   P4    │                               │
     * │         ┌────┴────┐    └────┬────┐                    │
     * │         │   P2    │         │   P6    │               │
     * │    ┌────┴────┐    └────┬────┴────┐    └────┬────┐     │
     * │    │   P1    │         │   P3    │         │   P7    │ │
     * │    └─────────┘         └─────────┘         └─────────┘ │
     * │  join(): insert = O(log n) ✓ (good enough)             │
     * │  leave(): erase = O(log n) ✓ (good enough)             │
     * │  Benefits: Automatic uniqueness, ordered, predictable  │
     * │  Perfect for chat room sizes (50-100 users)            │
     * └─────────────────────────────────────────────────────────┘
     */
        void join(ParticipantPtr participant);
        void leave(ParticipantPtr participant);

    /*
     * Message broadcasting - the heart of the chat system:
     *
     * My naive first version:
     *   void deliver(const Message& msg) {
     *       for (auto participant : participants) {
     *           participant->deliver(msg);  // Send to EVERYONE
     *       }
     *   }
     *
     * Seemed logical. Then I tested it:
     *   - I type "hello"
     *   - I see "hello" appear in my chat immediately (client-side)
     *   - Server broadcasts "hello" back to me
     *   - I see "hello" appear AGAIN
     *   - "Is this thing broken? Why do I see duplicates?"
     *
     * Ah! The person who SENT the message shouldn't receive it back.
     * They already see it in their client. Echo-back is confusing.
     *
     * So I need: deliver(ParticipantPtr sender, const Message& msg)
     *   - sender = who sent this message
     *   - Skip them in the broadcast loop
     *
     * Design nuance: I could've hidden this detail inside Session:
     *   Session calls room.broadcast(msg) without sender info
     *   Room figures out "this came from the Session that called me"
     *
     * But that's implicit magic. Explicit is better. The interface clearly
     * says "here's a message FROM sender, deliver to everyone EXCEPT sender."
     * No guessing, no hidden state.
     *
     * ┌─────────────────────────────────────────────────────────┐
     * │              Message Broadcasting Flow                  │
     * │                                                         │
     * │ User A types: "Hello everyone!"                        │
     * │                                                         │
     * │ ┌─────────┐                                             │
     * │ │Session A│ ──── write(msg) ────┐                      │
     * │ └─────────┘                     │                      │
     * │                                 ▼                      │
     * │                        ┌─────────────┐                │
     * │                        │    Room     │                │
     * │                        │             │                │
     * │                        │ deliver(A,  │                │
     * │                        │   "Hello")  │                │
     * │                        └─────────────┘                │
     * │                                 │                      │
     * │                 ┌───────────────┼───────────────┐      │
     * │                 │               │               │      │
     * │                 ▼               ▼               ▼      │
     * │         ┌─────────────┐ ┌─────────────┐ ┌─────────────┐│
     * │         │  Session B  │ │  Session C  │ │  Session D  ││
     * │         │             │ │             │ │             ││
     * │         │deliver(msg) │ │deliver(msg) │ │deliver(msg) ││
     * │         └─────────────┘ └─────────────┘ └─────────────┘│
     * │                 │               │               │      │
     * │                 ▼               ▼               ▼      │
     * │         ┌─────────────┐ ┌─────────────┐ ┌─────────────┐│
     * │         │   User B    │ │   User C    │ │   User D    ││
     * │         │  sees msg   │ │  sees msg   │ │  sees msg   ││
     * │         └─────────────┘ └─────────────┘ └─────────────┘│
     * │                                                         │
     * │ Note: Session A (sender) doesn't get echo-back!        │
     * │ They already see their message in their client.        │
     * └─────────────────────────────────────────────────────────┘
     */
    void deliver(ParticipantPtr sender, const Message& msg);

    private:
    std::set<ParticipantPtr> participants;

    /*
     * Message history - solving the "empty room" problem:
     *
     * Picture this: New user joins the room, sees completely empty chat.
     * They're thinking "Is anyone here? Is this thing working? Hello?"
     *
     * I need to show them recent messages so they understand the context.
     *
     * First attempt: Just broadcast new messages, no history.
     *   - Worked for testing with myself
     *   - Terrible user experience for real use
     *   - People kept asking "did my messages send? anyone there?"
     *
     * So I need message history. But how to store it efficiently?
     *
     * Tried vector<Message> first:
     *   messages.push_back(newMessage);        // O(1) - good
     *   if (messages.size() > 50) {
     *       messages.erase(messages.begin());   // O(n) - terrible!
     *   }
     *
     * The erase() shifts all 49 remaining elements. For an active chat room
     * where messages come in frequently, this becomes a performance killer.
     *
     * Considered list<Message>:
     *   messages.push_back(newMessage);        // O(1) - good
     *   if (messages.size() > 50) {
     *       messages.pop_front();              // O(1) - good
     *   }
     *
     * But what if I want to access message[i] later? Lists don't support that.
     * Also, pointer per element = more memory overhead.
     *
     * Finally settled on deque<Message>:
     *   - push_back() is O(1)
     *   - pop_front() is O(1)
     *   - Random access like messages[i] works
     *   - Better cache locality than list
     *
     * Perfect for the "sliding window" of recent messages pattern.
     * Deque = "double-ended queue" = best of vector and list for this use case.
     *
     * ┌─────────────────────────────────────────────────────────┐
     * │              Message History Container Wars             │
     * │                                                         │
     * │ VECTOR<Message> (Terrible for sliding window):         │
     * │  ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐     │
     * │  │ M1  │ M2  │ M3  │ M4  │ M5  │ M6  │ M7  │ M8  │     │
     * │  └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘     │
     * │                                                         │
     * │  New message arrives:                                   │
     * │  messages.push_back(M9);  // O(1) ✓                    │
     * │  if (size() > 8) {                                      │
     * │    messages.erase(begin()); // O(n) ✗ ✗ ✗              │
     * │    // Shifts M2,M3,M4,M5,M6,M7,M8,M9 all left!         │
     * │  }                                                      │
     * │                                                         │
     * │ LIST<Message> (Better, but...):                        │
     * │  ┌─────┐    ┌─────┐    ┌─────┐    ┌─────┐              │
     * │  │ M1  │───▶│ M2  │───▶│ M3  │───▶│ M4  │───▶...       │
     * │  └─────┘    └─────┘    └─────┘    └─────┘              │
     * │                                                         │
     * │  messages.push_back(M9);   // O(1) ✓                   │
     * │  messages.pop_front();     // O(1) ✓                   │
     * │  But: No random access, more memory overhead           │
     * │                                                         │
     * │ DEQUE<Message> (Perfect!):                             │
     * │     ┌─────────────────┬─────────────────┐               │
     * │     │   Chunk 1       │   Chunk 2       │               │
     * │     ├─────┬─────┬─────┼─────┬─────┬─────┤               │
     * │     │ M1  │ M2  │ M3  │ M4  │ M5  │ M6  │               │
     * │     └─────┴─────┴─────┴─────┴─────┴─────┘               │
     * │                                                         │
     * │  push_back(): O(1) ✓ (like vector)                     │
     * │  pop_front(): O(1) ✓ (like list)                       │
     * │  operator[]:  O(1) ✓ (like vector)                     │
     * │  Perfect for sliding window of recent messages!         │
     * └─────────────────────────────────────────────────────────┘
     */
        std::deque<Message> MessageQueue;

    /*
     * Capacity planning thoughts:
     *
     * I need a limit, or someone could connect 10,000 bots and crash my server.
     *
     * How did I pick 100?
     *   - Technical: 100 participants × 1 msg/sec = 100 broadcasts/sec
     *     Each broadcast goes to ~99 people = ~10,000 operations/sec
     *     My server can handle that comfortably
     *
     *   - Social: Research shows groups larger than ~150 people (Dunbar's number)
     *     don't function well as communities. 100 feels right for active chat.
     *
     *   - Practical: I've seen Discord voice channels cap at 99 users.
     *     Slack channels get unwieldy around 100 active participants.
     *
     * This isn't a hard limit in my current code (TODO), but documents intent.
     */
        static const size_t MaxParticipants = 100;
};

/*
 * ============================================================================
 * SESSION DESIGN - WHERE THE COMPLEXITY LIVES
 * ============================================================================
 *
 * Session - where all the async complexity lives:
 *
 * This represents one client's connection. Sounds simple, but async I/O
 * makes everything complicated. Here's what I need to handle:
 *   - TCP socket (reading/writing bytes)
 *   - Message parsing (bytes → Message objects)
 *   - Object lifetime (when is it safe to delete this Session?)
 *
 * The inheritance decisions:
 *
 * "public Participant" - obvious. Session IS a participant in the chat.
 *
 * "public enable_shared_from_this" - this one took me a while to understand.
 * Here's the problem I kept running into:
 *
 *   void Session::async_read() {
 *       boost::asio::async_read(socket, buffer,
 *           [this](error_code ec, size_t len) {
 *               // DANGER: 'this' might be garbage when this runs!
 *               this->processMessage();  // Potential crash
 *           });
 *   }
 *
 * The issue: async_read returns immediately, but the callback runs LATER
 * (when network data arrives). What if the Session gets deleted in between?
 *
 * My first "fix" attempt:
 *   auto self = std::shared_ptr<Session>(this);  // Create shared_ptr to myself
 *   async_read(socket, buffer, [self](error_code ec, size_t len) {
 *       self->processMessage();  // Now self keeps Session alive
 *   });
 *
 * Crashed immediately! Why? Because I created a SECOND shared_ptr with its
 * own reference count. The original shared_ptr (held by Room) has count=1.
 * My new shared_ptr also has count=1. They don't know about each other!
 * When Room's shared_ptr destructs, count goes 1→0, Session deleted.
 * My callback still thinks Session is alive (its count is still 1).
 * Classic double-deletion disaster.
 *
 * enable_shared_from_this fixes this. It lets me create a shared_ptr to
 * myself that SHARES the same reference count as existing shared_ptrs.
 * All shared_ptrs to the same Session cooperate properly.
 *
 * ┌─────────────────────────────────────────────────────────┐
 * │          The Async Lifetime Problem (Visualized)       │
 * │                                                         │
 * │ BROKEN APPROACH:                                        │
 * │  void Session::async_read() {                          │
 * │    async_read(socket, [this](error, bytes) {           │
 * │      this->process();  // DANGER!                      │
 * │    });                                                  │
 * │  }                                                      │
 * │                                                         │
 * │ Timeline:                                               │
 * │  t=0: ┌─────────┐     ┌─────────┐                     │
 * │       │  Room   │────▶│ Session │  ref_count: 1       │
 * │       └─────────┘     └─────────┘                     │
 * │                            │                           │
 * │                            ▼                           │
 * │       ┌─────────────────────────────────────────────┐  │
 * │       │ async_read() starts...                      │  │
 * │       │ Callback registered with 'this' pointer    │  │
 * │       └─────────────────────────────────────────────┘  │
 * │                                                         │
 * │  t=1: Room decides to remove session                   │
 * │       ┌─────────┐     ┌─────────┐                     │
 * │       │  Room   │  X  │ Session │  ref_count: 0       │
 * │       └─────────┘     └─────────┘  DELETED! 💀         │
 * │                                                         │
 * │  t=2: Network data arrives, callback tries to run...   │
 * │       ┌─────────────────────────────────────────────┐  │
 * │       │ this->process(); ◄─── CRASH! 'this' is      │  │
 * │       │                     garbage memory!         │  │
 * │       └─────────────────────────────────────────────┘  │
 * │                                                         │
 * │ FIXED WITH shared_from_this:                           │
 * │  void Session::async_read() {                          │
 * │    auto self = shared_from_this();                     │
 * │    async_read(socket, [self](error, bytes) {           │
 * │      self->process();  // SAFE!                        │
 * │    });                                                  │
 * │  }                                                      │
 * │                                                         │
 * │  t=0: ┌─────────┐     ┌─────────┐                     │
 * │       │  Room   │────▶│ Session │  ref_count: 1       │
 * │       └─────────┘     └─────────┘                     │
 * │                            │                           │
 * │                            ▼                           │
 * │       ┌─────────────────────────────────────────────┐  │
 * │       │ auto self = shared_from_this();             │  │
 * │       │ Now ref_count: 2                            │  │
 * │       │ Callback holds 'self' copy                  │  │
 * │       └─────────────────────────────────────────────┘  │
 * │                                                         │
 * │  t=1: Room removes session                              │
 * │       ┌─────────┐     ┌─────────┐                     │
 * │       │  Room   │  X  │ Session │  ref_count: 1       │
 * │       └─────────┘     └─────────┘  Still alive! ✓     │
 * │                            ▲                           │
 * │                            │                           │
 * │       ┌─────────────────────┘                         │
 * │       │ Callback still holds reference                │
 * │       └───────────────────────────────────────────────┘│
 * │                                                         │
 * │  t=2: Callback executes safely, then self destructs    │
 * │       Session finally deleted when ref_count → 0       │
 * └─────────────────────────────────────────────────────────┘
 */

class Session : public Participant, public std::enable_shared_from_this<Session> {
    public:
    /*
     * Constructor parameter decisions:
     *
     * tcp::socket socket - I'm taking ownership of this socket. The caller
     * creates it, then moves it to me. I'm responsible for closing it.
     *
     * Room& room - I need to talk to the room, but I don't own it. The room
     * probably outlives individual sessions. Reference makes this clear.
     *
     * Why not tcp::socket& ? Because sockets aren't copyable, and I need to
     * store it as a member. I have to move it.
     *
     * Why not Room* ? I could, but reference makes it clear that room must
     * exist for the lifetime of Session. No null checking needed.
     */
    Session(tcp::socket socket, Room& room);

    /*
     * The start() method - why not do everything in the constructor?
     *
     * My original attempt looked clean:
     *   Session(tcp::socket s, Room& r) : clientSocket(std::move(s)), room(r) {
     *       room.join(shared_from_this());  // Join the room immediately
     *       async_read();                   // Start reading from client
     *   }
     *
     * Crashed with std::bad_weak_ptr exception. Confused the hell out of me.
     *
     * After some digging, I learned: shared_from_this() doesn't work in constructors!
     * Here's why:
     *   1. Constructor runs first
     *   2. enable_shared_from_this setup happens AFTER constructor completes
     *   3. So shared_from_this() in constructor = no shared_ptr exists yet = crash
     *
     * The fix: Two-phase construction
     *   - Constructor: Just initialize member variables (safe, simple)
     *   - start(): Do the async work that needs shared_from_this()
     *
     * Usage pattern (caller must remember both steps):
     *   auto session = std::make_shared<Session>(std::move(socket), room);
     *   session->start();  // shared_from_this() now works
     *
     * Slight annoyance: Have to remember to call start(). But better than crashes.
     * Some libraries enforce this with factory methods that do both steps.
     */
        void start();

    /*
     * Implementing the Participant interface:
     *
     * deliver() - Room is telling me "here's a message for your client"
     *   I need to send this over the network to my connected client.
     *   But what if I'm already sending something? Queue it.
     *
     * write() - My client sent me a message to broadcast
     *   I need to tell Room "please broadcast this to everyone else"
     *   But first maybe I should validate it, add timestamp, etc.
     */
    void deliver(const Message& msg) override;
    void write(Message& msg) override;

    /*
     * The async operation design:
     *
     * async_read() needs to be a loop. Here's why:
     *   - Client connects
     *   - I start one async_read
     *   - Client sends message, async_read completes
     *   - I process the message... now what?
     *   - If I don't start another async_read, I'll never see more messages!
     *
     * So the pattern is:
     *   async_read() → callback processes message → calls async_read() again
     *
     * It's a recursive async loop that keeps going until client disconnects.
     *
     * The lifetime issue: That callback needs Session to still exist when it
     * runs. That's where shared_from_this() comes in.
     */
        void async_read();
        void readMessageBody();
    void async_write();

    private:
    /*
     * Data member design choices:
     *
     * tcp::socket clientSocket - This IS the connection to the client.
     * When this gets destroyed, connection closes automatically (RAII).
     *
     * boost::asio::streambuf buffer - Accumulates incoming data.
     * TCP is a stream protocol - data might arrive in chunks. "Hello World"
     * might arrive as "Hel" then "lo Wor" then "ld". streambuf handles this.
     *
     * Room& room - My link back to the central coordinator.
     *
     * The outgoing message queue - a subtle concurrency issue I discovered:
     *
     * I was testing with multiple users sending messages rapidly. Started getting
     * corrupted data on the receiving end. Messages like "HellWorldo" instead of
     * "Hello" and "World" as separate messages.
     *
     * Took me a while to figure out: I was calling async_write() multiple times
     * on the same socket without waiting for the previous one to complete!
     *
     * What was happening:
     *   1. Room calls session->deliver("Hello")
     *   2. I start async_write("Hello")
     *   3. Before that completes, Room calls session->deliver("World")
     *   4. I start ANOTHER async_write("World") on same socket
     *   5. Both writes run simultaneously → data gets mixed up
     *
     * TCP sockets aren't thread-safe for multiple simultaneous writes.
     *
     * Solution: Outgoing message queue + state machine
     *   - Only one async_write active at a time
     *   - If write in progress, queue new messages
     *   - When write completes, start next queued message
     *   - Ensures messages sent in order, no corruption
     *
     * Bonus: This also handles flow control. If client has slow network,
     * messages queue up here instead of blocking the entire chat room.
     * Each client gets messages at their own pace.
     *
     * ┌─────────────────────────────────────────────────────────┐
     * │          The Message Corruption Bug I Found            │
     * │                                                         │
     * │ BROKEN: Multiple simultaneous async_writes              │
     * │                                                         │
     * │  t=0: Room calls deliver("Hello")                      │
     * │       ┌─────────┐    ┌─────────────────────────────┐    │
     * │       │ Session │───▶│ async_write("Hello")        │    │
     * │       └─────────┘    │ ┌─────────────────────────┐ │    │
     * │                      │ │ Writing to socket...    │ │    │
     * │                      │ └─────────────────────────┘ │    │
     * │                      └─────────────────────────────┘    │
     * │                                                         │
     * │  t=1: Room calls deliver("World") before first done!   │
     * │       ┌─────────┐    ┌─────────────────────────────┐    │
     * │       │ Session │───▶│ async_write("World")        │    │
     * │       └─────────┘    │ ┌─────────────────────────┐ │    │
     * │                      │ │ ALSO writing to socket! │ │    │
     * │                      │ └─────────────────────────┘ │    │
     * │                      └─────────────────────────────┘    │
     * │                                                         │
     * │  Result: "Hello" and "World" get interleaved!          │
     * │          Client receives: "HellWorldo" ✗               │
     * │                                                         │
     * │ FIXED: Message queue + state machine                   │
     * │                                                         │
     * │  ┌─────────────────────────────────────────────────────┐│
     * │  │                Session                             ││
     * │  │                                                     ││
     * │  │  ┌─────────────────┐  ┌─────────────────────────┐  ││
     * │  │  │ Outgoing Queue  │  │    State Machine        │  ││
     * │  │  │                 │  │                         │  ││
     * │  │  │ ┌─────────────┐ │  │ ┌─────────────────────┐ │  ││
     * │  │  │ │   "Hello"   │ │  │ │ if (!writing) {     │ │  ││
     * │  │  │ └─────────────┘ │  │ │   start_write();    │ │  ││
     * │  │  │ ┌─────────────┐ │  │ │   writing = true;   │ │  ││
     * │  │  │ │   "World"   │ │  │ │ } else {            │ │  ││
     * │  │  │ └─────────────┘ │  │ │   queue.push(msg);  │ │  ││
     * │  │  │ ┌─────────────┐ │  │ │ }                   │ │  ││
     * │  │  │ │     ...     │ │  │ └─────────────────────┘ │  ││
     * │  │  │ └─────────────┘ │  │                         │  ││
     * │  │  └─────────────────┘  └─────────────────────────┘  ││
     * │  └─────────────────────────────────────────────────────┘│
     * │                                                         │
     * │  Flow:                                                  │
     * │  1. deliver("Hello") → queue.push() + start_write()     │
     * │  2. deliver("World") → queue.push() (write in progress) │
     * │  3. "Hello" write completes → start_write("World")      │
     * │  4. "World" write completes → check queue for more      │
     * │                                                         │
     * │  Result: Messages sent in order, no corruption! ✓      │
     * └─────────────────────────────────────────────────────────┘
     */
        tcp::socket clientSocket;
        Message incomingMessage;
        Room& room;
    std::deque<Message> outgoingMessages;
};

/*
 * ============================================================================
 * LESSONS LEARNED FROM BUILDING THIS
 * ============================================================================
 *
 * What started as "simple chat server" taught me:
 *
 * 1. Async programming is HARD but necessary
 *    - Blocking I/O = only one client at a time (useless for chat)
 *    - Async I/O = complex object lifetime management (but actually works)
 *
 * 2. Smart pointers are essential for async safety
 *    - Raw pointers in async world = guaranteed crashes
 *    - shared_ptr + enable_shared_from_this = automatic lifetime management
 *
 * 3. Small design decisions have big consequences
 *    - const& vs value parameters = 100x performance difference
 *    - Virtual destructors = difference between working and crashing
 *    - Container choice = difference between O(1) and O(n) operations
 *
 * 4. Abstraction enables flexibility
 *    - Participant interface = easy to add bots, loggers, admin tools
 *    - Mediator pattern = loose coupling, easy feature additions
 *
 * 5. Real testing reveals real problems
 *    - Works fine with 1 user ≠ works fine with 100 users
 *    - Message corruption only showed up under rapid concurrent access
 *    - User experience issues only visible with real people
 *
 * The complexity was worth it. This architecture scales to hundreds of
 * concurrent users, handles network failures gracefully, and provides a
 * solid foundation for adding features like private messages, file uploads,
 * user authentication, etc.
 *
 * Most importantly: It actually works as a real-time chat system!
 * ============================================================================
 */

/*
 * ============================================================================
 * PUTTING IT ALL TOGETHER - THE SYSTEM IN ACTION
 * ============================================================================
 *
 * Here's how a typical message flow works:
 *
 * 1. User A types "Hello everyone!" in their chat client
 *
 * 2. Client sends bytes over network to server
 *
 * 3. Session A's async_read callback fires:
 *    - Parses bytes into Message object
 *    - Calls this->write(message)
 *
 * 4. Session A's write() implementation:
 *    - Maybe adds timestamp to message
 *    - Calls room.deliver(shared_from_this(), message)
 *
 * 5. Room's deliver() method:
 *    - Iterates through all participants
 *    - For each participant != sender:
 *      - Calls participant->deliver(message)
 *
 * 6. Each other Session's deliver() method:
 *    - Queues message for transmission
 *    - If not currently sending, starts async_write
 *
 * 7. Each Session's async_write sends message to their client
 *
 * 8. Users B, C, D see "Hello everyone!" in their chat windows
 *
 * The beauty: All of this happens asynchronously. While User A's message is
 * being processed, User B can simultaneously send their own message. No
 * blocking, no waiting.
 *
 * The complexity: Object lifetime management in this async world is tricky.
 * But shared_ptr + enable_shared_from_this handles it automatically.
 *
 * What I learned building this:
 *   - Async programming is hard but necessary for performance
 *   - Smart pointers are essential for async safety
 *   - Abstract interfaces make systems flexible and testable
 *   - Mediator pattern simplifies complex communication
 *   - Small design decisions (const&, virtual destructors) matter a lot
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │                    COMPLETE MESSAGE FLOW DIAGRAM                       │
 * │                                                                         │
 * │ User A types "Hello!" in their chat client                             │
 * │                                                                         │
 * │ 1. Network Layer:                                                      │
 * │    ┌─────────────┐                                                     │
 * │    │ Client A    │ ──── TCP bytes ────▶ Session A                     │
 * │    │ (Browser)   │                      async_read()                   │
 * │    └─────────────┘                           │                         │
 * │                                              ▼                         │
 * │ 2. Message Parsing:                   ┌─────────────┐                  │
 * │    Raw bytes → Message object         │ Session A   │                  │
 * │    "0005Hello!" → msg.body="Hello!"   │             │                  │
 * │                                       │ parse()     │                  │
 * │                                       └─────────────┘                  │
 * │                                              │                         │
 * │ 3. Send to Room:                             ▼                         │
 * │    session->write(msg)                ┌─────────────┐                  │
 * │                                       │ Session A   │                  │
 * │                                       │ write(msg)  │                  │
 * │                                       └─────────────┘                  │
 * │                                              │                         │
 * │ 4. Room Broadcasting:                        ▼                         │
 * │    room.deliver(sessionA, msg)        ┌─────────────┐                  │
 * │                                       │    Room     │                  │
 * │                                       │             │                  │
 * │                                       │ deliver()   │                  │
 * │                                       └─────────────┘                  │
 * │                                              │                         │
 * │                               ┌──────────────┼──────────────┐           │
 * │ 5. Distribute to Others:      │              │              │           │
 * │    Skip sender, deliver       ▼              ▼              ▼           │
 * │    to all other participants                                            │
 * │                        ┌─────────────┐┌─────────────┐┌─────────────┐   │
 * │                        │ Session B   ││ Session C   ││    Bot D    │   │
 * │                        │             ││             ││             │   │
 * │                        │ deliver()   ││ deliver()   ││ deliver()   │   │
 * │                        └─────────────┘└─────────────┘└─────────────┘   │
 * │                               │              │              │           │
 * │ 6. Queue for Transmission:    ▼              ▼              ▼           │
 * │    Each session queues        │              │              │           │
 * │    message if not currently   │              │              │           │
 * │    writing to socket          │              │              │           │
 * │                        ┌─────────────┐┌─────────────┐┌─────────────┐   │
 * │                        │ OutQueue B  ││ OutQueue C  ││ AI Process  │   │
 * │                        │ ["Hello!"]  ││ ["Hello!"]  ││ analyze()   │   │
 * │                        └─────────────┘└─────────────┘└─────────────┘   │
 * │                               │              │              │           │
 * │ 7. Async Write:               ▼              ▼              ▼           │
 * │    Send over network          │              │              │           │
 * │    (or process for bots)      │              │              │           │
 * │                        ┌─────────────┐┌─────────────┐┌─────────────┐   │
 * │                        │async_write()││async_write()││ maybe reply │   │
 * │                        │             ││             ││             │   │
 * │                        └─────────────┘└─────────────┘└─────────────┘   │
 * │                               │              │                         │
 * │ 8. Client Display:            ▼              ▼                         │
 * │    Users see message     ┌─────────────┐┌─────────────┐                │
 * │    in their chat         │ Client B    ││ Client C    │                │
 * │                          │ shows       ││ shows       │                │
 * │                          │ "Hello!"    ││ "Hello!"    │                │
 * │                          └─────────────┘└─────────────┘                │
 * │                                                                         │
 * │ Key Async Points:                                                      │
 * │ • Steps 1,7,8 happen asynchronously on network threads                │
 * │ • Multiple messages can be "in flight" simultaneously                  │
 * │ • shared_ptr keeps Sessions alive during async operations              │
 * │ • Message queues prevent corruption from concurrent writes             │
 * │ • Each participant processes at their own pace                        │
 * └─────────────────────────────────────────────────────────────────────────┘
 * ============================================================================
 */

#endif // CHATROOM_HPP
