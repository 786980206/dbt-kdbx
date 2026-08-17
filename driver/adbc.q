akdb:use`kx.arrowkdb;

trade:([]time:2024.01.01D09:30:00.000000000+til 1000; sym:1000?`msft`ibm`goog`aapl; price:100.5+1000?1.0; size:100+1000?900);

// @api: AdbcStatementExecuteQuery
AdbcStatementExecuteQuery:{[sql]
    qresult:value sql;
    $[98=type qresult;
        akdb.ipc.serializeArrowFromTable[qresult;::];
        '"only table results supported"
    ]};

// @api: AdbcStatementExecuteSchema
AdbcStatementExecuteSchema:{[sql]
    qresult:value sql;
    $[98=type qresult;
        akdb.ipc.serializeArrowFromTable[0!qresult;::];
        '"only table results supported"
    ]};

// @api: AdbcStatementExecuteUpdate
AdbcStatementExecuteUpdate:{[sql]
    // MVP: execute q expression, return 0 rows affected
    value sql;
    :0j};

// @api: AdbcStatementBulkIngest
AdbcStatementBulkIngest:{[table;mode;ipc]
    data:akdb.ipc.parseArrowToTable[ipc;::];
    $[mode~`create;
        upsert;][table;data];
    :count data};

// @api: AdbcConnectionGetInfo
AdbcConnectionGetInfo:{[infoCodes]
    info:()!();
    info[0]:"kdb-x";
    info[1]:"5.0.0";
    info[2]:"9.0.0";
    info[3]:1b;
    info[4]:0b;
    info[100]:"adbc_driver_kdbx";
    info[101]:"0.1.0";
    info[102]:"9.0.0";
    info[103]:0000001;
    :infoCodes!info[infoCodes]};

// @api: AdbcConnectionGetObjects
AdbcConnectionGetObjects:{[depth;catalog;schema;table;column]
    mt:meta trade;
    cols_t:cols trade;
    n:count cols_t;
    tc:([] column_name:string cols_t; ordinal_position:1+til n; remarks:n#enlist ""; xdbc_type_name:string value each value mt);
    table_dict:`table_name`table_type`table_columns`table_constraints!(`trade;`TABLE;tc;enlist ());
    schema_dict:`db_schema_tables`db_schema_name!(enlist table_dict;`main);
    catalogs:`catalog_name`catalog_db_schemas!(`kdb;enlist schema_dict);
    :catalogs};

// @api: AdbcConnectionGetTableSchema
AdbcConnectionGetTableSchema:{[table]
    tbl:value table;
    akdb.ipc.serializeArrowFromTable[0!tbl;::]};

// @api: AdbcConnectionGetTableTypes
AdbcConnectionGetTableTypes:{enlist`TABLE};

show "MARKER server ready";
