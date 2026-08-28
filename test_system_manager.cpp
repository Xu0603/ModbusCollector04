#include "system_manager.h"

#include <iostream>
#include <iomanip>
#include <cstdint>

int main()
{
    std::cout << "========================================\n";
    std::cout << "FLOAT ABCD 单项测试\n";
    std::cout << "========================================\n\n";

    uint16_t reg[2];

    reg[0] = 0x3F80;
    reg[1] = 0x0000;

    std::cout
        << "reg[0] = 0x"
        << std::hex
        << std::setw(4)
        << std::setfill('0')
        << reg[0]
        << "\n";

    std::cout
        << "reg[1] = 0x"
        << std::hex
        << std::setw(4)
        << std::setfill('0')
        << reg[1]
        << "\n";

    uint32_t raw =
        (static_cast<uint32_t>(reg[0]) << 16)
        |
        reg[1];

    std::cout
        << "组合后的32位数据 = 0x"
        << std::hex
        << std::setw(8)
        << std::setfill('0')
        << raw
        << "\n\n";

    double value =
        SystemManager::convert(
            reg,
            2,
            "FLOAT",
            "ABCD"
        );

    std::cout
        << std::dec
        << "FLOAT ABCD = "
        << value
        << "\n";

    return 0;
}