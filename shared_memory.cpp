#include "shared_memory.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <pthread.h>

#include <cstring>
#include <iostream>

#define SHM_NAME "/modbus_data"


// ============================================================
// 初始化进程间 mutex
// ============================================================

static bool initMutex(SharedData* data)
{
    pthread_mutexattr_t attr;

    if (pthread_mutexattr_init(&attr) != 0)
    {
        std::cerr << "pthread_mutexattr_init 失败\n";
        return false;
    }

    if (pthread_mutexattr_setpshared(
            &attr,
            PTHREAD_PROCESS_SHARED) != 0)
    {
        std::cerr << "pthread_mutexattr_setpshared 失败\n";

        pthread_mutexattr_destroy(&attr);

        return false;
    }

    if (pthread_mutex_init(
            &data->mutex,
            &attr) != 0)
    {
        std::cerr << "pthread_mutex_init 失败\n";

        pthread_mutexattr_destroy(&attr);

        return false;
    }

    pthread_mutexattr_destroy(&attr);

    return true;
}


// ============================================================
// 打开 / 创建共享内存
// ============================================================

static SharedData* openSharedMemory()
{
    int fd = shm_open(
        SHM_NAME,
        O_CREAT | O_RDWR,
        0666
    );

    if (fd < 0)
    {
        perror("shm_open");
        return nullptr;
    }


    struct stat st;

    if (fstat(fd, &st) < 0)
    {
        perror("fstat");

        close(fd);

        return nullptr;
    }


    bool firstCreate = (st.st_size == 0);


    // 调整共享内存大小
    if (ftruncate(
            fd,
            sizeof(SharedData)) < 0)
    {
        perror("ftruncate");

        close(fd);

        return nullptr;
    }


    SharedData* data =
        static_cast<SharedData*>(
            mmap(
                nullptr,
                sizeof(SharedData),
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                fd,
                0
            )
        );


    close(fd);


    if (data == MAP_FAILED)
    {
        perror("mmap");

        return nullptr;
    }


    // ========================================================
    // 第一次创建共享内存
    // ========================================================

    if (firstCreate)
    {
        // 先清零整个共享内存
        std::memset(
            data,
            0,
            sizeof(SharedData)
        );


        // 初始化进程间 mutex
        if (!initMutex(data))
        {
            munmap(
                data,
                sizeof(SharedData)
            );

            return nullptr;
        }


        data->count = 0;
    }


    return data;
}


// ============================================================
// 初始化共享内存
//
// 注意：正常情况下程序第一次创建共享内存时已经初始化。
// 这个函数不要每次启动 ModbusCollector 都清空数据。
// ============================================================

void initSharedMemory()
{
    SharedData* data =
        openSharedMemory();


    if (data == nullptr)
    {
        return;
    }


    // 加锁
    pthread_mutex_lock(
        &data->mutex
    );


    if (data->count < 0 ||
        data->count > MAX_VARIABLES)
    {
        data->count = 0;

        std::memset(
            data->vars,
            0,
            sizeof(data->vars)
        );
    }


    // 解锁
    pthread_mutex_unlock(
        &data->mutex
    );


    munmap(
        data,
        sizeof(SharedData)
    );
}


