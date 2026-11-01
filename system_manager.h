#pragma once

#include <cstdint>
#include <string>
#include <vector>

class SystemManager
{
public:

    // 根据数据类型，返回需要读取的 Modbus 寄存器数量
    static int getRegisterCount(
        const std::string& datatype
    );


    // 将 Modbus 寄存器转换成最终的数据值
    //
    // reg       : Modbus读取到的寄存器
    // count     : 寄存器数量
    // datatype  : INT16 / UINT16 / INT32 / UINT32 / FLOAT / DOUBLE
    // byteOrder : ABCD / BADC / CDAB / DCBA
    //
    // 返回值统一使用 double 保存
    static double convert(
        const uint16_t* reg,
        int count,
        const std::string& datatype,
        const std::string& byteOrder
    );

    // 将逻辑值编码为可直接写入 Modbus 的寄存器。
    // 与 convert() 互为逆操作。
    static std::vector<std::uint16_t> encode(
        double value,
        const std::string& datatype,
        const std::string& byteOrder
    );


private:

    // 2个16位寄存器转换成32位原始数据
    static uint32_t makeUint32(
        const uint16_t* reg,
        const std::string& byteOrder
    );


    // 4个16位寄存器转换成64位原始数据
    static uint64_t makeUint64(
        const uint16_t* reg,
        const std::string& byteOrder
    );


    // 检查字节序是否合法
    static bool isValidByteOrder(
        const std::string& byteOrder
    );
};
