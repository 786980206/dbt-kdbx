
err:use`qtk.err;

// @kind function
// @subcategory pdb
// @overview Get all partitions of current database.
// It's similar to [.Q.PV](https://code.kx.com/q/ref/dotq/#qpv-partition-values) but will return an empty list if
// the database is not partitioned.
// @return {date[] | month[] | int[] | ()} Partitions of the database, or an empty list
// if the database is not a partitioned database.
// @see .qtk.pdb.getPartitions
// @see getModifiedPartitions
getPartitions:{
  @[value; `.Q.PV; ()]
 };

// @kind function
// @subcategory pdb
// @overview Get all partitions of current database, subject to modification by [`.Q.view`](https://code.kx.com/q/ref/dotq/#qview-subview).
// It's similar to [.Q.pv](https://code.kx.com/q/ref/dotq/#qpv-modified-partition-values) but will return an empty list
// if the database is not partitioned.
// @return {date[] | month[] | int[] | ()} Partitions of the database subject to modification by `.Q.view`,
// or an empty list if the database is not partitioned.
// @see getPartitions
getModifiedPartitions:{
  @[value; `.Q.pv; ()]
 };

// @kind function
// @subcategory pdb
// @overview Get partition field of the current database.
// It's similar to [.Q.pf](https://code.kx.com/q/ref/dotq/#qpf-partition-field) but will return a null symbol if
// the database is not partitioned.
// @return {symbol} Partition field of the database, either of ``#!q `date`month`year`int ``, or a null symbol
// if the database is not a partitioned database.
// @see .qtk.pdb.getPartitionField
getPartitionField:{
  @[value; `.Q.pf; `]
 };

// @kind function
// @subcategory pdb
// @overview Get partitioned tables of the current database.
// It's similar to [.Q.pt](https://code.kx.com/q/ref/dotq/#qpt-partitioned-tables) but will return an empty symbol vector
// if the database is not partitioned.
// @return {symbol[]} Partitioned tables of the database, or empty symbol vector if the database is not a partitioned database.
getPartitionedTables:{
  @[value; `.Q.pt; enlist `]
 };

// @kind function
// @subcategory pdb
// @overview Get all segments.
// It's similar to [.Q.P](https://code.kx.com/q/ref/dotq/#qp-segments) but will return an empty list
// if the database is not partitioned.
// @return {hsym[] | ()} Segments of the database, or an empty list
// if the database is not a partitioned database.
getSegments:{
  @[value; `.Q.P; ()]
 };

// @kind function
// @subcategory pdb
// @overview Partitions per segment.
// @return {dict} A dictionary from segments to partitions in each segment. It's empty if the database doesn't contain
// any segment.
getPartitionsPerSegment:{
  getSegments[]!@[value; `.Q.D; ()]
 };

// @kind function
// @subcategory pdb
// @overview Count rows of a table per partition, subject to modification by [`.Q.view`](https://code.kx.com/q/ref/dotq/#qview-subview).
// @param tableName {symbol} A partitioned table by name.
// @return {dict} A dictionary from partitions to row count of the table in each partition.
// @throws {NotAPartitionedTableError} If the table is not a partitioned table.
countPerPartition:{[tableName]
  rowCounts:
    @[.Q.cn get@;
      tableName;
      {[msg;tableName]
        'err.compose[`NotAPartitionedTableError; string tableName]
      }[; tableName]
     ];
  getModifiedPartitions[]!rowCounts
 };

// @kind function
// @subcategory pdb
// @overview Count rows of all tables per partition.
// @return {dict} A table keyed by partition and each column is row count of a partitioned table in each partition.
// @throws {RuntimeError: no partition} If there is no partition.
countAllPerPartition:{
  partitionedTables:getPartitionedTables[];
  countPerPartition each partitionedTables;
  rowCountsByTable:
    @[value; `.Q.pn;
      {'err.compose[`RuntimeError; "no partition"]}
     ];
  rowCountsByTable[`partition]:getModifiedPartitions[];
  `partition xkey flip rowCountsByTable
 };


export:([getPartitions; getModifiedPartitions; getPartitionField; getPartitionedTables; getSegments; getPartitionsPerSegment; countPerPartition; countAllPerPartition]);