
os:use`qtk.os;

// @kind data
// @subcategory db
// @overview A flag to trigger auto reload after a structural change made by a QTK function to the database. It's set by default.
autoReload:1b;

// @kind data
// @subcategory db
// @overview sym data in memory.
.z.m.sym:`;

// @kind function
// @subcategory db
// @overview Load sym file in a database directory while keeping a backup of the original one in memory.
// @param dbDir {hsym} A database directory.
// @param sym {symbol} Name of sym file.
// @return {symbol} The name of sym file if it's loaded successfully; null symbol otherwise, e.g. if the sym file doesn't exist.
loadSym:{[dbDir;sym]
  symFile:.Q.dd[dbDir;sym];
  if[not os.path.isFile symFile; :`];
  if[sym in system enlist"v"; .z.m[sym]:get sym];
  load .Q.dd[dbDir;sym];
  sym
 };

// @kind function
// @subcategory db
// @overview Recover in-memory backup sym data.
// @param sym {symbol} Name of sym data.
// @return {symbol} The name of sym file if it's recovered successfully; null symbol otherwise, e.g. if there is no backup of such name.
recoverSym:{[sym]
  oldSym:.z.m[sym];
  if[11h<>type oldSym; :`];
  sym set oldSym;
  delete sym from .z.M;
  sym
 };

// @kind function
// @subcategory db
// @overview Load database in a given directory.
// @param dir {string | hsym} Directory.
// @see reload
.z.m.load:{[dir]
  dirStr:$[10h=type dir; dir; 1_string dir];
  system "l ",dirStr;
 };

// @kind function
// @subcategory db
// @overview Reload current database.
// @see load
reload:{
  .z.m.load enlist".";
 };

// @kind function
// @private
// @overview Enumerate a value against sym.
// @param val {any} A value.
// @return {enum} Enumerated value against sym file in the current directory if the value is a symbol or a symbol vector;
//   otherwise the same value as-is.
p_enumerate:{[val]
  p_enumerateAgainst[`:.; `sym; val]
 };

// @kind function
// @private
// @overview Enumerate a value against a domain.
// @param dir {hsym} Handle to a directory.
// @param val {any} A value.
// @param domain {symbol} Name of domain.
// @return {enum} Enumerated value against the domain in the directory if the value is a symbol or a symbol vector;
//   otherwise the same value as-is.
p_enumerateAgainst:{[dir;domain;val]
  if[11<>abs type val; :val];
  .Q.dd[dir; domain]?val
 };

// @kind function
// @private
// @overview Get all columns of an on-disk table.
// @param tablePath {hsym} Path to a splayed/partitioned table.
// @return {symbol[]} Columns of the table.
p_getColumns:{[tablePath]
  get .Q.dd[tablePath; `.d]
 };

// @kind function
// @private
// @overview Save a table of data to an on-disk table.
// @param tablePath {hsym} Path to an on-disk table.
// @param tableData {table} A table of data.
// @return {hsym} The path to the table.
p_saveTable:{[tablePath;tableData]
  columns:cols tableData;
  @[tablePath; columns; ,; tableData columns];
  if[not p_dotDExists tablePath; @[tablePath; `.d; :; columns]];
  tablePath
 };

// @kind function
// @private
// @overview Get row count of an on-disk table. Count of the first column is used.
// @param tablePath {hsym} Path to an on-disk table.
// @return {long} Row count of the table.
p_rowCount:{[tablePath]
  allColumns:p_getColumns tablePath;
  count get .Q.dd[tablePath; first allColumns]
 };

// @kind function
// @private
// @overview Check if `.d` file exists in a path of a splayed/partitioned table.
// @param tablePath {hsym} Path to an on-disk table..
// @return {boolean} `1b` if `.d` exists; `0b` otherwise.
p_dotDExists:{[tablePath]
  filesInPartition:os.listDir tablePath;
  `.d in filesInPartition
 };

// @kind function
// @private
// @overview Get default value based on a path to a partitioned table and a column. The default value is type-specific
// null if it's a simple column, an empty typed list if it's a compound column, or an empty general list.
// @param tablePath {symbol} A file symbol to a partitioned table.
// @param column {symbol} A column name of the table.
// @return {any} Default value of the column.
p_defaultValue:{[tablePath;column]
  columnValue:tablePath column;
  columnType:.Q.ty columnValue;
  $[columnType in .Q.a; first 0#columnValue;
    columnType in .Q.A; lower[columnType]$();
    ()
   ]
 };

export:([autoReload; loadSym; recoverSym; .z.m.load; reload; p_enumerate; p_enumerateAgainst; p_getColumns; p_saveTable; p_rowCount; p_dotDExists; p_defaultValue]);