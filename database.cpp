#include "database.h"

#include <sqlite3.h>

#include <iostream>
#include <string>
#include <vector>


namespace
{

// ============================================================
// 安全读取 SQLite TEXT
// ============================================================

std::string columnText(
    sqlite3_stmt *stmt,
    int column
)
{
    const unsigned char *text =
        sqlite3_column_text(
            stmt,
            column
        );

    if (text == nullptr)
    {
        return std::string();
    }

    return reinterpret_cast<
        const char *
    >(text);
}


// ============================================================
// 打印 SQLite 错误
// ============================================================

void printSqliteError(
    const char *prefix,
    sqlite3 *conn
)
{
    std::cerr
        << prefix
        << ": ";

    if (conn)
    {
        std::cerr
            << sqlite3_errmsg(conn);
    }
    else
    {
        std::cerr
            << "unknown sqlite error";
    }

    std::cerr
        << std::endl;
}

} // namespace


// ============================================================
// 旧接口：加载所有启用点
//
// 暂时保留，兼容现有代码。
// ============================================================

std::vector<Point>
loadPoints(
    std::string db
)
{
    std::vector<Point> result;

    sqlite3 *conn = nullptr;


    if (
        sqlite3_open(
            db.c_str(),
            &conn
        )
        !=
        SQLITE_OK
    )
    {
        printSqliteError(
            "打开数据库失败",
            conn
        );

        if (conn)
        {
            sqlite3_close(conn);
        }

        return result;
    }


    const char *sql =
        "SELECT "
        "id, "
        "task_id, "
        "name, "
        "slave_id, "
        "function, "
        "address, "
        "datatype, "
        "byte_order, "
        "period, "
        "enable, "
        "direction "
        "FROM modbus_points "
        "WHERE enable = 1 "
        "ORDER BY id;";


    sqlite3_stmt *stmt = nullptr;


    if (
        sqlite3_prepare_v2(
            conn,
            sql,
            -1,
            &stmt,
            nullptr
        )
        !=
        SQLITE_OK
    )
    {
        printSqliteError(
            "读取 modbus_points 准备 SQL 失败",
            conn
        );

        sqlite3_close(conn);

        return result;
    }


    while (
        sqlite3_step(stmt)
        ==
        SQLITE_ROW
    )
    {
        Point p;

        p.id =
            sqlite3_column_int(
                stmt,
                0
            );

        p.taskId =
            sqlite3_column_int(
                stmt,
                1
            );

        p.name =
            columnText(
                stmt,
                2
            );

        p.slave =
            sqlite3_column_int(
                stmt,
                3
            );

        p.function =
            sqlite3_column_int(
                stmt,
                4
            );

        p.address =
            sqlite3_column_int(
                stmt,
                5
            );

        p.datatype =
            columnText(
                stmt,
                6
            );

        p.byteOrder =
            columnText(
                stmt,
                7
            );

        p.period =
            sqlite3_column_int(
                stmt,
                8
            );

        p.enable =
            sqlite3_column_int(
                stmt,
                9
            )
            !=
            0;

        p.direction = columnText(stmt, 10);
        if (p.direction != "OUTPUT")
            p.direction = "INPUT";


        result.push_back(
            p
        );
    }


    sqlite3_finalize(
        stmt
    );

    sqlite3_close(
        conn
    );


    return result;
}


// ============================================================
// 新接口：加载 Modbus 任务
// ============================================================

