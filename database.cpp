#include "database.h"

#include <sqlite3.h>

#include <iostream>

namespace {

std::string columnText(sqlite3_stmt *stmt, int column)
{
    const auto *text = sqlite3_column_text(stmt, column);
    return text ? reinterpret_cast<const char *>(text) : std::string();
}

void printSqliteError(const char *prefix, sqlite3 *db)
{
    std::cerr << prefix << ": " << (db ? sqlite3_errmsg(db) : "unknown sqlite error")
              << std::endl;
}

bool openDatabase(const std::string &path, sqlite3 **db)
{
    if (sqlite3_open(path.c_str(), db) == SQLITE_OK)
        return true;
    printSqliteError("打开数据库失败", *db);
    if (*db)
        sqlite3_close(*db);
    *db = nullptr;
    return false;
}

void ensureRtuColumns(sqlite3 *db)
{
    // ALTER TABLE 没有 IF NOT EXISTS；重复列错误可忽略。这样旧 TCP 数据库无需单独升级。
    static const char *migrations[] = {
        "ALTER TABLE modbus_tasks ADD COLUMN transport TEXT NOT NULL DEFAULT 'TCP';",
        "ALTER TABLE modbus_tasks ADD COLUMN serial_port TEXT NOT NULL DEFAULT '/dev/ttyS0';",
        "ALTER TABLE modbus_tasks ADD COLUMN baud_rate INTEGER NOT NULL DEFAULT 9600;",
        "ALTER TABLE modbus_tasks ADD COLUMN parity TEXT NOT NULL DEFAULT 'N';",
        "ALTER TABLE modbus_tasks ADD COLUMN data_bits INTEGER NOT NULL DEFAULT 8;",
        "ALTER TABLE modbus_tasks ADD COLUMN stop_bits INTEGER NOT NULL DEFAULT 1;"
    };
    for (const char *sql : migrations)
        sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}

std::string normalizedDirection(sqlite3_stmt *stmt, int column)
{
    return columnText(stmt, column) == "OUTPUT" ? "OUTPUT" : "INPUT";
}

ModbusTask readTask(sqlite3_stmt *stmt)
{
    ModbusTask task;
    task.id = sqlite3_column_int(stmt, 0);
    task.name = columnText(stmt, 1);
    task.host = columnText(stmt, 2);
    task.port = sqlite3_column_int(stmt, 3);
    task.slaveId = sqlite3_column_int(stmt, 4);
    task.period = sqlite3_column_int(stmt, 5);
    task.timeout = sqlite3_column_int(stmt, 6);
    task.retryCount = sqlite3_column_int(stmt, 7);
    task.reconnectInterval = sqlite3_column_int(stmt, 8);
    task.enable = sqlite3_column_int(stmt, 9) != 0;
    task.transport = columnText(stmt, 10);
    task.serialPort = columnText(stmt, 11);
    task.baudRate = sqlite3_column_int(stmt, 12);
    const std::string parity = columnText(stmt, 13);
    task.parity = parity.empty() ? 'N' : parity.front();
    task.dataBits = sqlite3_column_int(stmt, 14);
    task.stopBits = sqlite3_column_int(stmt, 15);
    return task;
}

Point readLegacyPoint(sqlite3_stmt *stmt)
{
    Point point;
    point.id = sqlite3_column_int(stmt, 0);
    point.taskId = sqlite3_column_int(stmt, 1);
    point.name = columnText(stmt, 2);
    point.slave = sqlite3_column_int(stmt, 3);
    point.function = sqlite3_column_int(stmt, 4);
    point.address = sqlite3_column_int(stmt, 5);
    point.datatype = columnText(stmt, 6);
    point.byteOrder = columnText(stmt, 7);
    point.period = sqlite3_column_int(stmt, 8);
    point.enable = sqlite3_column_int(stmt, 9) != 0;
    point.direction = normalizedDirection(stmt, 10);
    return point;
}

constexpr const char *kTaskColumns =
    "id,name,host,port,slave_id,period,timeout,retry_count,reconnect_interval,enable,"
    "transport,serial_port,baud_rate,parity,data_bits,stop_bits";

} // namespace

std::vector<Point> loadPoints(std::string dbPath)
{
    std::vector<Point> points;
    sqlite3 *db = nullptr;
    if (!openDatabase(dbPath, &db))
        return points;

    const char *sql =
        "SELECT id,task_id,name,slave_id,function,address,datatype,byte_order,period,enable,"
        "direction FROM modbus_points WHERE enable=1 ORDER BY id;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        printSqliteError("读取 modbus_points 准备 SQL 失败", db);
        sqlite3_close(db);
        return points;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW)
        points.push_back(readLegacyPoint(stmt));

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return points;
}

