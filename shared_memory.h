#pragma once

#include <string>
#include <pthread.h>

#define MAX_VARIABLES 20


struct Variable
{
    // ========================================================
    // 多任务唯一身份
    // ========================================================

    int taskId;

    int pointId;


    char name[32];

    int slave;

    int function;

    int address;

    char datatype[16];

    // ========================================================
    // 大小端
    // ABCD / BADC / CDAB / DCBA
    // ========================================================

    char byteOrder[16];


    // ========================================================
    // 读写方向
    // INPUT / OUTPUT
    // ========================================================

    char direction[16];


    // ============================================================
    // 当前对外显示值
    //
    // forced == 0:
    //     value = Modbus 实际采集值
    //
    // forced == 1:
    //     value = forcedValue
    // ============================================================

    double value;


    // ============================================================
    // 强制状态
    //
    // 0 = 正常
    // 1 = 强制
    //
    // 强制只影响共享内存中的逻辑点，
    // 不向真实 Modbus 设备发送 FC06 / FC16。
    // ============================================================

    int forced;


    // ============================================================
    // 强制值
    // ============================================================

    double forcedValue;


    char timestamp[32];


    // ========================================================
    // Modbus TCP 真实原始 TX
    // ========================================================

    char txTelegram[256];


    // ========================================================
    // Modbus TCP 真实原始 RX
    // ========================================================

    char rxTelegram[256];


    // ========================================================
    // 保留旧字段，兼容旧代码
    // ========================================================

    char telegram[256];


    int status;
};


struct SharedData
{
    pthread_mutex_t mutex;

    int count;

    Variable vars[MAX_VARIABLES];
};

// ============================================================
// 共享内存同步时使用的逻辑点身份
//
// 一个 Modbus 逻辑点由：
//     taskId + pointId
// 唯一确定。
// ============================================================

struct SharedPointIdentity
{
    int taskId;
    int pointId;
};

void initSharedMemory();


SharedData* getSharedMemory();


// ============================================================
// 旧接口
// ============================================================

void writeSharedMemory(
    std::string name,
    int slave,
    int function,
    int address,
    std::string datatype,
    double value,
    std::string timestamp,
    std::string telegram,
    int status
);


// ============================================================
// 新接口
//
// byteOrder:
// ABCD / BADC / CDAB / DCBA
//
// direction:
// INPUT / OUTPUT
//
// TX / RX:
// 真实 Modbus TCP 报文
// ============================================================

void writeSharedMemoryTelegram(
    int taskId,
    int pointId,
    std::string name,
    int slave,
    int function,
    int address,
    std::string datatype,
    std::string byteOrder,
    std::string direction,
    double value,
    std::string timestamp,
    std::string txTelegram,
    std::string rxTelegram,
    int status
);
// ============================================================
// 强制共享内存中的逻辑点
//
// 只修改共享内存。
// 不写真实 Modbus 设备。
// ============================================================

bool forceSharedMemoryValue(
    int taskId,
    int pointId,
    double value
);


// ============================================================
// 解除共享内存逻辑点强制
//
// 解除后，下一次正常 Modbus 采集会重新更新 value。
// ============================================================

bool releaseSharedMemoryValue(
    int taskId,
    int pointId
);

// ============================================================
// 根据当前有效配置同步共享内存变量
//
// 功能：
//     删除已经不在当前 Modbus 配置中的旧变量。
//
// 注意：
//     对仍然有效的变量不做重建，
//     因此可以保留：
//         value
//         forced
//         forcedValue
//         timestamp
//         TX / RX
// ============================================================

void syncSharedMemoryPoints(
    const SharedPointIdentity* points,
    int pointCount
);