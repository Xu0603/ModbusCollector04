#include "system_manager.h"

#include <cstring>
#include <iostream>
#include <iomanip>
#include <stdexcept>


// ============================================================
// 获取数据类型需要占用多少个 Modbus Holding/Input Register
// ============================================================

int SystemManager::getRegisterCount(
    const std::string& datatype
)
{
    if (datatype == "INT16" ||
        datatype == "UINT16")
    {
        return 1;
    }


    if (datatype == "INT32" ||
        datatype == "UINT32" ||
        datatype == "FLOAT")
    {
        return 2;
    }


    if (datatype == "DOUBLE")
    {
        return 4;
    }


    // 未知类型
    return 0;
}


// ============================================================
// 检查字节序
// ============================================================

bool SystemManager::isValidByteOrder(
    const std::string& byteOrder
)
{
    return
        byteOrder == "ABCD" ||
        byteOrder == "BADC" ||
        byteOrder == "CDAB" ||
        byteOrder == "DCBA";
}


// ============================================================
// 2个16位寄存器转换成32位数据
//
// reg[0] = 第一个 Modbus Register
// reg[1] = 第二个 Modbus Register
//
// 例如：
//
// ABCD
// reg[0] = 0x1234
// reg[1] = 0x5678
//
// 最终：
// 0x12345678
// ============================================================

uint32_t SystemManager::makeUint32(
    const uint16_t* reg,
    const std::string& byteOrder
)
{
    uint16_t r0 = reg[0];
    uint16_t r1 = reg[1];


    uint8_t A = (r0 >> 8) & 0xFF;
    uint8_t B = r0 & 0xFF;

    uint8_t C = (r1 >> 8) & 0xFF;
    uint8_t D = r1 & 0xFF;


    uint32_t raw = 0;


    if (byteOrder == "ABCD")
    {
        raw =
            ((uint32_t)A << 24) |
            ((uint32_t)B << 16) |
            ((uint32_t)C << 8)  |
            ((uint32_t)D);
    }


    else if (byteOrder == "BADC")
    {
        raw =
            ((uint32_t)B << 24) |
            ((uint32_t)A << 16) |
            ((uint32_t)D << 8)  |
            ((uint32_t)C);
    }


    else if (byteOrder == "CDAB")
    {
        raw =
            ((uint32_t)C << 24) |
            ((uint32_t)D << 16) |
            ((uint32_t)A << 8)  |
            ((uint32_t)B);
    }


    else if (byteOrder == "DCBA")
    {
        raw =
            ((uint32_t)D << 24) |
            ((uint32_t)C << 16) |
            ((uint32_t)B << 8)  |
            ((uint32_t)A);
    }


    else
    {
        throw std::runtime_error(
            "未知的字节序: " + byteOrder
        );
    }


    return raw;
}


// ============================================================
// 4个16位寄存器转换成64位数据
//
// 同样按照 ABCD / BADC / CDAB / DCBA
// 对每个字节进行排列
//
// 64位数据实际上有8个字节：
//
// A B C D E F G H
//
// 这里将4个Register：
//
// reg[0] = A B
// reg[1] = C D
// reg[2] = E F
// reg[3] = G H
//
// ============================================================

uint64_t SystemManager::makeUint64(
    const uint16_t* reg,
    const std::string& byteOrder
)
{
    uint8_t bytes[8];


    bytes[0] = (reg[0] >> 8) & 0xFF;
    bytes[1] = reg[0] & 0xFF;

    bytes[2] = (reg[1] >> 8) & 0xFF;
    bytes[3] = reg[1] & 0xFF;

    bytes[4] = (reg[2] >> 8) & 0xFF;
    bytes[5] = reg[2] & 0xFF;

    bytes[6] = (reg[3] >> 8) & 0xFF;
    bytes[7] = reg[3] & 0xFF;


    uint8_t result[8];


    if (byteOrder == "ABCD")
    {
        for (int i = 0; i < 8; ++i)
        {
            result[i] = bytes[i];
        }
    }


    else if (byteOrder == "BADC")
    {
        result[0] = bytes[1];
        result[1] = bytes[0];

        result[2] = bytes[3];
        result[3] = bytes[2];

        result[4] = bytes[5];
        result[5] = bytes[4];

        result[6] = bytes[7];
        result[7] = bytes[6];
    }


    else if (byteOrder == "CDAB")
    {
        result[0] = bytes[2];
        result[1] = bytes[3];

        result[2] = bytes[0];
        result[3] = bytes[1];

        result[4] = bytes[6];
        result[5] = bytes[7];

        result[6] = bytes[4];
        result[7] = bytes[5];
    }


    else if (byteOrder == "DCBA")
    {
        for (int i = 0; i < 8; ++i)
        {
            result[i] = bytes[7 - i];
        }
    }


    else
    {
        throw std::runtime_error(
            "未知的字节序: " + byteOrder
        );
    }


    uint64_t raw = 0;


    for (int i = 0; i < 8; ++i)
    {
        raw =
            (raw << 8) |
            result[i];
    }


    return raw;
}


