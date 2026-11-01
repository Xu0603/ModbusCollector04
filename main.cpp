#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>
#include <utility>
#include <map>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

#include <modbus/modbus.h>

#include "database.h"
#include "modbus_client.h"
#include "shared_memory.h"
#include "modbus_server.h"


// ============================================================
// 兼容 modbus_client.cpp 现有 extern 声明
//
// 当前真实读取已经使用 modbus_client.cpp 内部的 raw TCP socket。
// 后面彻底整理 modbus_client.cpp 时再删除这个全局变量。
// ============================================================

modbus_t *ctx = nullptr;


// ============================================================
// 运行时任务
//
// ModbusTask 是数据库配置。
// nextPoll 是该任务下一次应该执行的时间。
// ============================================================

struct ModbusTaskRuntime
{
    ModbusTask config;

    std::chrono::steady_clock::time_point nextPoll;
};

// ============================================================
// 根据 Modbus 配置创建运行时调度表
//
// reload 配置以后也复用这个函数。
// ============================================================

static std::vector<ModbusTaskRuntime>
buildTaskRuntimes(
    const std::vector<ModbusTask> &tasks
)
{
    std::vector<ModbusTaskRuntime>
        runtimes;


    const auto now =
        std::chrono::steady_clock::now();


    for (
        const auto &task :
        tasks
    )
    {
        if (!task.enable)
        {
            continue;
        }


        if (task.period <= 0)
        {
            std::cerr
                << "[TASK] 非法采集周期，跳过任务: "
                << task.name
                << std::endl;

            continue;
        }


        ModbusTaskRuntime runtime;

        runtime.config =
            task;


        // 新加载的任务立即允许执行第一次采集
        runtime.nextPoll =
            now;


        runtimes.push_back(
            runtime
        );
    }


    return runtimes;
}

// ============================================================
// 根据当前 Modbus 配置同步共享内存中的有效逻辑点
// ============================================================

static void syncSharedMemoryWithTasks(
    const std::vector<ModbusTask> &tasks
)
{
    std::vector<SharedPointIdentity>
        validSharedPoints;


    for (
        const auto &task :
        tasks
    )
    {
        if (!task.enable)
        {
            continue;
        }


        for (
            const auto &point :
            task.points
        )
        {
            if (!point.enable)
            {
                continue;
            }


            SharedPointIdentity identity;

            identity.taskId =
                task.id;

            identity.pointId =
                point.id;


            validSharedPoints.push_back(
                identity
            );
        }
    }


    syncSharedMemoryPoints(
        validSharedPoints.empty()
            ? nullptr
            : validSharedPoints.data(),

        static_cast<int>(
            validSharedPoints.size()
        )
    );


    std::cout
        << "[SHM] 当前有效逻辑点数量: "
        << validSharedPoints.size()
        << std::endl;
}

// ============================================================
// 判断两个 Modbus 配置是否相同
//
// 用于配置热加载。
// 只有真正发生配置变化时才重建调度器。
// ============================================================

static bool sameModbusConfig(
    const std::vector<ModbusTask> &a,
    const std::vector<ModbusTask> &b
)
{
    if (a.size() != b.size())
    {
        return false;
    }


    for (
        std::size_t i = 0;
        i < a.size();
        ++i
    )
    {
        const ModbusTask &left =
            a[i];

        const ModbusTask &right =
            b[i];


        if (
            left.id != right.id ||
            left.name != right.name ||
            left.host != right.host ||
            left.port != right.port ||
            left.slaveId != right.slaveId ||
            left.period != right.period ||
            left.timeout != right.timeout ||
            left.retryCount != right.retryCount ||
            left.reconnectInterval != right.reconnectInterval ||
            left.enable != right.enable ||
            left.points.size() != right.points.size()
        )
        {
            return false;
        }


        for (
            std::size_t p = 0;
            p < left.points.size();
            ++p
        )
        {
            const Point &lp =
                left.points[p];

            const Point &rp =
                right.points[p];


            if (
                lp.id != rp.id ||
                lp.taskId != rp.taskId ||
                lp.name != rp.name ||
                lp.slave != rp.slave ||
                lp.function != rp.function ||
                lp.address != rp.address ||
                lp.datatype != rp.datatype ||
                lp.byteOrder != rp.byteOrder ||
                lp.direction != rp.direction ||
                lp.period != rp.period ||
                lp.enable != rp.enable
            )
            {
                return false;
            }
        }
    }


    return true;
}

