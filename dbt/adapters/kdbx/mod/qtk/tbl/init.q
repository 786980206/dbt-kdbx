.z.m.type:use`qtk.type;
utils:use`qtk.utils;
pdb:use`qtk.pdb;
db:use`qtk.db;
os:use`qtk.os;
err:use`qtk.err;

// @kind function
// @subcategory tbl
// @overview Get table type, either of `` `Plain`Serialized`Splayed`Partitioned ``. Note that tables in segmented database
// are classified as Partitioned.
//
// - See also [.Q.qp](https://code.kx.com/q/ref/dotq/#qqp-is-partitioned).
// @param t {table | symbol | hsym | (hsym; symbol; symbol)} Table or table reference.
// @return {symbol} Table type.
// @throws {ValueError} If `t` is a symbol vector but not a valid partitioned table ID.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/getType; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// `Partitioned=.qtk.tbl.getType tabRef
getType:{[t]
  v:$[-11h=type t;
      [
        if[":"=first str:string t;
           :$["/"=last str; `Splayed; `Serialized]];
        tvar:@[get; t; ::];
        if[tvar~(::); :`Plain];   // t is undefined, treated as the name of a new plain table
        tvar
        ];
      11h=type t;
      [
       // format: (dbDir; pfield; tableName)
       if[3<>count t; 'err.compose[`ValueError; "expect 3 elements"]];
       if[":"<>first string first t; 'err.compose[`ValueError; "expect hsym as the first element"]];
       if[not t[1] in `int`date`month`year; 'err.compose[`ValueError; "expect a valid partition field"]];
       :`Partitioned
        ];
      t
   ];
  isPartitioned:.Q.qp v;
  $[isPartitioned~1b; `Partitioned;
    isPartitioned~0b; `Splayed;
    `Plain
   ]
 };

// @kind function
// @subcategory tbl
// @overview Get metadata of a table. It's similar to [meta](https://code.kx.com/q/ref/meta/) but supports all table types.
// For partitioned table, the latest partition is used.
// @param t {table | symbol | hsym | (hsym; symbol; symbol)} Table or table reference.
// @return {table} Metadata of the table.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/meta; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// ([c:`date`c1] t:"dj"; f:`; a:`)~.qtk.tbl.meta tabRef
.z.m.meta:{[t]
  if[(tt:type t) in 98 99h; :meta t];

  tabRefDesc:describe t;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;

  $[tableType=`Plain;
    meta t;
    tableType=`Serialized;
    meta get t;
    tableType=`Splayed;
    [
      if[not t like "*/"; :meta t];
      dbDir:tabRefDesc`dbDir;
      db.loadSym[dbDir;`sym];
      tableMeta:meta t;
      db.recoverSym `sym;
      tableMeta
      ];
    // tableType=`Partitioned
    [
      if[-11h=tt; :meta t];
      dbDir:tabRefDesc`dbDir;
      parField:tabRefDesc`parField;
      lastPartition:last pdb.getPartitions dbDir;
      db.loadSym[dbDir;`sym];
      tableMeta:meta .Q.dd[;`] .Q.par[dbDir; lastPartition; tableName];
      tableMeta:([c:enlist parField] t:enlist "dmii" `date`month`year`int?parField; f:`; a:`) upsert tableMeta;
      db.recoverSym `sym;
      tableMeta
      ]
   ]
 };

// @kind function
// @subcategory tbl
// @overview Get foreign keys of a table. It's similar to [fkeys](https://code.kx.com/q/ref/fkeys/) but supports table name besides value.
// For partitioned table, the latest partition is used. Note that this is supported only for the current database.
// @param t {table | symbol} Table or table name.
// @return {dict} A dictionary that maps foreign-key columns to their tables.
foreignKeys:{[t]
  $[type[t] in 98 99h; fkeys t; fkeys get t]
 };

// @kind function
// @subcategory tbl
// @overview Create a new table with given data.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference.
// @param data {table} Table data.
// @return {symbol | hsym | (hsym; symbol; symbol)} The table reference.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/create; `date; `PartitionedTable);
//
// tabRef~.qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)]
create:{[tabRef;data]
  tabRefDesc:describe tabRef;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;

  $[tableType in `Plain`Serialized;
    tabRef set data;
    tableType=`Splayed;
    [
      dbDir:tabRefDesc`dbDir;
      tablePath:.Q.dd[dbDir; tableName];
      p_addTable[tablePath; .Q.en[dbDir;data]];
      if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
      ];
    [
      dbDir:tabRefDesc`dbDir;
      parField:tabRefDesc`parField;
      parValues:distinct ?[data; (); (); parField];
      tablePaths:.Q.par[dbDir; ; tableName] each parValues;
      dataByPartition:flip each value parField xgroup .Q.en[dbDir;data];
      p_addTable'[tablePaths; dataByPartition];
      if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
      ]
   ];
  tabRef
 };

// @kind function
// @private
// @subcategory tbl
// @overview Add an on-disk table.
// @param tablePath {hsym} Path to an on-disk table.
// @param data {table} Table data. Symbol columns must be enumerated and the table is not keyed.
// @return {hsym} The path to the table.
p_addTable:{[tablePath;data]
  @[tablePath; `; :; data];
  tablePath
 };

// @kind function
// @private
// @subcategory tbl
// @overview Return a table with a single row that matches a given table schema.
// @param tabMeta {table} Metadata of a table.
// @return {table} A table with a single row that matches the metadata.
p_singleton:{[tabMeta]
  tabMeta:0!tabMeta;
  v:enlist each .z.m.type.defaults raze string tabMeta`t;
  flip (tabMeta`c)!v
  };

// @kind function
// @private
// @subcategory tbl
// @overview Return an empty table that matches a given table schema.
// @param tabMeta {table} Metadata of a table.
// @return {table} An empty table that matches the metadata.
p_empty:{[tabMeta]
  0#p_singleton tabMeta
 };

// @kind function
// @subcategory tbl
// @overview Drop a table.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference.
// @return {symbol | hsym | (hsym; symbol; symbol)} The table reference.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/drop; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// tabRef~.qtk.tbl.drop tabRef
drop:{[tabRef]
  tabRefDesc:describe tabRef;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;

  $[tableType=`Plain;
    ![`.; (); 0b; enlist tableName];
    tableType=`Serialized;
    os.remove tabRef;
    tableType=`Splayed;
    [
      dbDir:tabRefDesc`dbDir;
      if[os.path.samefile[dbDir; `:.]; ![`.; (); 0b; enlist tableName]];
      tablePath:.Q.dd[dbDir; tableName];
      os.rmtree tablePath;
      ];
    // tableType=`Partitioned
    [
      dbDir:tabRefDesc`dbDir;
      if[os.path.samefile[dbDir; `:.]; ![`.; (); 0b; enlist tableName]];
      tablePaths:.Q.par[dbDir; ; tableName] each pdb.getPartitions dbDir;
      os.rmtree each tablePaths;
      ]
   ];

  tabRef
 };

// @kind function
// @subcategory tbl
// @overview Describe a table reference.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference.
// @return description {dict (type:symbol; name:symbol; dbDir:hsym; parField:symbol)} A dictionary describing the table reference.
// @desc description.type [Table type](#qtktblgettype).
// @desc description.name Table name.
// @desc description.dbDir Database directory, or null symbol if not applicable.
// @desc description.parField Partition field or null symbol if not applicable.
// @throws {TypeError} If `tabRef` is not of valid type.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/describe; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// (`type`name`dbDir`parField!(`Partitioned; `PartitionedTable; `:/tmp/qtk/tbl/describe ; `date))~.qtk.tbl.describe tabRef
describe:{[tabRef]
  if[11h<>abs type tabRef; 'err.compose[`TypeError; "expect symbol or symbol vector"]];
  tableType:getType tabRef;

  dbDir:tableName:parField:`;
  $[tableType=`Plain;
    tableName:tabRef;
    tableType=`Serialized;
    [
      split:` vs tabRef;
      dbDir:first split;
      tableName:last split
      ];
    tableType=`Splayed;
    [
      $["/"=last string tabRef;
        [
          // tabRef is a full path
         split:` vs `$-1 _ string tabRef;
         dbDir:first split;
         tableName:last split
          ];
        [
          // tabRef is the table name
         dbDir:`:.;
         tableName:tabRef
          ]
       ];
      ];
    [
      $[11h=type tabRef;
        [
          // tabRef is a full path
         dbDir:tabRef[0];
         parField:tabRef[1];
         tableName:tabRef[2]
          ];
        [
          // tabRef is the table name
         dbDir:`:.;
         parField:pdb.getPartitionField dbDir;
         tableName:tabRef
          ]
       ]
      ]
   ];

  .[!;] flip (
    (`type;tableType);
    (`name;tableName);
    (`dbDir;dbDir);
    (`parField;parField)
  )
 };

// @kind function
// @subcategory tbl
// @overview Insert data into a table.
// For partitioned tables, data need to be sorted by partitioned field.
// Partial data are acceptable; the missing columns will be filled by type compliant nulls for simple columns
// or empty lists for compound columns.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference
// @param data {table} Table data.
// @return {symbol | hsym | (hsym; symbol; symbol)} The table reference.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/insert; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// tabRef~.qtk.tbl.insert[tabRef; ([] date:2022.01.03 2022.01.04; c1:3 4)]
.z.m.insert:{[tabRef;data]
  tabRefDesc:describe tabRef;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;

  $[tableType in `Plain`Serialized;
    tabRef insert data;
    tableType=`Splayed;
    [
      dbDir:tabRefDesc`dbDir;
      tablePath:.Q.dd[dbDir; tableName];
      completeData:.Q.en[dbDir;] 1 _ (p_singleton .z.m.meta tabRef) upsert data;     // in case data have partial columns
      p_insert[tablePath; completeData]
      ];
    // tableType=`Partitioned
    [
      dbDir:tabRefDesc`dbDir;
      completeData:.Q.en[dbDir;] 1 _ (p_singleton .z.m.meta tabRef) upsert data;     // in case data have partial columns
      parField:tabRefDesc`parField;
      parValues:?[completeData; (); (); (distinct;parField)];
      tablePaths:.Q.par[dbDir; ; tableName] each parValues;
      dataByPartition:flip each value parField xgroup completeData;
      p_insert'[tablePaths; dataByPartition];
      ]
   ];
  tabRef
 };

// @kind function
// @private
// @subcategory tbl
// @overview Insert data into a table.
// @param tablePath {hsym} Path to an on-disk table.
// @param data {table} Table data. Symbol columns must be enumerated and the table is not keyed.
// @return {hsym} The path to the table.
p_insert:{[tablePath;data]
  .Q.dd[tablePath; `] upsert data
 };

// @kind function
// @subcategory tbl
// @overview Update values in certain columns of a table, similar to [functional update](https://code.kx.com/q/basics/funsql/#update)
// but support all table types.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference.
// @param criteria {any[]} A list of criteria where the select is applied to, or empty list for the whole table.
// For partitioned tables, if partition field is included in the criteria, it has to be the first in the list.
// @param groupings {dict | 0b} A mapping of grouping columns, or `0b` for no grouping.
// @param columns {dict} Mappings from column names to columns/expressions.
// @return {symbol | hsym | (hsym; symbol; symbol)} The table reference.
// @throws {ColumnNotFoundError} If a column from `columns` doesn't exist.
.z.m.update:{[tabRef;criteria;groupings;columns]
  raiseIfColumnNotFound[tabRef;] each key columns;

  tabRefDesc:describe tabRef;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;
  $[tableType=`Plain;
    ![tabRef; criteria; groupings; columns];
    tableType=`Serialized;
    tabRef set ![get tabRef; criteria; groupings; columns];
    tableType=`Splayed;
    [
      dbDir:tabRefDesc`dbDir;
      tablePath:.Q.dd[dbDir; tableName];
      p_update[dbDir; tablePath; criteria; groupings; columns];
      if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
      ];
    // tableType=`Partitioned
    [
      dbDir:tabRefDesc`dbDir;
      partitions:pdb.getPartitions dbDir;
      parField:pdb.getPartitionField dbDir;

      if[(first criteria)[1]~parField;
         partitions:?[flip enlist[parField]!enlist[partitions]; enlist first criteria; (); parField];
         criteria:1_criteria
       ];

      tablePaths:.Q.par[dbDir; ; tableName] each partitions;
      p_update[dbDir; ; criteria; groupings; columns] each tablePaths;
      if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
      ]
   ];

  tabRef
 };

// @kind function
// @private
// @subcategory tbl
// @overview Update values in certain columns of an on-disk table.
// @param dbDir {hsym} DB directory.
// @param tablePath {hsym} Path to an on-disk table.
// @param criteria {any[]} A list of criteria where the update is applied to, or empty list if it's applied to the whole table.
// @param groupings {dict | 0b} A mapping of grouping columns, or `0b` for no grouping.
// @param columns {dict} Mappings from column names to columns/expressions.
// @return {hsym} The path to the table.
// @throws {TypeError | type} If it's a partial update and the new values don't have the same type as other values.
p_update:{[dbDir;tablePath;criteria;groupings;columns]
  updated:?[tablePath; criteria; groupings; columns,((enlist `index)!(enlist `i))];
  updated:.Q.en[dbDir;] $[99h=type updated; ungroup value updated; updated];
  if[0=count updated; :tablePath];

  i:0;
  allColumns:db.p_getColumns tablePath;
  do[count columns;
     column:key[columns] [i];
     columnVal:updated column;
     $[column in allColumns;
       [
         // update an existing column
         columnPath:.Q.dd[tablePath; column];
         $[criteria~();
           .[columnPath; (); :; columnVal];             // rewrite the whole column
           (newType:.Q.ty[columnVal])=oldType:.Q.ty[columnData:get columnPath];
           columnPath set @[columnData; updated`index; :; columnVal];  // update values at certain indices
           'err.compose[`TypeError; "mix type ",newType," with ",oldType," on column ",string column]
          ];
         ];
        // new column
       p_addColumn[tablePath; column; columnVal]
      ];
     i +: 1;
   ];
  tablePath
 };

