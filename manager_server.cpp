#include "manager_server.h"

#include "shared_memory.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include <cstring>
#include <cstdlib>
#include <cstdio>

#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <cmath>


// PYSM 协议
//
// RetroDcsDemo 的 ManagerClient 使用：
//
// 4 bytes   magic       "PYSM"
// 2 bytes   version     1
// 2 bytes   messageType
// 4 bytes   requestId
// 4 bytes   payloadLength
//
// 总计 16 bytes
//
// 所有整数使用 BigEndian

namespace
{

constexpr uint16_t PYSM_VERSION = 1;

constexpr uint16_t MSG_PING = 1;
constexpr uint16_t MSG_SNAPSHOT = 2;
constexpr uint16_t MSG_FORCE_VALUE = 3;

constexpr size_t PYSM_HEADER_SIZE = 16;

constexpr uint32_t MAX_PAYLOAD_SIZE = 1024 * 1024;


// 写入 uint16 大端

void writeU16(
    unsigned char* buffer,
    uint16_t value
)
{
    buffer[0] =
        static_cast<unsigned char>((value >> 8) & 0xff);

    buffer[1] =
        static_cast<unsigned char>(value & 0xff);
}


// 写入 uint32 大端

void writeU32(
    unsigned char* buffer,
    uint32_t value
)
{
    buffer[0] =
        static_cast<unsigned char>((value >> 24) & 0xff);

    buffer[1] =
        static_cast<unsigned char>((value >> 16) & 0xff);

    buffer[2] =
        static_cast<unsigned char>((value >> 8) & 0xff);

    buffer[3] =
        static_cast<unsigned char>(value & 0xff);
}


// 从 uint16 大端读取

uint16_t readU16(
    const unsigned char* buffer
)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(buffer[0]) << 8) |
        static_cast<uint16_t>(buffer[1])
    );
}


// 从 uint32 大端读取

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


// 从 JSON 中读取简单字符串
//
// 这里暂时不引入 JSON 库，避免给你的 ModbusCollector
// 增加额外依赖。
//
// 用于读取：
// "configHash":"xxx"
// "value":"123.45"

std::string getJsonString(
    const std::string& json,
    const std::string& key
)
{
    const std::string search =
        "\"" + key + "\"";

    size_t keyPos =
        json.find(search);

    if (keyPos == std::string::npos)
        return {};

    size_t colon =
        json.find(':', keyPos + search.size());

    if (colon == std::string::npos)
        return {};

    size_t quoteStart =
        json.find('"', colon + 1);

    if (quoteStart == std::string::npos)
        return {};

    ++quoteStart;

    std::string result;

    bool escape = false;

    for (size_t i = quoteStart;
         i < json.size();
         ++i)
    {
        char c = json[i];

        if (escape)
        {
            switch (c)
            {
            case '"':
                result.push_back('"');
                break;

            case '\\':
                result.push_back('\\');
                break;

            case '/':
                result.push_back('/');
                break;

            case 'n':
                result.push_back('\n');
                break;

            case 'r':
                result.push_back('\r');
                break;

            case 't':
                result.push_back('\t');
                break;

            default:
                result.push_back(c);
                break;
            }

            escape = false;
            continue;
        }

        if (c == '\\')
        {
            escape = true;
            continue;
        }

        if (c == '"')
            break;

        result.push_back(c);
    }

    return result;
}


// 从 JSON 中读取整数

