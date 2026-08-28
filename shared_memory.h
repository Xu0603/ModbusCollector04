#pragma once

#include <string>
#include <pthread.h>


#define MAX_VARIABLES 20


struct Variable
{
    char name[32];

    int slave;

    int function;

    int address;

    char datatype[16];

    double value;

    char timestamp[32];

    char telegram[256];

    int status;
};


struct SharedData
{
    pthread_mutex_t mutex;

    int count;

    Variable vars[MAX_VARIABLES];
};


void initSharedMemory();


void writeSharedMemory(
    std::string name,
    int slave,
    int function,
    int address,
    std::string datatype,
    double value,
    std::string timestamp,
    std::string telegram,
    int status
);


SharedData* getSharedMemory();