// ============================================================
// 写入共享内存
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
)
{
    SharedData* data =
        openSharedMemory();


    if (data == nullptr)
    {
        return;
    }


    // ========================================================
    // 写共享内存之前加锁
    // ========================================================

    pthread_mutex_lock(
        &data->mutex
    );


    bool found = false;


    // ========================================================
    // 查找已有变量
    // ========================================================

    for (int i = 0; i < data->count; i++)
    {
        if (strcmp(
                data->vars[i].name,
                name.c_str()) == 0)
        {
            data->vars[i].slave =
                slave;

            data->vars[i].function =
                function;

            data->vars[i].address =
                address;


            strncpy(
                data->vars[i].datatype,
                datatype.c_str(),
                sizeof(data->vars[i].datatype) - 1
            );

            data->vars[i].datatype[
                sizeof(data->vars[i].datatype) - 1
            ] = '\0';

            strncpy(
                data->vars[i].byteOrder,
                "ABCD",
                sizeof(data->vars[i].byteOrder) - 1
            );

            data->vars[i].byteOrder[
                sizeof(data->vars[i].byteOrder) - 1
            ] = '\0';

            data->vars[i].value =
                value;


            strncpy(
                data->vars[i].timestamp,
                timestamp.c_str(),
                sizeof(data->vars[i].timestamp) - 1
            );

            data->vars[i].timestamp[
                sizeof(data->vars[i].timestamp) - 1
            ] = '\0';


            strncpy(
                data->vars[i].telegram,
                telegram.c_str(),
                sizeof(data->vars[i].telegram) - 1
            );

            data->vars[i].telegram[
                sizeof(data->vars[i].telegram) - 1
            ] = '\0';


            data->vars[i].status =
                status;


            found = true;

            break;
        }
    }


    // ========================================================
    // 新增变量
    // ========================================================

    if (!found)
    {
        if (data->count < MAX_VARIABLES)
        {
            int index =
                data->count;


            strncpy(
                data->vars[index].name,
                name.c_str(),
                sizeof(data->vars[index].name) - 1
            );

            data->vars[index].name[
                sizeof(data->vars[index].name) - 1
            ] = '\0';


            data->vars[index].slave =
                slave;


            data->vars[index].function =
                function;


            data->vars[index].address =
                address;


            strncpy(
                data->vars[index].datatype,
                datatype.c_str(),
                sizeof(data->vars[index].datatype) - 1
            );

            data->vars[index].datatype[
                sizeof(data->vars[index].datatype) - 1
            ] = '\0';

            strncpy(
                data->vars[index].byteOrder,
                "ABCD",
                sizeof(data->vars[index].byteOrder) - 1
            );

            data->vars[index].byteOrder[
                sizeof(data->vars[index].byteOrder) - 1
            ] = '\0';

            strncpy(
                data->vars[index].direction,
                "INPUT",
                sizeof(data->vars[index].direction) - 1
            );

            data->vars[index].direction[
                sizeof(data->vars[index].direction) - 1
            ] = '\0';

            data->vars[index].value =
                value;


            strncpy(
                data->vars[index].timestamp,
                timestamp.c_str(),
                sizeof(data->vars[index].timestamp) - 1
            );

            data->vars[index].timestamp[
                sizeof(data->vars[index].timestamp) - 1
            ] = '\0';


            strncpy(
                data->vars[index].telegram,
                telegram.c_str(),
                sizeof(data->vars[index].telegram) - 1
            );

            data->vars[index].telegram[
                sizeof(data->vars[index].telegram) - 1
            ] = '\0';


            data->vars[index].status =
                status;


            data->count++;
        }
    }


    // ========================================================
    // 写完以后解锁
    // ========================================================

    pthread_mutex_unlock(
        &data->mutex
    );


    munmap(
        data,
        sizeof(SharedData)
    );
}