long long getJsonInteger(
    const std::string& json,
    const std::string& key,
    long long defaultValue = 0
)
{
    const std::string search =
        "\"" + key + "\"";

    size_t keyPos =
        json.find(search);

    if (keyPos == std::string::npos)
        return defaultValue;

    size_t colon =
        json.find(':', keyPos + search.size());

    if (colon == std::string::npos)
        return defaultValue;

    size_t start =
        colon + 1;

    while (
        start < json.size() &&
        std::isspace(
            static_cast<unsigned char>(json[start])
        )
    )
    {
        ++start;
    }

    size_t end = start;

    if (
        end < json.size() &&
        (json[end] == '-' || json[end] == '+')
    )
    {
        ++end;
    }

    while (
        end < json.size() &&
        std::isdigit(
            static_cast<unsigned char>(json[end])
        )
    )
    {
        ++end;
    }

    if (end == start)
        return defaultValue;

    try
    {
        return std::stoll(
            json.substr(
                start,
                end - start
            )
        );
    }
    catch (...)
    {
        return defaultValue;
    }
}


// 读取 double

double getJsonDouble(
    const std::string& json,
    const std::string& key,
    double defaultValue = 0.0
)
{
    const std::string search =
        "\"" + key + "\"";

    size_t keyPos =
        json.find(search);

    if (keyPos == std::string::npos)
        return defaultValue;

    size_t colon =
        json.find(':', keyPos + search.size());

    if (colon == std::string::npos)
        return defaultValue;

    size_t start =
        colon + 1;

    while (
        start < json.size() &&
        std::isspace(
            static_cast<unsigned char>(json[start])
        )
    )
    {
        ++start;
    }

    size_t end = start;

    while (
        end < json.size() &&
        (
            std::isdigit(
                static_cast<unsigned char>(json[end])
            ) ||
            json[end] == '-' ||
            json[end] == '+' ||
            json[end] == '.' ||
            json[end] == 'e' ||
            json[end] == 'E'
        )
    )
    {
        ++end;
    }

    if (end == start)
        return defaultValue;

    try
    {
        return std::stod(
            json.substr(
                start,
                end - start
            )
        );
    }
    catch (...)
    {
        return defaultValue;
    }
}

} // namespace


// 构造

ManagerServer::ManagerServer(
    const std::string& host,
    uint16_t port,
    const std::string& configHash
)
    :
    host_(host),
    port_(port),
    configHash_(configHash),
    serverFd_(-1),
    running_(false)
{
}


// 析构

ManagerServer::~ManagerServer()
{
    stop();
}


// 创建 TCP Server

bool ManagerServer::createServerSocket()
{
    serverFd_ =
        ::socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

    if (serverFd_ < 0)
    {
        perror("socket");

        return false;
    }


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
        perror("setsockopt");

        close(serverFd_);

        serverFd_ = -1;

        return false;
    }


    sockaddr_in address{};

    address.sin_family =
        AF_INET;

    address.sin_port =
        htons(port_);


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
                << "无效的监听地址: "
                << host_
                << std::endl;

            close(serverFd_);

            serverFd_ = -1;

            return false;
        }
    }


    if (
        bind(
            serverFd_,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ) < 0
    )
    {
        perror("bind");

        close(serverFd_);

        serverFd_ = -1;

        return false;
    }


    if (
        listen(
            serverFd_,
            10
        ) < 0
    )
    {
        perror("listen");

        close(serverFd_);

        serverFd_ = -1;

        return false;
    }


    return true;
}


// 关闭 TCP Server

void ManagerServer::closeServerSocket()
{
    if (serverFd_ >= 0)
    {
        close(serverFd_);

        serverFd_ = -1;
    }
}


// start

bool ManagerServer::start()
{
    if (running_)
        return true;


    if (!createServerSocket())
        return false;


    running_ = true;


    std::cout
        << "===================================="
        << std::endl;

    std::cout
        << "system-managerd 启动成功"
        << std::endl;

    std::cout
        << "监听地址: "
        << host_
        << ":"
        << port_
        << std::endl;

    std::cout
        << "共享内存: /modbus_data"
        << std::endl;

    std::cout
        << "===================================="
        << std::endl;


    return true;
}


// stop

void ManagerServer::stop()
{
    if (!running_)
        return;


    running_ = false;

    closeServerSocket();
}


// run

