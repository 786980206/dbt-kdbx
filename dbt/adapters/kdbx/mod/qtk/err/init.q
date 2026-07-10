
// @kind data
// @subcategory err
// @overview A list of supported error types.
Error:`u#
  `ColumnExistsError`ColumnNameError`ColumnNotFoundError,
  `DirectoryNotFoundError`FileNotFoundError`ImportError,
  `ModuleNameError`ModuleNotFoundError`NameExistsError,
  `NotADirectoryError`NotAPartitionedTableError`PackageNotFoundError`RuntimeError`ValueError`SchemaError,
  `TableNameError`TypeError`UnknownError;


// @kind function
// @subcategory err
// @overview Compose an error message composed of error type and description.
// @param errorType {symbol} Error type, which should be one of [Error](#qtkerrerror).
// @param description {string} Error description.
// @return {string} An error message of format "{errorType}: {msg}".
// @throws {UnknownError} If `errorType` is not supported.
compose:{[errorType;description]
  if[not errorType in Error; '"UnknownError: ",string errorType];
  string[errorType],": ",description
 };

export:([Error;compose]);