std::vector<ModbusTask>
loadModbusTasks(
    const std::string &db
)
{
    std::vector<ModbusTask> tasks;

    sqlite3 *conn = nullptr;


    // ========================================================
    // 1. 打开数据库
    // ========================================================

    if (
        sqlite3_open(
            db.c_str(),
            &conn
        )
        !=
        SQLITE_OK
    )
    {
        printSqliteError(
            "打开数据库失败",
            conn
        );

        if (conn)
        {
            sqlite3_close(
                conn
            );
        }

        return tasks;
    }

    // 向后兼容：已有 TCP 数据库首次启动时自动补齐 RTU 配置列。
    sqlite3_exec(conn, "ALTER TABLE modbus_tasks ADD COLUMN transport TEXT NOT NULL DEFAULT 'TCP';", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "ALTER TABLE modbus_tasks ADD COLUMN serial_port TEXT NOT NULL DEFAULT '/dev/ttyS0';", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "ALTER TABLE modbus_tasks ADD COLUMN baud_rate INTEGER NOT NULL DEFAULT 9600;", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "ALTER TABLE modbus_tasks ADD COLUMN parity TEXT NOT NULL DEFAULT 'N';", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "ALTER TABLE modbus_tasks ADD COLUMN data_bits INTEGER NOT NULL DEFAULT 8;", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "ALTER TABLE modbus_tasks ADD COLUMN stop_bits INTEGER NOT NULL DEFAULT 1;", nullptr, nullptr, nullptr);


    // ========================================================
    // 2. 加载所有启用的 Modbus 任务
    // ========================================================

    const char *taskSql =
        "SELECT "
        "id, "
        "name, "
        "host, "
        "port, "
        "slave_id, "
        "period, "
        "timeout, "
        "retry_count, "
        "reconnect_interval, "
        "enable, transport, serial_port, baud_rate, parity, data_bits, stop_bits "
        "FROM modbus_tasks "
        "WHERE enable = 1 "
        "ORDER BY id;";


    sqlite3_stmt *taskStmt =
        nullptr;


    if (
        sqlite3_prepare_v2(
            conn,
            taskSql,
            -1,
            &taskStmt,
            nullptr
        )
        !=
        SQLITE_OK
    )
    {
        printSqliteError(
            "读取 modbus_tasks 准备 SQL 失败",
            conn
        );

        sqlite3_close(
            conn
        );

        return tasks;
    }


    while (
        sqlite3_step(taskStmt)
        ==
        SQLITE_ROW
    )
    {
        ModbusTask task;

        task.id =
            sqlite3_column_int(
                taskStmt,
                0
            );

        task.name =
            columnText(
                taskStmt,
                1
            );

        task.host =
            columnText(
                taskStmt,
                2
            );

        task.port =
            sqlite3_column_int(
                taskStmt,
                3
            );

        task.slaveId =
            sqlite3_column_int(
                taskStmt,
                4
            );

        task.period =
            sqlite3_column_int(
                taskStmt,
                5
            );

        task.timeout =
            sqlite3_column_int(
                taskStmt,
                6
            );

        task.retryCount =
            sqlite3_column_int(
                taskStmt,
                7
            );

        task.reconnectInterval =
            sqlite3_column_int(
                taskStmt,
                8
            );

        task.enable =
            sqlite3_column_int(
                taskStmt,
                9
            )
            !=
            0;

        task.transport = columnText(taskStmt, 10);
        task.serialPort = columnText(taskStmt, 11);
        task.baudRate = sqlite3_column_int(taskStmt, 12);
        const std::string parity = columnText(taskStmt, 13);
        task.parity = parity.empty() ? 'N' : parity.front();
        task.dataBits = sqlite3_column_int(taskStmt, 14);
        task.stopBits = sqlite3_column_int(taskStmt, 15);


        tasks.push_back(
            task
        );
    }


    sqlite3_finalize(
        taskStmt
    );


    // ========================================================
    // 3. 为每一个任务读取自己的点
    // ========================================================

    const char *pointSql =
        "SELECT "
        "id, "
        "task_id, "
        "name, "
        "slave_id, "
        "function, "
        "address, "
        "datatype, "
        "byte_order, "
        "period, "
        "enable, "
        "direction "
        "FROM modbus_points "
        "WHERE task_id = ? "
        "AND enable = 1 "
        "ORDER BY id;";


    sqlite3_stmt *pointStmt =
        nullptr;


    if (
        sqlite3_prepare_v2(
            conn,
            pointSql,
            -1,
            &pointStmt,
            nullptr
        )
        !=
        SQLITE_OK
    )
    {
        printSqliteError(
            "读取任务点表准备 SQL 失败",
            conn
        );

        sqlite3_close(
            conn
        );

        return {};
    }


    for (
        auto &task :
        tasks
    )
    {
        sqlite3_reset(
            pointStmt
        );

        sqlite3_clear_bindings(
            pointStmt
        );


        sqlite3_bind_int(
            pointStmt,
            1,
            task.id
        );


        while (
            sqlite3_step(pointStmt)
            ==
            SQLITE_ROW
        )
        {
            Point p;

            p.id =
                sqlite3_column_int(
                    pointStmt,
                    0
                );

            p.taskId =
                sqlite3_column_int(
                    pointStmt,
                    1
                );

            p.name =
                columnText(
                    pointStmt,
                    2
                );

            // -----------------------------------------------
            // 这里暂时仍读取旧字段。
            //
            // 但真正运行时：
            // slave / period 应该属于 task。
            //
            // 为了让下一阶段改动更安全，
            // 这里直接使用任务配置覆盖旧值。
            // -----------------------------------------------

            p.slave =
                task.slaveId;

            p.function =
                sqlite3_column_int(
                    pointStmt,
                    4
                );

            p.address =
                sqlite3_column_int(
                    pointStmt,
                    5
                );

            p.datatype =
                columnText(
                    pointStmt,
                    6
                );

            p.byteOrder =
                columnText(
                    pointStmt,
                    7
                );

            p.period =
                task.period;

            p.enable =
                sqlite3_column_int(
                    pointStmt,
                    9
                )
                !=
                0;

            p.direction = columnText(pointStmt, 10);
            if (p.direction != "OUTPUT")
                p.direction = "INPUT";


            task.points.push_back(
                p
            );
        }
    }


    sqlite3_finalize(
        pointStmt
    );

    sqlite3_close(
        conn
    );


    return tasks;
}
// ============================================================
// 根据 taskId + pointId 加载任务和点
// ============================================================

