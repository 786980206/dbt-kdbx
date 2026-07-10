{{ config(materialized='table', storage_type='partitioned', partition_field=var('partition_field', 'date')) }}
select date, sym, price, size from {{ ref('trade') }}
