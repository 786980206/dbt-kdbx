from dataclasses import dataclass
from dbt.adapters.contracts.connection import Credentials


@dataclass
class KDBXCredentials(Credentials):
    """kdb+ 连接凭证：local(EmbeddedQ)/remote(SyncQConnection)"""
    database: str = "data"               # 文件系统路径，支持相对路径（基于 dbt 项目根目录）
    schema: str = ""                 # kdb+ 无 schema，留空
    mode: str = "local"
    host: str = "localhost"
    port: int = 5000
    username: str = ""
    password: str = ""
    storage_type: str = "serialized" # serialized | splayed | partitioned
    partition_field: str = "date"
    threads: int = 1

    def __post_init__(self):
        if self.mode not in ("local", "remote"):
            raise ValueError(f"mode 需为 local/remote，收到: {self.mode}")
        if self.storage_type not in ("serialized", "splayed", "partitioned"):
            raise ValueError(f"storage_type 需为 serialized/splayed/partitioned，收到: {self.storage_type}")

    @property
    def type(self) -> str:
        return "kdbx"

    @property
    def unique_field(self) -> str:
        return "local" if self.mode == "local" else f"{self.host}:{self.port}"

    def _connection_keys(self) -> tuple:
        return "mode", "host", "port", "database", "storage_type", "partition_field"