void ManagerServer::run()
{
    if (!running_)
        return;


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
            if (
                errno == EINTR
            )
            {
                continue;
            }

            if (!running_)
                break;

            perror("accept");

            continue;
        }


        handleClient(clientFd);


        close(clientFd);
    }
}


// readExact

bool ManagerServer::readExact(
    int fd,
    void* buffer,
    size_t size
)
{
    char* ptr =
        static_cast<char*>(buffer);

    size_t received = 0;


    while (received < size)
    {
        ssize_t n =
            recv(
                fd,
                ptr + received,
                size - received,
                0
            );


        if (n == 0)
            return false;


        if (n < 0)
        {
            if (errno == EINTR)
                continue;

            return false;
        }


        received +=
            static_cast<size_t>(n);
    }


    return true;
}


// writeAll

bool ManagerServer::writeAll(
    int fd,
    const void* buffer,
    size_t size
)
{
    const char* ptr =
        static_cast<const char*>(buffer);

    size_t sent = 0;


    while (sent < size)
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
                continue;

            return false;
        }


        if (n == 0)
            return false;


        sent +=
            static_cast<size_t>(n);
    }


    return true;
}


// 发送 PYSM 响应

bool ManagerServer::sendResponse(
    int clientFd,
    uint16_t messageType,
    uint32_t requestId,
    const std::string& json
)
{
    const uint16_t responseType =
        static_cast<uint16_t>(
            messageType | 0x8000
        );


    if (json.size() > MAX_PAYLOAD_SIZE)
        return false;


    unsigned char header[
        PYSM_HEADER_SIZE
    ]{};


    header[0] = 'P';
    header[1] = 'Y';
    header[2] = 'S';
    header[3] = 'M';


    writeU16(
        header + 4,
        PYSM_VERSION
    );

    writeU16(
        header + 6,
        responseType
    );

    writeU32(
        header + 8,
        requestId
    );

    writeU32(
        header + 12,
        static_cast<uint32_t>(
            json.size()
        )
    );


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


    if (
        !json.empty() &&
        !writeAll(
            clientFd,
            json.data(),
            json.size()
        )
    )
    {
        return false;
    }


    return true;
}


// 处理一个客户端

void ManagerServer::handleClient(
    int clientFd
)
{
    unsigned char header[
        PYSM_HEADER_SIZE
    ]{};


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


    if (
        std::memcmp(
            header,
            "PYSM",
            4
        ) != 0
    )
    {
        std::cerr
            << "收到无效 PYSM 帧头"
            << std::endl;

        return;
    }


    const uint16_t version =
        readU16(header + 4);

    const uint16_t messageType =
        readU16(header + 6);

    const uint32_t requestId =
        readU32(header + 8);

    const uint32_t payloadLength =
        readU32(header + 12);


    if (version != PYSM_VERSION)
    {
        sendResponse(
            clientFd,
            messageType,
            requestId,
            R"({"ok":false,"error":"unsupported_version"})"
        );

        return;
    }


    if (
        payloadLength >
        MAX_PAYLOAD_SIZE
    )
    {
        sendResponse(
            clientFd,
            messageType,
            requestId,
            R"({"ok":false,"error":"payload_too_large"})"
        );

        return;
    }


    std::string payload;

    if (payloadLength > 0)
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


    const std::string response =
        handleRequest(
            messageType,
            payload
        );


    sendResponse(
        clientFd,
        messageType,
        requestId,
        response
    );
}


// 请求分发