// ============================================================
// 打印任务配置
// ============================================================

static void printTaskConfig(
    const std::vector<ModbusTask> &tasks
)
{
    std::cout
        << "========================================"
        << std::endl;

    std::cout
        << "读取到 Modbus 任务数量: "
        << tasks.size()
        << std::endl;


    for (
        const auto &task :
        tasks
    )
    {
        std::cout
            << std::endl
            << "[Modbus任务]"
            << std::endl;

        std::cout
            << "ID      : "
            << task.id
            << std::endl;

        std::cout
            << "名称    : "
            << task.name
            << std::endl;

        std::cout
            << "设备    : "
            << task.host
            << ":"
            << task.port
            << std::endl;

        std::cout
            << "Unit ID : "
            << task.slaveId
            << std::endl;

        std::cout
            << "周期    : "
            << task.period
            << " ms"
            << std::endl;

        std::cout
            << "超时    : "
            << task.timeout
            << " ms"
            << std::endl;

        std::cout
            << "重试    : "
            << task.retryCount
            << std::endl;

        std::cout
            << "重连    : "
            << task.reconnectInterval
            << " ms"
            << std::endl;

        std::cout
            << "点数    : "
            << task.points.size()
            << std::endl;


        for (
            const auto &point :
            task.points
        )
        {
            std::cout
                << "    Point ID="
                << point.id
                << " name="
                << point.name
                << " FC="
                << point.function
                << " address="
                << point.address
                << " type="
                << point.datatype
                << " direction="
                << point.direction
                << std::endl;
        }
    }


    std::cout
        << "========================================"
        << std::endl;
}


// ============================================================
// 执行一个完整 Modbus 任务
//
// 重点：
//
// 一个任务周期到了以后，连续采集该任务下面所有点。
// 不再对每一个 Point 单独 sleep(period)。
// ============================================================

static void executeTask(
    ModbusTaskConnection &connection,
    const ModbusTask &task
)
{
    std::cout
        << std::endl
        << "############################################################"
        << std::endl;

    std::cout
        << "[TASK START]"
        << " id="
        << task.id
        << " name="
        << task.name
        << " period="
        << task.period
        << "ms"
        << " points="
        << task.points.size()
        << std::endl;

    std::cout
        << "############################################################"
        << std::endl;


    const auto taskStart =
        std::chrono::steady_clock::now();


    for (
        const auto &point :
        task.points
    )
    {
        readModbus(
            connection,
            task,
            point
        );
    }


    const auto taskEnd =
        std::chrono::steady_clock::now();


    const auto cost =
        std::chrono::duration_cast<
            std::chrono::milliseconds
        >(
            taskEnd - taskStart
        )
        .count();


    std::cout
        << "############################################################"
        << std::endl;

    std::cout
        << "[TASK END]"
        << " id="
        << task.id
        << " name="
        << task.name
        << " cost="
        << cost
        << "ms"
        << std::endl;

    std::cout
        << "############################################################"
        << std::endl;
}

// ============================================================
// Modbus 任务 Worker
//
// 一个 Worker 对应一个 ModbusTask。
//
// 每个 Worker 拥有：
//     1. 自己的任务配置
//     2. 自己的采集线程
//     3. 自己的停止信号
//     4. 自己的等待条件变量
//
// TCP Connection 将在 Worker 线程内部创建，
// 不再由 main 主线程共享或操作。
// ============================================================

struct ModbusTaskWorker
{
    ModbusTask config;

    std::thread thread;

    std::atomic<bool> stopRequested{false};

    std::mutex waitMutex;

    std::condition_variable waitCv;
};

