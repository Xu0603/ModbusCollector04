#include "modbus_server.h"

#include "shared_memory.h"
#include "modbus_client.h"
#include "database.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/mman.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <unistd.h>
#include <errno.h>

#include <cstring>
#include <cstdio>

#include <iostream>
#include <sstream>
#include <iomanip>

#include <cmath>
#include <limits>


// ============================================================
// PYSM 协议
//
// 和 system-managerd 保持一致
//
// Header:
//
// 4 bytes   magic        "PYSM"
// 2 bytes   version      1
// 2 bytes   messageType
// 4 bytes   requestId
// 4 bytes   payloadLength
//
// 总计 16 bytes
//
// 所有整数 Big Endian
// ============================================================

namespace
{

constexpr uint16_t PYSM_VERSION = 1;


// ============================================================
// 消息类型
// ============================================================

constexpr uint16_t MSG_PING =
    1;

constexpr uint16_t MSG_SNAPSHOT =
    2;

constexpr uint16_t MSG_FORCE_VALUE =
    3;

constexpr uint16_t MSG_RELEASE_FORCE =
    4;

// ============================================================
// 最大 Payload
// ============================================================

constexpr uint32_t MAX_PAYLOAD_SIZE =
    1024 * 1024;


// ============================================================
// PYSM Header 大小
// ============================================================

constexpr size_t PYSM_HEADER_SIZE =
    16;


// ============================================================
// 写 uint16 Big Endian
// ============================================================

void writeU16(
    unsigned char* buffer,
    uint16_t value
)
{
    buffer[0] =
        static_cast<unsigned char>(
            (value >> 8) & 0xff
        );

    buffer[1] =
        static_cast<unsigned char>(
            value & 0xff
        );
}


// ============================================================
// 写 uint32 Big Endian
// ============================================================

void writeU32(
    unsigned char* buffer,
    uint32_t value
)
{
    buffer[0] =
        static_cast<unsigned char>(
            (value >> 24) & 0xff
        );

    buffer[1] =
        static_cast<unsigned char>(
            (value >> 16) & 0xff
        );

    buffer[2] =
        static_cast<unsigned char>(
            (value >> 8) & 0xff
        );

    buffer[3] =
        static_cast<unsigned char>(
            value & 0xff
        );
}


// ============================================================
// 读取 uint16 Big Endian
// ============================================================

uint16_t readU16(
    const unsigned char* buffer
)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(buffer[0]) << 8) |
        static_cast<uint16_t>(buffer[1])
    );
}


// ============================================================
// 读取 uint32 Big Endian
// ============================================================

uint32_t readU32(
    const unsigned char* buffer
)
{
    return
        (static_cast<uint32_t>(buffer[0]) << 24) |
        (static_cast<uint32_t>(buffer[1]) << 16) |
        (static_cast<uint32_t>(buffer[2]) << 8) |
        static_cast<uint32_t>(buffer[3]);
}

}


// ============================================================
// 构造
// ============================================================

ModbusServer::ModbusServer(
    const std::string& host,
    uint16_t port
)
    :
    host_(host),
    port_(port),
    serverFd_(-1),
    running_(false)
{
}


// ============================================================
// 析构
// ============================================================

ModbusServer::~ModbusServer()
{
    stop();
}


// ============================================================
// 创建 TCP Server
// ============================================================

bool ModbusServer::createServerSocket()
{
    serverFd_ =
        ::socket(
            AF_INET,
            SOCK_STREAM,
            0
        );


    if (serverFd_ < 0)
    {
        perror(
            "[MODBUS SERVER] socket"
        );

        return false;
    }


    // ========================================================
    // SO_REUSEADDR
    // ========================================================

    int reuse = 1;


    if (
        setsockopt(
            serverFd_,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)
        ) < 0
    )
    {
        perror(
            "[MODBUS SERVER] setsockopt"
        );

        close(serverFd_);

        serverFd_ = -1;

        return false;
    }


    // ========================================================
    // Server 地址
    // ========================================================

    sockaddr_in address{};


    address.sin_family =
        AF_INET;


    address.sin_port =
        htons(port_);


    // ========================================================
    // 监听所有网卡
    // ========================================================

    if (
        host_ == "0.0.0.0"
    )
    {
        address.sin_addr.s_addr =
            htonl(INADDR_ANY);
    }
    else
    {
        if (
            inet_pton(
                AF_INET,
                host_.c_str(),
                &address.sin_addr
            ) != 1
        )
        {
            std::cerr
                << "[MODBUS SERVER] 无效监听地址: "
                << host_
                << std::endl;

            close(serverFd_);

            serverFd_ = -1;

            return false;
        }
    }


    // ========================================================
    // bind
    // ========================================================

    if (
        bind(
            serverFd_,
            reinterpret_cast<sockaddr*>(
                &address
            ),
            sizeof(address)
        ) < 0
    )
    {
        perror(
            "[MODBUS SERVER] bind"
        );

        close(serverFd_);

        serverFd_ = -1;

        return false;
    }


    // ========================================================
    // listen
    // ========================================================

    if (
        listen(
            serverFd_,
            10
        ) < 0
    )
    {
        perror(
            "[MODBUS SERVER] listen"
        );

        close(serverFd_);

        serverFd_ = -1;

        return false;
    }


    return true;
}


