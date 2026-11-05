#include "modbus_client.h"
#include "system_manager.h"

#include <modbus/modbus.h>

#include <iostream>
#include <iomanip>
#include <cstring>
#include <sstream>
#include <vector>
#include <cerrno>
#include <cstdint>
#include <string>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <mutex>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include "shared_memory.h"
#include "time_utils.h"

extern modbus_t *ctx;

namespace {

// ============================================================
// 第一阶段：独立的 Modbus TCP 原始报文通道
//
// 注意：
// 这一阶段故意不再对 libmodbus 的 socket 直接调用 send()/recv()。
// 当前 main.cpp 仍然创建 ctx，但本文件使用独立 TCP socket 访问
// 127.0.0.1:502，避免与 libmodbus 的内部通信状态互相干扰。
// ============================================================

constexpr const char *MODBUS_HOST = "127.0.0.1";
constexpr int MODBUS_PORT = 502;
constexpr int SOCKET_TIMEOUT_MS = 2000;



// ============================================================
// 字节数组转 HEX
// ============================================================

static std::string bytesToHex(
    const uint8_t *data,
    size_t length
)
{
    std::ostringstream oss;

    oss << std::hex
        << std::uppercase
        << std::setfill('0');

    for (size_t i = 0; i < length; ++i)
    {
        if (i != 0)
            oss << ' ';

        oss << std::setw(2)
            << static_cast<unsigned int>(data[i]);
    }

    return oss.str();
}



// ============================================================
// 等待 socket 有数据
//
// 使用 poll()，避免直接 recv() 时出现
//
// Resource temporarily unavailable
//
// ============================================================

static bool waitReadable(
    int fd,
    int timeoutMs
)
{
    struct pollfd pfd{};

    pfd.fd = fd;
    pfd.events = POLLIN;

    while (true)
    {
        int rc = ::poll(
            &pfd,
            1,
            timeoutMs
        );

        if (rc > 0)
        {
            if (pfd.revents & POLLIN)
            {
                return true;
            }

            if (pfd.revents &
                (POLLERR | POLLHUP | POLLNVAL))
            {
                std::cerr
                    << "[MODBUS] socket 状态异常，revents=0x"
                    << std::hex
                    << pfd.revents
                    << std::dec
                    << std::endl;

                return false;
            }

            continue;
        }

        if (rc == 0)
        {
            std::cerr
                << "[MODBUS] 等待响应超时"
                << std::endl;

            return false;
        }

        if (errno == EINTR)
        {
            continue;
        }

        std::cerr
            << "[MODBUS] poll 失败: "
            << std::strerror(errno)
            << std::endl;

        return false;
    }
}


// ============================================================
// 完整发送
// ============================================================

static bool sendAll(
    int fd,
    const uint8_t *data,
    size_t length
)
{
    size_t sent = 0;

    while (sent < length)
    {
        ssize_t rc = ::send(
            fd,
            data + sent,
            length - sent,
            MSG_NOSIGNAL
        );

        if (rc < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            std::cerr
                << "[MODBUS] TX 发送失败: "
                << std::strerror(errno)
                << std::endl;

            return false;
        }

        if (rc == 0)
        {
            std::cerr
                << "[MODBUS] send 返回 0"
                << std::endl;

            return false;
        }

        sent += static_cast<size_t>(rc);
    }

    return true;
}


// ============================================================
// 完整接收指定长度
// ============================================================

static bool recvAll(
    int fd,
    uint8_t *data,
    size_t length
)
{
    size_t received = 0;

    while (received < length)
    {
        if (!waitReadable(
                fd,
                SOCKET_TIMEOUT_MS))
        {
            return false;
        }

        ssize_t rc = ::recv(
            fd,
            data + received,
            length - received,
            0
        );

        if (rc < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            std::cerr
                << "[MODBUS] RX 接收失败: "
                << std::strerror(errno)
                << std::endl;

            return false;
        }

        if (rc == 0)
        {
            std::cerr
                << "[MODBUS] Modbus服务器关闭了 TCP 连接"
                << std::endl;

            return false;
        }

        received += static_cast<size_t>(rc);
    }

    return true;
}


// ============================================================
// 建立独立的原始 Modbus TCP 连接
// ============================================================

static bool connectRawSocket(
    ModbusTaskConnection &connection,
    const std::string &host,
    int port
)
{
    // ========================================================
    // 当前任务已经连接到正确 endpoint
    // ========================================================

    if (
        connection.socketFd >= 0
        &&
        connection.connectedHost == host
        &&
        connection.connectedPort == port
    )
    {
        return true;
    }


    // ========================================================
    // 当前任务已有旧连接，先关闭
    // ========================================================

    if (connection.socketFd >= 0)
    {
        std::cout
            << "[MODBUS] Task TCP endpoint 变化: "
            << connection.connectedHost
            << ":"
            << connection.connectedPort
            << " -> "
            << host
            << ":"
            << port
            << std::endl;


        ::close(
            connection.socketFd
        );


        connection.socketFd = -1;

        connection.connectedHost.clear();

        connection.connectedPort = -1;
    }


    // ========================================================
    // 参数检查
    // ========================================================

    if (
        host.empty()
        ||
        port <= 0
        ||
        port > 65535
    )
    {
        std::cerr
            << "[MODBUS] 非法 TCP 地址: "
            << host
            << ":"
            << port
            << std::endl;

        return false;
    }


    // ========================================================
    // 创建当前任务自己的 socket
    // ========================================================

    int fd =
        ::socket(
            AF_INET,
            SOCK_STREAM,
            0
        );


    if (fd < 0)
    {
        std::cerr
            << "[MODBUS] 创建 TCP socket 失败: "
            << std::strerror(errno)
            << std::endl;

        return false;
    }


    // ========================================================
    // Socket timeout
    //
    // 这一阶段继续保持原来的 2000ms。
    // 后面再正式接入 task.timeout。
    // ========================================================

    struct timeval timeout{};

    timeout.tv_sec =
        SOCKET_TIMEOUT_MS / 1000;

    timeout.tv_usec =
        (SOCKET_TIMEOUT_MS % 1000)
        *
        1000;


    ::setsockopt(
        fd,
        SOL_SOCKET,
        SO_SNDTIMEO,
        &timeout,
        sizeof(timeout)
    );


    ::setsockopt(
        fd,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &timeout,
        sizeof(timeout)
    );


    // ========================================================
    // IPv4
    // ========================================================

    struct sockaddr_in server{};

    server.sin_family =
        AF_INET;

    server.sin_port =
        htons(
            static_cast<uint16_t>(
                port
            )
        );


    if (
        ::inet_pton(
            AF_INET,
            host.c_str(),
            &server.sin_addr
        )
        !=
        1
    )
    {
        std::cerr
            << "[MODBUS] IP 地址无效: "
            << host
            << std::endl;

        ::close(fd);

        return false;
    }


    // ========================================================
    // 建立当前任务自己的 TCP 连接
    // ========================================================

    if (
        ::connect(
            fd,
            reinterpret_cast<
                struct sockaddr *
            >(
                &server
            ),
            sizeof(server)
        )
        <
        0
    )
    {
        std::cerr
            << "[MODBUS] 连接 "
            << host
            << ":"
            << port
            << " 失败: "
            << std::strerror(errno)
            << std::endl;

        ::close(fd);

        return false;
    }


    connection.socketFd =
        fd;

    connection.connectedHost =
        host;

    connection.connectedPort =
        port;


    std::cout
        << "[MODBUS] Task 独立 TCP 通道连接成功: "
        << host
        << ":"
        << port
        << " fd="
        << connection.socketFd
        << std::endl;


    return true;
}


// ============================================================
// Transaction ID
// ============================================================

static uint16_t nextTransactionId(
    ModbusTaskConnection &connection
)
{
    uint16_t id =
        static_cast<uint16_t>(
            connection.transactionId
        );


    ++connection.transactionId;


    // Transaction ID = 0 时跳过 0
    if (connection.transactionId == 0)
    {
        connection.transactionId = 1;
    }


    if (id == 0)
    {
        id = 1;
    }


    return id;
}


// ============================================================
// 写入一个 uint16，大端
// ============================================================

static void appendU16(
    std::vector<uint8_t> &frame,
    uint16_t value
)
{
    frame.push_back(
        static_cast<uint8_t>(
            (value >> 8) & 0xFF
        )
    );

    frame.push_back(
        static_cast<uint8_t>(
            value & 0xFF
        )
    );
}


// ============================================================
// 从字节数组读取 uint16，大端
// ============================================================

static uint16_t readU16(
    const uint8_t *data
)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8) |
        static_cast<uint16_t>(data[1])
    );
}

