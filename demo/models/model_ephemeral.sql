{{ config(materialized='ephemeral') }}
select sym, price from {{ ref('trade') }}