// ============================================================
// 关闭 Server
// ============================================================

void ModbusServer::closeServerSocket()
{
    if (serverFd_ >= 0)
    {
        close(serverFd_);

        serverFd_ = -1;
    }
}


// ============================================================
// start
// ============================================================

bool ModbusServer::start()
{
    if (running_)
    {
        return true;
    }


    if (!createServerSocket())
    {
        return false;
    }


    running_ = true;


    std::cout
        << std::endl
        << "=========================================="
        << std::endl;

    std::cout
        << "Modbus报文 TCP Server 启动成功"
        << std::endl;

    std::cout
        << "监听地址: "
        << host_
        << ":"
        << port_
        << std::endl;

    std::cout
        << "数据来源: /modbus_data"
        << std::endl;

    std::cout
        << "协议: PYSM"
        << std::endl;

    std::cout
        << "=========================================="
        << std::endl;


    return true;
}


// ============================================================
// stop
// ============================================================

void ModbusServer::stop()
{
    if (!running_)
    {
        return;
    }


    running_ = false;


    closeServerSocket();
}


// ============================================================
// run
// ============================================================

void ModbusServer::run()
{
    if (!running_)
    {
        return;
    }


    while (running_)
    {
        sockaddr_in clientAddress{};


        socklen_t clientLength =
            sizeof(clientAddress);


        int clientFd =
            accept(
                serverFd_,
                reinterpret_cast<sockaddr*>(
                    &clientAddress
                ),
                &clientLength
            );


        if (clientFd < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }


            if (!running_)
            {
                break;
            }


            perror(
                "[MODBUS SERVER] accept"
            );


            continue;
        }


        char clientIp[
            INET_ADDRSTRLEN
        ] = {0};


        inet_ntop(
            AF_INET,
            &clientAddress.sin_addr,
            clientIp,
            sizeof(clientIp)
        );


        std::cout
            << "[MODBUS SERVER] 客户端连接: "
            << clientIp
            << ":"
            << ntohs(
                clientAddress.sin_port
            )
            << std::endl;


        handleClient(
            clientFd
        );


        close(
            clientFd
        );


        std::cout
            << "[MODBUS SERVER] 客户端断开"
            << std::endl;
    }
}


// ============================================================
// readExact
// ============================================================

bool ModbusServer::readExact(
    int fd,
    void* buffer,
    size_t size
)
{
    char* ptr =
        static_cast<char*>(buffer);


    size_t received = 0;


    while (
        received < size
    )
    {
        ssize_t n =
            recv(
                fd,
                ptr + received,
                size - received,
                0
            );


        if (n == 0)
        {
            return false;
        }


        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }


            return false;
        }


        received +=
            static_cast<size_t>(
                n
            );
    }


    return true;
}


// ============================================================
// writeAll
// ============================================================

bool ModbusServer::writeAll(
    int fd,
    const void* buffer,
    size_t size
)
{
    const char* ptr =
        static_cast<const char*>(
            buffer
        );


    size_t sent = 0;


    while (
        sent < size
    )
    {
        ssize_t n =
            send(
                fd,
                ptr + sent,
                size - sent,
                MSG_NOSIGNAL
            );


        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }


            return false;
        }


        if (n == 0)
        {
            return false;
        }


        sent +=
            static_cast<size_t>(
                n
            );
    }


    return true;
}


// ============================================================
// 发送响应
// ============================================================

