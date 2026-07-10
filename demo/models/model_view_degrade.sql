{{ config(materialized='view', storage_type='serialized') }}
select sym, price from {{ ref('trade') }}