// ============================================================
// 单个 Modbus 任务的采集线程
//
// 每个任务独立执行本函数。
//
// Task1 的 readModbus() 阻塞，不会阻塞 Task2 的 Worker。
//
// 每个 Worker 内部创建自己的 TCP Connection。
// ============================================================

static void runModbusTaskWorker(
    ModbusTaskWorker *worker
)
{
    if (worker == nullptr)
    {
        return;
    }


    const ModbusTask &task =
        worker->config;


    // ========================================================
    // 当前 Worker 独占的 TCP 连接
    //
    // 其他 Worker 无法访问这个 connection。
    // ========================================================

    ModbusTaskConnection connection;


    std::cout
        << "[WORKER] 采集线程启动"
        << " taskId="
        << task.id
        << " name="
        << task.name
        << " period="
        << task.period
        << "ms"
        << std::endl;


    // ========================================================
    // 初始化当前任务的采集周期
    // ========================================================

    const auto period =
        std::chrono::milliseconds(
            task.period
        );


    auto nextPoll =
        std::chrono::steady_clock::now();


    // ========================================================
    // 当前 Worker 的独立采集循环
    // ========================================================

    while (
        !worker->stopRequested.load()
    )
    {
        // ----------------------------------------------------
        // 等待当前任务的下一次采集时间
        //
        // 使用 condition_variable：
        //
        // 正常情况：
        //     等待到 nextPoll。
        //
        // 收到停止请求：
        //     立即唤醒，不需要等待完整采集周期。
        // ----------------------------------------------------

        {
            std::unique_lock<std::mutex> lock(
                worker->waitMutex
            );


            worker->waitCv.wait_until(
                lock,
                nextPoll,

                [worker]()
                {
                    return worker->stopRequested.load();
                }
            );
        }


        // ----------------------------------------------------
        // 检查是否要求停止
        // ----------------------------------------------------

        if (
            worker->stopRequested.load()
        )
        {
            break;
        }


        // ----------------------------------------------------
        // 保存本次计划采集时间
        // ----------------------------------------------------

        const auto scheduledTime =
            nextPoll;


        // ----------------------------------------------------
        // 使用当前线程独占的 TCP Connection
        // 执行完整任务采集
        // ----------------------------------------------------

        executeTask(
            connection,
            task
        );


        // ----------------------------------------------------
        // 如果采集期间收到了停止请求，
        // 不再进入下一轮。
        // ----------------------------------------------------

        if (
            worker->stopRequested.load()
        )
        {
            break;
        }


        // ----------------------------------------------------
        // 计算下一次采集时间
        //
        // 与旧调度器保持相同的周期计算方式。
        // ----------------------------------------------------

        nextPoll =
            scheduledTime
            +
            period;


        const auto afterTask =
            std::chrono::steady_clock::now();


        // ----------------------------------------------------
        // 如果采集耗时超过一个周期，
        // 跳过已经错过的采集时间，不疯狂补采。
        // ----------------------------------------------------

        while (
            nextPoll <= afterTask
        )
        {
            nextPoll += period;
        }
    }


    // ========================================================
    // Worker 退出前，关闭自己的 TCP Connection
    //
    // 不会影响其他任务正在使用的 Socket。
    // ========================================================

    closeModbusTaskConnection(
        connection
    );


    std::cout
        << "[WORKER] 采集线程退出"
        << " taskId="
        << task.id
        << " name="
        << task.name
        << std::endl;
}

// ============================================================
// 启动一个 Modbus 任务 Worker
//
// 创建独立线程并开始采集。
// ============================================================