bool ModbusServer::sendResponse(
    int clientFd,
    uint16_t messageType,
    uint32_t requestId,
    const std::string& json
)
{
    // ========================================================
    // 响应类型
    //
    // request:
    //
    // 0x0001
    // 0x0002
    //
    // response:
    //
    // 0x8001
    // 0x8002
    // ========================================================

    const uint16_t responseType =
        static_cast<uint16_t>(
            messageType | 0x8000
        );


    if (
        json.size() >
        MAX_PAYLOAD_SIZE
    )
    {
        return false;
    }


    unsigned char header[
        PYSM_HEADER_SIZE
    ] = {0};


    // ========================================================
    // Magic
    // ========================================================

    header[0] = 'P';
    header[1] = 'Y';
    header[2] = 'S';
    header[3] = 'M';


    // ========================================================
    // Version
    // ========================================================

    writeU16(
        header + 4,
        PYSM_VERSION
    );


    // ========================================================
    // Message Type
    // ========================================================

    writeU16(
        header + 6,
        responseType
    );


    // ========================================================
    // Request ID
    // ========================================================

    writeU32(
        header + 8,
        requestId
    );


    // ========================================================
    // Payload Length
    // ========================================================

    writeU32(
        header + 12,
        static_cast<uint32_t>(
            json.size()
        )
    );


    // ========================================================
    // 发送 Header
    // ========================================================

    if (
        !writeAll(
            clientFd,
            header,
            sizeof(header)
        )
    )
    {
        return false;
    }


    // ========================================================
    // 发送 JSON
    // ========================================================

    if (
        !json.empty()
    )
    {
        if (
            !writeAll(
                clientFd,
                json.data(),
                json.size()
            )
        )
        {
            return false;
        }
    }


    return true;
}


// ============================================================
// handleClient
// ============================================================

void ModbusServer::handleClient(
    int clientFd
)
{
    while (running_)
    {
        unsigned char header[
            PYSM_HEADER_SIZE
        ] = {0};


        // ====================================================
        // 读取 Header
        // ====================================================

        if (
            !readExact(
                clientFd,
                header,
                sizeof(header)
            )
        )
        {
            return;
        }


        // ====================================================
        // 检查 Magic
        // ====================================================

        if (
            std::memcmp(
                header,
                "PYSM",
                4
            ) != 0
        )
        {
            std::cerr
                << "[MODBUS SERVER] "
                << "收到无效 PYSM 帧头"
                << std::endl;

            return;
        }


        // ====================================================
        // 解析 Header
        // ====================================================

        const uint16_t version =
            readU16(
                header + 4
            );


        const uint16_t messageType =
            readU16(
                header + 6
            );


        const uint32_t requestId =
            readU32(
                header + 8
            );


        const uint32_t payloadLength =
            readU32(
                header + 12
            );


        // ====================================================
        // Version
        // ====================================================

        if (
            version != PYSM_VERSION
        )
        {
            std::cerr
                << "[MODBUS SERVER] "
                << "不支持的协议版本: "
                << version
                << std::endl;

            return;
        }


        // ====================================================
        // Payload 大小检查
        // ========================================================

        if (
            payloadLength >
            MAX_PAYLOAD_SIZE
        )
        {
            std::cerr
                << "[MODBUS SERVER] "
                << "Payload过大: "
                << payloadLength
                << std::endl;

            return;
        }


        // ====================================================
        // 读取 Payload
        // ========================================================

        std::string payload;


        if (
            payloadLength > 0
        )
        {
            payload.resize(
                payloadLength
            );


            if (
                !readExact(
                    clientFd,
                    payload.data(),
                    payloadLength
                )
            )
            {
                return;
            }
        }


        // ====================================================
        // 处理请求
        // ========================================================

        std::string response =
            handleRequest(
                messageType,
                payload
            );


        // ====================================================
        // 发送响应
        // ========================================================

        if (
            !sendResponse(
                clientFd,
                messageType,
                requestId,
                response
            )
        )
        {
            return;
        }
    }
}


// ============================================================
// handleRequest
// ============================================================

std::string ModbusServer::handleRequest(
    uint16_t messageType,
    const std::string& payload
)
{
    switch (messageType)
    {
    case MSG_PING:

        return handlePing(
            payload
        );


    case MSG_SNAPSHOT:

        return handleSnapshot();

    case MSG_FORCE_VALUE:

        return handleForceValue(
            payload
        );

    case MSG_RELEASE_FORCE:

        return handleReleaseForce(
            payload
        );

    default:
    {
        std::ostringstream oss;


        oss
            << "{"
            << "\"ok\":false,"
            << "\"error\":\"unknown_message_type\","
            << "\"messageType\":"
            << messageType
            << "}";


        return oss.str();
    }
    }
}


// ============================================================
// handlePing
// ============================================================

std::string ModbusServer::handlePing(
    const std::string& payload
)
{
    (void)payload;


    return
        "{"
        "\"ok\":true,"
        "\"server\":\"modbus-server\","
        "\"port\":19091,"
        "\"protocol\":\"PYSM\""
        "}";
}