bool loadModbusTaskPoint(
    const std::string &db,
    int taskId,
    int pointId,
    ModbusTask &task,
    Point &point,
    std::string &error
)
{
    error.clear();

    task = ModbusTask{};
    point = Point{};


    if (taskId <= 0)
    {
        error = "非法 taskId";
        return false;
    }


    if (pointId <= 0)
    {
        error = "非法 pointId";
        return false;
    }


    sqlite3 *conn = nullptr;


    if (
        sqlite3_open(
            db.c_str(),
            &conn
        )
        !=
        SQLITE_OK
    )
    {
        error =
            conn
            ? sqlite3_errmsg(conn)
            : "无法打开数据库";

        if (conn)
        {
            sqlite3_close(conn);
        }

        return false;
    }


    sqlite3_exec(conn, "ALTER TABLE modbus_tasks ADD COLUMN transport TEXT NOT NULL DEFAULT 'TCP';", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "ALTER TABLE modbus_tasks ADD COLUMN serial_port TEXT NOT NULL DEFAULT '/dev/ttyS0';", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "ALTER TABLE modbus_tasks ADD COLUMN baud_rate INTEGER NOT NULL DEFAULT 9600;", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "ALTER TABLE modbus_tasks ADD COLUMN parity TEXT NOT NULL DEFAULT 'N';", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "ALTER TABLE modbus_tasks ADD COLUMN data_bits INTEGER NOT NULL DEFAULT 8;", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "ALTER TABLE modbus_tasks ADD COLUMN stop_bits INTEGER NOT NULL DEFAULT 1;", nullptr, nullptr, nullptr);


    // ========================================================
    // 1. 查任务
    // ========================================================

    const char *taskSql =
        "SELECT "
        "id, "
        "name, "
        "host, "
        "port, "
        "slave_id, "
        "period, "
        "timeout, "
        "retry_count, "
        "reconnect_interval, "
        "enable, transport, serial_port, baud_rate, parity, data_bits, stop_bits "
        "FROM modbus_tasks "
        "WHERE id = ?;";


    sqlite3_stmt *taskStmt = nullptr;


    if (
        sqlite3_prepare_v2(
            conn,
            taskSql,
            -1,
            &taskStmt,
            nullptr
        )
        !=
        SQLITE_OK
    )
    {
        error = sqlite3_errmsg(conn);

        sqlite3_close(conn);

        return false;
    }


    sqlite3_bind_int(
        taskStmt,
        1,
        taskId
    );


    if (
        sqlite3_step(taskStmt)
        !=
        SQLITE_ROW
    )
    {
        sqlite3_finalize(taskStmt);
        sqlite3_close(conn);

        error = "指定的 Modbus 任务不存在";

        return false;
    }


    task.id =
        sqlite3_column_int(
            taskStmt,
            0
        );

    task.name =
        columnText(
            taskStmt,
            1
        );

    task.host =
        columnText(
            taskStmt,
            2
        );

    task.port =
        sqlite3_column_int(
            taskStmt,
            3
        );

    task.slaveId =
        sqlite3_column_int(
            taskStmt,
            4
        );

    task.period =
        sqlite3_column_int(
            taskStmt,
            5
        );

    task.timeout =
        sqlite3_column_int(
            taskStmt,
            6
        );

    task.retryCount =
        sqlite3_column_int(
            taskStmt,
            7
        );

    task.reconnectInterval =
        sqlite3_column_int(
            taskStmt,
            8
        );

    task.enable =
        sqlite3_column_int(
            taskStmt,
            9
        )
        !=
        0;

    task.transport = columnText(taskStmt, 10);
    task.serialPort = columnText(taskStmt, 11);
    task.baudRate = sqlite3_column_int(taskStmt, 12);
    const std::string taskParity = columnText(taskStmt, 13);
    task.parity = taskParity.empty() ? 'N' : taskParity.front();
    task.dataBits = sqlite3_column_int(taskStmt, 14);
    task.stopBits = sqlite3_column_int(taskStmt, 15);


    sqlite3_finalize(
        taskStmt
    );


    if (!task.enable)
    {
        sqlite3_close(conn);

        error = "指定的 Modbus 任务已停用";

        return false;
    }


    // ========================================================
    // 2. 查点
    //
    // 这里必须同时限制：
    //
    // id = pointId
    // task_id = taskId
    //
    // 防止客户端把错误 taskId 和 pointId 拼在一起。
    // ========================================================

    const char *pointSql =
        "SELECT "
        "id, "
        "task_id, "
        "name, "
        "function, "
        "address, "
        "datatype, "
        "byte_order, "
        "enable, "
        "direction "
        "FROM modbus_points "
        "WHERE id = ? "
        "AND task_id = ?;";


    sqlite3_stmt *pointStmt = nullptr;


    if (
        sqlite3_prepare_v2(
            conn,
            pointSql,
            -1,
            &pointStmt,
            nullptr
        )
        !=
        SQLITE_OK
    )
    {
        error = sqlite3_errmsg(conn);

        sqlite3_close(conn);

        return false;
    }


    sqlite3_bind_int(
        pointStmt,
        1,
        pointId
    );

    sqlite3_bind_int(
        pointStmt,
        2,
        taskId
    );


    if (
        sqlite3_step(pointStmt)
        !=
        SQLITE_ROW
    )
    {
        sqlite3_finalize(pointStmt);
        sqlite3_close(conn);

        error =
            "指定点不存在，或该点不属于指定 Modbus 任务";

        return false;
    }


    point.id =
        sqlite3_column_int(
            pointStmt,
            0
        );

    point.taskId =
        sqlite3_column_int(
            pointStmt,
            1
        );

    point.name =
        columnText(
            pointStmt,
            2
        );

    point.function =
        sqlite3_column_int(
            pointStmt,
            3
        );

    point.address =
        sqlite3_column_int(
            pointStmt,
            4
        );

    point.datatype =
        columnText(
            pointStmt,
            5
        );

    point.byteOrder =
        columnText(
            pointStmt,
            6
        );

    point.enable =
        sqlite3_column_int(
            pointStmt,
            7
        )
        !=
        0;

    point.direction = columnText(pointStmt, 8);
    if (point.direction != "OUTPUT")
        point.direction = "INPUT";


    // 兼容旧代码。
    //
    // 真正运行参数来自 task，
    // 但暂时同步到 Point，避免其他旧逻辑受到影响。

    point.slave =
        task.slaveId;

    point.period =
        task.period;


    sqlite3_finalize(
        pointStmt
    );

    sqlite3_close(
        conn
    );


    if (!point.enable)
    {
        error = "指定 Modbus 点已停用";
        return false;
    }


    return true;
}