static std::unique_ptr<ModbusTaskWorker>
startModbusTaskWorker(
    const ModbusTask &task
)
{
    // ========================================================
    // 禁用任务不启动 Worker
    // ========================================================

    if (!task.enable)
    {
        return nullptr;
    }


    // ========================================================
    // 检查采集周期
    // ========================================================

    if (task.period <= 0)
    {
        std::cerr
            << "[WORKER] 非法采集周期，无法启动任务"
            << " taskId="
            << task.id
            << " period="
            << task.period
            << std::endl;

        return nullptr;
    }


    // ========================================================
    // 创建 Worker 对象
    // ========================================================

    auto worker =
        std::make_unique<ModbusTaskWorker>();


    worker->config =
        task;


    worker->stopRequested.store(
        false
    );


    // ========================================================
    // 创建任务独立线程
    //
    // Worker 对象由管理器持有。
    // 线程停止并 join() 之前不能销毁 Worker。
    // ========================================================

    ModbusTaskWorker *workerPtr =
        worker.get();


    worker->thread =
        std::thread(
            [workerPtr]()
            {
                runModbusTaskWorker(
                    workerPtr
                );
            }
        );


    return worker;
}

// ============================================================
// 停止一个 Modbus 任务 Worker
//
// 1. 设置停止信号
// 2. 唤醒正在等待下一个采集周期的线程
// 3. 等待当前线程退出
//
// Worker 自己负责关闭 TCP Socket。
// ============================================================

static void stopModbusTaskWorker(
    ModbusTaskWorker &worker
)
{
    std::cout
        << "[WORKER] 请求停止任务"
        << " taskId="
        << worker.config.id
        << std::endl;


    // ========================================================
    // 设置停止信号
    // ========================================================

    worker.stopRequested.store(
        true
    );


    // ========================================================
    // 唤醒等待下一次采集的 Worker
    // ========================================================

    worker.waitCv.notify_all();


    // ========================================================
    // 等待 Worker 正常退出
    //
    // 必须先 join，再销毁 Worker 对象。
    // ========================================================

    if (
        worker.thread.joinable()
    )
    {
        worker.thread.join();
    }


    std::cout
        << "[WORKER] 任务已经停止"
        << " taskId="
        << worker.config.id
        << std::endl;
}

// ============================================================
// Modbus Worker 管理器
//
// 根据数据库中的最新任务配置，同步当前正在运行的 Worker。
//
// 已存在且配置没有变化的任务：
//     保留原来的线程和 TCP 连接。
//
// 已删除、禁用或者配置发生变化的任务：
//     先通知旧 Worker 停止，并等待线程退出。
//
// 新增或者需要重新启动的任务：
//     创建新的 Worker 线程。
//
// 特别注意：
//     必须先停止失效 Worker，
//     再同步共享内存并启动新 Worker。
// ============================================================

