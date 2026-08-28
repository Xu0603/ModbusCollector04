#include "simread_config.h"

#include <fstream>
#include <iostream>


bool loadSimreadConfig(
    const std::string& filename,
    SimreadConfig& config
)
{
    std::ifstream file(filename);


    if(!file.is_open())
    {
        std::cerr
            << "无法打开simread配置文件:"
            << filename
            << std::endl;

        return false;
    }


    std::string line;


    while(std::getline(file, line))
    {
        // 空行
        if(line.empty())
            continue;


        // 注释
        if(line[0] == '#')
            continue;


        std::size_t pos =
            line.find('=');


        if(pos == std::string::npos)
            continue;


        std::string key =
            line.substr(
                0,
                pos
            );


        std::string value =
            line.substr(
                pos + 1
            );


        if(key == "name")
        {
            config.name = value;
        }


        else if(key == "datatype")
        {
            config.datatype = value;
        }


        else if(key == "byteorder")
        {
            config.byteOrder = value;
        }
    }


    file.close();


    // ========================================================
    // 检查配置
    // ========================================================

    if(config.datatype.empty())
    {
        std::cerr
            << "simread配置缺少datatype"
            << std::endl;

        return false;
    }


    if(config.byteOrder.empty())
    {
        config.byteOrder = "ABCD";
    }


    return true;
}