// ============================================================
// 写入共享内存
//
// 保存完整 Modbus 实时数据：
//
// name
// slave
// function
// address
// datatype
// byteOrder
// value
// timestamp
// TX
// RX
// status
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
)
{
    SharedData* data =
        openSharedMemory();

    if (data == nullptr)
    {
        return;
    }


    // ========================================================
    // 加锁
    // ========================================================

    pthread_mutex_lock(
        &data->mutex
    );


    bool found = false;


    // ========================================================
    // 查找已有变量
    // ========================================================

    for (
        int i = 0;
        i < data->count;
        ++i
    )
    {
        if (
            data->vars[i].taskId == taskId
            &&
            data->vars[i].pointId == pointId
        )
        {
            Variable& variable =
                data->vars[i];

            variable.taskId =
                taskId;

            variable.pointId =
                pointId;

            variable.slave =
                slave;

            variable.function =
                function;

            variable.address =
                address;


            // datatype

            strncpy(
                variable.datatype,
                datatype.c_str(),
                sizeof(variable.datatype) - 1
            );

            variable.datatype[
                sizeof(variable.datatype) - 1
            ] = '\0';


            // byteOrder

            strncpy(
                variable.byteOrder,
                byteOrder.c_str(),
                sizeof(variable.byteOrder) - 1
            );

            variable.byteOrder[
                sizeof(variable.byteOrder) - 1
            ] = '\0';

            // direction

            strncpy(
                variable.direction,
                direction.c_str(),
                sizeof(variable.direction) - 1
            );

            variable.direction[
                sizeof(variable.direction) - 1
            ] = '\0';

            // ========================================================
            // value
            //
            // 正常状态：
            //     使用本次 Modbus 实际采集值。
            //
            // 强制状态：
            //     Modbus 仍然正常采集，TX/RX、timestamp 等仍然更新，
            //     但是不能覆盖共享内存中对外显示的强制值。
            // ========================================================

            if (variable.forced == 0)
            {
                variable.value =
                    value;
            }
            else
            {
                variable.value =
                    variable.forcedValue;
            }


            // timestamp

            strncpy(
                variable.timestamp,
                timestamp.c_str(),
                sizeof(variable.timestamp) - 1
            );

            variable.timestamp[
                sizeof(variable.timestamp) - 1
            ] = '\0';


            // TX

            strncpy(
                variable.txTelegram,
                txTelegram.c_str(),
                sizeof(variable.txTelegram) - 1
            );

            variable.txTelegram[
                sizeof(variable.txTelegram) - 1
            ] = '\0';


            // RX

            strncpy(
                variable.rxTelegram,
                rxTelegram.c_str(),
                sizeof(variable.rxTelegram) - 1
            );

            variable.rxTelegram[
                sizeof(variable.rxTelegram) - 1
            ] = '\0';


            // 兼容旧 telegram

            const std::string telegram =
                "TX: " +
                txTelegram +
                " | RX: " +
                rxTelegram;


            strncpy(
                variable.telegram,
                telegram.c_str(),
                sizeof(variable.telegram) - 1
            );

            variable.telegram[
                sizeof(variable.telegram) - 1
            ] = '\0';


            variable.status =
                status;


            found = true;

            break;
        }
    }


    // ========================================================
    // 新变量
    // ========================================================

    if (!found)
    {
        if (
            data->count <
            MAX_VARIABLES
        )
        {
            const int index =
                data->count;


            Variable& variable =
                data->vars[index];

            // 多任务唯一身份

            variable.taskId =
                taskId;

            variable.pointId =
                pointId;

            // name

            strncpy(
                variable.name,
                name.c_str(),
                sizeof(variable.name) - 1
            );

            variable.name[
                sizeof(variable.name) - 1
            ] = '\0';


            variable.slave =
                slave;

            variable.function =
                function;

            variable.address =
                address;


            // datatype

            strncpy(
                variable.datatype,
                datatype.c_str(),
                sizeof(variable.datatype) - 1
            );

            variable.datatype[
                sizeof(variable.datatype) - 1
            ] = '\0';


            // byteOrder

            strncpy(
                variable.byteOrder,
                byteOrder.c_str(),
                sizeof(variable.byteOrder) - 1
            );

            variable.byteOrder[
                sizeof(variable.byteOrder) - 1
            ] = '\0';

            strncpy(
                variable.direction,
                direction.c_str(),
                sizeof(variable.direction) - 1
            );

            variable.direction[
                sizeof(variable.direction) - 1
            ] = '\0';


            // ========================================================
            // value
            // ========================================================

            variable.value =
                value;


            // ========================================================
            // 强制状态初始化
            //
            // 新创建的逻辑点默认不是强制状态。
            // ========================================================

            variable.forced =
                0;

            variable.forcedValue =
                0.0;


            // ========================================================
            // timestamp
            // ========================================================

            strncpy(
                variable.timestamp,
                timestamp.c_str(),
                sizeof(variable.timestamp) - 1
            );

            variable.timestamp[
                sizeof(variable.timestamp) - 1
            ] = '\0';


            // TX

            strncpy(
                variable.txTelegram,
                txTelegram.c_str(),
                sizeof(variable.txTelegram) - 1
            );

            variable.txTelegram[
                sizeof(variable.txTelegram) - 1
            ] = '\0';


            // RX

            strncpy(
                variable.rxTelegram,
                rxTelegram.c_str(),
                sizeof(variable.rxTelegram) - 1
            );

            variable.rxTelegram[
                sizeof(variable.rxTelegram) - 1
            ] = '\0';


            // telegram 兼容字段

            const std::string telegram =
                "TX: " +
                txTelegram +
                " | RX: " +
                rxTelegram;


            strncpy(
                variable.telegram,
                telegram.c_str(),
                sizeof(variable.telegram) - 1
            );

            variable.telegram[
                sizeof(variable.telegram) - 1
            ] = '\0';


            variable.status =
                status;


            data->count++;
        }
    }


    // ========================================================
    // 解锁
    // ========================================================

    pthread_mutex_unlock(
        &data->mutex
    );


    munmap(
        data,
        sizeof(SharedData)
    );
}

