#include "time_utils.h"

#include <ctime>
#include <sstream>
#include <iomanip>


std::string getTime()
{


time_t now=time(nullptr);


tm *t=
localtime(&now);


std::stringstream ss;


ss
<<1900+t->tm_year
<<"-"
<<std::setw(2)
<<std::setfill('0')
<<1+t->tm_mon
<<"-"
<<std::setw(2)
<<t->tm_mday
<<" "
<<std::setw(2)
<<t->tm_hour
<<":"
<<std::setw(2)
<<t->tm_min
<<":"
<<std::setw(2)
<<t->tm_sec;


return ss.str();


}