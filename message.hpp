#include <boost/asio.hpp>
#include <string>
#include <cstring>
#include <cstdio>
#include <stdexcept>

#ifndef MESSAGE_HPP
#define MESSAGE_HPP

// Beacon: Message Class
// Handles message formatting for network communication.
// Protocol: [Header: 4 bytes] + [Body: Variable length]
class Message {
public:
    // Beacon: Constants
    static const size_t maxBytes = 512;  // Max message body size
    static const size_t header = 4;      // Header is 4 bytes

    // Beacon: Constructors
    Message() : bodyLength_(0) {}
    Message(const std::string& message) {
        setBodyLength(message.size());
        encodeHeader();
        encodeBody(message);
    }

    // Beacon: Sender Operations
    void encodeHeader() {
        char temp_header[header + 1] = "";
        std::sprintf(temp_header, "%4d", static_cast<int>(bodyLength_));
        std::memcpy(data, temp_header, header);
    }

    void encodeBody(const std::string& message) {
        std::memcpy(data + header, message.c_str(), bodyLength_);
    }

    // Beacon: Receiver Operations
    bool decodeHeader() {
        char temp_header[header + 1] = "";
        std::memcpy(temp_header, data, header);
        temp_header[header] = '\0';
        int header_value = std::atoi(temp_header);
        if (header_value < 0 || header_value > static_cast<int>(maxBytes)) {
            bodyLength_ = 0;
            return false;
        }
        bodyLength_ = static_cast<size_t>(header_value);
        return true;
    }

    // Beacon: Data Access
    std::string getData() const {
        int total_length = header + bodyLength_;
        return std::string(data, total_length);
    }

    std::string getBody() const {
        return std::string(data + header, bodyLength_);
    }

    // Beacon: Utility Functions
    size_t setBodyLength(size_t newLength) {
        if (newLength > maxBytes) {
            throw std::length_error("Message length exceeds maximum allowed size of "
                                  + std::to_string(maxBytes) + " bytes");
        }
        bodyLength_ = newLength;
        return bodyLength_;
    }

    size_t getBodyLength() const {
        return bodyLength_;
    }

    void setBody(const std::string& body) {
        setBodyLength(body.size());
        encodeHeader();
        encodeBody(body);
    }

    // Raw byte array for the message (header + body)
    char data[header + maxBytes];

private:
    // Beacon: Private Data
    size_t bodyLength_; // Actual number of bytes in the message body
};

#endif // MESSAGE_HPP
