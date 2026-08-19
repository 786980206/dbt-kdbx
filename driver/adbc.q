akdb:use`kx.arrowkdb;

trade:([]time:2024.01.01D09:30:00.000000000+til 1000; sym:1000?`msft`ibm`goog`aapl; price:100.5+1000?1.0; size:100+1000?900);

// @api: AdbcStatementExecuteQuery
// Execute a q expression and serialize the resulting table to Arrow.
// Accept plain (98) and keyed (99) tables; keyed -> unkey before Arrow
// serialization. A scalar result (e.g. `1` from the connection probe) is
// wrapped into a single-row table so the query path always yields a stream.
AdbcStatementExecuteQuery:{[sql]
    r:value sql;
    $[99=type r; akdb.ipc.serializeArrowFromTable[0!r;::];
      98=type r; akdb.ipc.serializeArrowFromTable[r;::];
      akdb.ipc.serializeArrowFromTable[([] result:enlist r);::]
    ]};

// @api: AdbcStatementExecuteSchema
AdbcStatementExecuteSchema:{[sql]
    qresult:value sql;
    $[99=type qresult; akdb.ipc.serializeArrowFromTable[0!qresult;::];
      98=type qresult; akdb.ipc.serializeArrowFromTable[qresult;::];
      '"only table results supported"
    ]};

// @api: AdbcStatementExecuteUpdate
// Execute a DML/DDL expression and report rows affected. q update/delete
// are functional (no mutation); dbt materialisations use upsert, so the
// server-side should execute a persisting expression. For now: value the
// sql; if it yields a scalar long, return it, else return 0.
AdbcStatementExecuteUpdate:{[sql]
    result:value sql;
    $[-7=type result; result; -6=type result; result; 0j]};

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
    cols_t:cols trade;
    n:count cols_t;
    // column_name as a symbol list: pykx IPC / C parsing turns q strings into
    // char lists, which the C GetObjects parser does not handle for string
    // columns. symbols round-trip cleanly and map to Arrow string.
    // xdbc_type_name: q type codes ("p" timestamp, "s" symbol, "f" float,
    // "j" long, ...). `string exec t from meta ...` yields one char list per
    // column, which round-trips to the C parser cleanly.
    tc:([] column_name:cols_t; ordinal_position:1+til n; remarks:n#enlist ""; xdbc_type_name:string exec t from meta trade);
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