// ============================================================
// 核心转换函数
// ============================================================

double SystemManager::convert(
    const uint16_t* reg,
    int count,
    const std::string& datatype,
    const std::string& byteOrder
)
{
    std::cout
    << "[DEBUG] datatype = ["
    << datatype
    << "] byteOrder = ["
    << byteOrder
    << "] count = "
    << count
    << std::endl;

for (int i = 0; i < count; ++i)
{
    std::cout
        << "[DEBUG] reg["
        << i
        << "] = 0x"
        << std::hex
        << std::setw(4)
        << std::setfill('0')
        << reg[i]
        << std::dec
        << std::endl;
}
    // --------------------------------------------------------
    // 检查数据类型需要的寄存器数量
    // --------------------------------------------------------

    int requiredCount =
        getRegisterCount(datatype);


    if (requiredCount == 0)
    {
        throw std::runtime_error(
            "未知的数据类型: " + datatype
        );
    }


    if (count < requiredCount)
    {
        throw std::runtime_error(
            "Modbus寄存器数量不足"
        );
    }


    // --------------------------------------------------------
    // 检查字节序
    // --------------------------------------------------------

    if (!isValidByteOrder(byteOrder))
    {
        throw std::runtime_error(
            "未知的字节序: " + byteOrder
        );
    }


    // ========================================================
    // INT16
    // ========================================================

    if (datatype == "INT16")
    {
        int16_t value =
            static_cast<int16_t>(reg[0]);

        return static_cast<double>(value);
    }


    // ========================================================
    // UINT16
    // ========================================================

    if (datatype == "UINT16")
    {
        uint16_t value =
            reg[0];

        return static_cast<double>(value);
    }


    // ========================================================
    // INT32
    // ========================================================

    if (datatype == "INT32")
    {
        uint32_t raw =
            makeUint32(
                reg,
                byteOrder
            );


        int32_t value;


        std::memcpy(
            &value,
            &raw,
            sizeof(value)
        );


        return static_cast<double>(value);
    }


    // ========================================================
    // UINT32
    // ========================================================

    if (datatype == "UINT32")
    {
        uint32_t raw =
            makeUint32(
                reg,
                byteOrder
            );


        return static_cast<double>(raw);
    }


    // ========================================================
    // FLOAT
    // ========================================================

    if (datatype == "FLOAT")
{
    uint32_t raw =
        makeUint32(
            reg,
            byteOrder
        );

    std::cout
        << "[DEBUG FLOAT]"
        << " byteOrder=["
        << byteOrder
        << "]"
        << " raw=0x"
        << std::hex
        << std::setw(8)
        << std::setfill('0')
        << raw
        << std::dec
        << std::endl;

    float value = 0.0f;

    std::memcpy(
        &value,
        &raw,
        sizeof(value)
    );

    std::cout
        << "[DEBUG FLOAT]"
        << " value="
        << value
        << std::endl;

    return static_cast<double>(value);
}


    // ========================================================
    // DOUBLE
    // ========================================================

    if (datatype == "DOUBLE")
{
    uint64_t raw =
        makeUint64(
            reg,
            byteOrder
        );

    std::cout
        << "[DEBUG DOUBLE]"
        << " byteOrder=["
        << byteOrder
        << "]"
        << " raw=0x"
        << std::hex
        << std::setw(16)
        << std::setfill('0')
        << raw
        << std::dec
        << std::endl;

    double value = 0.0;

    std::memcpy(
        &value,
        &raw,
        sizeof(value)
    );

    std::cout
        << "[DEBUG DOUBLE]"
        << " value="
        << value
        << std::endl;

    return value;
}


    throw std::runtime_error(
        "无法转换的数据类型: " + datatype
    );
}