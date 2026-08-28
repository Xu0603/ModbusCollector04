#include <iostream>

#include <thread>

#include <chrono>

#include <modbus/modbus.h>


#include "database.h"

#include "modbus_client.h"
#include "shared_memory.h"


modbus_t *ctx;



int main()
{
initSharedMemory();

ctx =
modbus_new_tcp(
"127.0.0.1",
502
);


if(ctx==NULL)
{
    std::cout<<"创建失败\n";
    return -1;
}


modbus_set_slave(ctx,1);


if(
modbus_connect(ctx)<0
)
{
    std::cout<<"连接失败\n";
    return -1;
}


modbus_set_debug(ctx,TRUE);


std::cout
<<"Modbus连接成功\n";

initSharedMemory();

auto points=
loadPoints(
"/home/xu/桌面/ModbusCollector/modbus_config.db"
);


while(true)
{


std::cout
<<"读取到变量数量:"
<<points.size()
<<std::endl;


for(auto p:points)
{


modbus_set_slave(
ctx,
p.slave
);



auto start=
std::chrono::steady_clock::now();


readModbus(p);



auto end=
std::chrono::steady_clock::now();



int cost=
std::chrono::duration_cast<
std::chrono::milliseconds>
(end-start)
.count();



if(cost<p.period)

std::this_thread::sleep_for(

std::chrono::milliseconds(
p.period-cost)

);



}



}


modbus_close(ctx);

modbus_free(ctx);


return 0;

}
