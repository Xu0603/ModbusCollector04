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