static uint16_t modbusCrc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) : static_cast<uint16_t>(crc >> 1);
    }
    return crc;
}

static bool connectRtu(ModbusTaskConnection &connection, const ModbusTask &task, std::string &error)
{
    if (connection.rtuContext && connection.connectedSerialPort == task.serialPort &&
        connection.connectedBaudRate == task.baudRate && connection.connectedParity == task.parity &&
        connection.connectedDataBits == task.dataBits && connection.connectedStopBits == task.stopBits) {
        modbus_set_slave(connection.rtuContext, task.slaveId);
        return true;
    }
    if (connection.rtuContext) {
        modbus_close(connection.rtuContext);
        modbus_free(connection.rtuContext);
        connection.rtuContext = nullptr;
    }
    connection.rtuContext = modbus_new_rtu(task.serialPort.c_str(), task.baudRate, task.parity,
                                            task.dataBits, task.stopBits);
    if (!connection.rtuContext) { error = "无法创建 RTU 串口上下文"; return false; }
    modbus_set_slave(connection.rtuContext, task.slaveId);
    modbus_set_response_timeout(connection.rtuContext,
        static_cast<uint32_t>(task.timeout / 1000), static_cast<uint32_t>((task.timeout % 1000) * 1000));
    if (modbus_connect(connection.rtuContext) == -1) {
        error = std::string("打开串口失败: ") + modbus_strerror(errno);
        modbus_free(connection.rtuContext);
        connection.rtuContext = nullptr;
        return false;
    }
    connection.connectedSerialPort = task.serialPort;
    connection.connectedBaudRate = task.baudRate;
    connection.connectedParity = task.parity;
    connection.connectedDataBits = task.dataBits;
    connection.connectedStopBits = task.stopBits;
    return true;
}

