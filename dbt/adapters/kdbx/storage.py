"""存储策略：通过 pykx handle 调 qtk API，支持 serialized/splayed/partitioned 三种模式"""
from pathlib import Path
import pykx as kx
from dbt.adapters.kdbx.credentials import KDBXCredentials
from dbt.adapters.kdbx.relation import KDBXRelation


class KDBXStorage:
    def __init__(self, credentials:KDBXCredentials):
        self.credentials = credentials
        self.database = Path(self.credentials.database)

    def _table_ref(self, relation:KDBXRelation):
        """用 Python 对象构建 tableRef（不拼接 q 字符串），storage_type 与 relation 关联：
        relation 自身指定 > 项目默认配置。"""
        storage_type = relation.storage_type or self.credentials.storage_type
        partition_field = relation.partition_field or self.credentials.partition_field
        if storage_type == "serialized":
            return f":{(self.database / relation.identifier).as_posix()}"
        elif storage_type == "splayed":
            return f":{(self.database / relation.identifier).as_posix()}/"
        elif storage_type == "partitioned":
            return kx.SymbolVector([f":{self.database.as_posix()}", partition_field, relation.identifier])

    def create_table_as(self, relation, compiled_q):
        return ("{.qtk.tbl.create[x; 0!value string y]}",self._table_ref(relation), compiled_q)

    def drop_relation(self, relation):
        return ("{.qtk.tbl.drop[x]}", self._table_ref(relation))

    def rename_relation(self, from_rel, to_rel):
        return ("{.qtk.tbl.drop y;.qtk.tbl.rename[x; y]}",self._table_ref(from_rel), to_rel.identifier)

    def get_columns(self, relation):
        return ("{.qtk.tbl.columns[x]}", self._table_ref(relation))

    def incremental_upsert(self, relation, compiled_q, unique_keys):
        return ("{.qtk.tbl.upsert[x; 0!value string y;z]}", self._table_ref(relation), compiled_q, kx.SymbolVector(unique_keys))

    def incremental_insert(self, relation, compiled_q):
        return ("{.qtk.tbl.insert[x; 0!value string y]}", self._table_ref(relation), compiled_q)

    def list_relations(self):
        return ("tables[]")