static void reconcileModbusWorkers(
    std::map<
        int,
        std::unique_ptr<ModbusTaskWorker>
    > &workers,

    const std::vector<ModbusTask> &tasks
)
{
    // ========================================================
    // 1. 检查已经存在的 Worker
    //
    // 删除任务、禁用任务、修改任务配置时，
    // 停止对应的旧 Worker。
    // ========================================================

    for (
        auto it = workers.begin();
        it != workers.end();
    )
    {
        const int taskId =
            it->first;


        ModbusTaskWorker &worker =
            *(it->second);


        // ----------------------------------------------------
        // 在数据库最新配置中寻找当前任务
        // ----------------------------------------------------

        const auto taskIt =
            std::find_if(
                tasks.begin(),
                tasks.end(),

                [taskId](
                    const ModbusTask &task
                )
                {
                    return
                        task.id == taskId
                        &&
                        task.enable
                        &&
                        task.period > 0;
                }
            );


        // ----------------------------------------------------
        // 判断是否需要停止当前 Worker
        // ----------------------------------------------------

        bool shouldStop =
            false;


        if (
            taskIt == tasks.end()
        )
        {
            // 任务已删除、禁用或采集周期无效
            shouldStop = true;
        }
        else
        {
            // ------------------------------------------------
            // 当前任务仍然存在。
            //
            // 比较 Worker 正在使用的配置
            // 和 SQLite 中的最新配置。
            //
            // 复用现有 sameModbusConfig()，
            // 包括任务属性及其点表配置。
            // ------------------------------------------------

            const std::vector<ModbusTask> oldConfig{
                worker.config
            };


            const std::vector<ModbusTask> newConfig{
                *taskIt
            };


            if (
                !sameModbusConfig(
                    oldConfig,
                    newConfig
                )
            )
            {
                shouldStop = true;
            }
        }


        // ----------------------------------------------------
        // 配置未变化：保留原来的 Worker
        // ----------------------------------------------------

        if (
            !shouldStop
        )
        {
            ++it;

            continue;
        }


        // ----------------------------------------------------
        // 停止旧 Worker
        //
        // stopModbusTaskWorker() 内部会：
        //     设置停止信号
        //     唤醒等待线程
        //     join()
        //
        // Worker 自己负责关闭 TCP Socket。
        // ----------------------------------------------------

        std::cout
            << "[WORKER MANAGER] 停止旧任务"
            << " taskId="
            << taskId
            << std::endl;


        stopModbusTaskWorker(
            worker
        );


        // ----------------------------------------------------
        // 线程已经退出，现在可以安全删除 Worker
        // ----------------------------------------------------

        it =
            workers.erase(
                it
            );
    }


    // ========================================================
    // 2. 同步共享内存
    //
    // 此时已经删除的任务不可能继续写入共享内存。
    //
    // 未变化的 Worker 可以继续正常采集。
    //
    // syncSharedMemoryPoints() 使用现有的共享内存 mutex，
    // 因此不会与正常采集写入发生数据竞争。
    // ========================================================

    syncSharedMemoryWithTasks(
        tasks
    );


    // ========================================================
    // 3. 为新增任务或配置变化的任务启动 Worker
    // ========================================================

    for (
        const auto &task :
        tasks
    )
    {
        // ----------------------------------------------------
        // 禁用任务不启动
        // ----------------------------------------------------

        if (
            !task.enable
        )
        {
            continue;
        }


        // ----------------------------------------------------
        // 无效采集周期不启动
        // ----------------------------------------------------

        if (
            task.period <= 0
        )
        {
            std::cerr
                << "[WORKER MANAGER] 非法采集周期"
                << " taskId="
                << task.id
                << std::endl;

            continue;
        }


        // ----------------------------------------------------
        // 如果这个任务已经有 Worker，
        // 说明它的配置没有变化。
        //
        // 不重复创建线程。
        // ----------------------------------------------------

        if (
            workers.find(
                task.id
            )
            !=
            workers.end()
        )
        {
            continue;
        }


        // ----------------------------------------------------
        // 为当前任务创建独立的 Worker
        // ----------------------------------------------------

        std::cout
            << "[WORKER MANAGER] 启动新任务"
            << " taskId="
            << task.id
            << " name="
            << task.name
            << std::endl;


        auto worker =
            startModbusTaskWorker(
                task
            );


        if (
            worker
        )
        {
            workers.emplace(
                task.id,
                std::move(
                    worker
                )
            );
        }
    }


    // ========================================================
    // 4. 输出当前 Worker 数量
    // ========================================================

    std::cout
        << "[WORKER MANAGER] 当前运行线程数量: "
        << workers.size()
        << std::endl;
}