static bool transactRtu(ModbusTaskConnection &connection, const ModbusTask &task, uint8_t function,
                        int address, int count, const uint16_t *writeRegisters, uint16_t *readRegisters,
                        std::vector<uint8_t> &txFrame, std::vector<uint8_t> &rxFrame, std::string &error)
{
    // RTU 是单主请求/响应总线。同一串口上的周期采集与人工 OUTPUT
    // 必须互斥，避免响应被另一个线程或文件描述符读走。
    static std::mutex serialLocksGuard;
    static std::map<std::string, std::shared_ptr<std::mutex>> serialLocks;
    std::shared_ptr<std::mutex> serialLock;
    {
        std::lock_guard<std::mutex> guard(serialLocksGuard);
        auto &entry = serialLocks[task.serialPort];
        if (!entry) entry = std::make_shared<std::mutex>();
        serialLock = entry;
    }
    std::lock_guard<std::mutex> transactionGuard(*serialLock);

    if (!connectRtu(connection, task, error)) return false;
    std::vector<uint8_t> request;
    request.push_back(static_cast<uint8_t>(task.slaveId));
    request.push_back(function);
    appendU16(request, static_cast<uint16_t>(address));
    if (function == 0x06) appendU16(request, writeRegisters[0]);
    else {
        appendU16(request, static_cast<uint16_t>(count));
        if (function == 0x10) {
            request.push_back(static_cast<uint8_t>(count * 2));
            for (int i = 0; i < count; ++i) appendU16(request, writeRegisters[i]);
        }
    }
    txFrame = request;
    const uint16_t crc = modbusCrc16(request.data(), request.size());
    txFrame.push_back(static_cast<uint8_t>(crc & 0xFF));
    txFrame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));

    if (modbus_send_raw_request(connection.rtuContext, request.data(), static_cast<int>(request.size())) == -1) {
        error = std::string("RTU 发送失败: ") + modbus_strerror(errno);
        return false;
    }
    uint8_t response[MODBUS_RTU_MAX_ADU_LENGTH] = {0};
    const int rc = modbus_receive_confirmation(connection.rtuContext, response);
    if (rc < 0) { error = std::string("RTU 响应失败: ") + modbus_strerror(errno); return false; }
    rxFrame.assign(response, response + rc);
    if (rc < 5 || response[0] != static_cast<uint8_t>(task.slaveId)) { error = "RTU 响应站号或长度错误"; return false; }
    if (response[1] == static_cast<uint8_t>(function | 0x80)) {
        error = "设备返回 Modbus 异常码 " + std::to_string(response[2]); return false;
    }
    if (response[1] != function) { error = "RTU 响应功能码不匹配"; return false; }
    if ((function == 0x03 || function == 0x04)) {
        if (response[2] != count * 2 || rc < 3 + count * 2 + 2) { error = "RTU 读取响应长度错误"; return false; }
        for (int i = 0; i < count; ++i) readRegisters[i] = readU16(&response[3 + i * 2]);
    } else if (rc < 8 || readU16(&response[2]) != static_cast<uint16_t>(address) ||
               readU16(&response[4]) != static_cast<uint16_t>(function == 0x06 ? writeRegisters[0] : count)) {
        error = "RTU 写响应内容不匹配"; return false;
    }
    return true;
}


// ============================================================
// 真正的 Modbus TCP 原始报文读取
// ============================================================