// ============================================================
// handleSnapshot
//
// 从共享内存读取当前 Modbus 数据
// ============================================================

std::string ModbusServer::handleSnapshot()
{
    SharedData* data =
        getSharedMemory();


    if (
        data == nullptr
    )
    {
        return
            "{"
            "\"ok\":false,"
            "\"error\":\"shared_memory_unavailable\""
            "}";
    }


    // ========================================================
    // 加锁
    // ========================================================

    if (
        pthread_mutex_lock(
            &data->mutex
        ) != 0
    )
    {
        munmap(
            data,
            sizeof(SharedData)
        );


        return
            "{"
            "\"ok\":false,"
            "\"error\":\"shared_memory_lock_failed\""
            "}";
    }


    // ========================================================
    // 读取 count
    // ========================================================

    int count =
        data->count;


    if (
        count < 0
    )
    {
        count = 0;
    }


    if (
        count > MAX_VARIABLES
    )
    {
        count =
            MAX_VARIABLES;
    }


    // ========================================================
    // 直接在锁内生成 JSON
    //
    // 这样可以保证：
    //
    // name/value/timestamp/telegram
    //
    // 来自同一次快照
    // ========================================================

    std::ostringstream json;

    // ========================================================
    // 读取 Modbus 任务配置
    //
    // 注意：
    // variables[] 仍然来自共享内存，保证实时数据快照不变。
    // tasks[] 只负责向 Qt 提供任务名称、端点、Slave、采集周期等配置。
    // ========================================================

    const std::string databasePath =
        "/home/xu/桌面/ModbusCollector/modbus_config.db";

    const std::vector<ModbusTask> tasks =
        loadModbusTasks(
            databasePath
        );

    json
        << "{"
        << "\"ok\":true,"
        << "\"server\":\"modbus-server\","
        << "\"count\":"
        << count
        << ","

        // ====================================================
        // Modbus 任务配置
        // ====================================================

        << "\"tasks\":[";


    for (
        std::size_t taskIndex = 0;
        taskIndex < tasks.size();
        ++taskIndex
    )
    {
        const ModbusTask& task =
            tasks[taskIndex];


        if (taskIndex > 0)
        {
            json << ",";
        }


        json
            << "{"

            << "\"taskId\":"
            << task.id
            << ","

            << "\"name\":\""
            << jsonEscape(task.name)
            << "\","

            << "\"host\":\""
            << jsonEscape(task.host)
            << "\","

            << "\"port\":"
            << task.port
            << ","

            << "\"slaveId\":"
            << task.slaveId
            << ","

            << "\"period\":"
            << task.period
            << ","

            << "\"timeout\":"
            << task.timeout
            << ","

            << "\"retryCount\":"
            << task.retryCount
            << ","

            << "\"reconnectInterval\":"
            << task.reconnectInterval
            << ","

            << "\"enable\":"
            << (
                task.enable
                ? "true"
                : "false"
            )

            << "}";
    }


    // ========================================================
    // 保留原 variables[]，兼容当前 Qt
    // ========================================================

    json
        << "],"
        << "\"variables\":[";


    for (
        int i = 0;
        i < count;
        ++i
    )
    {
        const Variable& variable =
            data->vars[i];

        // direction is configuration, not telemetry. Prefer the freshly
        // loaded database value so an INPUT/OUTPUT change is visible in the
        // very next snapshot, even before the polling worker's next cycle.
        std::string configuredDirection = variable.direction;
        for (const auto &task : tasks)
        {
            if (task.id != variable.taskId)
                continue;
            for (const auto &point : task.points)
            {
                if (point.id == variable.pointId)
                {
                    configuredDirection = point.direction;
                    break;
                }
            }
            break;
        }


        if (
            i > 0
        )
        {
            json << ",";
        }


        json
            << "{";


        // ====================================================
        // 多 Modbus 任务真实身份
        // ====================================================

        json
            << "\"taskId\":"
            << variable.taskId
            << ","

            << "\"pointId\":"
            << variable.pointId
            << ",";


        // ====================================================
        // name
        // ====================================================

        json
            << "\"name\":\""
            << jsonEscape(
                variable.name
            )
            << "\",";


        // ====================================================
        // slave
        // ====================================================

        json
            << "\"slave\":"
            << variable.slave
            << ",";


        // ====================================================
        // function
        // ====================================================

        json
            << "\"function\":"
            << variable.function
            << ",";


        // ====================================================
        // address
        // ====================================================

        json
            << "\"address\":"
            << variable.address
            << ",";


        // ====================================================
        // datatype
        // ====================================================

        json
            << "\"datatype\":\""
            << jsonEscape(
                variable.datatype
            )
            << "\",";

        // ====================================================
        // byteOrder
        // ====================================================

        json
            << "\"byteOrder\":\""
            << jsonEscape(
                variable.byteOrder
            )
            << "\",";


        // ====================================================
        // direction
        // ====================================================

        json
            << "\"direction\":\""
            << jsonEscape(
                configuredDirection
            )
            << "\",";

        // ====================================================
        // value
        // ====================================================

        json
            << "\"value\":"
            << doubleToJson(
                variable.value
            )
            << ",";


        // ====================================================
        // timestamp
        // ====================================================

        json
            << "\"timestamp\":\""
            << jsonEscape(
                variable.timestamp
            )
            << "\",";


        // ====================================================
        // TX Telegram
        // ====================================================

        json
            << "\"txTelegram\":\""
            << jsonEscape(
                variable.txTelegram
            )
            << "\",";


        // ====================================================
        // RX Telegram
        // ====================================================

        json
            << "\"rxTelegram\":\""
            << jsonEscape(
                variable.rxTelegram
            )
            << "\",";


        // ====================================================
        // telegram
        //
        // 保留兼容字段
        // ====================================================

        json
            << "\"telegram\":\""
            << jsonEscape(
                variable.telegram
            )
            << "\",";


        // ====================================================
        // status
        //
        // 表示底层 Modbus 通信质量：
        //     1 = GOOD
        //     0 = BAD
        //
        // 强制状态不占用 status，单独通过 forced 表示。
        // ====================================================

        json
            << "\"status\":"
            << variable.status
            << ",";


        // ====================================================
        // forced
        //
        // false = 正常实时值
        // true  = 当前逻辑点处于强制状态
        // ====================================================

        json
            << "\"forced\":"
            << (
                variable.forced != 0
                ? "true"
                : "false"
            )
            << ",";


        // ====================================================
        // forcedValue
        //
        // 保存当前逻辑点的强制值。
        // 未强制时通常为 0。
        // ====================================================

        json
            << "\"forcedValue\":"
            << variable.forcedValue;


        json
            << "}";
    }


    json
        << "]"
        << "}";

    // ========================================================
    // 解锁
    // ========================================================

    pthread_mutex_unlock(
        &data->mutex
    );


    // ========================================================
    // 解除 mmap
    // ========================================================

    munmap(
        data,
        sizeof(SharedData)
    );


    return json.str();
}

