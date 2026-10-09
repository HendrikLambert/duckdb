HTTP headers can contain sensitive information. This redacts request and response header values unless their names are explicitly allowed. It also redacts usernames and passwords in URLs, entire query strings, and fragments. Set `redact_http_logs = false` to log the original values.

Example:

```sql
CALL enable_logging('HTTP', storage = 'memory');
SET enable_external_file_cache = false;
CREATE SECRET demo (
    TYPE HTTP,
    SCOPE 'https://raw.githubusercontent.com/duckdb/duckdb/',
    EXTRA_HTTP_HEADERS MAP {'X-Demo-Token': 'dummy-secret-token'}
);

FROM read_csv('https://raw.githubusercontent.com/duckdb/duckdb/v2.0-cyanoptera/data/csv/header.csv?demo_token=dummy-url-token');

SELECT DISTINCT request.headers['X-Demo-Token'] AS token, request.url AS url
FROM duckdb_logs_parsed('HTTP');
-- token    | url
-- redacted | https://raw.githubusercontent.com/duckdb/duckdb/v2.0-cyanoptera/data/csv/header.csv?redacted

SET redact_http_logs = false;
CALL truncate_duckdb_logs();

FROM read_csv('https://raw.githubusercontent.com/duckdb/duckdb/v2.0-cyanoptera/data/csv/header.csv?demo_token=dummy-url-token');

SELECT DISTINCT request.headers['X-Demo-Token'] AS token, request.url AS url
FROM duckdb_logs_parsed('HTTP');
-- token              | url
-- dummy-secret-token | https://raw.githubusercontent.com/duckdb/duckdb/v2.0-cyanoptera/data/csv/header.csv?demo_token=dummy-url-token
```

Fixes: https://github.com/duckdblabs/duckdb-internal/issues/11194
Fixes: https://github.com/duckdb/duckdb/issues/25705
Part of: https://github.com/duckdblabs/duckdb-internal/issues/11204
