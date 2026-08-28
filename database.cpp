#include "database.h"

#include <sqlite3.h>
#include <iostream>



std::vector<Point>
loadPoints(std::string db)
{

std::vector<Point> result;


sqlite3 *conn;


sqlite3_open(
db.c_str(),
&conn
);



std::string sql=
"select name,slave_id,function,address,datatype,byte_order,period "
"from modbus_points where enable=1;";



sqlite3_stmt *stmt;


sqlite3_prepare_v2(
conn,
sql.c_str(),
-1,
&stmt,
NULL
);



while(
sqlite3_step(stmt)
==SQLITE_ROW)
{

Point p;


p.name=
(char*)sqlite3_column_text(stmt,0);


p.slave=
sqlite3_column_int(stmt,1);


p.function=
sqlite3_column_int(stmt,2);


p.address=
sqlite3_column_int(stmt,3);


p.datatype=
(char*)sqlite3_column_text(stmt,4);


p.byteOrder=
(char*)sqlite3_column_text(stmt,5);


p.period=
sqlite3_column_int(stmt,6);
result.push_back(p);

}


sqlite3_finalize(stmt);

sqlite3_close(conn);


return result;

}
