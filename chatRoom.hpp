#include "message.hpp"
#include <iostream>
#include <set>
#include <memory>
#include <deque>
#include <boost/asio.hpp>

#ifndef CHATROOM_HPP
#define CHATROOM_HPP

using boost::asio::ip::tcp;

// Beacon: Participant Interface
// Abstract interface for chat participants (users, bots, etc.).
class Participant {
public:
    virtual void deliver(const Message& msg) = 0;
    virtual void write(Message& msg) = 0;
    virtual ~Participant() = default;
};

typedef std::shared_ptr<Participant> ParticipantPtr;

// Beacon: Room Class
// Manages all participants and message broadcasting.
class Room {
public:
    void join(ParticipantPtr participant);
    void leave(ParticipantPtr participant);
    void deliver(ParticipantPtr sender, const Message& msg);

private:
    std::set<ParticipantPtr> participants;
    std::deque<Message> MessageQueue;
    static const size_t MaxParticipants = 100;
};

// Beacon: Session Class
// Represents a single client connection.
class Session : public Participant, public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, Room& room);
    void start();
    void deliver(const Message& msg) override;
    void write(Message& msg) override;
    void async_read();
    void readMessageBody();
    void async_write();

private:
    tcp::socket clientSocket;
    Message incomingMessage;
    Room& room;
    std::deque<Message> outgoingMessages;
};

#endif // CHATROOM_HPP
