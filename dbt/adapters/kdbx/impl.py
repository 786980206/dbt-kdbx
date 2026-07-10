from dbt.adapters.sql import SQLAdapter
from dbt.adapters.base import available
from dbt.adapters.kdbx.connections import KDBXConnectionManager
from dbt.adapters.kdbx.credentials import KDBXCredentials
from dbt.adapters.kdbx.relation import KDBXRelation
from dbt.adapters.kdbx.column import KDBXColumn
from dbt.adapters.kdbx.storage import KDBXStorage
import os
import warnings


class KDBXAdapter(SQLAdapter):
    """dbt-kdbx adapter：通过 pykx + qtk 操作 kdb+"""
    ConnectionManager = KDBXConnectionManager
    Column = KDBXColumn
    Relation = KDBXRelation

    def __init__(self, config, mp_context):
        super().__init__(config, mp_context)
        # 跟新目录配置
        credentials:KDBXCredentials = config.credentials
        project_root = getattr(config, 'project_root', None) or os.getcwd()
        if not os.path.isabs(credentials.database):
            credentials.database = os.path.abspath(os.path.join(project_root, credentials.database))
        print(f"[dbt-kdbx] database = {credentials.database}")
        self.ConnectionManager.credentials = credentials
        self._storage = KDBXStorage( credentials )

    @classmethod
    def date_function(cls):
        return ".z.p"

    @classmethod
    def is_cancelable(cls):
        return False

    def debug_query(self):
        return "1+1"

    def get_columns_in_relation(self, relation):
        return self.execute( self._storage.get_columns(relation) )

    def list_relations_without_caching(self, schema_relation):
        raw = self.execute( self._storage.list_relations() )
        # pykx SymbolVector 转为 BaseRelation 列表
        print(raw)
        relations = [self.Relation.create(database=self._storage.database.as_posix(),schema="",identifier=str(name)) for name in raw]
        return relations

    def list_schemas(self, database: str):
        return [""]  # kdb+ 无 schema，返回空 schema 占位
    
    @available
    def create_table_as(self, relation, sql):
        return self.execute( self._storage.create_table_as(relation, sql) )

    @available
    def create_view_as(self, relation, sql):
        warnings.warn("kdb+ 不原生支持 view，降级为 table")
        return self.create_table_as(relation, sql)

    def drop_relation(self, relation):
        return self.execute( self._storage.drop_relation(relation) )

    def rename_relation(self, from_relation, to_relation):
        return self.execute( self._storage.rename_relation(from_relation, to_relation) )

    def incremental_upsert(self, relation, sql, unique_keys, config):
        return self.execute( self._storage.incremental_upsert(relation, sql, unique_keys) )

    def incremental_insert(self, relation, sql, config):
        return self.execute( self._storage.incremental_insert(relation, sql) )

    def load_seed_data(self):
        pass  # 本期不实现
