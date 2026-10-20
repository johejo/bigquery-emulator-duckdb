-- Fixtures for tests/e2e/goclient/testdata/{scalars,errors,unverified}/external_query_mysql.txt.
-- The server's default time zone is +09:00, so TIMESTAMP tests detect failure to use UTC.
CREATE TABLE types (
  tiny TINYINT(1),
  float_value FLOAT,
  double_value DOUBLE,
  unsigned_big BIGINT UNSIGNED,
  decimal65 DECIMAL(65, 0),
  ts TIMESTAMP(6),
  year_value YEAR,
  enum_value ENUM('first', 'second'),
  set_value SET('a', 'b'),
  bit_value BIT(1),
  geometry_value GEOMETRY
);
SET time_zone = '+09:00';
INSERT INTO types VALUES (
  2, 1.5, 1.5, 18446744073709551615, 12345678901234567890123456789012345678901234567890123456789012345,
  '2026-01-02 12:04:05.123456', 2026, 'second', 'a,b', b'1', ST_GeomFromText('POINT(1 1)')
);
