#include "shared_memory.h"

#include <iostream>
#include <unistd.h>
#include <pthread.h>


int main()
{
    SharedData *ptr =
        getSharedMemory();


    if(ptr == nullptr)
    {
        std::cout
            << "共享内存打开失败\n";

        return -1;
    }


    while(true)
    {
        // ====================================================
        // 加锁
        // ====================================================

        pthread_mutex_lock(
            &ptr->mutex
        );


        std::cout
            << "\n====================\n";


        for(int i = 0; i < ptr->count; i++)
        {
            std::cout << "任务ID:" << ptr->vars[i].taskId << std::endl;
            std::cout << "点ID:" << ptr->vars[i].pointId << std::endl;

            std::cout
                << "变量名:"
                << ptr->vars[i].name
                << std::endl;


            std::cout
                << "站号:"
                << ptr->vars[i].slave
                << std::endl;


            std::cout
                << "功能码:"
                << ptr->vars[i].function
                << std::endl;


            std::cout
                << "地址:"
                << ptr->vars[i].address
                << std::endl;


            std::cout
                << "数据类型:"
                << ptr->vars[i].datatype
                << std::endl;

            std::cout
                << "大小端:"
                << ptr->vars[i].byteOrder
                << std::endl;

            std::cout
                << "读写方向:"
                << ptr->vars[i].direction
                << std::endl;


            std::cout
                << "数据值:"
                << ptr->vars[i].value
                << std::endl;


            std::cout
                << "时间:"
                << ptr->vars[i].timestamp
                << std::endl;


            std::cout
                << "报文:"
                << ptr->vars[i].telegram
                << std::endl;


            std::cout
                << "状态:"
                << ptr->vars[i].status
                << std::endl;


            std::cout
                << "------------------"
                << std::endl;
        }


        // ====================================================
        // 解锁
        // ====================================================

        pthread_mutex_unlock(
            &ptr->mutex
        );


        sleep(1);
    }


    return 0;
}