#include "chatRoom.hpp"
#include <iostream>

// Beacon: Room Implementation
void Room::join(ParticipantPtr participant) {
    participants.insert(participant);
    for (const auto& msg : MessageQueue) {
        participant->deliver(msg);
    }
}

void Room::leave(ParticipantPtr participant) {
    participants.erase(participant);
}

void Room::deliver(ParticipantPtr sender, const Message& msg) {
    MessageQueue.push_back(msg);
    if (MessageQueue.size() > 50) {
        MessageQueue.pop_front();
    }

    for (auto participant : participants) {
        if (participant != sender) {
            participant->deliver(msg);
        }
    }
}

// Beacon: Session Implementation
Session::Session(tcp::socket socket, Room& room)
    : clientSocket(std::move(socket)), room(room) {
}

void Session::deliver(const Message& msg) {
    outgoingMessages.push_back(msg);
    if (outgoingMessages.size() == 1) {
        async_write();
    }
}

void Session::write(Message& msg) {
    room.deliver(shared_from_this(), msg);
}

void Session::start() {
    room.join(shared_from_this());
    async_read();
}

void Session::async_read() {
    auto self = shared_from_this();
    boost::asio::async_read(clientSocket,
        boost::asio::buffer(incomingMessage.data, Message::header),
        [this, self](boost::system::error_code ec, std::size_t bytes_transferred) {
            if (!ec) {
                if (incomingMessage.decodeHeader()) {
                    readMessageBody();
                } else {
                    std::cout << "Invalid message header from client" << std::endl;
                    room.leave(shared_from_this());
                }
            } else {
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
    auto self = shared_from_this();
    boost::asio::async_read(clientSocket,
        boost::asio::buffer(incomingMessage.data + Message::header, incomingMessage.getBodyLength()),
        [this, self](boost::system::error_code ec, std::size_t bytes_transferred) {
            if (!ec) {
                write(incomingMessage);
                async_read();
            } else {
                room.leave(shared_from_this());
                std::cout << "Read body error: " << ec.message() << std::endl;
            }
        });
}

void Session::async_write() {
    if (outgoingMessages.empty()) {
        return;
    }

    auto self = shared_from_this();
    const Message& msg = outgoingMessages.front();
    size_t totalLength = Message::header + msg.getBodyLength();

    boost::asio::async_write(clientSocket,
        boost::asio::buffer(msg.data, totalLength),
        [this, self](boost::system::error_code ec, std::size_t bytes_transferred) {
            if (!ec) {
                outgoingMessages.pop_front();
                if (!outgoingMessages.empty()) {
                    async_write();
                }
            } else {
                std::cout << "Write error: " << ec.message() << std::endl;
                room.leave(shared_from_this());
            }
        });
}

// Beacon: Server Infrastructure
void start_accept(tcp::acceptor& acceptor, Room& room) {
    auto socket = std::make_shared<tcp::socket>(acceptor.get_executor());
    acceptor.async_accept(*socket,
        [&acceptor, &room, socket](boost::system::error_code ec) {
            if (!ec) {
                auto session = std::make_shared<Session>(std::move(*socket), room);
                session->start();
                std::cout << "New client connected" << std::endl;
            } else {
                std::cout << "Accept error: " << ec.message() << std::endl;
            }
            start_accept(acceptor, room);
        });
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            std::cerr << "Usage: " << argv[0] << " <port>\n";
            return 1;
        }

        Room room;
        boost::asio::io_context io;
        tcp::endpoint endpoint(tcp::v4(), std::atoi(argv[1]));
        tcp::acceptor acceptor(io, endpoint);

        std::cout << "Chat server listening on port " << argv[1] << std::endl;

        start_accept(acceptor, room);
        io.run();

    } catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