bool ensureSharedMemoryOutputPoint(
    int taskId,
    int pointId,
    const std::string &name,
    int slave,
    int function,
    int address,
    const std::string &datatype,
    const std::string &byteOrder
)
{
    SharedData *data = openSharedMemory();
    if (!data)
        return false;

    pthread_mutex_lock(&data->mutex);

    Variable *variable = nullptr;
    for (int i = 0; i < data->count; ++i) {
        if (data->vars[i].taskId == taskId && data->vars[i].pointId == pointId) {
            variable = &data->vars[i];
            break;
        }
    }

    if (!variable && data->count < MAX_VARIABLES) {
        variable = &data->vars[data->count++];
        std::memset(variable, 0, sizeof(Variable));
        variable->taskId = taskId;
        variable->pointId = pointId;
    }

    if (variable) {
        variable->slave = slave;
        variable->function = function;
        variable->address = address;
        strncpy(variable->name, name.c_str(), sizeof(variable->name) - 1);
        strncpy(variable->datatype, datatype.c_str(), sizeof(variable->datatype) - 1);
        strncpy(variable->byteOrder, byteOrder.c_str(), sizeof(variable->byteOrder) - 1);
        strncpy(variable->direction, "OUTPUT", sizeof(variable->direction) - 1);
        variable->name[sizeof(variable->name) - 1] = '\0';
        variable->datatype[sizeof(variable->datatype) - 1] = '\0';
        variable->byteOrder[sizeof(variable->byteOrder) - 1] = '\0';
        variable->direction[sizeof(variable->direction) - 1] = '\0';
        variable->forced = 0;
        variable->forcedValue = 0.0;
    }

    pthread_mutex_unlock(&data->mutex);
    munmap(data, sizeof(SharedData));
    return variable != nullptr;
}

// ============================================================
// 强制共享内存中的逻辑点
//
// 重要：
// 这里只修改共享内存。
// 不发送任何 Modbus FC06 / FC16。
// 不修改真实设备寄存器。
// ============================================================

bool forceSharedMemoryValue(
    int taskId,
    int pointId,
    double value
)
{
    SharedData* data =
        openSharedMemory();

    if (data == nullptr)
    {
        return false;
    }


    pthread_mutex_lock(
        &data->mutex
    );


    bool found =
        false;


    for (
        int i = 0;
        i < data->count;
        ++i
    )
    {
        Variable& variable =
            data->vars[i];


        if (
            variable.taskId == taskId
            &&
            variable.pointId == pointId
        )
        {
            // ---------------------------------------------
            // 保存强制值
            // ---------------------------------------------

            variable.forcedValue =
                value;


            // ---------------------------------------------
            // 开启强制状态
            // ---------------------------------------------

            variable.forced =
                1;


            // ---------------------------------------------
            // 当前对外显示值立即变成强制值
            // ---------------------------------------------

            variable.value =
                value;


            found =
                true;

            break;
        }
    }


    pthread_mutex_unlock(
        &data->mutex
    );


    munmap(
        data,
        sizeof(SharedData)
    );


    return found;
}


// ============================================================
// 解除共享内存逻辑点强制
//
// 这里只清除 forced 标志。
// 不主动写设备。
//
// 下一轮正常 Modbus 采集到来以后：
//     writeSharedMemoryTelegram()
// 会重新把实际采集值写入 variable.value。
// ============================================================