std::string ModbusServer::handleForceValue(
    const std::string& payload
)
{
    // ========================================================
    // 新版 Force / 写入请求格式：
    //
    // {
    //     "taskId": 2,
    //     "pointId": 9,
    //     "name": "test1",
    //     "value": "123"
    // }
    //
    // 真正用于唯一定位点的是：
    //
    // taskId + pointId
    //
    // name 只用于日志和辅助检查，不再作为数据库定位依据。
    // ========================================================


    // ========================================================
    // JSON 字符串字段提取
    // ========================================================

    auto extractString =
        [&payload](
            const std::string& key,
            std::string& result
        ) -> bool
    {
        const std::string pattern =
            "\"" + key + "\"";

        size_t pos =
            payload.find(
                pattern
            );


        if (
            pos ==
            std::string::npos
        )
        {
            return false;
        }


        pos =
            payload.find(
                ':',
                pos + pattern.size()
            );


        if (
            pos ==
            std::string::npos
        )
        {
            return false;
        }


        ++pos;


        while (
            pos < payload.size() &&
            (
                payload[pos] == ' ' ||
                payload[pos] == '\t' ||
                payload[pos] == '\r' ||
                payload[pos] == '\n'
            )
        )
        {
            ++pos;
        }


        if (
            pos >= payload.size() ||
            payload[pos] != '"'
        )
        {
            return false;
        }


        ++pos;


        std::string value;


        while (
            pos < payload.size()
        )
        {
            const char c =
                payload[pos];


            if (c == '"')
            {
                result =
                    value;

                return true;
            }


            if (
                c == '\\' &&
                pos + 1 < payload.size()
            )
            {
                ++pos;

                value.push_back(
                    payload[pos]
                );

                ++pos;

                continue;
            }


            value.push_back(c);

            ++pos;
        }


        return false;
    };


    // ========================================================
    // JSON 整数字段提取
    //
    // Qt 的 QJsonObject 会把：
    //
    // taskId
    // pointId
    //
    // 发送成 JSON number，而不是字符串。
    // ========================================================

    auto extractInt =
        [&payload](
            const std::string& key,
            int& result
        ) -> bool
    {
        const std::string pattern =
            "\"" + key + "\"";

        size_t pos =
            payload.find(
                pattern
            );


        if (
            pos ==
            std::string::npos
        )
        {
            return false;
        }


        pos =
            payload.find(
                ':',
                pos + pattern.size()
            );


        if (
            pos ==
            std::string::npos
        )
        {
            return false;
        }


        ++pos;


        while (
            pos < payload.size() &&
            (
                payload[pos] == ' ' ||
                payload[pos] == '\t' ||
                payload[pos] == '\r' ||
                payload[pos] == '\n'
            )
        )
        {
            ++pos;
        }


        const size_t begin =
            pos;


        if (
            pos < payload.size() &&
            (
                payload[pos] == '+' ||
                payload[pos] == '-'
            )
        )
        {
            ++pos;
        }


        const size_t digitBegin =
            pos;


        while (
            pos < payload.size() &&
            payload[pos] >= '0' &&
            payload[pos] <= '9'
        )
        {
            ++pos;
        }


        if (
            digitBegin ==
            pos
        )
        {
            return false;
        }


        const std::string numberText =
            payload.substr(
                begin,
                pos - begin
            );


        try
        {
            size_t consumed = 0;

            const long long value =
                std::stoll(
                    numberText,
                    &consumed
                );


            if (
                consumed !=
                numberText.size()
            )
            {
                return false;
            }


            if (
                value <
                std::numeric_limits<int>::min()
                ||
                value >
                std::numeric_limits<int>::max()
            )
            {
                return false;
            }


            result =
                static_cast<int>(
                    value
                );


            return true;
        }
        catch (...)
        {
            return false;
        }
    };


    // ========================================================
    // 1. taskId
    // ========================================================

    int taskId = 0;


    if (
        !extractInt(
            "taskId",
            taskId
        )
        ||
        taskId <= 0
    )
    {
        return
            "{"
            "\"ok\":false,"
            "\"error\":\"missing_task_id\","
            "\"message\":\"写入请求缺少有效的 taskId\""
            "}";
    }


    // ========================================================
    // 2. pointId
    // ========================================================

    int pointId = 0;


    if (
        !extractInt(
            "pointId",
            pointId
        )
        ||
        pointId <= 0
    )
    {
        return
            "{"
            "\"ok\":false,"
            "\"error\":\"missing_point_id\","
            "\"message\":\"写入请求缺少有效的 pointId\""
            "}";
    }


    // ========================================================
    // 3. name
    //
    // name 不再用于定位。
    // 暂时保留，方便日志和客户端兼容。
    // ========================================================

    std::string requestName;

    extractString(
        "name",
        requestName
    );


    // ========================================================
    // 4. value
    // ========================================================

    std::string valueText;


    if (
        !extractString(
            "value",
            valueText
        )
    )
    {
        return
            "{"
            "\"ok\":false,"
            "\"error\":\"missing_value\","
            "\"message\":\"写入请求缺少 value\""
            "}";
    }


    double value = 0.0;


    try
    {
        size_t consumed = 0;


        value =
            std::stod(
                valueText,
                &consumed
            );


        if (
            consumed !=
            valueText.size()
        )
        {
            return
                "{"
                "\"ok\":false,"
                "\"error\":\"invalid_value\","
                "\"message\":\"写入值不是有效数字\""
                "}";
        }
    }
    catch (...)
    {
        return
            "{"
            "\"ok\":false,"
            "\"error\":\"invalid_value\","
            "\"message\":\"写入值不是有效数字\""
            "}";
    }


    if (
        !std::isfinite(value)
    )
    {
        return
            "{"
            "\"ok\":false,"
            "\"error\":\"invalid_value\","
            "\"message\":\"写入值必须是有限数字\""
            "}";
    }


    // ========================================================
    // 5. 根据 taskId + pointId 从数据库加载真正配置
    //
    // 不再从共享内存按 name 找点。
    // ========================================================

    const std::string databasePath =
        "/home/xu/桌面/ModbusCollector/modbus_config.db";


    ModbusTask task;

    Point point;

    std::string loadError;


    if (
        !loadModbusTaskPoint(
            databasePath,
            taskId,
            pointId,
            task,
            point,
            loadError
        )
    )
    {
        std::ostringstream oss;

        oss
            << "{"
            << "\"ok\":false,"
            << "\"error\":\"point_not_found\","
            << "\"message\":\""
            << jsonEscape(
                loadError
            )
            << "\""
            << "}";

        return oss.str();
    }


    // ========================================================
    // 6. 如果客户端也传了 name，进行辅助一致性检查
    //
    // 不使用 name 定位点。
    //
    // 如果 Qt 的表格状态已经过期，
    // pointId 与 name 不一致时拒绝写入，
    // 防止误操作。
    // ========================================================

    if (
        !requestName.empty()
        &&
        requestName != point.name
    )
    {
        std::ostringstream oss;

        oss
            << "{"
            << "\"ok\":false,"
            << "\"error\":\"point_identity_mismatch\","
            << "\"message\":\"点身份不一致：请求点名="
            << jsonEscape(
                requestName
            )
            << "，数据库点名="
            << jsonEscape(
                point.name
            )
            << "\""
            << "}";

        return oss.str();
    }


    // ========================================================
    // 7. 检查是否允许写入
    //
    // FC03 = Holding Register
    //      允许通过 FC06 / FC16 写
    //
    // FC04 = Input Register
    //      不允许写
    // ========================================================

    if (
        point.function != 3
    )
    {
        return
            "{"
            "\"ok\":false,"
            "\"error\":\"point_not_writable\","
            "\"message\":\"当前点不是 Holding Register，不能写入\""
            "}";
    }

    const bool physicalOutput =
        point.direction == "OUTPUT";


    // ========================================================
    // 8. 打印此次写入真正使用的任务配置
    //
    // 后面测试多任务时非常重要。
    // ========================================================

    std::cout
        << std::endl
        << "============================================================"
        << std::endl;

    std::cout
        << "[MODBUS WRITE REQUEST]"
        << std::endl;

    std::cout
        << "taskId   : "
        << task.id
        << std::endl;

    std::cout
        << "taskName : "
        << task.name
        << std::endl;

    std::cout
        << "pointId  : "
        << point.id
        << std::endl;

    std::cout
        << "pointName: "
        << point.name
        << std::endl;

    std::cout
        << "device   : "
        << task.host
        << ":"
        << task.port
        << std::endl;

    std::cout
        << "slave    : "
        << task.slaveId
        << std::endl;

    std::cout
        << "address  : "
        << point.address
        << std::endl;

    std::cout
        << "datatype : "
        << point.datatype
        << std::endl;

    std::cout
        << "value    : "
        << value
        << std::endl;

    std::cout
        << "============================================================"
        << std::endl;


    // ========================================================
    // 9. OUTPUT 点执行真实 Modbus 写入；INPUT 点保留逻辑强制。
    //
    // 强制对象使用：
    //     taskId + pointId
    //
    // 因此即使两个任务指向同一个真实 Modbus 寄存器，
    // 也可以分别进行逻辑强制，互不影响。
    // ========================================================

    bool ok = false;
    std::string writeError;

    if (physicalOutput)
    {
        ok = writeModbus(task, point, value, writeError);
        if (ok)
        {
            // OUTPUT 显示下一轮 FC03 回读值，不伪造强制值。
            releaseSharedMemoryValue(task.id, point.id);
        }
    }
    else
    {
        ok = forceSharedMemoryValue(task.id, point.id, value);
    }


    if (!ok)
    {
        std::ostringstream oss;

        oss
            << "{"
            << "\"ok\":false,"
            << "\"error\":\""
            << (physicalOutput ? "modbus_write_failed" : "shared_memory_point_not_found")
            << "\","
            << "\"message\":\""
            << jsonEscape(physicalOutput ? writeError : "共享内存中未找到指定逻辑点")
            << "\""
            << "}";

        return oss.str();
    }


    // ========================================================
    // 10. 成功响应
    // ========================================================

    std::ostringstream oss;


    oss
        << "{"
        << "\"ok\":true,"
        << "\"taskId\":"
        << task.id
        << ","
        << "\"pointId\":"
        << point.id
        << ","
        << "\"name\":\""
        << jsonEscape(
            point.name
        )
        << "\","
        << "\"value\":"
        << value
        << ","
        << "\"forced\":"
        << (physicalOutput ? "false" : "true")
        << ","
        << "\"physicalWrite\":"
        << (physicalOutput ? "true" : "false")
        << ","
        << "\"message\":\""
        << (physicalOutput ? "Modbus 设备写入成功" : "共享内存逻辑点强制成功")
        << "\""
        << "}";


    return oss.str();
}

