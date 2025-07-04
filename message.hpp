#include <boost/asio.hpp>
#include <string>
#include <cstring>
#include <cstdio>
#include <stdexcept>

#ifndef MESSAGE_HPP
#define MESSAGE_HPP

/*
 * ============================================================================
 * MESSAGE CLASS - Network Communication Protocol
 * ============================================================================
 *
 * Purpose: Handles message formatting for network communication
 *
 * Protocol Design:
 *   [Header: 4 bytes] + [Body: Variable length]
 *
 *   Header Format: 4-digit number representing body length
 *   Examples: "0005Hello" = 5-byte message "Hello"
 *            "0025This is a longer message" = 25-byte message
 *
 * Why This Design?
 *   - Solves the "message boundary problem" in network streams
 *   - Receiver knows exactly how much data to read
 *   - Prevents buffer overflows and incomplete reads
 * ============================================================================
 */

class Message {

public:
    // ========================================================================
    // CONSTANTS - Protocol Specifications
    // ========================================================================

    static const size_t maxBytes = 512;  // Maximum message body size
    static const size_t header = 4;      // Header is always 4 bytes

    /*
     * Why these values?
     * - maxBytes = 512: Good for chat messages, fits in network buffers
     * - header = 4: Can represent 0000-9999, allowing up to 9999 byte messages
     */

    // ========================================================================
    // CONSTRUCTORS - Creating Message Objects
    // ========================================================================

    // Default constructor: Creates empty message
    Message() : bodyLength_(0) {}

    // Parameterized constructor: Creates message from string (SENDER SIDE)
    Message(const std::string& message) {
        setBodyLength(message.size());  // Validate and set length
        encodeHeader();                 // Convert length to 4-byte header
        encodeBody(message);           // Copy message content after header
    }

    // ========================================================================
    // SENDER OPERATIONS - Preparing Messages for Transmission
    // ========================================================================

    /*
     * encodeHeader() - Step 1 of message preparation
     *
     * Converts bodyLength_ into a 4-character string header
     * Example: bodyLength_ = 25 → header = "  25" (with leading spaces)
     *
     * Memory Layout After:
     * data = [' ', ' ', '2', '5', ?, ?, ?, ...]
     *         ↑________________↑
     *         Header (4 bytes)
     */
    void encodeHeader() {
        // Create temporary buffer for formatting (size 5 = 4 header + 1 null terminator)
        char temp_header[header + 1] = "";

        // Format bodyLength_ as 4-digit string with leading spaces
        std::sprintf(temp_header, "%4d", static_cast<int>(bodyLength_));

        // Copy exactly 4 bytes to start of data array (no null terminator)
        std::memcpy(data, temp_header, header);
    }

    /*
     * encodeBody() - Step 2 of message preparation
     *
     * Copies message content after the header
     * Uses pointer arithmetic: data + header points to byte 4
     *
     * Memory Layout After:
     * data = [' ', ' ', '2', '5', 'H', 'e', 'l', 'l', 'o']
     *         ↑________________↑  ↑____________________↑
     *         Header (4 bytes)   Body (5 bytes)
     */
    void encodeBody(const std::string& message) {
        // Copy message content starting after header
        // data + header = pointer to position 4 in array
        std::memcpy(data + header, message.c_str(), bodyLength_);
    }

    // ========================================================================
    // RECEIVER OPERATIONS - Parsing Incoming Messages
    // ========================================================================

    /*
     * decodeHeader() - Extract message length from received data
     *
     * This is the reverse of encodeHeader()
     * Converts first 4 bytes back to a number
     *
     * Example: data = "  25Hello" → bodyLength_ = 25
     *
     * Returns: true if header is valid, false if corrupted/invalid
     */
    bool decodeHeader() {
        // Create temporary buffer for header extraction
        char temp_header[header + 1] = "";

        // Copy exactly 4 bytes from start of data
        std::memcpy(temp_header, data, header);

        // Ensure null termination for atoi()
        temp_header[header] = '\0';

        // Convert string to integer
        int header_value = std::atoi(temp_header);

        // Validate the extracted value
        if (header_value < 0 || header_value > static_cast<int>(maxBytes)) {
            bodyLength_ = 0;
            return false;  // Invalid header - reject message
        }

        // Store validated length
        bodyLength_ = static_cast<size_t>(header_value);
        return true;  // Success
    }

    // ========================================================================
    // DATA ACCESS - Getting Message Content
    // ========================================================================

    /*
     * getData() - Returns complete message (header + body)
     *
     * This is what gets sent over the network
     * Example: "  25This is the message body"
     */
    std::string getData() const {
        int total_length = header + bodyLength_;
        return std::string(data, total_length);
    }

    /*
     * getBody() - Returns only the message content (no header)
     *
     * This is what the application actually wants to read
     * Example: "This is the message body"
     */
    std::string getBody() const {
        // Extract substring starting at position 'header' for 'bodyLength_' characters
        return std::string(data + header, bodyLength_);
    }

    // ========================================================================
    // UTILITY FUNCTIONS - Support Operations
    // ========================================================================

    /*
     * setBodyLength() - Safely update message body length
     *
     * Includes bounds checking to prevent buffer overflows
     * Throws exception for invalid lengths
     */
    size_t setBodyLength(size_t newLength) {
        if (newLength > maxBytes) {
            throw std::length_error("Message length exceeds maximum allowed size of "
                                  + std::to_string(maxBytes) + " bytes");
        }
        bodyLength_ = newLength;
        return bodyLength_;
    }

    /*
     * getBodyLength() - Get current body length
     */
    size_t getBodyLength() const {
        return bodyLength_;
    }

    /*
     * setBody() - Set message body content
     *
     * This method allows setting the body content directly and encodes
     * the header automatically. Useful for server receiving text messages.
     */
    void setBody(const std::string& body) {
        setBodyLength(body.size());
        encodeHeader();
        encodeBody(body);
    }

    /*
     * data - Raw byte array storing complete message
     * Made public for direct buffer access needed by async read/write operations
     * Layout: [Header: 4 bytes][Body: up to 512 bytes]
     * Total size: 516 bytes
     */
    char data[header + maxBytes];

private:
    // ========================================================================
    // PRIVATE DATA MEMBERS
    // ========================================================================

    /*
     * bodyLength_ - Actual number of bytes used in body portion
     * Range: 0 to maxBytes
     * Used by all methods to know valid data boundaries
     */
    size_t bodyLength_;
};

/*
 * ============================================================================
 * USAGE EXAMPLE
 * ============================================================================
 *
 * // SENDING A MESSAGE:
 * Message outgoing("Hello, World!");      // Constructor encodes automatically
 * std::string wireData = outgoing.getData(); // Get "  13Hello, World!"
 * // Send wireData over network...
 *
 * // RECEIVING A MESSAGE:
 * Message incoming;
 * // Receive data into incoming.data from network...
 * if (incoming.decodeHeader()) {           // Parse the header
 *     std::string text = incoming.getBody(); // Extract message content
 *     std::cout << "Received: " << text << std::endl;
 * }
 *
 * ============================================================================
 */

#endif // MESSAGE_HPP
