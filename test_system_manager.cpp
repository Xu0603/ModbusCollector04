#include "system_manager.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string &message) {
    if (!ok) throw std::runtime_error(message);
}

std::vector<std::uint16_t> wire32(std::uint32_t raw, const std::string &order) {
    const std::uint8_t a = raw >> 24, b = raw >> 16, c = raw >> 8, d = raw;
    std::uint8_t x[4]{};
    if (order == "ABCD") { x[0]=a; x[1]=b; x[2]=c; x[3]=d; }
    else if (order == "BADC") { x[0]=b; x[1]=a; x[2]=d; x[3]=c; }
    else if (order == "CDAB") { x[0]=c; x[1]=d; x[2]=a; x[3]=b; }
    else { x[0]=d; x[1]=c; x[2]=b; x[3]=a; }
    return {static_cast<std::uint16_t>((x[0] << 8) | x[1]),
            static_cast<std::uint16_t>((x[2] << 8) | x[3])};
}

std::vector<std::uint16_t> wire64(std::uint64_t raw, const std::string &order) {
    std::uint8_t logical[8]{}, x[8]{};
    for (int i = 0; i < 8; ++i) logical[i] = raw >> (56 - i * 8);
    if (order == "ABCD") std::memcpy(x, logical, 8);
    else if (order == "BADC")
        for (int i = 0; i < 8; i += 2) { x[i]=logical[i+1]; x[i+1]=logical[i]; }
    else if (order == "CDAB") {
        x[0]=logical[2]; x[1]=logical[3]; x[2]=logical[0]; x[3]=logical[1];
        x[4]=logical[6]; x[5]=logical[7]; x[6]=logical[4]; x[7]=logical[5];
    } else for (int i = 0; i < 8; ++i) x[i] = logical[7-i];
    std::vector<std::uint16_t> result(4);
    for (int i = 0; i < 4; ++i)
        result[i] = static_cast<std::uint16_t>((x[i*2] << 8) | x[i*2+1]);
    return result;
}

void test16Bit() {
    for (const std::string order : {"ABCD", "BADC", "CDAB", "DCBA"}) {
        const std::uint16_t u = (order == "BADC" || order == "DCBA") ? 0x3412 : 0x1234;
        require(SystemManager::convert(&u, 1, "UINT16", order) == 4660.0,
                "UINT16 " + order);
        const std::uint16_t s = (order == "BADC" || order == "DCBA") ? 0xFEFF : 0xFFFE;
        require(SystemManager::convert(&s, 1, "INT16", order) == -2.0,
                "INT16 " + order);
    }
}

void test32Bit() {
    for (const std::string order : {"ABCD", "BADC", "CDAB", "DCBA"}) {
        auto u = wire32(0xF2345678U, order);
        require(SystemManager::convert(u.data(), 2, "UINT32", order) == 4063516280.0,
                "UINT32 " + order);
        const std::int32_t expectedInt = -123456789;
        std::uint32_t intRaw{}; std::memcpy(&intRaw, &expectedInt, 4);
        auto s = wire32(intRaw, order);
        require(SystemManager::convert(s.data(), 2, "INT32", order) == expectedInt,
                "INT32 " + order);
        const float expectedFloat = -123.25F;
        std::uint32_t floatRaw{}; std::memcpy(&floatRaw, &expectedFloat, 4);
        auto f = wire32(floatRaw, order);
        require(SystemManager::convert(f.data(), 2, "FLOAT", order) == expectedFloat,
                "FLOAT " + order);
    }
}

void testDouble() {
    const double expected = -987654.125;
    std::uint64_t raw{}; std::memcpy(&raw, &expected, 8);
    for (const std::string order : {"ABCD", "BADC", "CDAB", "DCBA"}) {
        auto regs = wire64(raw, order);
        require(SystemManager::convert(regs.data(), 4, "DOUBLE", order) == expected,
                "DOUBLE " + order);
    }
}

void testValidation() {
    std::uint16_t regs[2]{};
    bool threw = false;
    try { SystemManager::convert(regs, 1, "UINT32", "ABCD"); }
    catch (...) { threw = true; }
    require(threw, "short register count");
    threw = false;
    try { SystemManager::convert(regs, 2, "UINT32", "INVALID"); }
    catch (...) { threw = true; }
    require(threw, "invalid byte order");

    threw = false;
    try { SystemManager::encode(12.5, "INT16", "ABCD"); }
    catch (...) { threw = true; }
    require(threw, "fractional integer output must fail");

    threw = false;
    try { SystemManager::encode(70000, "UINT16", "ABCD"); }
    catch (...) { threw = true; }
    require(threw, "out-of-range UINT16 output must fail");
}

void testSendReceiveRoundTrip() {
    struct Case { const char *type; double value; };
    const Case cases[] = {
        {"INT16", -12345.0}, {"UINT16", 54321.0},
        {"INT32", -123456789.0}, {"UINT32", 4063516280.0},
        {"FLOAT", -123.25}, {"DOUBLE", -987654.125}
    };
    for (const auto &test : cases) {
        for (const std::string order : {"ABCD", "BADC", "CDAB", "DCBA"}) {
            const auto registers = SystemManager::encode(test.value, test.type, order);
            const double actual = SystemManager::convert(
                registers.data(), static_cast<int>(registers.size()), test.type, order);
            require(actual == test.value,
                    std::string(test.type) + " send/receive " + order);
        }
    }
}

} // namespace

int main() {
    try {
        test16Bit(); test32Bit(); testDouble(); testValidation();
        testSendReceiveRoundTrip();
        std::cout << "All Modbus datatype tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Modbus datatype test failed: " << e.what() << '\n';
        return 1;
    }
}
