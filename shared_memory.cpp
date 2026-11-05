#include "shared_memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

namespace {

constexpr const char *kSharedMemoryName = "/modbus_data";

template <std::size_t N>
void copyText(char (&destination)[N], const std::string &source)
{
    std::strncpy(destination, source.c_str(), N - 1);
    destination[N - 1] = '\0';
}

bool initMutex(SharedData *data)
{
    pthread_mutexattr_t attributes;
    if (pthread_mutexattr_init(&attributes) != 0) {
        std::cerr << "pthread_mutexattr_init 失败\n";
        return false;
    }
    if (pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED) != 0) {
        std::cerr << "pthread_mutexattr_setpshared 失败\n";
        pthread_mutexattr_destroy(&attributes);
        return false;
    }
    const int result = pthread_mutex_init(&data->mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
    if (result != 0)
        std::cerr << "pthread_mutex_init 失败\n";
    return result == 0;
}

SharedData *mapSharedMemory(bool create)
{
    const int flags = create ? O_CREAT | O_RDWR : O_RDWR;
    const int fd = shm_open(kSharedMemoryName, flags, 0666);
    if (fd < 0) {
        perror("shm_open");
        return nullptr;
    }

    struct stat status {};
    if (fstat(fd, &status) < 0) {
        perror("fstat");
        close(fd);
        return nullptr;
    }
    const bool firstCreate = status.st_size == 0;
    if (create && ftruncate(fd, sizeof(SharedData)) < 0) {
        perror("ftruncate");
        close(fd);
        return nullptr;
    }

    auto *data = static_cast<SharedData *>(mmap(
        nullptr, sizeof(SharedData), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    close(fd);
    if (data == MAP_FAILED) {
        perror("mmap");
        return nullptr;
    }

    if (firstCreate) {
        std::memset(data, 0, sizeof(SharedData));
        // 该互斥量位于共享内存中，必须显式设置为进程间共享。
        if (!initMutex(data)) {
            munmap(data, sizeof(SharedData));
            return nullptr;
        }
    }
    return data;
}

void unmapSharedMemory(SharedData *data)
{
    munmap(data, sizeof(SharedData));
}

Variable *findVariable(SharedData *data, int taskId, int pointId)
{
    for (int i = 0; i < data->count; ++i) {
        if (data->vars[i].taskId == taskId && data->vars[i].pointId == pointId)
            return &data->vars[i];
    }
    return nullptr;
}

Variable *appendVariable(SharedData *data)
{
    if (data->count >= MAX_VARIABLES)
        return nullptr;
    Variable *variable = &data->vars[data->count++];
    std::memset(variable, 0, sizeof(Variable));
    return variable;
}

void updateTelegram(Variable &variable, const std::string &tx, const std::string &rx)
{
    copyText(variable.txTelegram, tx);
    copyText(variable.rxTelegram, rx);
    copyText(variable.telegram, "TX: " + tx + " | RX: " + rx);
}

} // namespace

void initSharedMemory()
{
    SharedData *data = mapSharedMemory(true);
    if (!data)
        return;

    pthread_mutex_lock(&data->mutex);
    if (data->count < 0 || data->count > MAX_VARIABLES) {
        data->count = 0;
        std::memset(data->vars, 0, sizeof(data->vars));
    }
    pthread_mutex_unlock(&data->mutex);
    unmapSharedMemory(data);
}

void writeSharedMemory(std::string name, int slave, int function, int address,
                       std::string datatype, double value, std::string timestamp,
                       std::string telegram, int status)
{
    SharedData *data = mapSharedMemory(true);
    if (!data)
        return;

    pthread_mutex_lock(&data->mutex);
    Variable *variable = nullptr;
    for (int i = 0; i < data->count; ++i) {
        if (std::strcmp(data->vars[i].name, name.c_str()) == 0) {
            variable = &data->vars[i];
            break;
        }
    }
    if (!variable)
        variable = appendVariable(data);

    if (variable) {
        copyText(variable->name, name);
        variable->slave = slave;
        variable->function = function;
        variable->address = address;
        copyText(variable->datatype, datatype);
        copyText(variable->byteOrder, "ABCD");
        copyText(variable->direction, "INPUT");
        variable->value = value;
        copyText(variable->timestamp, timestamp);
        copyText(variable->telegram, telegram);
        variable->status = status;
    }
    pthread_mutex_unlock(&data->mutex);
    unmapSharedMemory(data);
}

void writeSharedMemoryTelegram(int taskId, int pointId, std::string name, int slave,
                               int function, int address, std::string datatype,
                               std::string byteOrder, std::string direction, double value,
                               std::string timestamp, std::string txTelegram,
                               std::string rxTelegram, int status)
{
    SharedData *data = mapSharedMemory(true);
    if (!data)
        return;

    pthread_mutex_lock(&data->mutex);
    Variable *variable = findVariable(data, taskId, pointId);
    if (!variable)
        variable = appendVariable(data);

    if (variable) {
        variable->taskId = taskId;
        variable->pointId = pointId;
        copyText(variable->name, name);
        variable->slave = slave;
        variable->function = function;
        variable->address = address;
        copyText(variable->datatype, datatype);
        copyText(variable->byteOrder, byteOrder);
        copyText(variable->direction, direction);
        // 强制仅改变展示值，采集时间、状态和原始报文仍持续刷新。
        variable->value = variable->forced ? variable->forcedValue : value;
        copyText(variable->timestamp, timestamp);
        updateTelegram(*variable, txTelegram, rxTelegram);
        variable->status = status;
    }
    pthread_mutex_unlock(&data->mutex);
    unmapSharedMemory(data);
}

bool ensureSharedMemoryOutputPoint(int taskId, int pointId, const std::string &name,
                                   int slave, int function, int address,
                                   const std::string &datatype,
                                   const std::string &byteOrder)
{
    SharedData *data = mapSharedMemory(true);
    if (!data)
        return false;

    pthread_mutex_lock(&data->mutex);
    Variable *variable = findVariable(data, taskId, pointId);
    if (!variable)
        variable = appendVariable(data);
    if (variable) {
        variable->taskId = taskId;
        variable->pointId = pointId;
        copyText(variable->name, name);
        variable->slave = slave;
        variable->function = function;
        variable->address = address;
        copyText(variable->datatype, datatype);
        copyText(variable->byteOrder, byteOrder);
        copyText(variable->direction, "OUTPUT");
        variable->forced = 0;
        variable->forcedValue = 0.0;
    }
    const bool created = variable != nullptr;
    pthread_mutex_unlock(&data->mutex);
    unmapSharedMemory(data);
    return created;
}

bool forceSharedMemoryValue(int taskId, int pointId, double value)
{
    SharedData *data = mapSharedMemory(true);
    if (!data)
        return false;

    pthread_mutex_lock(&data->mutex);
    Variable *variable = findVariable(data, taskId, pointId);
    if (variable) {
        variable->forcedValue = value;
        variable->forced = 1;
        variable->value = value;
    }
    const bool found = variable != nullptr;
    pthread_mutex_unlock(&data->mutex);
    unmapSharedMemory(data);
    return found;
}

bool releaseSharedMemoryValue(int taskId, int pointId)
{
    SharedData *data = mapSharedMemory(true);
    if (!data)
        return false;

    pthread_mutex_lock(&data->mutex);
    Variable *variable = findVariable(data, taskId, pointId);
    if (variable) {
        variable->forced = 0;
        variable->forcedValue = 0.0;
        // value 在下一次 INPUT 采集时恢复，避免用未知的设备值覆盖当前显示。
    }
    const bool found = variable != nullptr;
    pthread_mutex_unlock(&data->mutex);
    unmapSharedMemory(data);
    return found;
}

void syncSharedMemoryPoints(const SharedPointIdentity *points, int pointCount)
{
    SharedData *data = mapSharedMemory(true);
    if (!data)
        return;

    pthread_mutex_lock(&data->mutex);
    if (data->count < 0 || data->count > MAX_VARIABLES) {
        data->count = 0;
        std::memset(data->vars, 0, sizeof(data->vars));
    }

    int writeIndex = 0;
    for (int readIndex = 0; readIndex < data->count; ++readIndex) {
        const Variable &variable = data->vars[readIndex];
        bool valid = false;
        for (int pointIndex = 0; pointIndex < pointCount; ++pointIndex) {
            if (points[pointIndex].taskId == variable.taskId
                && points[pointIndex].pointId == variable.pointId) {
                valid = true;
                break;
            }
        }
        if (!valid)
            continue;
        if (writeIndex != readIndex)
            data->vars[writeIndex] = variable;
        ++writeIndex;
    }
    for (int i = writeIndex; i < data->count; ++i)
        std::memset(&data->vars[i], 0, sizeof(Variable));
    data->count = writeIndex;

    pthread_mutex_unlock(&data->mutex);
    unmapSharedMemory(data);
}

SharedData *getSharedMemory()
{
    return mapSharedMemory(false);
}
