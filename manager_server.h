#pragma once

#include <cstdint>
#include <string>

class ManagerServer
{
public:
    ManagerServer(
        const std::string& host = "127.0.0.1",
        uint16_t port = 19090,
        const std::string& configHash = ""
    );

    ~ManagerServer();

    bool start();

    void stop();

    void run();

private:
    std::string host_;
    uint16_t port_;
    std::string configHash_;

    int serverFd_;

    bool running_;

    bool createServerSocket();

    void closeServerSocket();

    void handleClient(int clientFd);

    bool readExact(
        int fd,
        void* buffer,
        size_t size
    );

    bool writeAll(
        int fd,
        const void* buffer,
        size_t size
    );

    bool sendResponse(
        int clientFd,
        uint16_t messageType,
        uint32_t requestId,
        const std::string& json
    );

    std::string handleRequest(
        uint16_t messageType,
        const std::string& payload
    );

    std::string handlePing(
        const std::string& payload
    );

    std::string handleSnapshot();

    std::string handleForceValue(
        const std::string& payload
    );

    static std::string jsonEscape(
        const std::string& value
    );

    static std::string doubleToJson(
        double value
    );

    static long long timestampToNs(
        const std::string& timestamp
    );
};