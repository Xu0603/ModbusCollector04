#pragma once

#include <string>
#include <vector>


// ============================================================
// Modbus 点
// ============================================================
//
// Point 只描述“这个点本身是什么”。
//
// 注意：
// slave / period 暂时继续保留，兼容现有代码。
// 后面修改 main.cpp 和 modbus_client.cpp 后，
// 真正运行时将以 ModbusTask 中的 slaveId / period 为准。
// ============================================================

struct Point
{
    int id = 0;

    int taskId = 0;

    std::string name;

    int slave = 1;

    int function = 3;

    int address = 0;

    std::string datatype;

    std::string byteOrder = "ABCD";

    std::string direction = "INPUT";

    int period = 1000;

    bool enable = true;
};


// ============================================================
// Modbus 采集任务
// ============================================================
//
// 一个任务对应：
//
// host
// port
// slaveId
// period
// timeout
// retryCount
// reconnectInterval
//
// 并拥有自己的一组 points。
// ============================================================

struct ModbusTask
{
    int id = 0;

    std::string name;

    std::string host = "127.0.0.1";

    int port = 502;

    // TCP / RTU。旧数据库自动按 TCP 处理。
    std::string transport = "TCP";
    std::string serialPort = "/dev/ttyS0";
    int baudRate = 9600;
    char parity = 'N';
    int dataBits = 8;
    int stopBits = 1;

    int slaveId = 1;

    int period = 1000;

    int timeout = 1000;

    int retryCount = 2;

    int reconnectInterval = 3000;

    bool enable = true;

    std::vector<Point> points;
};


// ============================================================
// 旧接口
//
// 暂时保留。
// 现有 main.cpp / modbus_server.cpp 可能仍然依赖它。
// ============================================================

std::vector<Point>
loadPoints(
    std::string db
);


// ============================================================
// 新接口
//
// 从：
//     modbus_tasks
//
// 加载所有启用任务，随后根据：
//     modbus_points.task_id
//
// 把每个点挂到对应任务下面。
// ============================================================

std::vector<ModbusTask>
loadModbusTasks(
    const std::string &db
);
// ============================================================
// 根据 taskId + pointId 查找一个 Modbus 点及其所属任务
//
// 用于 Modbus 写入。
// pointId 是 modbus_points.id，全局唯一。
// taskId 用于再次校验这个点确实属于指定任务。
// ============================================================

bool loadModbusTaskPoint(
    const std::string &db,
    int taskId,
    int pointId,
    ModbusTask &task,
    Point &point,
    std::string &error
);
