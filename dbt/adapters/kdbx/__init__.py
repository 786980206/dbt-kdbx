from dbt.adapters.kdbx.impl import KDBXAdapter
from dbt.adapters.kdbx.credentials import KDBXCredentials
from dbt.adapters.factory import AdapterPlugin
from dbt.include import kdbx as include

Plugin = AdapterPlugin(
    adapter=KDBXAdapter,
    credentials=KDBXCredentials,
    include_path=include.PACKAGE_PATH,
)
