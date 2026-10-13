SELECT x, s, a, r FROM UNNEST([
  STRUCT(1 AS x, 'a,"b"' AS s, [2, 3] AS a, STRUCT('é' AS f) AS r),
  (NULL, '', [], NULL)]) WITH OFFSET ORDER BY offset