static bool readModbusTcpRaw(
    ModbusTaskConnection &connection,
    const std::string &host,
    int port,
    int slave,
    int function,
    int address,
    int count,
    uint16_t *reg,
    std::vector<uint8_t> &txFrame,
    std::vector<uint8_t> &rxFrame
)
{
    txFrame.clear();
    rxFrame.clear();


    // --------------------------------------------------------
    // 参数检查
    // --------------------------------------------------------

    if (slave < 0 || slave > 255)
    {
        std::cerr
            << "[MODBUS] 非法 Unit ID: "
            << slave
            << std::endl;

        return false;
    }


    if (function != 3 &&
        function != 4)
    {
        std::cerr
            << "[MODBUS] 第一阶段只支持功能码 03 / 04，当前: "
            << function
            << std::endl;

        return false;
    }


    if (address < 0 ||
        address > 0xFFFF)
    {
        std::cerr
            << "[MODBUS] 非法寄存器地址: "
            << address
            << std::endl;

        return false;
    }


    if (count <= 0 ||
        count > 125)
    {
        std::cerr
            << "[MODBUS] 非法读取数量: "
            << count
            << std::endl;

        return false;
    }


    // --------------------------------------------------------
    // 建立原始 TCP 通道
    // --------------------------------------------------------

    if (
        !connectRawSocket(
            connection,
            host,
            port
        )
    )
    {
        return false;
    }


    // --------------------------------------------------------
    // Transaction ID
    // --------------------------------------------------------

    const uint16_t transactionId =
        nextTransactionId(
            connection
        );


    // ========================================================
    // 构造 Modbus TCP 请求
    //
    // MBAP Header:
    //
    // Transaction ID   2 bytes
    // Protocol ID      2 bytes
    // Length           2 bytes
    // Unit ID          1 byte
    //
    // PDU:
    //
    // Function         1 byte
    // Address          2 bytes
    // Quantity         2 bytes
    //
    // 总长度：
    //
    // 2 + 2 + 2 + 1 + 1 + 2 + 2
    //
    // = 12 bytes
    // ========================================================

    txFrame.reserve(12);


    // Transaction ID

    appendU16(
        txFrame,
        transactionId
    );


    // Protocol ID = 0

    appendU16(
        txFrame,
        0
    );


    // Length = 6
    //
    // Unit ID 1
    // Function 1
    // Address 2
    // Quantity 2

    appendU16(
        txFrame,
        6
    );


    // Unit ID

    txFrame.push_back(
        static_cast<uint8_t>(slave)
    );


    // Function

    txFrame.push_back(
        static_cast<uint8_t>(function)
    );


    // Address

    appendU16(
        txFrame,
        static_cast<uint16_t>(address)
    );


    // Quantity

    appendU16(
        txFrame,
        static_cast<uint16_t>(count)
    );


    // ========================================================
    // 输出真正的 TX
    // ========================================================

    std::cout
        << std::endl
        << "============================================================"
        << std::endl;

    std::cout
        << "[MODBUS TX] 真实原始报文"
        << std::endl;

    std::cout
        << "slave    : "
        << slave
        << std::endl;

    std::cout
        << "function : "
        << function
        << std::endl;

    std::cout
        << "address  : "
        << address
        << std::endl;

    std::cout
        << "quantity : "
        << count
        << std::endl;

    std::cout
        << "length   : "
        << txFrame.size()
        << " bytes"
        << std::endl;

    std::cout
        << "HEX      : "
        << bytesToHex(
            txFrame.data(),
            txFrame.size()
        )
        << std::endl;


    // ========================================================
    // 发送完整 Modbus TCP 请求
    // ========================================================

    if (!sendAll(
            connection.socketFd,
            txFrame.data(),
            txFrame.size()))
    {
        if (connection.socketFd >= 0)
        {
            ::close(
                connection.socketFd
            );

            connection.socketFd = -1;

            connection.connectedHost.clear();

            connection.connectedPort = -1;
        }

        return false;
    }


    // ========================================================
    // 接收 MBAP Header + Unit ID
    //
    // 一共 7 字节
    //
    // [0..1] Transaction ID
    // [2..3] Protocol ID
    // [4..5] Length
    // [6]    Unit ID
    // ========================================================

    uint8_t header[7] = {0};


    if (!recvAll(
            connection.socketFd,
            header,
            sizeof(header)))
    {
        if (connection.socketFd >= 0)
        {
            ::close(
                connection.socketFd
            );

            connection.socketFd = -1;

            connection.connectedHost.clear();

            connection.connectedPort = -1;
        }
        return false;
    }


    // ========================================================
    // 解析 Header
    // ========================================================

    const uint16_t responseTransactionId =
        readU16(&header[0]);


    const uint16_t protocolId =
        readU16(&header[2]);


    const uint16_t mbapLength =
        readU16(&header[4]);


    const uint8_t responseSlave =
        header[6];


    // ========================================================
    // Transaction ID 检查
    // ========================================================

    if (responseTransactionId !=
        transactionId)
    {
        std::cerr
            << "[MODBUS] Transaction ID 不匹配！"
            << std::endl;

        std::cerr
            << "发送: "
            << transactionId
            << std::endl;

        std::cerr
            << "接收: "
            << responseTransactionId
            << std::endl;

        if (connection.socketFd >= 0)
        {
            ::close(
                connection.socketFd
            );

            connection.socketFd = -1;

            connection.connectedHost.clear();

            connection.connectedPort = -1;
        }

        return false;
    }


    // ========================================================
    // Protocol ID
    // ========================================================

    if (protocolId != 0)
    {
        std::cerr
            << "[MODBUS] Protocol ID 错误: "
            << protocolId
            << std::endl;

        if (connection.socketFd >= 0)
        {
            ::close(
                connection.socketFd
            );

            connection.socketFd = -1;

            connection.connectedHost.clear();

            connection.connectedPort = -1;
        }

        return false;
    }


    // ========================================================
    // MBAP Length
    //
    // Length = Unit ID + PDU
    // ========================================================

    if (mbapLength < 2 ||
        mbapLength > 253)
    {
        std::cerr
            << "[MODBUS] 非法 MBAP Length: "
            << mbapLength
            << std::endl;

        if (connection.socketFd >= 0)
        {
            ::close(
                connection.socketFd
            );

            connection.socketFd = -1;

            connection.connectedHost.clear();

            connection.connectedPort = -1;
        }

        return false;
    }


    // ========================================================
    // Header 已经读取了 Unit ID
    //
    // 所以剩余：
    //
    // mbapLength - 1
    //
    // ========================================================

    const size_t remainingLength =
        static_cast<size_t>(
            mbapLength - 1
        );


    std::vector<uint8_t> remaining(
        remainingLength
    );


    if (!recvAll(
            connection.socketFd,
            remaining.data(),
            remaining.size()))
    {
        if (connection.socketFd >= 0)
        {
            ::close(
                connection.socketFd
            );

            connection.socketFd = -1;

            connection.connectedHost.clear();

            connection.connectedPort = -1;
        }

        return false;
    }


    // ========================================================
    // 拼接完整 RX 原始报文
    // ========================================================

    rxFrame.reserve(
        sizeof(header) +
        remaining.size()
    );


    rxFrame.insert(
        rxFrame.end(),
        header,
        header + sizeof(header)
    );


    rxFrame.insert(
        rxFrame.end(),
        remaining.begin(),
        remaining.end()
    );


    // ========================================================
    // 输出真正 RX
    // ========================================================

    std::cout
        << "[MODBUS RX] 真实原始报文"
        << std::endl;

    std::cout
        << "length   : "
        << rxFrame.size()
        << " bytes"
        << std::endl;

    std::cout
        << "HEX      : "
        << bytesToHex(
            rxFrame.data(),
            rxFrame.size()
        )
        << std::endl;


    // ========================================================
    // Unit ID 检查
    // ========================================================

    if (responseSlave !=
        static_cast<uint8_t>(slave))
    {
        std::cerr
            << "[MODBUS] Unit ID 不匹配，发送: "
            << slave
            << "，接收: "
            << static_cast<int>(
                responseSlave
            )
            << std::endl;

        return false;
    }


    // ========================================================
    // RX 最小长度
    //
    // 7 bytes MBAP
    // + Function
    //
    // = 8 bytes
    // ========================================================

    if (rxFrame.size() < 8)
    {
        std::cerr
            << "[MODBUS] RX 帧长度不足，无法解析功能码"
            << std::endl;

        return false;
    }


    const uint8_t responseFunction =
        rxFrame[7];


    // ========================================================
    // Modbus Exception Response
    //
    // 例如：
    //
    // 01 83 02
    //
    // 0x83 = 0x03 | 0x80
    // ========================================================

    if (responseFunction ==
        static_cast<uint8_t>(
            function | 0x80
        ))
    {
        if (rxFrame.size() < 9)
        {
            std::cerr
                << "[MODBUS] 异常响应长度不足"
                << std::endl;

            return false;
        }


        const uint8_t exceptionCode =
            rxFrame[8];


        std::cerr
            << "[MODBUS] Modbus Exception Response"
            << std::endl;


        std::cerr
            << "function      : 0x"
            << std::hex
            << std::uppercase
            << static_cast<int>(
                responseFunction
            )
            << std::dec
            << std::endl;


        std::cerr
            << "exception code: 0x"
            << std::hex
            << std::uppercase
            << static_cast<int>(
                exceptionCode
            )
            << std::dec
            << std::endl;


        return false;
    }


    // ========================================================
    // Function Code 检查
    // ========================================================

    if (responseFunction !=
        static_cast<uint8_t>(function))
    {
        std::cerr
            << "[MODBUS] Function Code 不匹配，发送: "
            << function
            << "，接收: "
            << static_cast<int>(
                responseFunction
            )
            << std::endl;

        return false;
    }


    // ========================================================
    // Byte Count
    // ========================================================

    if (rxFrame.size() < 9)
    {
        std::cerr
            << "[MODBUS] RX 帧长度不足，无法读取 Byte Count"
            << std::endl;

        return false;
    }


    const uint8_t byteCount =
        rxFrame[8];


    const size_t expectedByteCount =
        static_cast<size_t>(count) * 2;


    if (byteCount !=
        expectedByteCount)
    {
        std::cerr
            << "[MODBUS] Byte Count 不正确"
            << std::endl;

        std::cerr
            << "期望: "
            << expectedByteCount
            << std::endl;

        std::cerr
            << "实际: "
            << static_cast<int>(
                byteCount
            )
            << std::endl;

        return false;
    }


    // ========================================================
    // 检查完整帧长度
    //
    // 7 MBAP
    // + 1 Function
    // + 1 Byte Count
    // + N Data
    //
    // = 9 + Data
    // ========================================================

    const size_t expectedFrameLength =
        9 + expectedByteCount;


    if (rxFrame.size() !=
        expectedFrameLength)
    {
        std::cerr
            << "[MODBUS] RX 帧长度与 Byte Count 不一致"
            << std::endl;

        std::cerr
            << "期望完整长度: "
            << expectedFrameLength
            << std::endl;

        std::cerr
            << "实际完整长度: "
            << rxFrame.size()
            << std::endl;

        return false;
    }


    // ========================================================
    // 提取寄存器
    //
    // Modbus TCP 数据是 Big Endian
    //
    // 例如：
    //
    // 30 39
    //
    // = 0x3039
    // = 12345
    // ========================================================

    for (int i = 0; i < count; ++i)
    {
        const size_t offset =
            9 +
            static_cast<size_t>(
                i * 2
            );


        reg[i] =
            readU16(
                &rxFrame[offset]
            );
    }


    // ========================================================
    // 输出寄存器
    // ========================================================

    std::cout
        << "寄存器数量: "
        << count
        << std::endl;


    for (int i = 0; i < count; ++i)
    {
        std::cout
            << "reg["
            << i
            << "] = 0x"
            << std::hex
            << std::uppercase
            << std::setw(4)
            << std::setfill('0')
            << reg[i]
            << std::dec
            << std::setfill(' ')
            << std::endl;
    }


    std::cout
        << "============================================================"
        << std::endl;


    return true;
}

} // namespace


