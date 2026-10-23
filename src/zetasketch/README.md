# zetasketch

A C++ port of the HyperLogLog++ aggregator of [google/zetasketch](https://github.com/google/zetasketch)
at commit `d098c49efb5a5bc70eec89c028a38b14b930ca98`, whose sketches BigQuery's `HLL_COUNT`
functions read and write. Its hash function is Guava's `Fingerprint2011`. Both are licensed under
the Apache License 2.0, a copy of which is in [LICENSE](LICENSE); each file names the sources it
was ported from.

| File | Ported from |
| --- | --- |
| `fingerprint2011.cc` | Guava's `com/google/common/hash/Fingerprint2011.java` |
| `fingerprint2011_test.cc` | Guava's `com/google/common/hash/Fingerprint2011Test.java` |
| `data.cc` | `java/com/google/zetasketch/internal/hllplus/Data.java`, whose tables it holds unchanged |
| `hyperloglog_plus_plus.cc` | `java/com/google/zetasketch/HyperLogLogPlusPlus.java` and, under `java/com/google/zetasketch/internal/hllplus/`, `Encoding.java`, `NormalRepresentation.java`, `SparseRepresentation.java` and `State.java` |

The port reads and writes the `AggregatorStateProto` wire format directly, as `State.java` does,
so it needs neither zetasketch's `.proto` files nor protobuf. It differs from zetasketch in these
ways:

- It keeps the sparse data decoded, and rejects sparse data whose values do not fit the sparse
  encoding, where zetasketch fails later or not at all.
- It hashes no values; callers pass the hashes and pick the value type, which may be any number.
  Sketches merge when their value types are equal or one of them is unknown.
- Lowering the precisions of a sparse representation downgrades the values still in its buffer,
  which zetasketch adds as they are.
- Parsing a sketch in the normal representation drops any sparse data it also carries.