std::string ManagerServer::handleRequest(
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


// JSON escape

std::string ManagerServer::jsonEscape(
    const std::string& value
)
{
    std::string result;

    result.reserve(
        value.size() + 16
    );


    for (unsigned char c : value)
    {
        switch (c)
        {
        case '"':
            result += "\\\"";
            break;

        case '\\':
            result += "\\\\";
            break;

        case '\b':
            result += "\\b";
            break;

        case '\f':
            result += "\\f";
            break;

        case '\n':
            result += "\\n";
            break;

        case '\r':
            result += "\\r";
            break;

        case '\t':
            result += "\\t";
            break;

        default:

            if (c < 0x20)
            {
                char buffer[8];

                std::snprintf(
                    buffer,
                    sizeof(buffer),
                    "\\u%04x",
                    c
                );

                result += buffer;
            }
            else
            {
                result +=
                    static_cast<char>(c);
            }

            break;
        }
    }


    return result;
}


// double 转 JSON

std::string ManagerServer::doubleToJson(
    double value
)
{
    if (std::isnan(value))
        return "null";

    if (std::isinf(value))
        return "null";


    std::ostringstream stream;

    stream
        << std::setprecision(15)
        << value;


    return stream.str();
}


// 时间字符串转换为纳秒
//
// ModbusCollector 当前时间格式：
//
// YYYY-MM-DD HH:MM:SS
//
// 例如：
// 2026-08-26 22:30:15

long long ManagerServer::timestampToNs(
    const std::string& timestamp
)
{
    if (timestamp.size() < 19)
        return 0;


    std::tm tm{};


    try
    {
        tm.tm_year =
            std::stoi(
                timestamp.substr(0, 4)
            ) - 1900;

        tm.tm_mon =
            std::stoi(
                timestamp.substr(5, 2)
            ) - 1;

        tm.tm_mday =
            std::stoi(
                timestamp.substr(8, 2)
            );

        tm.tm_hour =
            std::stoi(
                timestamp.substr(11, 2)
            );

        tm.tm_min =
            std::stoi(
                timestamp.substr(14, 2)
            );

        tm.tm_sec =
            std::stoi(
                timestamp.substr(17, 2)
            );
    }
    catch (...)
    {
        return 0;
    }


    time_t value =
        std::mktime(&tm);


    if (value == static_cast<time_t>(-1))
        return 0;


    return static_cast<long long>(
        value
    ) * 1000000000LL;
}


// PING

std::string ManagerServer::handlePing(
    const std::string& payload
)
{
    std::string requestConfigHash =
        getJsonString(
            payload,
            "configHash"
        );


    std::ostringstream json;


    json
        << "{"
        << "\"ok\":true,"
        << "\"service\":\"system-managerd\","
        << "\"managerPid\":"
        << static_cast<long long>(getpid())
        << ","
        << "\"configVersion\":1,"
        << "\"configHash\":\""
        << jsonEscape(configHash_)
        << "\","
        << "\"sharedMemory\":\"/modbus_data\","
        << "\"protocolVersion\":1"
        << "}";


    return json.str();
}


// 在线数据快照

std::string ManagerServer::handleSnapshot()
{
    SharedData* data =
        getSharedMemory();


    if (data == nullptr)
    {
        return
            R"({"ok":false,"error":"shared_memory_not_available","message":"无法打开 /modbus_data"})";
    }


    std::ostringstream json;


    json
        << "{"
        << "\"ok\":true,"
        << "\"points\":[";


    bool first = true;


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
            R"({"ok":false,"error":"shared_memory_lock_failed"})";
    }


    int count =
        data->count;


    if (
        count < 0 ||
        count > MAX_VARIABLES
    )
    {
        count = 0;
    }


    for (
        int i = 0;
        i < count;
        ++i
    )
    {
        const Variable& variable =
            data->vars[i];


        if (!first)
            json << ",";

        first = false;


        const int pointId =
            i + 1;


        const std::string direction =
            variable.direction[0] != '\0'
                ? variable.direction
                : "INPUT";


        std::string status =
            variable.status == 1
                ? "Good"
                : "Bad_Comm";


        const long long timestampNs =
            timestampToNs(
                variable.timestamp
            );


        json
            << "{"

            // 旧兼容 ID
            // 暂时保留，避免影响 Qt 现有代码

            << "\"id\":"
            << pointId
            << ","

            << "\"systemPointId\":"
            << pointId
            << ","


            // 多 Modbus 任务真实身份

            << "\"taskId\":"
            << variable.taskId
            << ","

            << "\"pointId\":"
            << variable.pointId
            << ","


            // name

            << "\"name\":\""
            << jsonEscape(
                variable.name
            )
            << "\","

            << "\"dataType\":\""
            << jsonEscape(
                variable.datatype
            )
            << "\","

            << "\"direction\":\""
            << direction
            << "\","

            << "\"value\":"
            << doubleToJson(
                variable.value
            )
            << ","

            << "\"status\":\""
            << status
            << "\","

            << "\"timestampNs\":\""
            << timestampNs
            << "\","

            << "\"sequence\":"
            << i + 1
            << ","

            << "\"slave\":"
            << variable.slave
            << ","

            << "\"function\":"
            << variable.function
            << ","

            << "\"address\":"
            << variable.address
            << ","

            << "\"telegram\":\""
            << jsonEscape(
                variable.telegram
            )
            << "\""

            << "}";
    }


    pthread_mutex_unlock(
        &data->mutex
    );


    munmap(
        data,
        sizeof(SharedData)
    );


    json
        << "],"
        << "\"count\":"
        << count
        << "}";


    return json.str();
}