// ============================================================
// 关闭指定任务自己的 Modbus TCP 连接
//
// 不会影响其他任务的 TCP Socket。
// ============================================================

void closeModbusTaskConnection(
    ModbusTaskConnection &connection
)
{
    if (connection.socketFd >= 0)
    {
        ::close(
            connection.socketFd
        );

        connection.socketFd = -1;
    }

    connection.connectedHost.clear();

    connection.connectedPort = -1;

    if (connection.rtuContext)
    {
        modbus_close(connection.rtuContext);
        modbus_free(connection.rtuContext);
        connection.rtuContext = nullptr;
    }
    connection.connectedSerialPort.clear();
    connection.connectedBaudRate = 0;
}

// ============================================================
// 对外的 Modbus 读取函数
// ============================================================

void readModbus(
    ModbusTaskConnection &connection,
    const ModbusTask &task,
    const Point &p
)
{
    uint16_t reg[8] = {0};


    // ========================================================
    // 根据数据类型确定寄存器数量
    // ========================================================

    int count = 1;


    if (p.datatype == "FLOAT" ||
        p.datatype == "INT32" ||
        p.datatype == "UINT32")
    {
        count = 2;
    }
    else if (p.datatype == "DOUBLE")
    {
        count = 4;
    }


    if (count > 8)
    {
        std::cerr
            << "[MODBUS] 内部寄存器缓冲区不足"
            << std::endl;

        return;
    }


    // ========================================================
    // 第一阶段支持：
    //
    // 03 Read Holding Registers
    // 04 Read Input Registers
    // ========================================================

    if (p.function != 3 &&
        p.function != 4)
    {
        std::cout
            << "暂不支持功能码:"
            << p.function
            << std::endl;

        return;
    }


    // ========================================================
    // 保存真正 TX / RX 原始报文
    // ========================================================

    std::vector<uint8_t> txFrame;
    std::vector<uint8_t> rxFrame;


    // ========================================================
    // 执行真正 Modbus TCP 通讯
    // ========================================================

    std::string communicationError;
    const bool ok = task.transport == "RTU"
        ? transactRtu(connection, task, static_cast<uint8_t>(p.function), p.address, count,
                      nullptr, reg, txFrame, rxFrame, communicationError)
        : readModbusTcpRaw(
            connection,
            task.host,
            task.port,
            task.slaveId,
            p.function,
            p.address,
            count,
            reg,
            txFrame,
            rxFrame
        );


    if (!ok)
    {
        std::cout
            << "读取失败"
            << (communicationError.empty() ? "" : ": " + communicationError)
            << std::endl;

        return;
    }


    // ========================================================
    // 时间
    // ========================================================

    const std::string timestamp =
        getTime();


// ============================================================
// 生成真实 TX 报文
// ============================================================

std::stringstream txTelegramStream;

for (unsigned char byte : txFrame)
{
    txTelegramStream
        << std::uppercase
        << std::hex
        << std::setw(2)
        << std::setfill('0')
        << static_cast<int>(byte)
        << " ";
}


const std::string txTelegram =
    txTelegramStream.str();


// ============================================================
// 生成真实 RX 报文
// ============================================================

std::stringstream rxTelegramStream;

for (unsigned char byte : rxFrame)
{
    rxTelegramStream
        << std::uppercase
        << std::hex
        << std::setw(2)
        << std::setfill('0')
        << static_cast<int>(byte)
        << " ";
}


const std::string rxTelegram =
    rxTelegramStream.str();


// ============================================================
// 使用 SystemManager 统一进行数据类型转换
//
// SystemManager 会根据：
//     p.datatype
//     p.byteOrder
//
// 自动处理：
//     INT16
//     UINT16
//     INT32
//     UINT32
//     FLOAT
//     DOUBLE
//
// 特别是 FLOAT / DOUBLE 会按照数据库中的
// ABCD / BADC / CDAB / DCBA 进行字节序转换。
// ============================================================

double value = 0.0;

try
{
    value =
        SystemManager::convert(
            reg,
            count,
            p.datatype,
            p.byteOrder
        );
}
catch (const std::exception& e)
{
    std::cerr
        << "[MODBUS] 数据转换失败: "
        << e.what()
        << std::endl;

    return;
}


    // ========================================================
    // 输出变量
    // ========================================================

    std::cout
    << "变量:"
    << p.name
    << " 类型:"
    << p.datatype
    << " 字节序:"
    << p.byteOrder
    << " 地址:"
    << p.address
    << " 值:"
    << value
    << std::endl;


    // ========================================================
    // 第一阶段保持原来的共享内存接口不变
    //
    // 第二阶段再增加：
    //
    // txFrame
    // rxFrame
    //
    // ========================================================

    // ============================================================

// Direction is point configuration. Do not infer it from the read function
// code: an OUTPUT holding-register point may still use FC03 for feedback.
const std::string direction =
    p.direction == "OUTPUT" ? "OUTPUT" : "INPUT";

// ============================================================
// 写入完整 Modbus 实时数据到共享内存
// ============================================================

writeSharedMemoryTelegram(
    task.id,
    p.id,
    p.name,
    task.slaveId,
    p.function,
    p.address,
    p.datatype,
    p.byteOrder,
    direction,
    value,
    timestamp,
    txTelegram,
    rxTelegram,
    1
);
}
// ============================================================
// 强制写入 Modbus 数据
//
// UINT16 / INT16:
//     FC06
//
// UINT32 / INT32 / FLOAT:
//     FC16，2 registers
//
// DOUBLE:
//     FC16，4 registers
// ============================================================

