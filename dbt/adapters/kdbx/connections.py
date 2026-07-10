import os
import threading
from contextlib import contextmanager

from dbt.adapters.contracts.connection import AdapterResponse
from dbt.adapters.kdbx.credentials import KDBXCredentials
from dbt.adapters.sql.connections import SQLConnectionManager
from dbt_common.exceptions import DbtRuntimeError

# 启用 pykx 多线程特性（对远程连接有用，对本地不影响）
os.environ.setdefault('PYKX_THREADING', '1')
os.environ.setdefault('PYKX_BETA_FEATURES', '1')

import pykx as kx


class KdbxCursor:
    """伪装游标，将 pykx 的返回值转换为 dbt 所需的 DB‑API 2.0 风格。"""
    def __init__(self, connection):
        self._connection = connection          # KdbxConnection 实例
        self._handle = connection._handle      # 原始 kx.q 或 SyncQConnection
        self._mode = connection._mode          # 'local' 或 'remote'
        self._lock = connection._lock          # 仅 local 模式使用
        self._results = None                   # 存储 fetchall 结果
        self.description = None                # 列元数据（dbt 强烈依赖）
        self._last_executed = None

    def execute(self, sql, bindings=None):
        """执行查询，并解析结果为 DB‑API 格式。"""
        # 若有参数绑定，此处可扩展处理（如使用 pykx 的参数化）
        # 为简化，忽略 bindings，直接执行 sql
        try:
            if self._mode == 'local':
                with self._lock:
                    # print("\n\n")
                    # print(f"""{"+"*50}\n{sql}\n{"-"*50}""")
                    result = self._handle(sql) if isinstance(sql, str) else self._handle(*sql)
                    # print(result)
                    # print("-"*50)
                    # print("\n\n")
            else:
                result = self._handle(sql) if isinstance(sql, str) else self._handle(*sql)
        except Exception as e:
            raise DbtRuntimeError(f"kdb+ 执行错误: {e}") from e

        self._last_executed = sql
        self._results = result
        self.description = None

        return self

    def fetchall(self):
        return self._results or []

    def fetchone(self):
        return self._results[0] if self._results else None

    def fetchmany(self, size=None):
        if size is None:
            return self.fetchall()
        return self._results[:size] if self._results else []


class KdbxConnection:
    """包装 pykx 连接，提供 DB‑API 风格的 cursor() 方法。"""
    def __init__(self, handle, mode, lock=None):
        self._handle = handle
        self._mode = mode          # 'local' 或 'remote'
        self._lock = lock          # 仅 local 模式使用

    def cursor(self):
        return KdbxCursor(self)

    def close(self):
        if self._mode == 'remote' and hasattr(self._handle, 'close'):
            self._handle.close()


class KDBXConnectionManager(SQLConnectionManager):
    TYPE = "kdbx"
    credentials: KDBXCredentials = None

    # 本地模式全局锁（保护 kx.q 的并发访问）
    _q_lock = threading.Lock()

    def __init__(self, profile, mp_context):
        super().__init__(profile, mp_context)
        creds = self.profile.credentials
        self._mode = getattr(creds, 'mode', 'remote')
        self._host = getattr(creds, 'host', 'localhost')
        self._database = getattr(creds, 'database', os.path.abspath('data'))
        self._port = getattr(creds, 'port', 5000)
        self._username = getattr(creds, 'username', None)
        self._password = getattr(creds, 'password', None)

    @classmethod
    def open(cls, connection):
        """建立连接，返回包装后的 KdbxConnection 对象。"""
        if connection.state == 'open':
            return connection

        creds = connection.credentials
        mode = getattr(creds, 'mode', 'remote')

        if mode == 'local':
            # 本地嵌入式模式：使用全局 kx.q，并确保初始化
            with cls._q_lock:
                # 根据需要执行初始化命令（例如加载 qtk 库）
                kx.q(".qtk:use`qtk")
                kx.q("{.qtk.db.load[hsym x]}", cls.credentials.database)
            handle = kx.q
            lock = cls._q_lock
        else:
            # 远程连接模式：每个连接独立
            handle = kx.SyncQConnection(
                host=getattr(creds, 'host', 'localhost'),
                port=getattr(creds, 'port', 5000),
                username=getattr(creds, 'username', None),
                password=getattr(creds, 'password', None),
            )
            lock = None
            handle(".qtk:use`qtk")
            handle("{.qtk.db.load[hsym x]}", cls.credentials.database)

        wrapped = KdbxConnection(handle, mode, lock)
        connection.handle = wrapped
        connection.state = 'open'
        return connection
    
    def execute(
        self,
        sql,
        auto_begin: bool = False,
        fetch: bool = False,
        limit = None,
    ):
        """
        重写 excute sql 是元组时，直接解包传递给 cursor.execute，
        避免父类对 SQL 进行额外处理。
        """
        # 字符情况，调用父类默认处理
        if isinstance(sql, str): return super().execute(sql, auto_begin, fetch, limit)
        # 其他情况
        connection = self.get_thread_connection()
        cursor = connection.handle.cursor()
        cursor.execute(sql)
        response = self.get_response(cursor)
        if fetch:
            table = self.get_result_from_cursor(cursor, limit)
        else:
            from dbt_common.clients.agate_helper import empty_table
            table = empty_table()
        return response, table
          
    def get_response(self, cursor):
        """从游标获取执行响应（dbt 要求必须实现）。"""
        # 可根据实际情况从 cursor 中提取影响行数等信息
        return AdapterResponse(_message="OK", rows_affected=-1)

    def begin(self):
        """占位：kdb+ 不支持传统事务。"""
        pass

    def commit(self):
        """占位。"""
        pass

    def rollback(self):
        """占位。"""
        pass

    def cancel(self, connection):
        """取消执行（暂未实现）。"""
        pass

    @contextmanager
    def exception_handler(self, connection):
        """异常处理包装器，统一转换为 dbt 异常。"""
        try:
            yield
        except Exception as e:
            import traceback
            traceback.print_exc()
            raise DbtRuntimeError(f"kdb+ 执行错误: {e}") from e

    # 注意：如果需要对参数绑定做特殊处理，可以重写 add_query，
    # 但此处父类已调用 cursor.execute(sql, bindings)，我们的游标已支持。