int main()
{
    // ========================================================
    // 1. 初始化共享内存
    // ========================================================

    initSharedMemory();


    // ========================================================
    // 2. 启动提供给 Qt 的 ModbusServer
    // ========================================================

    ModbusServer modbusServer(
        "127.0.0.1",
        19091
    );


    if (!modbusServer.start())
    {
        std::cerr
            << "ModbusServer 启动失败"
            << std::endl;

        return -1;
    }


    std::cout
        << "ModbusServer 启动成功"
        << std::endl;

    std::cout
        << "监听地址: 127.0.0.1:19091"
        << std::endl;


    // ========================================================
    // 3. Server 单独线程运行
    // ========================================================

    std::thread serverThread(
        [&modbusServer]()
        {
            modbusServer.run();
        }
    );


    // ========================================================
    // 4. 从数据库加载初始 Modbus 任务配置
    // ========================================================

    const std::string databasePath =
        "/home/xu/桌面/ModbusCollector/modbus_config.db";


    auto tasks =
        loadModbusTasks(
            databasePath
        );


    printTaskConfig(
        tasks
    );


    // ========================================================
    // 5. 创建 Worker 管理表
    //
    // Key   : ModbusTask ID
    // Value : 当前任务的 Worker 对象
    //
    // 每个 Worker 内部拥有：
    //     std::thread
    //     stopRequested
    //     waitCv
    //     当前任务配置
    //
    // TCP Connection 由 Worker 线程在运行时创建。
    // ========================================================

    std::map<
        int,
        std::unique_ptr<ModbusTaskWorker>
    > workers;


    // ========================================================
    // 6. 根据初始任务配置启动 Worker
    //
    // 如果数据库中有两个有效任务，
    // 这里就会创建两个采集 Worker。
    //
    // 如果当前没有任务，
    // workers 将保持为空，main 线程继续等待配置变化。
    // ========================================================

    std::cout
        << std::endl
        << "========================================"
        << std::endl
        << "启动 Modbus 多线程任务调度器"
        << std::endl;


    reconcileModbusWorkers(
        workers,
        tasks
    );


    std::cout
        << "[MAIN] 初始 Worker 数量: "
        << workers.size()
        << std::endl;

    std::cout
        << "========================================"
        << std::endl;


    // ========================================================
    // 7. 配置热加载检查周期
    //
    // main 线程以后只负责配置检查，
    // 不再直接执行 Modbus 采集。
    // ========================================================

    const auto configCheckInterval =
        std::chrono::milliseconds(
            1000
        );


    auto nextConfigCheck =
        std::chrono::steady_clock::now()
        +
        configCheckInterval;


    // ========================================================
    // 8. 主线程配置管理循环
    //
    // Worker 线程：
    //     各自独立采集 Modbus 数据。
    //
    // main 线程：
    //     每 1 秒检查一次 SQLite。
    //
    // 不再使用旧的 runtimes / nextPoll 调度表。
    // ========================================================

    while (true)
    {
        const auto current =
            std::chrono::steady_clock::now();


        // ----------------------------------------------------
        // 还没有到下一次配置检查时间
        // ----------------------------------------------------

        if (
            current < nextConfigCheck
        )
        {
            std::this_thread::sleep_until(
                nextConfigCheck
            );

            continue;
        }


        // ====================================================
        // 重新读取数据库配置
        // ====================================================

        auto newTasks =
            loadModbusTasks(
                databasePath
            );


        // ====================================================
        // 判断配置是否真正发生变化
        //
        // 没有变化：
        //     Worker 保持不变。
        //
        // 发生变化：
        //     停止失效 Worker。
        //     同步共享内存。
        //     启动新增 Worker。
        // ====================================================

        if (
            !sameModbusConfig(
                tasks,
                newTasks
            )
        )
        {
            std::cout
                << std::endl
                << "========================================"
                << std::endl
                << "[CONFIG] 检测到 Modbus 配置变化"
                << std::endl;


            // ------------------------------------------------
            // 根据最新配置调整 Worker
            //
            // 这里会处理：
            //     新建任务
            //     删除任务
            //     启用 / 禁用任务
            //     修改任务参数
            //     新增 / 删除 / 修改逻辑点
            //
            // 没变化的 Worker 不会被重启。
            // ------------------------------------------------

            reconcileModbusWorkers(
                workers,
                newTasks
            );


            // ------------------------------------------------
            // Worker 已按照新配置完成同步，
            // 更新当前配置快照。
            // ------------------------------------------------

            tasks =
                std::move(
                    newTasks
                );


            printTaskConfig(
                tasks
            );


            std::cout
                << "[CONFIG] 多线程热加载完成"
                << std::endl
                << "[CONFIG] 当前运行 Worker 数量: "
                << workers.size()
                << std::endl
                << "========================================"
                << std::endl;
        }


        // ====================================================
        // 安排下一次配置检查
        // ====================================================

        nextConfigCheck =
            std::chrono::steady_clock::now()
            +
            configCheckInterval;
    }


    // ========================================================
    // 当前程序保持常驻运行。
    //
    // 后续增加统一退出信号处理时，
    // 再在这里停止所有 Worker 和 ModbusServer。
    // ========================================================

    return 0;
}
