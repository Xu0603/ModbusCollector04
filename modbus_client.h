#pragma once

#include "database.h"

#include <string>

struct _modbus;
using modbus_t = struct _modbus;

// 每个任务独占一个连接对象，避免不同线程混用 socket、事务号或 RTU 上下文。
struct ModbusTaskConnection
{
    int socketFd = -1;
    std::string connectedHost;
    int connectedPort = -1;
    unsigned short transactionId = 1;

    modbus_t *rtuContext = nullptr;
    std::string connectedSerialPort;
    int connectedBaudRate = 0;
    char connectedParity = 'N';
    int connectedDataBits = 0;
    int connectedStopBits = 0;
};

void closeModbusTaskConnection(ModbusTaskConnection &connection);
void readModbus(ModbusTaskConnection &connection, const ModbusTask &task, const Point &point);

// 根据点的数据类型选择 FC06 或 FC16，并返回实际发送、接收的原始报文。
bool writeModbus(const ModbusTask &task, const Point &point, double value,
                 std::string &error, std::string &txTelegram,
                 std::string &rxTelegram);