// 强制修改点值
//
// RetroDcsDemo 的 forceOnlineValue() 会发送：
//
// {
//     "pointId": 1,
//     "value": "123.45",
//     "configHash": "..."
// }
//
// 当前阶段我们把 pointId 映射到共享内存数组下标：
// pointId 1 -> vars[0]
// pointId 2 -> vars[1]
// ...
//
// 后续正式整合 SQLite 点表后，
// 这里再改成 systemPointId / 数据库 point.id 的映射。

std::string ManagerServer::handleForceValue(
    const std::string& payload
)
{
    const long long pointId =
        getJsonInteger(
            payload,
            "pointId",
            0
        );


    const std::string valueText =
        getJsonString(
            payload,
            "value"
        );


    if (
        pointId <= 0 ||
        pointId > MAX_VARIABLES
    )
    {
        return
            R"({"ok":false,"error":"invalid_point_id","message":"点ID无效"})";
    }


    if (valueText.empty())
    {
        return
            R"({"ok":false,"error":"invalid_value","message":"强制值为空"})";
    }


    double value = 0.0;


    try
    {
        value =
            std::stod(
                valueText
            );
    }
    catch (...)
    {
        return
            R"({"ok":false,"error":"invalid_value","message":"强制值不是有效数字"})";
    }


    SharedData* data =
        getSharedMemory();


    if (data == nullptr)
    {
        return
            R"({"ok":false,"error":"shared_memory_not_available","message":"无法打开 /modbus_data"})";
    }


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
            R"({"ok":false,"error":"shared_memory_lock_failed"})";
    }


    int index =
        static_cast<int>(
            pointId - 1
        );


    if (
        index >= data->count
    )
    {
        pthread_mutex_unlock(
            &data->mutex
        );

        munmap(
            data,
            sizeof(SharedData)
        );


        return
            R"({"ok":false,"error":"point_not_found","message":"共享内存中不存在该点"})";
    }


    data->vars[index].value =
        value;


    // 强制值写入以后保持当前通信状态。
    //
    // 后续可以增加 force flag，
    // 区分真实采集值和强制值。


    const std::string pointName =
        data->vars[index].name;


    pthread_mutex_unlock(
        &data->mutex
    );


    munmap(
        data,
        sizeof(SharedData)
    );


    std::cout
        << "强制值: "
        << pointName
        << " = "
        << value
        << std::endl;


    std::ostringstream json;

    json
        << "{"
        << "\"ok\":true,"
        << "\"pointId\":"
        << pointId
        << ","
        << "\"value\":"
        << doubleToJson(value)
        << ","
        << "\"message\":\"force value applied\""
        << "}";


    return json.str();
}
