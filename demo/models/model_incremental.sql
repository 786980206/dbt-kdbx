{{ config(materialized='incremental', storage_type='serialized', unique_keys=['date', 'sym']) }}
select date, sym, price, size from {{ ref('trade') }} where date = .z.d
