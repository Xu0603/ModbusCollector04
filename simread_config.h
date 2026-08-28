#pragma once

#include <string>


struct SimreadConfig
{
    // 变量名称
    std::string name;

    // 显示的数据类型
    std::string datatype;

    // 字节序
    std::string byteOrder;
};


// 读取配置文件
bool loadSimreadConfig(
    const std::string& filename,
    SimreadConfig& config
);