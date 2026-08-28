#include "modbus_client.h"
#include "system_manager.h"

#include <modbus/modbus.h>

#include <iostream>
#include <iomanip>
#include <cstring>
#include <sstream>
#include <vector>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <string>

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

static int g_rawSocket = -1;
static std::atomic<uint16_t> g_transactionId(1);


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
// 关闭原始 TCP socket
// ============================================================

static void closeRawSocket()
{
    if (g_rawSocket >= 0)
    {
        ::close(g_rawSocket);
        g_rawSocket = -1;
    }
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

static bool connectRawSocket()
{
    if (g_rawSocket >= 0)
    {
        return true;
    }

    int fd = ::socket(
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


    // --------------------------------------------------------
    // 设置发送/接收超时
    // --------------------------------------------------------

    struct timeval timeout{};

    timeout.tv_sec =
        SOCKET_TIMEOUT_MS / 1000;

    timeout.tv_usec =
        (SOCKET_TIMEOUT_MS % 1000) * 1000;


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


    // --------------------------------------------------------
    // 设置服务器地址
    // --------------------------------------------------------

    struct sockaddr_in server{};

    server.sin_family = AF_INET;

    server.sin_port =
        htons(MODBUS_PORT);


    if (::inet_pton(
            AF_INET,
            MODBUS_HOST,
            &server.sin_addr) != 1)
    {
        std::cerr
            << "[MODBUS] IP 地址无效: "
            << MODBUS_HOST
            << std::endl;

        ::close(fd);

        return false;
    }


    // --------------------------------------------------------
    // TCP连接
    // --------------------------------------------------------

    if (::connect(
            fd,
            reinterpret_cast<struct sockaddr *>(&server),
            sizeof(server)) < 0)
    {
        std::cerr
            << "[MODBUS] 连接 "
            << MODBUS_HOST
            << ":"
            << MODBUS_PORT
            << " 失败: "
            << std::strerror(errno)
            << std::endl;

        ::close(fd);

        return false;
    }


    g_rawSocket = fd;


    std::cout
        << "[MODBUS] 原始 TCP 通道连接成功: "
        << MODBUS_HOST
        << ":"
        << MODBUS_PORT
        << std::endl;


    return true;
}


// ============================================================
// Transaction ID
// ============================================================

static uint16_t nextTransactionId()
{
    uint16_t id =
        g_transactionId.fetch_add(
            1,
            std::memory_order_relaxed
        );


    if (id == 0)
    {
        id =
            g_transactionId.fetch_add(
                1,
                std::memory_order_relaxed
            );
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


// ============================================================
// 真正的 Modbus TCP 原始报文读取
// ============================================================

static bool readModbusTcpRaw(
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

    if (!connectRawSocket())
    {
        return false;
    }


    // --------------------------------------------------------
    // Transaction ID
    // --------------------------------------------------------

    const uint16_t transactionId =
        nextTransactionId();


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
            g_rawSocket,
            txFrame.data(),
            txFrame.size()))
    {
        closeRawSocket();

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
            g_rawSocket,
            header,
            sizeof(header)))
    {
        closeRawSocket();

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

        closeRawSocket();

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

        closeRawSocket();

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

        closeRawSocket();

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
            g_rawSocket,
            remaining.data(),
            remaining.size()))
    {
        closeRawSocket();

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
// 对外的 Modbus 读取函数
// ============================================================

void readModbus(Point p)
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

    const bool ok =
        readModbusTcpRaw(
            p.slave,
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
            << std::endl;

        return;
    }


    // ========================================================
    // 时间
    // ========================================================

    const std::string timestamp =
        getTime();


    // ========================================================
    // 保留原来的 telegram 格式
    //
    // 注意：
    // 这里暂时仍然只保存摘要。
    //
    // 第一阶段真正原始报文已经存在：
    //
    // txFrame
    // rxFrame
    //
    // 第二阶段再把它们写进共享内存。
    // ========================================================

    std::stringstream telegram;


    telegram
        << std::hex
        << std::setfill('0');


    telegram
        << "slave:"
        << p.slave

        << " function:"
        << p.function

        << " address:"
        << p.address

        << " data:";


    for (int i = 0; i < count; ++i)
    {
        telegram
            << std::setw(4)
            << reg[i]
            << " ";
    }


    const std::string packet =
        telegram.str();


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

    writeSharedMemory(
        p.name,
        p.slave,
        p.function,
        p.address,
        p.datatype,
        value,
        timestamp,
        packet,
        1
    );
}