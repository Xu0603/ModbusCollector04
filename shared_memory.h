#pragma once

#include <pthread.h>

#include <string>

constexpr int MAX_VARIABLES = 20;

struct Variable
{
    // taskId 和 pointId 共同标识一个点，不同任务可以使用相同点名和地址。
    int taskId;
    int pointId;
    char name[32];
    int slave;
    int function;
    int address;
    char datatype[16];
    char byteOrder[16];
    char direction[16];

    // forced 只覆盖对外显示值，不向设备发送写命令。
    double value;
    int forced;
    double forcedValue;
    char timestamp[32];
    char txTelegram[256];
    char rxTelegram[256];
    char telegram[256]; // 兼容仍读取合并报文的旧客户端。
    int status;
};

struct SharedData
{
    pthread_mutex_t mutex;
    int count;
    Variable vars[MAX_VARIABLES];
};

struct SharedPointIdentity
{
    int taskId;
    int pointId;
};

void initSharedMemory();
SharedData *getSharedMemory();

void writeSharedMemory(std::string name, int slave, int function, int address,
                       std::string datatype, double value, std::string timestamp,
                       std::string telegram, int status);

void writeSharedMemoryTelegram(int taskId, int pointId, std::string name, int slave,
                               int function, int address, std::string datatype,
                               std::string byteOrder, std::string direction, double value,
                               std::string timestamp, std::string txTelegram,
                               std::string rxTelegram, int status);

bool forceSharedMemoryValue(int taskId, int pointId, double value);
bool releaseSharedMemoryValue(int taskId, int pointId);

// OUTPUT 点不参与周期读取，因此配置加载时先为在线数据创建占位项。
bool ensureSharedMemoryOutputPoint(int taskId, int pointId, const std::string &name,
                                   int slave, int function, int address,
                                   const std::string &datatype,
                                   const std::string &byteOrder);

// 删除配置中已不存在的点，保留其余点的值、强制状态和最近报文。
void syncSharedMemoryPoints(const SharedPointIdentity *points, int pointCount);