// @kind function
// @subcategory tbl
// @overview Select from a table similar to [functional select](https://code.kx.com/q/basics/funsql/#select)
// but support all table types.
// @param table {table | symbol | hsym} Table name, path or value.
// @param criteria {any[]} A list of criteria where the select is applied to, or empty list for the whole table.
// @param groupings {dict | boolean} A mapping of grouping columns, or `0b` for no grouping, `1b` for distinct.
// @param columns {dict} Mappings from column names to columns/expressions.
// @return {table} Selected data from the table.
.z.m.select:{[table;criteria;groupings;columns]
  ?[table; criteria; groupings; columns]
 };

// @kind function
// @subcategory tbl
// @overview Select from a table similar to [rank-5 functional select](https://code.kx.com/q/basics/funsql/#rank-5)
// but support all table types.
// @param table {table | symbol | hsym} Table name, path or value.
// @param criteria {any[]} A list of criteria where the select is applied to, or empty list for the whole table.
// @param groupings {dict | boolean} A mapping of grouping columns, or `0b` for no grouping, `1b` for distinct.
// @param columns {dict} Mappings from column names to columns/expressions.
// @param limit {int | long | (int;int) | (long;long)} Limit on rows to return.
// @return {table} Selected data from the table.
selectLimit:{[table;criteria;groupings;columns;limit]
  select[limit] from ?[table; criteria; groupings; columns]
 };

