
{% macro kdbx__create_table_as(temporary, relation, sql) -%}
  {% do adapter.create_table_as(relation, sql) %}
{%- endmacro %}


{% macro kdbx__create_view_as(relation, sql) -%}
  {% do adapter.create_view_as(relation, sql) %}
{%- endmacro %}