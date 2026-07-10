{{ config(materialized='table', storage_type='serialized') }}
select sym, price, size from {{ ref('trade') }} where date >= 2026.01.01