// ============================================================
// handleReleaseForce
//
// 解除共享内存中的逻辑强制。
//
// 注意：
//     这里只修改共享内存中的 forced 状态。
//     不向真实 Modbus 设备发送任何写请求。
// ============================================================

std::string ModbusServer::handleReleaseForce(
    const std::string& payload
)
{
    // ========================================================
    // 从 JSON 中提取整数：
    //
    // {
    //     "taskId": 1,
    //     "pointId": 1
    // }
    // ========================================================

    auto extractInt =
        [&payload](
            const std::string& key,
            int& result
        ) -> bool
    {
        const std::string pattern =
            "\"" + key + "\"";

        size_t pos =
            payload.find(
                pattern
            );

        if (
            pos ==
            std::string::npos
        )
        {
            return false;
        }


        pos =
            payload.find(
                ':',
                pos + pattern.size()
            );

        if (
            pos ==
            std::string::npos
        )
        {
            return false;
        }


        ++pos;


        while (
            pos < payload.size() &&
            (
                payload[pos] == ' ' ||
                payload[pos] == '\t' ||
                payload[pos] == '\r' ||
                payload[pos] == '\n'
            )
        )
        {
            ++pos;
        }


        if (
            pos >= payload.size()
        )
        {
            return false;
        }


        try
        {
            size_t consumed = 0;

            result =
                std::stoi(
                    payload.substr(pos),
                    &consumed
                );

            return consumed > 0;
        }
        catch (...)
        {
            return false;
        }
    };


    // ========================================================
    // 读取 taskId / pointId
    // ========================================================

    int taskId = 0;
    int pointId = 0;


    if (
        !extractInt(
            "taskId",
            taskId
        )
        ||
        !extractInt(
            "pointId",
            pointId
        )
    )
    {
        return
            "{"
            "\"ok\":false,"
            "\"error\":\"invalid_request\","
            "\"message\":\"缺少或无法解析 taskId / pointId\""
            "}";
    }


    if (
        taskId <= 0 ||
        pointId <= 0
    )
    {
        return
            "{"
            "\"ok\":false,"
            "\"error\":\"invalid_identity\","
            "\"message\":\"taskId / pointId 必须大于 0\""
            "}";
    }


    // ========================================================
    // 解除共享内存逻辑强制
    //
    // 不执行：
    //     writeModbus()
    //     FC06
    //     FC16
    // ========================================================

    const bool ok =
        releaseSharedMemoryValue(
            taskId,
            pointId
        );


    if (!ok)
    {
        std::ostringstream oss;

        oss
            << "{"
            << "\"ok\":false,"
            << "\"error\":\"shared_memory_point_not_found\","
            << "\"taskId\":"
            << taskId
            << ","
            << "\"pointId\":"
            << pointId
            << ","
            << "\"message\":\"共享内存中未找到指定逻辑点\""
            << "}";

        return oss.str();
    }


    // ========================================================
    // 成功
    //
    // 此时：
    //     forced      = 0
    //     forcedValue = 0
    //
    // value 不在这里伪造。
    // 下一轮真实 Modbus 采集会重新写入实际值。
    // ========================================================

    std::ostringstream oss;

    oss
        << "{"
        << "\"ok\":true,"
        << "\"taskId\":"
        << taskId
        << ","
        << "\"pointId\":"
        << pointId
        << ","
        << "\"forced\":false,"
        << "\"message\":\"共享内存逻辑点解除强制成功\""
        << "}";


    return oss.str();
}

