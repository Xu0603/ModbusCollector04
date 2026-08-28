#pragma once

#include <string>
#include <vector>


struct Point
{

std::string name;

int slave;

int function;

int address;

std::string datatype;

std::string byteOrder;

int period;

};


std::vector<Point>
loadPoints(std::string db);