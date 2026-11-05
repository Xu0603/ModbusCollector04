#pragma once

#include <string>
#include <vector>

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

struct ModbusTask
{
    int id = 0;
    std::string name;
    std::string host = "127.0.0.1";
    int port = 502;

    // transport 决定使用网络端点还是串口参数；旧数据库默认按 TCP 处理。
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

// 保留给仍按平面点表工作的旧调用方。
std::vector<Point> loadPoints(std::string db);

// 加载启用任务，并把每个启用点挂到所属任务下。
std::vector<ModbusTask> loadModbusTasks(const std::string &db);

// 写命令必须同时校验 taskId 和 pointId，避免跨任务误写设备。
bool loadModbusTaskPoint(const std::string &db, int taskId, int pointId,
                         ModbusTask &task, Point &point, std::string &error);
