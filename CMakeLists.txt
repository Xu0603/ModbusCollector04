cmake_minimum_required(VERSION 3.10)

project(ModbusCollector)

set(CMAKE_CXX_STANDARD 17)


find_package(SQLite3 REQUIRED)

find_library(MODBUS_LIB modbus)


# ============================================================
# ModbusCollector
# ============================================================

add_executable(
    ModbusCollector

    main.cpp
    database.cpp
    modbus_client.cpp
    shared_memory.cpp
    system_manager.cpp
    time_utils.cpp
    modbus_server.cpp
)


target_link_libraries(
    ModbusCollector

    ${SQLite3_LIBRARIES}
    ${MODBUS_LIB}
    pthread
)


# ============================================================
# shm_reader
# ============================================================

add_executable(
    shm_reader

    shm_reader.cpp
    shared_memory.cpp
    system_manager.cpp
    simread_config.cpp
)


target_link_libraries(
    shm_reader

    pthread
)


# ============================================================
# test_system_manager
# ============================================================

add_executable(
    test_system_manager

    test_system_manager.cpp
    system_manager.cpp
)


# ============================================================
# system-managerd
# ============================================================

add_executable(
    system-managerd

    manager_main.cpp
    manager_server.cpp
    shared_memory.cpp
)


target_link_libraries(
    system-managerd

    pthread
    rt
)