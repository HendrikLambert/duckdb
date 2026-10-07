CREATE TABLE list_aggregate_inputs(id INTEGER, l INTEGER[]);
INSERT INTO list_aggregate_inputs VALUES (1, [1, 2, NULL, 3]), (2, []), (3, NULL), (4, [NULL]), (5, [4]);
SELECT id, list_sum(l), list_count(l), list_aggregate(l, 'string_agg', '|') FROM list_aggregate_inputs ORDER BY id;
