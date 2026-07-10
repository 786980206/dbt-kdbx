from dataclasses import dataclass, field
from dbt.adapters.base.relation import BaseRelation
from dbt.adapters.contracts.relation import Policy


@dataclass(frozen=True, eq=False, repr=False)
class KDBXRelation(BaseRelation):
    """kdb+ 无 schema/database namespace，render 直接返回 identifier"""
    quote_policy: Policy = field(default_factory=lambda: Policy(
        database=False, schema=False, identifier=False,
    ))
    require_alias: bool = False
    storage_type: str = ""
    partition_field: str = ""

    @classmethod
    def create_from(cls, quoting, relation_config, **kwargs):
        # 模型节点（this）经 create_from 构建，其 config 里带 storage_type /
        # partition_field 等自定义键；抽到 kwargs 后由 create -> from_dict 落到
        # 实例字段上。this 带上后，incorporate / make_*_relation 经 replace 派生，
        # 所有子关系（含 rename / drop 收到的）都自动继承，无需额外维护。
        cfg = getattr(relation_config, "config", None)
        if cfg is not None:
            try:
                storage_type = cfg.get("storage_type")
                partition_field = cfg.get("partition_field")
            except Exception:
                storage_type = partition_field = None
            if storage_type is not None:
                kwargs["storage_type"] = storage_type
            if partition_field is not None:
                kwargs["partition_field"] = partition_field
        return super().create_from(quoting, relation_config, **kwargs)

    def render(self) -> str:
        return self.identifier or ""
