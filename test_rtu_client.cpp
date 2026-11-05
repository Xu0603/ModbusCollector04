#include "modbus_client.h"
#include "shared_memory.h"

#include <modbus/modbus.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

modbus_t *ctx = nullptr;

static uint16_t crc16(const uint8_t *data, std::size_t size) {
    uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) : static_cast<uint16_t>(crc >> 1);
    }
    return crc;
}

static void appendCrc(std::vector<uint8_t> &frame) {
    const uint16_t crc = crc16(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>(crc >> 8));
}

int main() {
    int master = -1, slave = -1;
    char slaveName[128] = {0};
    termios settings{};
    cfmakeraw(&settings);
    assert(openpty(&master, &slave, slaveName, &settings, nullptr) == 0);
    close(slave);

    std::thread simulator([master] {
        for (int requestIndex = 0; requestIndex < 3; ++requestIndex) {
            uint8_t buffer[256] = {0};
            std::size_t used = 0;
            const std::size_t expected = requestIndex == 1 ? 13 : 8;
            while (used < expected) {
                const ssize_t received = read(master, buffer + used, expected - used);
                if (received < 0 && errno == EIO) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                assert(received > 0);
                used += static_cast<std::size_t>(received);
            }
            assert(crc16(buffer, used - 2) == static_cast<uint16_t>(buffer[used - 2] | (buffer[used - 1] << 8)));
            std::vector<uint8_t> response;
            if (buffer[1] == 0x06) response.assign(buffer, buffer + 6);
            else if (buffer[1] == 0x10) {
                assert(buffer[1] == 0x10);
                response.assign(buffer, buffer + 6);
            } else {
                assert(buffer[1] == 0x03);
                response = {buffer[0], 0x03, 0x04, 0x12, 0x34, 0x56, 0x78};
            }
            appendCrc(response);
            assert(write(master, response.data(), response.size()) == static_cast<ssize_t>(response.size()));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        close(master);
    });

    ModbusTask task;
    task.transport = "RTU";
    task.serialPort = slaveName;
    task.baudRate = 9600;
    task.parity = 'N';
    task.dataBits = 8;
    task.stopBits = 1;
    task.slaveId = 1;
    task.timeout = 1000;

    Point point;
    point.address = 0;
    point.datatype = "UINT16";
    point.byteOrder = "ABCD";
    point.name = "rtu16";
    std::string error, tx, rx;
    assert(writeModbus(task, point, 12345, error, tx, rx));
    assert(tx.find("01 06 00 00 30 39") == 0);

    point.address = 1;
    point.datatype = "FLOAT";
    point.name = "rtuFloat";
    if (!writeModbus(task, point, 12.5, error, tx, rx)) {
        std::cerr << "RTU FC16 failed: " << error << " TX=" << tx << " RX=" << rx << "\n";
        assert(false);
    }
    assert(tx.find("01 10 00 01 00 02 04") == 0);

    task.id = 9876;
    point.id = 9876;
    point.address = 2;
    point.function = 3;
    point.datatype = "UINT32";
    point.direction = "INPUT";
    point.name = "rtuRead";
    ModbusTaskConnection readConnection;
    readModbus(readConnection, task, point);
    closeModbusTaskConnection(readConnection);
    SharedData *shared = getSharedMemory();
    bool foundReadValue = false;
    if (shared) {
        pthread_mutex_lock(&shared->mutex);
        for (int i = 0; i < shared->count; ++i)
            if (shared->vars[i].taskId == task.id && shared->vars[i].pointId == point.id) {
                foundReadValue = shared->vars[i].value == 305419896.0;
                break;
            }
        pthread_mutex_unlock(&shared->mutex);
    }
    assert(foundReadValue);

    simulator.join();
    std::cout << "All Modbus RTU pseudo-terminal tests passed\n";
    return 0;
}