bool writeModbus(
    const ModbusTask &task,
    const Point &point,
    double value,
    std::string &error,
    std::string &txTelegram,
    std::string &rxTelegram
)
{
    error.clear();
    txTelegram.clear();
    rxTelegram.clear();


    // ========================================================
    // 1. 参数检查
    // ========================================================

    if (
        task.slaveId < 0 ||
        task.slaveId > 255
    )
    {
        error = "非法 Unit ID";
        return false;
    }


    if (
        point.address < 0 ||
        point.address > 0xFFFF
    )
    {
        error = "非法 Modbus 地址";
        return false;
    }


    const int count =
        SystemManager::getRegisterCount(
            point.datatype
        );


    if (
        count != 1 &&
        count != 2 &&
        count != 4
    )
    {
        error =
            "不支持的数据类型: " +
            point.datatype;

        return false;
    }


    if (
        point.byteOrder != "ABCD" &&
        point.byteOrder != "BADC" &&
        point.byteOrder != "CDAB" &&
        point.byteOrder != "DCBA"
    )
    {
        error =
            "不支持的字节序: " +
            point.byteOrder;

        return false;
    }


    // ========================================================
    // 2. 将 value 转换成“标准顺序”的原始字节
    //
    // logicalBytes:
    //
    // 32 bit:
    //     A B C D
    //
    // 64 bit:
    //     A B C D E F G H
    // ========================================================

    uint8_t logicalBytes[8] = {0};


    if (point.datatype == "UINT16")
    {
        if (
            value < 0.0 ||
            value > 65535.0 ||
            std::trunc(value) != value
        )
        {
            error = "UINT16 必须是 0~65535 范围内的整数";
            return false;
        }


        const uint16_t raw =
            static_cast<uint16_t>(value);


        logicalBytes[0] =
            static_cast<uint8_t>(
                (raw >> 8) & 0xFF
            );

        logicalBytes[1] =
            static_cast<uint8_t>(
                raw & 0xFF
            );
    }
    else if (point.datatype == "INT16")
    {
        if (
            value < -32768.0 ||
            value > 32767.0 ||
            std::trunc(value) != value
        )
        {
            error = "INT16 必须是 -32768~32767 范围内的整数";
            return false;
        }


        const int16_t signedValue =
            static_cast<int16_t>(value);

        uint16_t raw = 0;


        std::memcpy(
            &raw,
            &signedValue,
            sizeof(raw)
        );


        logicalBytes[0] =
            static_cast<uint8_t>(
                (raw >> 8) & 0xFF
            );

        logicalBytes[1] =
            static_cast<uint8_t>(
                raw & 0xFF
            );
    }
    else if (point.datatype == "UINT32")
    {
        if (
            value < 0.0 ||
            value > 4294967295.0 ||
            std::trunc(value) != value
        )
        {
            error = "UINT32 必须是 0~4294967295 范围内的整数";
            return false;
        }


        const uint32_t raw =
            static_cast<uint32_t>(value);


        logicalBytes[0] =
            static_cast<uint8_t>(
                (raw >> 24) & 0xFF
            );

        logicalBytes[1] =
            static_cast<uint8_t>(
                (raw >> 16) & 0xFF
            );

        logicalBytes[2] =
            static_cast<uint8_t>(
                (raw >> 8) & 0xFF
            );

        logicalBytes[3] =
            static_cast<uint8_t>(
                raw & 0xFF
            );
    }
    else if (point.datatype == "INT32")
    {
        if (
            value < -2147483648.0 ||
            value > 2147483647.0 ||
            std::trunc(value) != value
        )
        {
            error = "INT32 必须是 -2147483648~2147483647 范围内的整数";
            return false;
        }


        const int32_t signedValue =
            static_cast<int32_t>(value);

        uint32_t raw = 0;


        std::memcpy(
            &raw,
            &signedValue,
            sizeof(raw)
        );


        logicalBytes[0] =
            static_cast<uint8_t>(
                (raw >> 24) & 0xFF
            );

        logicalBytes[1] =
            static_cast<uint8_t>(
                (raw >> 16) & 0xFF
            );

        logicalBytes[2] =
            static_cast<uint8_t>(
                (raw >> 8) & 0xFF
            );

        logicalBytes[3] =
            static_cast<uint8_t>(
                raw & 0xFF
            );
    }
    else if (point.datatype == "FLOAT")
    {
        if (value < -static_cast<double>(std::numeric_limits<float>::max()) ||
            value > static_cast<double>(std::numeric_limits<float>::max()))
        {
            error = "FLOAT 值超出 32 位浮点数范围";
            return false;
        }

        const float floatValue =
            static_cast<float>(value);

        uint32_t raw = 0;


        std::memcpy(
            &raw,
            &floatValue,
            sizeof(raw)
        );


        logicalBytes[0] =
            static_cast<uint8_t>(
                (raw >> 24) & 0xFF
            );

        logicalBytes[1] =
            static_cast<uint8_t>(
                (raw >> 16) & 0xFF
            );

        logicalBytes[2] =
            static_cast<uint8_t>(
                (raw >> 8) & 0xFF
            );

        logicalBytes[3] =
            static_cast<uint8_t>(
                raw & 0xFF
            );
    }
    else if (point.datatype == "DOUBLE")
    {
        uint64_t raw = 0;


        std::memcpy(
            &raw,
            &value,
            sizeof(raw)
        );


        for (
            int i = 0;
            i < 8;
            ++i
        )
        {
            logicalBytes[i] =
                static_cast<uint8_t>(
                    (raw >> (56 - i * 8)) &
                    0xFF
                );
        }
    }
    else
    {
        error =
            "不支持的数据类型: " +
            point.datatype;

        return false;
    }


    // ========================================================
    // 3. 根据 byteOrder 做 SystemManager::convert()
    //    的逆变换
    //
    // wireBytes 就是真正写入 Modbus Register 的字节
    // ========================================================

    uint8_t wireBytes[8] = {0};


    if (count == 1)
    {
        if (point.byteOrder == "BADC" || point.byteOrder == "DCBA")
        {
            wireBytes[0] = logicalBytes[1];
            wireBytes[1] = logicalBytes[0];
        }
        else
        {
            wireBytes[0] = logicalBytes[0];
            wireBytes[1] = logicalBytes[1];
        }
    }
    else if (count == 2)
    {
        if (point.byteOrder == "ABCD")
        {
            wireBytes[0] = logicalBytes[0];
            wireBytes[1] = logicalBytes[1];
            wireBytes[2] = logicalBytes[2];
            wireBytes[3] = logicalBytes[3];
        }
        else if (point.byteOrder == "BADC")
        {
            wireBytes[0] = logicalBytes[1];
            wireBytes[1] = logicalBytes[0];
            wireBytes[2] = logicalBytes[3];
            wireBytes[3] = logicalBytes[2];
        }
        else if (point.byteOrder == "CDAB")
        {
            wireBytes[0] = logicalBytes[2];
            wireBytes[1] = logicalBytes[3];
            wireBytes[2] = logicalBytes[0];
            wireBytes[3] = logicalBytes[1];
        }
        else // DCBA
        {
            wireBytes[0] = logicalBytes[3];
            wireBytes[1] = logicalBytes[2];
            wireBytes[2] = logicalBytes[1];
            wireBytes[3] = logicalBytes[0];
        }
    }
    else
    {
        // DOUBLE：8 bytes

        if (point.byteOrder == "ABCD")
        {
            for (int i = 0; i < 8; ++i)
            {
                wireBytes[i] =
                    logicalBytes[i];
            }
        }
        else if (point.byteOrder == "BADC")
        {
            for (int i = 0; i < 8; i += 2)
            {
                wireBytes[i] =
                    logicalBytes[i + 1];

                wireBytes[i + 1] =
                    logicalBytes[i];
            }
        }
        else if (point.byteOrder == "CDAB")
        {
            wireBytes[0] = logicalBytes[2];
            wireBytes[1] = logicalBytes[3];
            wireBytes[2] = logicalBytes[0];
            wireBytes[3] = logicalBytes[1];

            wireBytes[4] = logicalBytes[6];
            wireBytes[5] = logicalBytes[7];
            wireBytes[6] = logicalBytes[4];
            wireBytes[7] = logicalBytes[5];
        }
        else // DCBA
        {
            for (int i = 0; i < 8; ++i)
            {
                wireBytes[i] =
                    logicalBytes[7 - i];
            }
        }
    }


    // ========================================================
    // 4. 组装寄存器
    // ========================================================

    uint16_t registers[4] = {0};


    for (
        int i = 0;
        i < count;
        ++i
    )
    {
        registers[i] =
            static_cast<uint16_t>(
                (
                    static_cast<uint16_t>(
                        wireBytes[i * 2]
                    ) << 8
                )
                |
                static_cast<uint16_t>(
                    wireBytes[i * 2 + 1]
                )
            );
    }

    // 真实写入统一使用 SystemManager 的公共编码器，
    // 确保发送与接收的数据类型和字节序完全对称。
    try
    {
        const auto encoded = SystemManager::encode(
            value, point.datatype, point.byteOrder);
        for (int i = 0; i < count; ++i)
            registers[i] = encoded[static_cast<std::size_t>(i)];
    }
    catch (const std::exception &exception)
    {
        error = exception.what();
        return false;
    }


    // 使用独立原始 TCP 连接执行写入，同时保留实际 TX/RX 报文供监视器显示。
    ModbusTaskConnection connection;
    if (task.transport == "RTU")
    {
        std::vector<uint8_t> txFrame;
        std::vector<uint8_t> rxFrame;
        const uint8_t function = count == 1 ? 0x06 : 0x10;
        const bool ok = transactRtu(connection, task, function, point.address, count,
                                    registers, nullptr, txFrame, rxFrame, error);
        txTelegram = txFrame.empty() ? std::string() : bytesToHex(txFrame.data(), txFrame.size());
        rxTelegram = rxFrame.empty() ? std::string() : bytesToHex(rxFrame.data(), rxFrame.size());
        closeModbusTaskConnection(connection);
        return ok;
    }

    if (!connectRawSocket(connection, task.host, task.port))
    {
        error = "连接 Modbus 设备失败";
        return false;
    }

    std::vector<uint8_t> txFrame;
    const uint16_t transactionId = nextTransactionId(connection);
    const uint8_t function = count == 1 ? 6 : 16;
    // MBAP Length 包含 Unit ID 和 PDU，不包含前 6 字节 MBAP 字段。
    const uint16_t mbapLength = count == 1
        ? 6
        : static_cast<uint16_t>(7 + count * 2);

    appendU16(txFrame, transactionId);
    appendU16(txFrame, 0);
    appendU16(txFrame, mbapLength);
    txFrame.push_back(static_cast<uint8_t>(task.slaveId));
    txFrame.push_back(function);
    appendU16(txFrame, static_cast<uint16_t>(point.address));
    if (count == 1)
    {
        appendU16(txFrame, registers[0]);
    }
    else
    {
        appendU16(txFrame, static_cast<uint16_t>(count));
        txFrame.push_back(static_cast<uint8_t>(count * 2));
        for (int i = 0; i < count; ++i)
            appendU16(txFrame, registers[i]);
    }

    txTelegram = bytesToHex(txFrame.data(), txFrame.size());
    if (!sendAll(connection.socketFd, txFrame.data(), txFrame.size()))
    {
        error = function == 6 ? "FC06 发送失败" : "FC16 发送失败";
        closeModbusTaskConnection(connection);
        return false;
    }

    uint8_t header[7] = {0};
    if (!recvAll(connection.socketFd, header, sizeof(header)))
    {
        error = function == 6 ? "FC06 响应超时" : "FC16 响应超时";
        closeModbusTaskConnection(connection);
        return false;
    }
    const uint16_t responseLength = readU16(&header[4]);
    if (responseLength < 2 || responseLength > 253)
    {
        error = "写响应的 MBAP 长度非法";
        closeModbusTaskConnection(connection);
        return false;
    }
    std::vector<uint8_t> rxFrame(header, header + sizeof(header));
    std::vector<uint8_t> body(static_cast<size_t>(responseLength - 1));
    if (!recvAll(connection.socketFd, body.data(), body.size()))
    {
        error = "写响应接收不完整";
        closeModbusTaskConnection(connection);
        return false;
    }
    rxFrame.insert(rxFrame.end(), body.begin(), body.end());
    rxTelegram = bytesToHex(rxFrame.data(), rxFrame.size());
    closeModbusTaskConnection(connection);

    if (readU16(&header[0]) != transactionId || readU16(&header[2]) != 0 ||
        header[6] != static_cast<uint8_t>(task.slaveId))
    {
        error = "写响应的 MBAP 头与请求不匹配";
        return false;
    }
    if (body.empty() || body[0] != function)
    {
        if (!body.empty() && body[0] == static_cast<uint8_t>(function | 0x80))
            error = "设备返回 Modbus 异常码 " + std::to_string(body.size() > 1 ? body[1] : 0);
        else
            error = "写响应功能码与请求不匹配";
        return false;
    }
    if (body.size() != 5 || readU16(&body[1]) != static_cast<uint16_t>(point.address) ||
        readU16(&body[3]) != static_cast<uint16_t>(count == 1 ? registers[0] : count))
    {
        error = "写响应内容与请求不匹配";
        return false;
    }


    // ========================================================
    // 8. Debug
    // ========================================================

    std::cout
        << "[MODBUS WRITE] 成功"
        << " name=" << point.name
        << " slave=" << task.slaveId
        << " address=" << point.address
        << " datatype=" << point.datatype
        << " byteOrder=" << point.byteOrder
        << " value=" << value
        << " count=" << count
        << std::endl;


    return true;
}