std::vector<ModbusTask> loadModbusTasks(const std::string &dbPath)
{
    std::vector<ModbusTask> tasks;
    sqlite3 *db = nullptr;
    if (!openDatabase(dbPath, &db))
        return tasks;
    ensureRtuColumns(db);

    const std::string taskSql = std::string("SELECT ") + kTaskColumns
        + " FROM modbus_tasks WHERE enable=1 ORDER BY id;";
    sqlite3_stmt *taskStmt = nullptr;
    if (sqlite3_prepare_v2(db, taskSql.c_str(), -1, &taskStmt, nullptr) != SQLITE_OK) {
        printSqliteError("读取 modbus_tasks 准备 SQL 失败", db);
        sqlite3_close(db);
        return tasks;
    }
    while (sqlite3_step(taskStmt) == SQLITE_ROW)
        tasks.push_back(readTask(taskStmt));
    sqlite3_finalize(taskStmt);

    const char *pointSql =
        "SELECT id,task_id,name,slave_id,function,address,datatype,byte_order,period,enable,"
        "direction FROM modbus_points WHERE task_id=? AND enable=1 ORDER BY id;";
    sqlite3_stmt *pointStmt = nullptr;
    if (sqlite3_prepare_v2(db, pointSql, -1, &pointStmt, nullptr) != SQLITE_OK) {
        printSqliteError("读取任务点表准备 SQL 失败", db);
        sqlite3_close(db);
        return {};
    }

    for (auto &task : tasks) {
        sqlite3_reset(pointStmt);
        sqlite3_clear_bindings(pointStmt);
        sqlite3_bind_int(pointStmt, 1, task.id);
        while (sqlite3_step(pointStmt) == SQLITE_ROW) {
            Point point = readLegacyPoint(pointStmt);
            // slave 和周期属于连接任务。覆盖点表旧字段可兼容旧库，又避免运行参数出现两套来源。
            point.slave = task.slaveId;
            point.period = task.period;
            task.points.push_back(std::move(point));
        }
    }

    sqlite3_finalize(pointStmt);
    sqlite3_close(db);
    return tasks;
}

bool loadModbusTaskPoint(const std::string &dbPath, int taskId, int pointId,
                         ModbusTask &task, Point &point, std::string &error)
{
    error.clear();
    task = {};
    point = {};
    if (taskId <= 0) {
        error = "非法 taskId";
        return false;
    }
    if (pointId <= 0) {
        error = "非法 pointId";
        return false;
    }

    sqlite3 *db = nullptr;
    if (!openDatabase(dbPath, &db)) {
        error = "无法打开数据库";
        return false;
    }
    ensureRtuColumns(db);

    const std::string taskSql = std::string("SELECT ") + kTaskColumns
        + " FROM modbus_tasks WHERE id=?;";
    sqlite3_stmt *taskStmt = nullptr;
    if (sqlite3_prepare_v2(db, taskSql.c_str(), -1, &taskStmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        sqlite3_close(db);
        return false;
    }
    sqlite3_bind_int(taskStmt, 1, taskId);
    if (sqlite3_step(taskStmt) != SQLITE_ROW) {
        sqlite3_finalize(taskStmt);
        sqlite3_close(db);
        error = "指定的 Modbus 任务不存在";
        return false;
    }
    task = readTask(taskStmt);
    sqlite3_finalize(taskStmt);
    if (!task.enable) {
        sqlite3_close(db);
        error = "指定的 Modbus 任务已停用";
        return false;
    }

    // 两个条件缺一不可，否则调用方可能用其他任务的串口参数写入当前点。
    const char *pointSql =
        "SELECT id,task_id,name,function,address,datatype,byte_order,enable,direction "
        "FROM modbus_points WHERE id=? AND task_id=?;";
    sqlite3_stmt *pointStmt = nullptr;
    if (sqlite3_prepare_v2(db, pointSql, -1, &pointStmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        sqlite3_close(db);
        return false;
    }
    sqlite3_bind_int(pointStmt, 1, pointId);
    sqlite3_bind_int(pointStmt, 2, taskId);
    if (sqlite3_step(pointStmt) != SQLITE_ROW) {
        sqlite3_finalize(pointStmt);
        sqlite3_close(db);
        error = "指定点不存在，或该点不属于指定 Modbus 任务";
        return false;
    }

    point.id = sqlite3_column_int(pointStmt, 0);
    point.taskId = sqlite3_column_int(pointStmt, 1);
    point.name = columnText(pointStmt, 2);
    point.function = sqlite3_column_int(pointStmt, 3);
    point.address = sqlite3_column_int(pointStmt, 4);
    point.datatype = columnText(pointStmt, 5);
    point.byteOrder = columnText(pointStmt, 6);
    point.enable = sqlite3_column_int(pointStmt, 7) != 0;
    point.direction = normalizedDirection(pointStmt, 8);
    point.slave = task.slaveId;
    point.period = task.period;

    sqlite3_finalize(pointStmt);
    sqlite3_close(db);
    if (!point.enable) {
        error = "指定 Modbus 点已停用";
        return false;
    }
    return true;
}