// ============================================================
// JSON Escape
// ============================================================

std::string ModbusServer::jsonEscape(
    const std::string& value
)
{
    std::ostringstream oss;


    for (
        unsigned char c :
        value
    )
    {
        switch (c)
        {
        case '"':
            oss << "\\\"";
            break;


        case '\\':
            oss << "\\\\";
            break;


        case '\b':
            oss << "\\b";
            break;


        case '\f':
            oss << "\\f";
            break;


        case '\n':
            oss << "\\n";
            break;


        case '\r':
            oss << "\\r";
            break;


        case '\t':
            oss << "\\t";
            break;


        default:

            if (
                c < 0x20
            )
            {
                oss
                    << "\\u"
                    << std::hex
                    << std::setw(4)
                    << std::setfill('0')
                    << static_cast<int>(c)
                    << std::dec;
            }
            else
            {
                oss
                    << static_cast<char>(c);
            }

            break;
        }
    }


    return oss.str();
}


// ============================================================
// double 转 JSON
// ============================================================

std::string ModbusServer::doubleToJson(
    double value
)
{
    // ========================================================
    // NaN / Infinity
    //
    // JSON 不允许：
    //
    // NaN
    // Infinity
    // -Infinity
    //
    // 所以统一发送 null
    // ========================================================

    if (
        !std::isfinite(value)
    )
    {
        return "null";
    }


    std::ostringstream oss;


    oss
        << std::setprecision(15)
        << value;


    return oss.str();
}
