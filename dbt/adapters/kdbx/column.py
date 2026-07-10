from dbt.adapters.base.column import Column


class KDBXColumn(Column):
    """q 类型与 dbt 类型一一映射"""
    TYPE_MAP = {
        "boolean": "boolean", "byte": "byte",
        "short": "short", "int": "int", "long": "long",
        "real": "real", "float": "float",
        "char": "char", "symbol": "symbol",
        "timestamp": "timestamp", "date": "date", "time": "time",
        "datetime": "datetime", "timespan": "timespan",
        "month": "month", "second": "second", "minute": "minute",
    }

    @classmethod
    def translate_type(cls, dtype: str) -> str:
        return cls.TYPE_MAP.get(dtype, dtype)
