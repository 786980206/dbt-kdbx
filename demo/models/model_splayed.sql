{{ config(materialized='table', storage_type='splayed') }}
select sym, price, size from {{ ref('trade') }} where date >= 2026.01.01
