#include "manager_server.h"

#include <iostream>
#include <string>
#include <csignal>
#include <cstdlib>
#include <cstdint>


namespace
{

ManagerServer* g_server = nullptr;


// ============================================================
// Ctrl+C / SIGTERM
// ============================================================

void signalHandler(
    int signal
)
{
    std::cout
        << "\n收到信号 "
        << signal
        << "，正在停止 system-managerd..."
        << std::endl;


    if (g_server)
    {
        g_server->stop();
    }
}

}


// ============================================================
// 帮助
// ============================================================

void printUsage(
    const char* program
)
{
    std::cout
        << "用法：\n"
        << "  "
        << program
        << " [选项]\n\n"

        << "选项：\n"

        << "  --host <ip>\n"
        << "      TCP监听地址，默认 127.0.0.1\n\n"

        << "  --port <port>\n"
        << "      TCP监听端口，默认 19090\n\n"

        << "  --config-hash <hash>\n"
        << "      当前工程配置Hash，可选\n\n"

        << "  --help\n"
        << "      显示帮助\n"
        << std::endl;
}


// ============================================================
// main
// ============================================================

int main(
    int argc,
    char* argv[]
)
{
    std::string host =
        "127.0.0.1";


    uint16_t port =
        19090;


    std::string configHash;


    // ========================================================
    // 参数解析
    // ========================================================

    for (
        int i = 1;
        i < argc;
        ++i
    )
    {
        const std::string argument =
            argv[i];


        if (
            argument == "--help" ||
            argument == "-h"
        )
        {
            printUsage(
                argv[0]
            );

            return 0;
        }


        if (
            argument == "--host"
        )
        {
            if (i + 1 >= argc)
            {
                std::cerr
                    << "--host 缺少参数"
                    << std::endl;

                return 1;
            }

            host =
                argv[++i];

            continue;
        }


        if (
            argument == "--port"
        )
        {
            if (i + 1 >= argc)
            {
                std::cerr
                    << "--port 缺少参数"
                    << std::endl;

                return 1;
            }


            try
            {
                const int value =
                    std::stoi(
                        argv[++i]
                    );


                if (
                    value <= 0 ||
                    value > 65535
                )
                {
                    throw std::exception();
                }


                port =
                    static_cast<uint16_t>(
                        value
                    );
            }
            catch (...)
            {
                std::cerr
                    << "无效端口"
                    << std::endl;

                return 1;
            }

            continue;
        }


        if (
            argument == "--config-hash"
        )
        {
            if (i + 1 >= argc)
            {
                std::cerr
                    << "--config-hash 缺少参数"
                    << std::endl;

                return 1;
            }


            configHash =
                argv[++i];

            continue;
        }


        std::cerr
            << "未知参数: "
            << argument
            << std::endl;


        printUsage(
            argv[0]
        );


        return 1;
    }


    // ========================================================
    // 注册信号
    // ========================================================

    std::signal(
        SIGINT,
        signalHandler
    );

    std::signal(
        SIGTERM,
        signalHandler
    );


    // ========================================================
    // 创建 Server
    // ========================================================

    ManagerServer server(
        host,
        port,
        configHash
    );


    g_server =
        &server;


    // ========================================================
    // 启动
    // ========================================================

    if (!server.start())
    {
        std::cerr
            << "system-managerd 启动失败"
            << std::endl;

        g_server = nullptr;

        return 1;
    }


    // ========================================================
    // 进入服务循环
    // ========================================================

    server.run();


    g_server = nullptr;


    std::cout
        << "system-managerd 已停止"
        << std::endl;


    return 0;
}