bool releaseSharedMemoryValue(
    int taskId,
    int pointId
)
{
    SharedData* data =
        openSharedMemory();

    if (data == nullptr)
    {
        return false;
    }


    pthread_mutex_lock(
        &data->mutex
    );


    bool found =
        false;


    for (
        int i = 0;
        i < data->count;
        ++i
    )
    {
        Variable& variable =
            data->vars[i];


        if (
            variable.taskId == taskId
            &&
            variable.pointId == pointId
        )
        {
            variable.forced =
                0;

            variable.forcedValue =
                0.0;


            found =
                true;

            break;
        }
    }


    pthread_mutex_unlock(
        &data->mutex
    );


    munmap(
        data,
        sizeof(SharedData)
    );


    return found;
}

// ============================================================
// syncSharedMemoryPoints
//
// 根据当前有效的 Modbus 配置同步共享内存。
//
// 这里只删除已经失效的逻辑点。
//
// 不重新创建仍然有效的 Variable，
// 因此已有点的以下运行状态都会被保留：
//
//     value
//     forced
//     forcedValue
//     timestamp
//     txTelegram
//     rxTelegram
//     status
//
// 唯一身份：
//     taskId + pointId
// ============================================================

void syncSharedMemoryPoints(
    const SharedPointIdentity* points,
    int pointCount
)
{
    SharedData* data =
        openSharedMemory();


    if (data == nullptr)
    {
        return;
    }


    pthread_mutex_lock(
        &data->mutex
    );


    // ========================================================
    // 防御异常 count
    // ========================================================

    if (
        data->count < 0 ||
        data->count > MAX_VARIABLES
    )
    {
        data->count = 0;

        std::memset(
            data->vars,
            0,
            sizeof(data->vars)
        );
    }


    // ========================================================
    // 原地压缩
    //
    // readIndex:
    //     遍历原来的共享内存
    //
    // writeIndex:
    //     保存仍然有效的 Variable
    //
    // 例如：
    //
    // 原来：
    //     [1,1]
    //     [1,2]
    //     [2,9]
    //     [4,10]
    //     [4,11]
    //
    // 当前配置：
    //     [1,1]
    //     [1,2]
    //     [2,9]
    //
    // 最终：
    //     [1,1]
    //     [1,2]
    //     [2,9]
    // ========================================================

    int writeIndex = 0;


    for (
        int readIndex = 0;
        readIndex < data->count;
        ++readIndex
    )
    {
        const Variable& variable =
            data->vars[readIndex];


        bool valid = false;


        for (
            int pointIndex = 0;
            pointIndex < pointCount;
            ++pointIndex
        )
        {
            if (
                points[pointIndex].taskId
                    ==
                variable.taskId
                &&
                points[pointIndex].pointId
                    ==
                variable.pointId
            )
            {
                valid = true;
                break;
            }
        }


        // ====================================================
        // 已经不在当前配置中：
        // 直接跳过，相当于删除。
        // ====================================================

        if (!valid)
        {
            continue;
        }


        // ====================================================
        // 仍然有效：
        // 原样保留整个 Variable。
        //
        // 所以 forced / forcedValue 不会丢。
        // ====================================================

        if (
            writeIndex !=
            readIndex
        )
        {
            data->vars[writeIndex] =
                data->vars[readIndex];
        }


        ++writeIndex;
    }


    // ========================================================
    // 把尾部已经删除的旧 Variable 清零
    // ========================================================

    for (
        int i = writeIndex;
        i < data->count;
        ++i
    )
    {
        std::memset(
            &data->vars[i],
            0,
            sizeof(Variable)
        );
    }


    // ========================================================
    // 更新有效变量数量
    // ========================================================

    data->count =
        writeIndex;


    pthread_mutex_unlock(
        &data->mutex
    );


    munmap(
        data,
        sizeof(SharedData)
    );
}

// ============================================================
// 获取共享内存
// ============================================================

SharedData* getSharedMemory()
{
    int fd =
        shm_open(
            SHM_NAME,
            O_RDWR,
            0666
        );


    if (fd < 0)
    {
        perror("shm_open");

        return nullptr;
    }


    SharedData* data =
        static_cast<SharedData*>(
            mmap(
                nullptr,
                sizeof(SharedData),
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                fd,
                0
            )
        );


    close(fd);


    if (data == MAP_FAILED)
    {
        perror("mmap");

        return nullptr;
    }


    return data;
}