// @kind function
// @subcategory tbl
// @overview Select from a table similar to [rank-6 functional select](https://code.kx.com/q/basics/funsql/#rank-6)
// but support all table types.
// @param table {table | symbol | hsym} Table name, path or value.
// @param criteria {any[]} A list of criteria where the select is applied to, or empty list for the whole table.
// @param groupings {dict | boolean} A mapping of grouping columns, or `0b` for no grouping, `1b` for distinct.
// @param columns {dict} Mappings from column names to columns/expressions.
// @param limit {int | long | (int;int) | (long;long)} Limit on rows to return.
// @param sort {any[]} Sort the result by a column. The format is `(op;col)` where `op` is `>:` for descending and
//   `<:` for ascending, and `col` is the column to be ordered by.
// @return {table} Selected data from the table.
selectLimitSort:{[table;criteria;groupings;columns;limit;sort]
  ?[?[table; criteria; groupings; columns];
    ();
    0b;
    ();
    limit;
    sort]
 };

// @kind function
// @subcategory tbl
// @overview Delete rows of a table given certain criteria.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference.
// @param criteria {any[]} A list of criteria where matching rows will be deleted, or empty list to delete all rows.
// For partitioned tables, if partition field is included in the criteria, it has to be the first in the list.
// @return {tabRef} The table reference.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/deleteRows; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02 2022.01.02; c1:1 2 3)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// tabRef~.qtk.tbl.deleteRows[tabRef; enlist(=;`c1;3)]
deleteRows:{[tabRef;criteria]
  tabRefDesc:describe tabRef;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;

  $[tableType=`Plain;
    ![tabRef; criteria; 0b; `$()];
    tableType=`Serialized;
    tabRef set ![get tabRef; criteria; 0b; `$()];
    tableType=`Splayed;
    [
      dbDir:tabRefDesc`dbDir;
      tablePath:.Q.dd[dbDir; tableName];
      p_deleteRows[tablePath; criteria];
      ];
    // tableType=`Partitioned
    [
      dbDir:tabRefDesc`dbDir;
      partitions:pdb.getPartitions dbDir;
      parField:pdb.getPartitionField dbDir;

      if[(first criteria)[1]~parField;
         partitions:?[flip enlist[parField]!enlist[partitions]; enlist first criteria; (); parField];
         criteria:1_criteria
       ];

      tablePaths:.Q.par[dbDir; ; tableName] each partitions;
      p_deleteRows[; criteria] each tablePaths;
      ]
   ];
  tabRef
 };

// @kind function
// @private
// @subcategory tbl
// @overview Delete rows of an on-disk table given certain criteria, in a similar format to functional delete.
// @param tablePath {hsym} Path to an on-disk table.
// @param criteria {any[]} A list of criteria where matching rows will be deleted, or empty list if it's applied to the whole table.
// @return {hsym} The path to the table.
p_deleteRows:{[tablePath;criteria]
  indicesToDelete:exec index from ?[tablePath; criteria; 0b; (enlist `index)!(enlist `i)];
  if[0=count indicesToDelete; :tablePath];

  rowCount:db.p_rowCount tablePath;
  remainingIndices:(til rowCount) except indicesToDelete;

  i:0;
  allColumns:db.p_getColumns tablePath;
  do[count allColumns;
     columnPath:.Q.dd[tablePath; allColumns[i]];
     .[columnPath; (); :; get[columnPath] remainingIndices];
     i +: 1;
   ];
  tablePath
 };

// @kind function
// @subcategory tbl
// @overview upsert data into a table.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference
// @param data {table} Table data.
// @param uk {symbol[]} The columns to be used as unique key
// @return {symbol | hsym | (hsym; symbol; symbol)} The table reference.
// @doctest
.z.m.upsert:{[tabRef;data;uk]
  tabRefDesc:describe tabRef;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;

  $[tableType=`Plain;
    tabRef set 0!(uk xkey select from tabRef) upsert data;
    tableType=`Serialized;
    tabRef set 0!(uk xkey select from tabRef) upsert data;
    tableType=`Splayed;
    [
      dbDir:tabRefDesc`dbDir;
      data: .Q.en[dbDir; data];
      tabRef set 0!(uk xkey select from tabRef) upsert data;
    ];
    // tableType=`Partitioned
    [
      dbDir:tabRefDesc`dbDir;
      data: .Q.en[dbDir; data];
      parField:pdb.getPartitionField dbDir;
      par2criteria:{(in;(flip;(!;enlist y;(enlist,y)));?[z[x];();0b;y!y])}[;uk except parField;data] each group data parField;
      criterias:{((=;z;x);y)}[;;parField]'[key par2criteria;value par2criteria];
      deleteRows[tabRef] each criterias;
      .z.m.insert[tabRef; data];
      ]
   ];
  tabRef
 };


// @kind function
// @subcategory tbl
// @overview Raise ColumnNotFoundError if a column is not found from a table.
// @param table {table | symbol | hsym | (hsym; symbol; symbol)} Table value or reference.
// @param column {symbol} A column name.
// @throws {ColumnNotFoundError} If the column doesn't exist.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/raiseIfColumnNotFound; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// "ColumnNotFoundError: c2 on :/tmp/qtk/tbl/raiseIfColumnNotFound/date/PartitionedTable"~.[.qtk.tbl.raiseIfColumnNotFound; (tabRef; `c2); {x}]
raiseIfColumnNotFound:{[table;column]
  if[not columnExists[table; column];
     'err.compose[`ColumnNotFoundError; string[column],
     $[-11h=(tt:type table); " on ",string[table];
       11h=tt; " on ",string[` sv table];
       ""
      ]
       ]
   ];
 };

// @kind function
// @subcategory tbl
// @overview Check if a column exists in a table.
// For splayed tables, column existence requires that the column appears in `.d` file and its data file exists.
// For partitioned tables, it requires the condition holds for the latest partition.
// @param table {table | symbol | hsym | (hsym; symbol; symbol)} Table value or reference.
// @param column {symbol} Column name.
// @return {boolean} `1b` if the column exists in the table; `0b` otherwise.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/columnExists; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// .qtk.tbl.columnExists[tabRef;`c1]
columnExists:{[table;column]
  if[type[table] in 98 99h; :column in cols table];

  tabRefDesc:describe table;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;

  $[tableType=`Plain;
    column in cols table;
    tableType=`Serialized;
    column in cols get table;
    tableType=`Splayed;
    [
      dbDir:tabRefDesc`dbDir;
      tablePath:.Q.dd[dbDir; tableName];
      p_columnExists[tablePath; column]
      ];
    // tableType=`Partitioned
    [
      dbDir:tabRefDesc`dbDir;
      tablePath:.Q.par[dbDir; ; tableName] last pdb.getPartitions dbDir;
      p_columnExists[tablePath; column]
      ]
   ]
 };

// @kind function
// @private
// @subcategory tbl
// @overview Check if a column exists in an on-disk table. A column exists if it's listed in `.d` file and
// there is a file of the same name in the table path.
// @param tablePath {hsym} Path to an on-disk table.
// @param column {symbol} A column name.
// @return {boolean} `1b` if the column exists in the table; `0b` otherwise.
p_columnExists:{[tablePath;column]
  allColumns:db.p_getColumns tablePath;
  if[not column in allColumns; :0b];
  columnPath:.Q.dd[tablePath; column];
  os.path.isFile columnPath
 };

// @kind function
// @subcategory tbl
// @overview Raise ColumnExistsError if a column exists in a table.
// @param table {table | symbol | hsym | (hsym; symbol; symbol)} Table value or reference.
// @param column {symbol} A column name.
// @throws {ColumnExistsError} If the column exists.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/raiseIfColumnExists; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// "ColumnExistsError: c1 on :/tmp/qtk/tbl/raiseIfColumnExists/date/PartitionedTable"~.[.qtk.tbl.raiseIfColumnExists; (tabRef; `c1); {x}]
raiseIfColumnExists:{[table;column]
  if[columnExists[table; column];
     'err.compose[`ColumnExistsError; string[column],
     $[-11h=(tt:type table); " on ",string[table];
       11h=tt; " on ",string[` sv table];
       ""
      ]
       ]
   ];
 };

// @kind function
// @subcategory tbl
// @overview Get column names of a table. It's similar to [cols](https://code.kx.com/q/ref/cols/#cols) but supports all table types.
// For partitioned table, the latest partition is used.
// @param t {table | symbol | hsym | (hsym; symbol; symbol)} Table or table reference.
// @return {symbol[]} Column names.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/columns; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2; c2:`a`b)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// `date`c1`c2~.qtk.tbl.columns tabRef
columns:{[t]
  if[type[t] in 98 99h; :cols t];

  tabRefDesc:describe t;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;
  $[tableType in `Plain`Splayed;
    cols t;
    tableType=`Serialized;
    cols get t;
    // tableType=`Partitioned
    [
      dbDir:tabRefDesc`dbDir;
      tablePath:.Q.par[dbDir; ; tableName] last pdb.getPartitions dbDir;
      tabRefDesc[`parField],db.p_getColumns tablePath
      ]
   ]
 };

// @kind function
// @subcategory tbl
// @overview Add a column to a table with a given value.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference.
// @param column {symbol} Name of new column to be added.
// @param columnValue {any} Value to be set on the new column.
// @return {symbol | hsym | (hsym; symbol; symbol)} The table reference.
// @throws {ColumnNameError} If `column` is not a valid name.
// @throws {ColumnExistsError} If `column` already exists.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/addColumn; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// tabRef~.qtk.tbl.addColumn[tabRef; `c2; 0n]
addColumn:{[tabRef;column;columnValue]
  raiseIfColumnNameInvalid column;
  raiseIfColumnExists[tabRef; column];

  tabRefDesc:describe tabRef;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;
  $[tableType=`Plain;
    [
      if[-11h=type columnValue; columnValue:enlist columnValue];                         // enlist singleton symbol value
      ![tabRef; (); 0b; enlist[column]!enlist[columnValue]];
      ];
    tableType=`Serialized;
    [
      if[-11h=type columnValue; columnValue:enlist columnValue];                         // enlist singleton symbol value
      tabRef set ![get tabRef; (); 0b; enlist[column]!enlist[columnValue]];
      ];
    tableType=`Splayed;
    [
      dbDir:tabRefDesc`dbDir;
      tablePath:.Q.dd[dbDir; tableName];
      p_addColumn[tablePath; column; db.p_enumerateAgainst[dbDir;`sym;columnValue]];
      if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
      ];
    // tableType=`Partitioned
    [
      dbDir:tabRefDesc`dbDir;
      tablePaths:.Q.par[dbDir; ; tableName] each pdb.getPartitions dbDir;
      p_addColumn[; column; db.p_enumerateAgainst[dbDir;`sym;columnValue] ] each tablePaths;
      if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
      ]
   ];

  tabRef
 };

// @kind function
// @private
// @subcategory tbl
// @overview Add a column to an on-disk table with a given value.
// @param tablePath {hsym} Path to an on-disk table.
// @param column {symbol} Name of new column to be added.
// @param columnValue {any} Value to be set on the new column. It must be enumerated if it's a symbol or symbol vector.
// @return {hsym} The path to the table.
p_addColumn:{[tablePath;column;columnValue]
  allColumns:db.p_getColumns tablePath;
  countInPath:p_count tablePath;
  .[.Q.dd[tablePath; column]; (); :; countInPath#columnValue];
  @[tablePath; `.d; :; distinct allColumns,column];
  tablePath
 };

// @kind function
// @subcategory tbl
// @overview Raise ColumnNameError if a column name is not valid, i.e. it collides with q's reserved words and implicit column `i`.
// @param name {symbol} A column name.
// @throws {ColumnNameError} If the column name is not valid.
// @doctest
// .qtk:use`qtk;
//
// "ColumnNameError: abs"~@[.qtk.tbl.raiseIfColumnNameInvalid; `abs; {x}]
raiseIfColumnNameInvalid:{[name]
  if[(name in `i,.Q.res,key `.q) or name<>.Q.id name;
     'err.compose[`ColumnNameError; string name]
   ];
 };

// @kind function
// @subcategory tbl
// @overview Delete a column from a table.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference.
// @param column {symbol} A column to be deleted.
// @return {symbol | hsym | (hsym; symbol; symbol)} The table reference.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/deleteColumn; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2; c2:`a`b)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// tabRef~.qtk.tbl.deleteColumn[tabRef; `c2]
deleteColumn:{[tabRef;column]
  tabRefDesc:describe tabRef;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;

  $[tableType=`Plain;
    ![tabRef; (); 0b; enlist[column]];
    tableType=`Serialized;
    tabRef set ![get tabRef; (); 0b; enlist[column]];
    tableType=`Splayed;
    [
      dbDir:tabRefDesc`dbDir;
      tablePath:.Q.dd[dbDir; tableName];
      p_deleteColumn[tablePath; column];
      if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
      ];
    // tableType=`Partitioned
    [
      dbDir:tabRefDesc`dbDir;
      tablePaths:.Q.par[dbDir; ; tableName] each pdb.getPartitions dbDir;
      p_deleteColumn[; column] each tablePaths;
      if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
      ]
   ];

  tabRef
 };

// @kind function
// @private
// @subcategory tbl
// @overview Delete a column of an on-disk table and its data.
// @param tablePath {hsym} Path to an on-disk table.
// @param column {symbol} A column to be deleted.
// @return {hsym} The path to the table.
p_deleteColumn:{[tablePath;column]
  columnPath:.Q.dd[tablePath; column];
  p_deleteColumnData columnPath;
  p_deleteColumnHeader[tablePath; column];
  tablePath
 };

// @kind function
// @private
// @subcategory tbl
// @overview Delete a column header of an on-disk table.
// @param tablePath {hsym} Path to an on-disk table.
// @param column {symbol} A column to be deleted.
// @return {hsym} The path to the table.
p_deleteColumnHeader:{[tablePath;column]
  allColumns:db.p_getColumns tablePath;
  @[tablePath; `.d; :; allColumns except column];
  tablePath
 };

// @kind function
// @private
// @subcategory tbl
// @overview Delete a column on disk, including accompanying # and ## files (if any).
// @param columnPath {hsym} Path to a column of an on-disk table.
// @return {hsym} The path to the column.
p_deleteColumnData:{[columnPath]
  if[os.path.isFile columnPath;
     os.remove columnPath
   ];
  if[os.path.isFile dataFile:`$string[columnPath],"#";
     os.remove dataFile
   ];
  if[os.path.isFile dataFile:`$string[columnPath],"##";
     os.remove dataFile
   ];
  columnPath
 };

// @kind function
// @subcategory tbl
// @overview Rename column(s) from a table.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table name.
// @param nameDict {dict} A dictionary from existing name(s) to new name(s).
// @return {symbol | hsym | (hsym; symbol; symbol)} The table reference.
// @throws {ColumnNameError} If the column name is not valid.
// @throws {ColumnNotFoundError} If some column doesn't exist.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/renameColumns; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2; c2:`a`b)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// tabRef~.qtk.tbl.renameColumns[tabRef; `c1`c2!`c3`c4]
renameColumns:{[tabRef;nameDict]
  raiseIfColumnNotFound[tabRef;] each key nameDict;
  raiseIfColumnNameInvalid each value nameDict;

  tabRefDesc:describe tabRef;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;

  $[tableType in `Plain`Serialized;
    tabRef set nameDict xcol get tabRef;
    tableType=`Splayed;
    [
      dbDir:tabRefDesc`dbDir;
      tablePath:.Q.dd[dbDir; tableName];
      p_renameColumns[tablePath; nameDict];
      if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
      ];
    // tableType=`Partitioned
    [
      dbDir:tabRefDesc`dbDir;
      tablePaths:.Q.par[dbDir; ; tableName] each pdb.getPartitions dbDir;
      p_renameColumns[; nameDict] each tablePaths;
      if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
      ]
   ];

  tabRef
 };

// @kind function
// @private
// @subcategory tbl
// @overview Rename column(s) of an on-disk table.
// @param tablePath {hsym} Path to an on-disk table.
// @param nameDict {dict} A dictionary from old name(s) to new name(s).
// @return {hsym} The path to the table.
p_renameColumns:{[tablePath;nameDict]
  p_renameOneColumn[tablePath; ;]'[key nameDict; value nameDict];
  tablePath
 };

// @kind function
// @private
// @subcategory tbl
// @overview Rename a column of an on-disk table.
// @param tablePath {hsym} Path to an on-disk table.
// @param oldName {symbol} A column name of the table.
// @param newName {symbol} New column name.
// @return {hsym} The path to the table.
p_renameOneColumn:{[tablePath;oldName;newName]
  allColumns:db.p_getColumns tablePath;

  if[(not oldName in allColumns) or (newName in allColumns); :tablePath];

  oldColumnPath:.Q.dd[tablePath; oldName];
  newColumnPath:.Q.dd[tablePath; newName];
  db.p_renameColumnOnDisk[oldColumnPath; newColumnPath];

  newColumns:@[allColumns; first where allColumns=oldName; :; newName];
  @[tablePath; `.d; :; newColumns];
  tablePath
 };

// @kind function
// @private
// @subcategory tbl
// @overview Rename a column on disk. Column data along with accompanying # and ## files are moved.
// @param oldPath {hsym} A file symbol representing an existing column.
// @param newPath {hsym} A file symbol representing a new column.
db.p_renameColumnOnDisk:{[oldPath;newPath]
  os.move[oldPath; newPath];
  if[os.path.isFile dataFile:`$string[oldPath],"#";
     os.move[dataFile; `$string[newPath],"#"]
   ];
  if[os.path.isFile dataFile:`$string[oldPath],"##";
     os.move[dataFile; `$string[newPath],"##"]
   ];
 };

// @kind function
// @subcategory tbl
// @overview Reorder columns of a table.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference.
// @param firstColumns {symbol[]} First columns after reordering.
// @return {symbol | hsym | (hsym; symbol; symbol)} The table reference.
// @throws {ColumnNotFoundError} If some column in `firstColumns` doesn't exist.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/reorderColumns; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2; c2:`a`b)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// tabRef~.qtk.tbl.reorderColumns[tabRef; `c2]
reorderColumns:{[tabRef;firstColumns]
  raiseIfColumnNotFound[tabRef;] each firstColumns;

  tabRefDesc:describe tabRef;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;
  $[tableType in `Plain`Serialized;
    tabRef set firstColumns xcols get tabRef;
    tableType=`Splayed;
    [
      dbDir:tabRefDesc`dbDir;
      tablePath:.Q.dd[dbDir; tableName];
      p_reorderColumns[tablePath; firstColumns];
      if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
      ];
    // tableType=`Partitioned
    [
      dbDir:tabRefDesc`dbDir;
      tablePaths:.Q.par[dbDir; ; tableName] each pdb.getPartitions dbDir;
      p_reorderColumns[; firstColumns] each tablePaths;
      if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
      ]
   ];

  tabRef
 };

// @kind function
// @private
// @subcategory tbl
// @overview Reorder columns of an on-disk table with specified first columns.
// @param tablePath {hsym} Path to an on-disk table.
// @param firstColumns {dict} First columns after reordering.
// @return {hsym} The path to the table.
p_reorderColumns:{[tablePath;firstColumns]
  allColumns:db.p_getColumns tablePath;
  @[tablePath; `.d; :; firstColumns,allColumns except firstColumns];
  tablePath
 };

// @kind function
// @subcategory tbl
// @overview Copy an existing column of a table to a new column.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference.
// @param sourceColumn {symbol} Source column to copy from.
// @param targetColumn {symbol} Target column to copy to.
// @return {symbol | hsym | (hsym; symbol; symbol)} The table reference.
// @throws {ColumnNotFoundError} If `sourceColumn` doesn't exist.
// @throws {ColumnExistsError} If `targetColumn` exists.
// @throws {ColumnNameError} If name of `targetColumn` is not valid.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/copyColumn; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// .qtk.tbl.copyColumn[tabRef; `c1; `c2];
// .qtk.tbl.columnExists[tabRef; `c2]
copyColumn:{[tabRef;sourceColumn;targetColumn]
  raiseIfColumnNotFound[tabRef; sourceColumn];
  raiseIfColumnExists[tabRef; targetColumn];
  raiseIfColumnNameInvalid targetColumn;

  tabRefDesc:describe tabRef;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;
  $[tableType=`Plain;
    ![tabRef; (); 0b; enlist[targetColumn]!enlist[sourceColumn]];
    tableType=`Serialized;
    tabRef set ![get tabRef; (); 0b; enlist[targetColumn]!enlist[sourceColumn]];
    tableType=`Splayed;
    [
      dbDir:tabRefDesc`dbDir;
      tablePath:.Q.dd[dbDir; tableName];
      p_copyColumn[tablePath; sourceColumn; targetColumn];
      if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
      ];
    // tableType=`Partitioned
    [
      dbDir:tabRefDesc`dbDir;
      tablePaths:.Q.par[dbDir; ; tableName] each pdb.getPartitions dbDir;
      p_copyColumn[; sourceColumn; targetColumn] each tablePaths;
      if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
      ]
   ];

  tabRef
 };

// @kind function
// @private
// @subcategory tbl
// @overview Copy an existing column of an on-disk table to a new column.
// @param tablePath {hsym} Path to an on-disk table.
// @param sourceColumn {symbol} Source column to copy from.
// @param targetColumn {symbol} Target column to copy to.
// @return {hsym} The path to the table.
p_copyColumn:{[tablePath;sourceColumn;targetColumn]
  sourceColumnPath:.Q.dd[tablePath; sourceColumn];
  targetColumnPath:.Q.dd[tablePath; targetColumn];

  os.copy[sourceColumnPath; targetColumnPath];
  if[os.path.isFile dataFile:`$string[sourceColumnPath],"#";
     os.copy[dataFile; `$string[targetColumnPath],"#"]
   ];
  if[os.path.isFile dataFile:`$string[sourceColumnPath],"##";
     os.copy[dataFile; `$string[targetColumnPath],"##"]
   ];

  @[tablePath; `.d; ,; targetColumn];
  tablePath
 };

// @kind function
// @subcategory tbl
// @overview Apply a function to a column of a table.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference.
// @param column {symbol} Column where the function will be applied.
// @param function {fn(any[]) -> any[]} Function to be applied.
// @return {symbol | hsym | (hsym; symbol; symbol)} The table reference.
// @throws {ColumnNotFoundError} If `column` doesn't exist.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/apply; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// tabRef~.qtk.tbl.apply[tabRef; `c1; 2*]
apply:{[tabRef;column;function]
  raiseIfColumnNotFound[tabRef; column];

  tabRefDesc:describe tabRef;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;
  $[tableType=`Plain;
    ![tabRef; (); 0b; enlist[column]!enlist[(function;column)]];
    tableType=`Serialized;
    tabRef set ![get tabRef; (); 0b; enlist[column]!enlist[(function;column)]];
    tableType=`Splayed;
    [
      dbDir:tabRefDesc`dbDir;
      tablePath:.Q.dd[dbDir; tableName];
      p_apply[dbDir; tablePath; column; function];
      ];
    // tableType=`Partitioned
    [
      dbDir:tabRefDesc`dbDir;
      tablePaths:.Q.par[dbDir; ; tableName] each pdb.getPartitions dbDir;
      p_apply[dbDir; ; column; function] each tablePaths;
      ]
   ];

  tabRef
 };

// @kind function
// @private
// @subcategory tbl
// @overview Apply a function to a column of an on-disk table.
// @param dbDir {hsym} DB directory.
// @param tablePath {hsym} Path to an on-disk table.
// @param column {symbol} A column name of the table.
// @param function {fn(any[]) -> any[]} Function to be applied to the column.
// @return {hsym} The path to the table.
p_apply:{[dbDir;tablePath;column;function]
  columnPath:.Q.dd[tablePath; column];
  oldValue:get columnPath;
  oldAttr:attr oldValue;
  newValue:function oldValue;
  newAttr:attr newValue;
  if[(not oldValue~newValue) or (not oldAttr~newAttr);
     .[columnPath; (); :; db.p_enumerateAgainst[dbDir;`sym;newValue]]
   ];
  tablePath
 };

// @kind function
// @subcategory tbl
// @overview Cast the datatype of a column of a table.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference.
// @param column {symbol} Column whose datatype will be casted.
// @param newType {symbol | char} Name or character code of the new data type.
// @return {symbol | hsym | (hsym; symbol; symbol)} The table reference.
// @throws {ColumnNotFoundError} If `column` doesn't exist.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/castColumn; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// tabRef~.qtk.tbl.castColumn[tabRef; `c1; `int]
castColumn:{[tabRef;column;newType]
  apply[tabRef; column; newType$]
 };

// @kind function
// @subcategory tbl
// @overview Set attributes to a table. It's an extended form of [Set Attribute](https://code.kx.com/q/ref/set-attribute/)
// that is applicable to tables.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference.
// @param attrs {dict} A mapping from column names to attributes.
// @return {symbol | hsym | (hsym; symbol; symbol)} The table reference.
// @throws {ColumnNotFoundError} If some columns in `attrs` don't exist.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/setAttr; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// .qtk.tbl.setAttr[tabRef; enlist[`c1]!enlist[`s]];
// `s=.qtk.tbl.meta[tabRef][`c1;`a]
setAttr:{[tabRef;attrs]
  columns:key attrs;
  columnExistsList: columnExists[tabRef; ] each columns;
  if[not all columnExistsList;
    err.compose[`ColumnNotFoundError; "," sv string columns where not columnExistsList]
    ];

  apply[tabRef; ;]'[columns; {x#} each value attrs];
  tabRef
 };

// @kind function
// @subcategory tbl
// @overview Get attributes of a table. It's an extended from of [attr](https://code.kx.com/q/ref/attr/)
// that is applicable to tables.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference.
// @return {dict} A mapping from columns names to attributes, where columns without attributes are not included.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/getAttr; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
// .qtk.tbl.setAttr[tabRef; enlist[`c1]!enlist[`s]];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// (enlist[`c1]!enlist[`s])~.qtk.tbl.getAttr tabRef
getAttr:{[tabRef]
  exec c!a from .z.m.meta[tabRef] where not null a
 };

// @kind function
// @subcategory tbl
// @overview Count rows of a table.
// It's similar to [count](https://code.kx.com/q/ref/count/#count) but supports all table types.
// @param table {table | symbol | hsym | (hsym; symbol; symbol)} Table value or reference.
// @return {long} Row count of the table.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/count; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// 2=.qtk.tbl.count tabRef
.z.m.count:{[table]
  if[(tt:type table) in 98 99h; :count table];

  tabRefDesc:describe table;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;

  $[tableType in `Plain`Serialized;
    count get table;
    tableType=`Splayed;
    [
      if[not table like "*/"; :count get table];

      dbDir:tabRefDesc`dbDir;
      if[os.path.samefile[dbDir; `:.]; :count get tableName];

      tablePath:.Q.dd[dbDir; tableName];
      p_count tablePath
      ];
    // tableType=`Partitioned
    [
      if[-11h=tt; :count get table];

      dbDir:tabRefDesc`dbDir;
      if[os.path.samefile[dbDir; `:.]; :count get tableName];

      tablePaths:.Q.par[dbDir; ; tableName] each pdb.getPartitions dbDir;
      sum p_count each tablePaths
      ]
   ]
 };

// @kind function
// @private
// @subcategory tbl
// @overview Count rows of an on-disk table. Only the first column is taken into consideration.
// @param tablePath {hsym} Path to an on-disk table.
// @return {long} Row count of the table.
p_count:{[tablePath]
  firstColumn:first db.p_getColumns tablePath;
  count get .Q.dd[tablePath; firstColumn]
 };

// @kind function
// @subcategory tbl
// @overview Check if a table of given name exists.
// For splayed table not in the current database, it's deemed existent if the directory exists.
// For partitioned table not in the current database, it's deemed existent if the directory exists in either the first
// or the last partition.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference.
// @return {boolean} `1b` if the table exists; `0b` otherwise.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/exists; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// .qtk.tbl.exists tabRef
exists:{[tabRef]
  tabRefDesc:describe tabRef;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;

  $[tableType=`Plain;
    $[ utils.nameExists tabRef; .z.m.type.isTable tabRef; 0b];
    tableType=`Serialized;
    $[os.path.isFile tabRef; .z.m.type.isTable tabRef; 0b];
    tableType=`Splayed;
    [
      dbDir:tabRefDesc`dbDir;
      if[os.path.samefile[dbDir; `:.]; : utils.nameExists tableName];

      tablePath:.Q.dd[dbDir; tableName];
      os.path.isDir tablePath
      ];
    // tableType=`Partitioned
    [
      dbDir:tabRefDesc`dbDir;
      if[os.path.samefile[dbDir; `:.]; : utils.nameExists tableName];

      tablePaths:.Q.par[dbDir; ; tableName] each (first;last) @\: pdb.getPartitions dbDir;
      any os.path.isDir each tablePaths
      ]
   ]
 };

// @kind function
// @subcategory tbl
// @overview Get entries at given indices of a table.
// It's similar to [.Q.ind](https://code.kx.com/q/ref/dotq/#qind-partitioned-index) but has the following differences:
//
// - if `indices` are empty, an empty table of conforming schema is returned rather than an empty list.
// - if `indices` go out of bound, an empty table of conforming schema is returned rather than raising 'par error
// @param table {symbol | hsym | table} Table name, path or value.
// @param indices {int[] | long[]} Indices to select from.
// @return {table} Table at the given indices.
at:{[table;indices]
  if[type[table] in 98 99h;
     :$[1b~.Q.qp table;
        p_atSafe[`:.;table;indices];
        select from table where i in indices]];

  tabRefDesc:describe table;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;

  $[tableType in `Plain`Serialized`Splayed;
    select from table where i in indices;
    // tableType=`Partitioned
    [
      dbDir:tabRefDesc`dbDir;
      p_atSafe[dbDir; get tableName; indices]
      ]
   ]
 };

// @kind function
// @private
// @subcategory tbl
// @overview Get entries at given indices of a partitioned table.
// It's similar to [.Q.ind](https://code.kx.com/q/ref/dotq/#qind-partitioned-index) but has the following differences:
//
// - if `indices` are empty, an empty table of conforming schema is returned rather than an empty list.
// - if `indices` go out of bound, an empty table of conforming schema is returned rather than raising 'par error
// @param dbDir {hsym} DB directory.
// @param table {table} Partitioned table.
// @param indices {int[] | long[]} Indices to select from.
// @return {table} Table at the given indices.
p_atSafe:{[dbDir;table;indices]
  r:.[.Q.ind; (table;indices); ()];   // trap 'par error when indices are out of bound
  $[r~(); .Q.en[dbDir;] p_empty .z.m.meta table; r]
 };

// @kind function
// @subcategory tbl
// @overview Rename a table.
// @param tabRef {symbol | hsym | (hsym; symbol; symbol)} Table reference.
// @param newName {symbol} New name of the table.
// @return {symbol | hsym | (hsym; symbol; symbol)} New table reference.
// @throws {TableNameError} If the table name is not valid, i.e. it collides with q's reserved words
// @throws {NameExistsError} If the name is in use
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/rename; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// (`:/tmp/qtk/tbl/rename; `date; `NewPartitionedTable)~.qtk.tbl.rename[tabRef; `NewPartitionedTable]
rename:{[tabRef;newName]
  p_validateTableName newName;
  utils.raiseIfNameExists newName;

  tabRefDesc:describe tabRef;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;

  if[tableType=`Plain;
     newName set get tabRef;
     ![`.; (); 0b; enlist tabRef];
     :newName];

  if[tableType=`Serialized; :p_rename[tabRef; newName]];

  if[tableType=`Splayed;
     dbDir:tabRefDesc`dbDir;
     p_rename[.Q.dd[dbDir;tableName]; newName];
     if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
     if[os.path.samefile[dbDir; `:.];
        ![`.; (); 0b; enlist tableName];
        if[db.autoReload; db.reload[]]
       ];
     :$[tabRef like "*/"; ` sv (dbDir;newName;`); newName]];

  // tableType=`partitioned
  dbDir:tabRefDesc`dbDir;
  tablePaths:.Q.par[dbDir; ; tableName] each pdb.getPartitions dbDir;
  p_rename[; newName] each tablePaths;
  if[os.path.samefile[dbDir; `:.];
     ![`.; (); 0b; enlist tableName];
     if[db.autoReload; db.reload[]]
    ];
  $[11h=type tabRef; (dbDir; tabRefDesc`parField; newName); newName]
 };

// @kind function
// @private
// @subcategory tbl
// @overview Rename an on-disk table.
// @param tablePath {hsym} Path to an on-disk table.
// @param newName {symbol} New table name.
// @return {hsym} Path to the renamed table.
p_rename:{[tablePath;newName]
  newTablePath:.Q.dd[; newName] first ` vs tablePath;
  os.move[tablePath; newTablePath];
  newTablePath
 };

// @kind function
// @private
// @subcategory tbl
// @overview Validate table name.
// @param name {symbol} Table name.
// @throws {TableNameError} If the table name is not valid.
p_validateTableName:{[name]
  if[(name in .Q.res,key `.q) or name<>.Q.id name;
     'err.compose[`TableNameError; string name]
   ];
 };

// @kind function
// @subcategory tbl
// @overview Fix a partitioned table based on a good partition. Fixable issues include:
//
//   - add `.d` file if missing
//   - add missing columns to `.d` file
//   - add missing data files to disk filled by nulls for simple columns or empty lists for compound columns
//   - remove excessive columns from `.d` file but leave data files untouched
//   - put columns in the right order
// @param tabRef {symbol | (hsym; symbol; symbol)} Table reference.
// @param refPartition {date | month | int} A good partition to which the fixing refers.
// @return {symbol | (hsym; symbol; symbol)} The table reference.
// @throws {NotAPartitionedTableError} If the table is not a partitioned table.
// @doctest
// .qtk:use`qtk;
// tabRef:(`:/tmp/qtk/tbl/fix; `date; `PartitionedTable);
// .qtk.tbl.create[tabRef; ([] date:2022.01.01 2022.01.02; c1:1 2)];
// .qtk.os.remove "/tmp/qtk/tbl/fix/2022.01.02/Table/.d";
//
// // Or replace tabRef with `PartitionedTable if the database is loaded
// tabRef~.qtk.tbl.fix[tabRef; 2022.01.01]
fix:{[tabRef;refPartition]
  tabRefDesc:describe tabRef;
  tableType:tabRefDesc`type;
  tableName:tabRefDesc`name;

  if[tableType<>`Partitioned;
     'err.compose[`NotAPartitionedTableError; $[-11h=type tabRef; string tabRef; string ` sv tabRef]]
   ];

  dbDir:tabRefDesc`dbDir;
  refTablePath:.Q.par[dbDir; refPartition; tableName];
  refColumns:db.p_getColumns refTablePath;
  defaultValues:db.p_defaultValue[refTablePath;] each refColumns;
  tablePaths:.Q.par[dbDir; ; tableName] each pdb.getPartitions[dbDir] except refPartition;
  p_fix[; refColumns!defaultValues] each tablePaths;

  if[db.autoReload and os.path.samefile[dbDir; `:.]; db.reload[]];
  tabRef
 };

// @kind function
// @private
// @overview Fix an on-disk table based on a mapping between columns and their default values. Fixable issues include:
//
//   - add `.d` file if missing
//   - add missing columns to `.d` file
//   - add missing data files to disk filled by nulls for simple columns or empty lists for compound columns
//   - remove excessive columns from `.d` file but leave data files untouched
//   - put columns in the right order
// @param tablePath {hsym} Path to an on-disk table.
// @param columnDefaults {dict} A mapping between columns and their default values.
// @return {hsym} The path to the table.
p_fix:{[tablePath;columnDefaults]
  filesInPartition:os.listDir tablePath;
  addColumnFunc:p_addColumn[tablePath; ;];
  expectedColumns:key columnDefaults;

  if[not db.p_dotDExists tablePath; @[tablePath; `.d; :; expectedColumns]];

  // add missing columns
  allColumns:db.p_getColumns tablePath;
  if[count missingColumns:expectedColumns except allColumns;
     addColumnFunc'[missingColumns; columnDefaults missingColumns]
   ];

  // add missing data files
  allColumns:db.p_getColumns tablePath;
  if[count missingDataColumns:allColumns except filesInPartition;
     addColumnFunc'[missingDataColumns; columnDefaults missingDataColumns]
   ];

  // remove excessive columns
  allColumns:db.p_getColumns tablePath;
  if[count excessiveColumns:allColumns except expectedColumns;
     p_deleteColumnHeader[tablePath;] each excessiveColumns;
   ];

  // fix column order
  allColumns:db.p_getColumns tablePath;
  if[not allColumns~expectedColumns;
     p_reorderColumns[tablePath; expectedColumns]
   ];

  tablePath
 };


// @kind function
// @subcategory tbl
// @overview Return the key of a table if it's keyed table, or generic null otherwise.
// It's an alias of [key](https://code.kx.com/q/ref/key/#keys-of-a-keyed-table).
// @param t {table | symbol | hsym | (hsym; symbol; symbol)} Table or table reference.
// @return {table | ::} Key of the table.
// @doctest
// .qtk:use`qtk;
//
// ([] c1:`a`b)~key ([c1:`a`b] c2:1 2)
.z.m.key:{[t]
  if[type[t] in 98 99h; :@[key; t; {(::)}]];

  tabRefDesc:describe t;
  tableType:tabRefDesc`type;

  $[tableType in `Plain`Serialized;
    @[key get @; t; {(::)}];
    // splayed/partitioned tables cannot be keyed
    (::)
   ]
 };


// @kind function
// @subcategory tbl
// @overview Wrap a function that modifies a table but keep the original attributes.
// @param func {func} A function that modifies a table.
// @return {func} A wrapper function that keeps the original attributes.
keepAttr:{[func]
  {[table;func]
    attrs:getAttr table;
    func table;
    setAttr[table; attrs];
    table
  }[;func]
 };


export:([addColumn;apply;at;castColumn;columnExists;columns;copyColumn;.z.m.count;create;deleteColumn;deleteRows;.z.m.upsert;describe;drop;exists;fix;foreignKeys;getAttr;getType;.z.m.insert;keepAttr;.z.m.key;.z.m.meta;raiseIfColumnExists;raiseIfColumnNameInvalid;raiseIfColumnNotFound;rename;renameColumns;reorderColumns;.z.m.select;selectLimit;selectLimitSort;setAttr;.z.m.update;p_deleteColumnData;p_deleteColumn;p_addColumn]);