#include "message.hpp"
#include <iostream>
#include <boost/asio.hpp>
#include <thread>
#include <string>

using boost::asio::ip::tcp;

// Beacon: ChatClient Class
// Manages connection to the server and handles user input.
class ChatClient {
private:
    boost::asio::io_context io;
    tcp::socket socket;
    Message readMessage;
    std::string serverHost;
    std::string serverPort;

public:
    ChatClient(const std::string& host, const std::string& port)
        : socket(io), serverHost(host), serverPort(port) {
    }

    void connect() {
        try {
            tcp::resolver resolver(io);
            auto endpoints = resolver.resolve(serverHost, serverPort);
            boost::asio::connect(socket, endpoints);
            std::cout << "Connected to chat server!" << std::endl;
            std::cout << "Type messages and press Enter. Type 'quit' to exit.\n" << std::endl;
        } catch (std::exception& e) {
            std::cerr << "Connection failed: " << e.what() << std::endl;
            throw;
        }
    }

    void startReceiving() {
        boost::asio::async_read(socket,
            boost::asio::buffer(readMessage.data, Message::header),
            [this](boost::system::error_code ec, std::size_t bytes_transferred) {
                if (!ec) {
                    if (readMessage.decodeHeader()) {
                        readBodyData();
                    } else {
                        std::cerr << "Invalid message header received" << std::endl;
                        startReceiving();
                    }
                } else if (ec != boost::asio::error::operation_aborted) {
                    std::cerr << "Connection lost: " << ec.message() << std::endl;
                }
            });
    }

private:
    void readBodyData() {
        boost::asio::async_read(socket,
            boost::asio::buffer(readMessage.data + Message::header, readMessage.getBodyLength()),
            [this](boost::system::error_code ec, std::size_t bytes_transferred) {
                if (!ec) {
                    std::string messageBody = readMessage.getBody();
                    std::cout << messageBody << std::endl;
                    startReceiving();
                } else if (ec != boost::asio::error::operation_aborted) {
                    std::cerr << "Error reading message body: " << ec.message() << std::endl;
                }
            });
    }

public:
    void sendMessage(const std::string& messageText) {
        try {
            Message msg(messageText);
            boost::asio::write(socket, boost::asio::buffer(msg.data, Message::header + msg.getBodyLength()));
        } catch (std::exception& e) {
            std::cerr << "Failed to send message: " << e.what() << std::endl;
        }
    }

    void run() {
        startReceiving();
        std::thread ioThread([this]() {
            io.run();
        });

        std::string input;
        while (std::getline(std::cin, input)) {
            if (input == "quit" || input == "exit") {
                std::cout << "Disconnecting..." << std::endl;
                break;
            }
            if (!input.empty()) {
                sendMessage(input);
            }
        }

        socket.close();
        io.stop();
        ioThread.join();
    }
};

// Beacon: Main Function
int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <host> <port>" << std::endl;
        std::cerr << "Example: " << argv[0] << " localhost 8080" << std::endl;
        return 1;
    }

    try {
        ChatClient client(argv[1], argv[2]);
        client.connect();
        client.run();
    } catch (std::exception& e) {
        std::cerr << "Client error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
