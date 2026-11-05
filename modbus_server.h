#pragma once

#include <cstdint>
#include <string>

class ModbusServer
{
public:

    // 构造
    //
    // 默认：
    //
    // Host = 127.0.0.1
    // Port = 19091
    //
    // 19090 给 system-managerd
    // 19091 给 Modbus 报文监视

    ModbusServer(
        const std::string& host = "127.0.0.1",
        uint16_t port = 19091
    );


    // 析构

    ~ModbusServer();


    // 启动 TCP Server

    bool start();


    // 停止 TCP Server

    void stop();


    // 服务循环
    //
    // 等待 RetroDcsDemo 等客户端连接

    void run();


private:

    // TCP 配置

    std::string host_;

    uint16_t port_;


    // TCP Server socket

    int serverFd_;


    // Server运行状态

    bool running_;


    // 创建 Server socket

    bool createServerSocket();


    // 关闭 Server socket

    void closeServerSocket();


    // 处理一个客户端

    void handleClient(
        int clientFd
    );


    // 精确读取指定数量的字节

    bool readExact(
        int fd,
        void* buffer,
        size_t size
    );


    // 完整发送

    bool writeAll(
        int fd,
        const void* buffer,
        size_t size
    );


    // 发送响应
    //
    // 使用和 system-managerd 一样的 PYSM 协议
    //
    // Header:
    //
    // 4 bytes magic
    // 2 bytes version
    // 2 bytes messageType
    // 4 bytes requestId
    // 4 bytes payloadLength
    //
    // 总计16 bytes

    bool sendResponse(
        int clientFd,
        uint16_t messageType,
        uint32_t requestId,
        const std::string& json
    );


    // 处理请求

    std::string handleRequest(
        uint16_t messageType,
        const std::string& payload
    );


    // PING

    std::string handlePing(
        const std::string& payload
    );


    // 获取当前报文快照
    //
    // 数据来源：
    //
    // /modbus_data

    std::string handleSnapshot();

    std::string handleForceValue(
    const std::string& payload
);

    // 解除共享内存逻辑强制

    std::string handleReleaseForce(
        const std::string& payload
    );

    // JSON 转义

    static std::string jsonEscape(
        const std::string& value
    );


    // double 转 JSON

    static std::string doubleToJson(
        double value
    );
};