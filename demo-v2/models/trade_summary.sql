{{ config(materialized='view') }}
select price:sum price by sym from trade