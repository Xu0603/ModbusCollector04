#pragma once

#include "database.h"

#include <string>

struct _modbus;
typedef struct _modbus modbus_t;


// ============================================================
// 每个 Modbus 任务独立拥有一个 TCP 连接上下文
//
// 一个 ModbusTask Worker 对应一个 ModbusTaskConnection。
//
// 不同任务之间：
//     socket 完全隔离
//     host / port 完全隔离
//     TCP 生命周期完全隔离
//
// 后续多线程时，不允许多个任务共享同一个 connection。
// ============================================================

struct ModbusTaskConnection
{
    int socketFd = -1;

    std::string connectedHost;

    int connectedPort = -1;

    // 每个任务自己的 Transaction ID。
    // 一个 connection 只由一个 Worker 线程访问，
    // 因此这里不需要 atomic。
    unsigned short transactionId = 1;

    modbus_t *rtuContext = nullptr;
    std::string connectedSerialPort;
    int connectedBaudRate = 0;
    char connectedParity = 'N';
    int connectedDataBits = 0;
    int connectedStopBits = 0;
};


// ============================================================
// 关闭某一个任务自己的 TCP 连接
// ============================================================

void closeModbusTaskConnection(
    ModbusTaskConnection &connection
);


// ============================================================
// Modbus 读取
//
// 新接口：
//
// connection
//     当前任务独占的 TCP connection。
//
// task
//     当前 Modbus 任务配置。
//
// point
//     当前任务下的逻辑点。
// ============================================================

void readModbus(
    ModbusTaskConnection &connection,
    const ModbusTask &task,
    const Point &point
);


// ============================================================
// Modbus 写入
//
// 当前 Force 已经改成共享内存逻辑强制，
// 正常 Qt Force 不会调用这个物理写接口。
//
// 暂时保留，后面需要物理写功能时再继续整理。
// ============================================================

bool writeModbus(
    const ModbusTask &task,
    const Point &point,
    double value,
    std::string &error,
    std::string &txTelegram,
    std::string &rxTelegram
);
