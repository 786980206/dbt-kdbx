# dbt-kdbx 示例项目

## 运行方式

先建 trade 基表，再建各物化模型：

```bash
# 1. 建基表 trade（生成示例数据）
dbt run --project-dir example -m trade

# 2. serialized 表
dbt run --project-dir example -m model_serialized

# 3. splayed 表
dbt run --project-dir example -m model_splayed

# 4. 分区表
dbt run --project-dir example -m model_partitioned

# 5. 增量 upsert
dbt run --project-dir example -m model_incremental

# 6. view（降级为 table，不报错）
dbt run --project-dir example -m model_view_degrade

# 7. ephemeral（CTE，不在库中产生表）
dbt run --project-dir example -m model_ephemeral
```

注：database 路径 `data` 为相对路径，对应 kdb+ 工作目录下的 `data/` 文件夹。
