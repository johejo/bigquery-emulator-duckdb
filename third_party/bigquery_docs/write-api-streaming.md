# Stream data using the Storage Write API (gRPC)

The [BigQuery Storage Write API (gRPC)](https://docs.cloud.google.com/bigquery/docs/write-api) lets you continuously stream data into BigQuery tables so that records are available to query with minimal latency. Depending on your workload requirements, you can use the default stream for at-least-once writes or create streams in committed type with stream offsets for exactly-once delivery.

## Use the default stream for at-least-once semantics

If your application can accept the possibility of duplicate records appearing in the destination table, then we recommend using the [default stream](https://docs.cloud.google.com/bigquery/docs/write-api#default_stream) for streaming scenarios. The default stream uses committed type, so records are available to query as soon as the server acknowledges the write request, without exactly-once delivery.

The following code shows how to write data to the default stream:

### Java

To learn how to install and use the client library for BigQuery, see [BigQuery client libraries](https://docs.cloud.google.com/bigquery/docs/reference/storage/libraries) . For more information, see the [BigQuery Java API reference documentation](https://docs.cloud.google.com/java/docs/reference/google-cloud-bigquerystorage/latest/com.google.cloud.bigquery.storage.v1) .

To authenticate to BigQuery, set up Application Default Credentials. For more information, see [Set up authentication for client libraries](https://docs.cloud.google.com/bigquery/docs/authentication#client-libs) .

```java
import com.google.api.core.ApiFuture;
import com.google.api.core.ApiFutureCallback;
import com.google.api.core.ApiFutures;
import com.google.api.gax.batching.FlowControlSettings;
import com.google.api.gax.core.FixedExecutorProvider;
import com.google.api.gax.retrying.RetrySettings;
import com.google.cloud.bigquery.BigQuery;
import com.google.cloud.bigquery.BigQueryOptions;
import com.google.cloud.bigquery.QueryJobConfiguration;
import com.google.cloud.bigquery.TableResult;
import com.google.cloud.bigquery.storage.v1.AppendRowsRequest;
import com.google.cloud.bigquery.storage.v1.AppendRowsResponse;
import com.google.cloud.bigquery.storage.v1.BigQueryWriteClient;
import com.google.cloud.bigquery.storage.v1.BigQueryWriteSettings;
import com.google.cloud.bigquery.storage.v1.Exceptions;
import com.google.cloud.bigquery.storage.v1.Exceptions.AppendSerializationError;
import com.google.cloud.bigquery.storage.v1.Exceptions.MaximumRequestCallbackWaitTimeExceededException;
import com.google.cloud.bigquery.storage.v1.Exceptions.StorageException;
import com.google.cloud.bigquery.storage.v1.Exceptions.StreamWriterClosedException;
import com.google.cloud.bigquery.storage.v1.JsonStreamWriter;
import com.google.cloud.bigquery.storage.v1.TableName;
import com.google.common.util.concurrent.MoreExecutors;
import com.google.protobuf.ByteString;
import com.google.protobuf.Descriptors.DescriptorValidationException;
import java.io.IOException;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.Phaser;
import java.util.concurrent.atomic.AtomicInteger;
import javax.annotation.concurrent.GuardedBy;
import org.json.JSONArray;
import org.json.JSONObject;
import org.threeten.bp.Duration;

public class WriteToDefaultStream {

  public static void runWriteToDefaultStream()
      throws DescriptorValidationException, InterruptedException, IOException {
    // TODO(developer): Replace these variables before running the sample.
    String projectId = "MY_PROJECT_ID";
    String datasetName = "MY_DATASET_NAME";
    String tableName = "MY_TABLE_NAME";
    writeToDefaultStream(projectId, datasetName, tableName);
  }

  private static ByteString buildByteString() {
    byte[] bytes = new byte[] {1, 2, 3, 4, 5};
    return ByteString.copyFrom(bytes);
  }

  // Create a JSON object that is compatible with the table schema.
  private static JSONObject buildRecord(int i, int j) {
    JSONObject record = new JSONObject();
    StringBuilder sbSuffix = new StringBuilder();
    for (int k = 0; k < j; k++) {
      sbSuffix.append(k);
    }
    record.put("test_string", String.format("record %03d-%03d %s", i, j, sbSuffix.toString()));
    ByteString byteString = buildByteString();
    record.put("test_bytes", byteString);
    record.put(
        "test_geo",
        "POLYGON((-124.49 47.35,-124.49 40.73,-116.49 40.73,-116.49 47.35,-124.49 47.35))");
    return record;
  }

  public static void writeToDefaultStream(String projectId, String datasetName, String tableName)
      throws DescriptorValidationException, InterruptedException, IOException {
    TableName parentTable = TableName.of(projectId, datasetName, tableName);

    DataWriter writer = new DataWriter();
    // One time initialization for the worker.
    writer.initialize(parentTable);

    // Write two batches of fake data to the stream, each with 10 JSON records.  Data may be
    // batched up to the maximum request size:
    // https://cloud.google.com/bigquery/quotas#write-api-limits
    for (int i = 0; i < 2; i++) {
      JSONArray jsonArr = new JSONArray();
      for (int j = 0; j < 10; j++) {
        JSONObject record = buildRecord(i, j);
        jsonArr.put(record);
      }

      writer.append(new AppendContext(jsonArr));
    }

    // Final cleanup for the stream during worker teardown.
    writer.cleanup();
    verifyExpectedRowCount(parentTable, 12L);
    System.out.println("Appended records successfully.");
  }

  private static void verifyExpectedRowCount(TableName parentTable, long expectedRowCount)
      throws InterruptedException {
    String queryRowCount =
        "SELECT COUNT(*) FROM `"
            + parentTable.getProject()
            + "."
            + parentTable.getDataset()
            + "."
            + parentTable.getTable()
            + "`";
    QueryJobConfiguration queryConfig = QueryJobConfiguration.newBuilder(queryRowCount).build();
    BigQuery bigquery = BigQueryOptions.getDefaultInstance().getService();
    TableResult results = bigquery.query(queryConfig);
    long countRowsActual =
        Long.parseLong(results.getValues().iterator().next().get("f0_").getStringValue());
    if (countRowsActual != expectedRowCount) {
      throw new RuntimeException(
          "Unexpected row count. Expected: " + expectedRowCount + ". Actual: " + countRowsActual);
    }
  }

  private static class AppendContext {

    JSONArray data;

    AppendContext(JSONArray data) {
      this.data = data;
    }
  }

  private static class DataWriter {

    private static final int MAX_RECREATE_COUNT = 3;

    private BigQueryWriteClient client;

    // Track the number of in-flight requests to wait for all responses before shutting down.
    private final Phaser inflightRequestCount = new Phaser(1);
    private final Object lock = new Object();
    private JsonStreamWriter streamWriter;

    @GuardedBy("lock")
    private RuntimeException error = null;

    private AtomicInteger recreateCount = new AtomicInteger(0);

    private JsonStreamWriter createStreamWriter(String tableName)
        throws DescriptorValidationException, IOException, InterruptedException {
      // Configure in-stream automatic retry settings.
      // Error codes that are immediately retried:
      // * ABORTED, UNAVAILABLE, CANCELLED, INTERNAL, DEADLINE_EXCEEDED
      // Error codes that are retried with exponential backoff:
      // * RESOURCE_EXHAUSTED
      RetrySettings retrySettings =
          RetrySettings.newBuilder()
              .setInitialRetryDelay(Duration.ofMillis(500))
              .setRetryDelayMultiplier(1.1)
              .setMaxAttempts(5)
              .setMaxRetryDelay(Duration.ofMinutes(1))
              .build();

      // Use the JSON stream writer to send records in JSON format. Specify the table name to write
      // to the default stream.
      // For more information about JsonStreamWriter, see:
      // https://googleapis.dev/java/google-cloud-bigquerystorage/latest/com/google/cloud/bigquery/storage/v1/JsonStreamWriter.html
      return JsonStreamWriter.newBuilder(tableName, client)
          .setExecutorProvider(FixedExecutorProvider.create(Executors.newScheduledThreadPool(10)))
          .setChannelProvider(
              BigQueryWriteSettings.defaultGrpcTransportProviderBuilder()
                  .setKeepAliveTime(org.threeten.bp.Duration.ofMinutes(1))
                  .setKeepAliveTimeout(org.threeten.bp.Duration.ofMinutes(1))
                  .setKeepAliveWithoutCalls(true)
                  .setChannelsPerCpu(2)
                  .build())
          .setEnableConnectionPool(true)
          // This will allow connection pool to scale up better.
          .setFlowControlSettings(
              FlowControlSettings.newBuilder().setMaxOutstandingElementCount(100L).build())
          // If value is missing in json and there is a default value configured on bigquery
          // column, apply the default value to the missing value field.
          .setDefaultMissingValueInterpretation(
              AppendRowsRequest.MissingValueInterpretation.DEFAULT_VALUE)
          .setRetrySettings(retrySettings)
          .build();
    }

    public void initialize(TableName parentTable)
        throws DescriptorValidationException, IOException, InterruptedException {
      // Initialize client without settings, internally within stream writer a new client will be
      // created with full settings.
      client = BigQueryWriteClient.create();

      streamWriter = createStreamWriter(parentTable.toString());
    }

    public void append(AppendContext appendContext)
        throws DescriptorValidationException, IOException, InterruptedException {
      synchronized (this.lock) {
        if (!streamWriter.isUserClosed()
            && streamWriter.isClosed()
            && recreateCount.getAndIncrement() < MAX_RECREATE_COUNT) {
          streamWriter = createStreamWriter(streamWriter.getStreamName());
          this.error = null;
        }
        // If earlier appends have failed, we need to reset before continuing.
        if (this.error != null) {
          throw this.error;
        }
      }
      // Append asynchronously for increased throughput.
      ApiFuture<AppendRowsResponse> future = streamWriter.append(appendContext.data);
      ApiFutures.addCallback(
          future, new AppendCompleteCallback(this, appendContext), MoreExecutors.directExecutor());

      // Increase the count of in-flight requests.
      inflightRequestCount.register();
    }

    public void cleanup() {
      // Wait for all in-flight requests to complete.
      inflightRequestCount.arriveAndAwaitAdvance();

      client.close();
      // Close the connection to the server.
      streamWriter.close();

      // Verify that no error occurred in the stream.
      synchronized (this.lock) {
        if (this.error != null) {
          throw this.error;
        }
      }
    }

    static class AppendCompleteCallback implements ApiFutureCallback<AppendRowsResponse> {

      private final DataWriter parent;
      private final AppendContext appendContext;

      public AppendCompleteCallback(DataWriter parent, AppendContext appendContext) {
        this.parent = parent;
        this.appendContext = appendContext;
      }

      public void onSuccess(AppendRowsResponse response) {
        System.out.format("Append success\n");
        this.parent.recreateCount.set(0);
        done();
      }

      public void onFailure(Throwable throwable) {
        if (throwable instanceof AppendSerializationError) {
          AppendSerializationError ase = (AppendSerializationError) throwable;
          Map<Integer, String> rowIndexToErrorMessage = ase.getRowIndexToErrorMessage();
          if (rowIndexToErrorMessage.size() > 0) {
            // Omit the faulty rows
            JSONArray dataNew = new JSONArray();
            for (int i = 0; i < appendContext.data.length(); i++) {
              if (!rowIndexToErrorMessage.containsKey(i)) {
                dataNew.put(appendContext.data.get(i));
              } else {
                // process faulty rows by placing them on a dead-letter-queue, for instance
              }
            }

            // Retry the remaining valid rows, but using a separate thread to
            // avoid potentially blocking while we are in a callback.
            if (dataNew.length() > 0) {
              try {
                this.parent.append(new AppendContext(dataNew));
              } catch (DescriptorValidationException e) {
                throw new RuntimeException(e);
              } catch (IOException e) {
                throw new RuntimeException(e);
              } catch (InterruptedException e) {
                throw new RuntimeException(e);
              }
            }
            // Mark the existing attempt as done since we got a response for it
            done();
            return;
          }
        }

        boolean resendRequest = false;
        if (throwable instanceof MaximumRequestCallbackWaitTimeExceededException) {
          resendRequest = true;
        } else if (throwable instanceof StreamWriterClosedException) {
          if (!parent.streamWriter.isUserClosed()) {
            resendRequest = true;
          }
        }
        if (resendRequest) {
          // Retry this request.
          try {
            this.parent.append(new AppendContext(appendContext.data));
          } catch (DescriptorValidationException e) {
            throw new RuntimeException(e);
          } catch (IOException e) {
            throw new RuntimeException(e);
          } catch (InterruptedException e) {
            throw new RuntimeException(e);
          }
          // Mark the existing attempt as done since we got a response for it
          done();
          return;
        }

        synchronized (this.parent.lock) {
          if (this.parent.error == null) {
            StorageException storageException = Exceptions.toStorageException(throwable);
            this.parent.error =
                (storageException != null) ? storageException : new RuntimeException(throwable);
          }
        }
        done();
      }

      private void done() {
        // Reduce the count of in-flight requests.
        this.parent.inflightRequestCount.arriveAndDeregister();
      }
    }
  }
}
```

### Node.js

To learn how to install and use the client library for BigQuery, see [BigQuery client libraries](https://docs.cloud.google.com/bigquery/docs/reference/storage/libraries) .

To authenticate to BigQuery, set up Application Default Credentials. For more information, see [Set up authentication for client libraries](https://docs.cloud.google.com/bigquery/docs/authentication#client-libs) .

```
const {adapt, managedwriter} = require('@google-cloud/bigquery-storage');
const {WriterClient, JSONWriter} = managedwriter;

async function appendJSONRowsDefaultStream() {
  /**
   * TODO(developer): Uncomment the following lines before running the sample.
   */
  // projectId = 'my_project';
  // datasetId = 'my_dataset';
  // tableId = 'my_table';

  const destinationTable = `projects/${projectId}/datasets/${datasetId}/tables/${tableId}`;
  const writeClient = new WriterClient({projectId});

  try {
    const writeStream = await writeClient.getWriteStream({
      streamId: `${destinationTable}/streams/_default`,
      view: 'FULL',
    });
    const protoDescriptor = adapt.convertStorageSchemaToProto2Descriptor(
      writeStream.tableSchema,
      'root',
    );

    const connection = await writeClient.createStreamConnection({
      streamId: managedwriter.DefaultStream,
      destinationTable,
    });
    const streamId = connection.getStreamId();

    const writer = new JSONWriter({
      streamId,
      connection,
      protoDescriptor,
    });

    let rows = [];
    const pendingWrites = [];

    // Row 1
    let row = {
      row_num: 1,
      customer_name: 'Octavia',
    };
    rows.push(row);

    // Row 2
    row = {
      row_num: 2,
      customer_name: 'Turing',
    };
    rows.push(row);

    // Send batch.
    let pw = writer.appendRows(rows);
    pendingWrites.push(pw);

    rows = [];

    // Row 3
    row = {
      row_num: 3,
      customer_name: 'Bell',
    };
    rows.push(row);

    // Send batch.
    pw = writer.appendRows(rows);
    pendingWrites.push(pw);

    const results = await Promise.all(
      pendingWrites.map(pw => pw.getResult()),
    );
    console.log('Write results:', results);
  } catch (err) {
    console.log(err);
  } finally {
    writeClient.close();
  }
}
```

### Python

This example shows how to insert a record with two fields using the default stream:

```
from google.cloud import bigquery_storage_v1
from google.cloud.bigquery_storage_v1 import types
from google.cloud.bigquery_storage_v1 import writer
from google.protobuf import descriptor_pb2
import logging
import json

import sample_data_pb2

# The list of columns from the table's schema to search in the given data to write to BigQuery.
TABLE_COLUMNS_TO_CHECK = [
    "name",
    "age"
    ]

# Function to create a batch of row data to be serialized.
def create_row_data(data):
    row = sample_data_pb2.SampleData()
    for field in TABLE_COLUMNS_TO_CHECK:
      # Optional fields will be passed as null if not provided
      if field in data:
        setattr(row, field, data[field])
    return row.SerializeToString()

class BigQueryStorageWriteAppend(object):

    # The stream name is: projects/{project}/datasets/{dataset}/tables/{table}/_default
    def append_rows_proto2(
        project_id: str, dataset_id: str, table_id: str, data: dict
    ):

        write_client = bigquery_storage_v1.BigQueryWriteClient()
        parent = write_client.table_path(project_id, dataset_id, table_id)
        stream_name = f'{parent}/_default'
        write_stream = types.WriteStream()

        # Create a template with fields needed for the first request.
        request_template = types.AppendRowsRequest()

        # The request must contain the stream name.
        request_template.write_stream = stream_name

        # Generating the protocol buffer representation of the message descriptor.
        proto_schema = types.ProtoSchema()
        proto_descriptor = descriptor_pb2.DescriptorProto()
        sample_data_pb2.SampleData.DESCRIPTOR.CopyToProto(proto_descriptor)
        proto_schema.proto_descriptor = proto_descriptor
        proto_data = types.AppendRowsRequest.ProtoData()
        proto_data.writer_schema = proto_schema
        request_template.proto_rows = proto_data

        # Construct an AppendRowsStream to send an arbitrary number of requests to a stream.
        append_rows_stream = writer.AppendRowsStream(write_client, request_template)

        # Append proto2 serialized bytes to the serialized_rows repeated field using create_row_data.
        proto_rows = types.ProtoRows()
        for row in data:
            proto_rows.serialized_rows.append(create_row_data(row))

        # Appends data to the given stream.
        request = types.AppendRowsRequest()
        proto_data = types.AppendRowsRequest.ProtoData()
        proto_data.rows = proto_rows
        request.proto_rows = proto_data

        append_rows_stream.send(request)

        print(f"Rows to table: '{parent}' have been written.")

if __name__ == "__main__":

    ###### Uncomment the below block to provide additional logging capabilities ######
    #logging.basicConfig(
    #    level=logging.DEBUG,
    #    format="%(asctime)s [%(levelname)s] %(message)s",
    #    handlers=[
    #        logging.StreamHandler()
    #    ]
    #)
    ###### Uncomment the above block to provide additional logging capabilities ######

    with open('entries.json', 'r') as json_file:
        data = json.load(json_file)
    # Change this to your specific BigQuery project, dataset, table details
    BigQueryStorageWriteAppend.append_rows_proto2("PROJECT_ID","DATASET_ID", "TABLE_ID ",data=data)
```

This code example depends on the compiled protocol module `sample_data_pb2.py` . To create the compiled module, execute the `protoc --python_out=. sample_data.proto` command, where `protoc` is the protocol buffer compiler. The `sample_data.proto` file defines the format of the messages used in the Python example. To install the `protoc` compiler, follow the instructions in [Protocol Buffers - Google's data interchange format](https://github.com/protocolbuffers/protobuf) .

Here are the contents of the `sample_data.proto` file:

```
message SampleData {
  required string name = 1;
  required int64 age = 2;
}
```

This script consumes the `entries.json` file, which contains sample row data to be inserted into the BigQuery table:

```
{"name": "Jim", "age": 35}
{"name": "Jane", "age": 27}
```

### Rust

```rust
use anyhow::Result;
use arrow::datatypes::{DataType, Field, Schema};
use arrow::ipc::writer::StreamWriter;
use arrow_json::ReaderBuilder;
use google_cloud_bigquery::client::Write;
use google_cloud_bigquery::model::{ArrowRecordBatch, ArrowSchema};
use std::sync::Arc;
use tokio::task::JoinSet;

// The client library doesn't support a built-in JSON API surface.
//
// This example demonstrates how to write JSON data to BigQuery by first
// converting it to Arrow record batches using the `arrow-json` crate.

pub async fn sample(project_id: &str, dataset_id: &str, table_id: &str) -> anyhow::Result<()> {
    let client = Write::builder().build().await?;

    // Define the table schema
    let schema = Arc::new(Schema::new(vec![
        Field::new("string", DataType::Utf8, false),
        Field::new("int", DataType::Int64, false),
    ]));

    // Initialize an IPC stream writer and extract the serialized schema
    let mut ipc_writer = StreamWriter::try_new(Vec::new(), &schema)?;
    let schema_buf = std::mem::take(ipc_writer.get_mut());

    let table = format!("projects/{project_id}/datasets/{dataset_id}/tables/{table_id}");
    // Create a writer for the default stream
    let writer = client
        .open_default_stream(table)
        .build_arrow(ArrowSchema::new().set_serialized_schema(schema_buf))
        .await?;

    // Create a decoder to convert JSON to Arrow
    let mut decoder = ReaderBuilder::new(schema).build_decoder()?;

    let mut writes = JoinSet::new();
    for i in 0..100 {
        for j in 0..10 {
            // Generate example data (JSON bytes).
            let json = format!(r#"{{"string": "batch {i}", "int": {}}}"#, 10 * i + j);
            let _ = decoder.decode(json.as_bytes())?;
        }
        if let Some(batch) = decoder.flush()? {
            // Serialize the batch
            let batch = {
                ipc_writer.write(&batch)?;
                let batch_bytes = std::mem::take(ipc_writer.get_mut());
                ArrowRecordBatch::new().set_serialized_record_batch(batch_bytes)
            };
            // Write the batch to BigQuery
            writes.spawn(writer.append(batch).send());
        }
    }
    let results: Result<Vec<_>, _> = writes.join_all().await.into_iter().collect();
    let _ = results?;
    println!("Successfully wrote 100 record batches of 10 rows each.");

    Ok(())
}
```

### Use multiplexing

You enable [multiplexing](https://docs.cloud.google.com/bigquery/docs/write-api-best-practices#connection_pool_management) at the stream writer level for default stream only. To enable multiplexing in Java, call the `setEnableConnectionPool` method when you construct a `StreamWriter` or `JsonStreamWriter` object.

After enabling the connection pool, the Java client library manages your connections in the background, scaling up connections if the existing connections are considered too busy. For automatic scaling up to be more effective, you should consider lowering the `maxInflightRequests` limit.

```
// One possible way for constructing StreamWriter
StreamWriter.newBuilder(streamName)
              .setWriterSchema(protoSchema)
              .setEnableConnectionPool(true)
              .setMaxInflightRequests(100)
              .build();
// One possible way for constructing JsonStreamWriter
JsonStreamWriter.newBuilder(tableName, bigqueryClient)
              .setEnableConnectionPool(true)
              .setMaxInflightRequests(100)
              .build();
```

To enable multiplexing in Go, see [Connection Sharing (Multiplexing)](https://pkg.go.dev/cloud.google.com/go/bigquery/storage/managedwriter#hdr-Connection_Sharing__Multiplexing_) .

To enable multiplexing in Rust, call the `with_multiplexing(true)` method when you build a default stream writer:

```
let writer = client
    .open_default_stream(table_name)
    .with_multiplexing(true)
    .build_arrow(schema)
    .await?;
```

## Use committed type for exactly-once semantics

If you need exactly-once write semantics, create a write stream in committed type. In committed type, records are available for query as soon as the client receives acknowledgment from the backend.

Committed type provides exactly-once delivery within a stream through the use of record offsets. By using record offsets, the application specifies the next append offset in each call to [`AppendRows`](https://docs.cloud.google.com/bigquery/docs/reference/storage/rpc/google.cloud.bigquery.storage.v1#google.cloud.bigquery.storage.v1.BigQueryWrite.AppendRows) . The write operation is only performed if the offset value matches the next append offset. For more information, see [Manage stream offsets to achieve exactly-once semantics](https://docs.cloud.google.com/bigquery/docs/write-api-best-practices#manage_stream_offsets_to_achieve_exactly-once_semantics) .

If you don't provide an offset, then records are appended to the current end of the stream. In that case, if an append request returns an error, retrying it could result in the record appearing more than once in the stream.

To use committed type, perform the following steps:

### Java

1.  Call `CreateWriteStream` to create one or more streams in committed type.
2.  For each stream, call `AppendRows` in a loop to write batches of records.
3.  Call `FinalizeWriteStream` for each stream to release the stream. After you call this method, you cannot write any more rows to the stream. This step is optional in committed type, but helps to prevent exceeding the limit on active streams. For more information, see [Limit the rate of stream creation](https://docs.cloud.google.com/bigquery/docs/write-api-best-practices#limit_the_rate_of_stream_creation) .

### Node.js

1.  Call `createWriteStreamFullResponse` to create one or more streams in committed type.
2.  For each stream, call `appendRows` in a loop to write batches of records.
3.  Call `finalize` for each stream to release the stream. After you call this method, you cannot write any more rows to the stream. This step is optional in committed type, but helps to prevent exceeding the limit on active streams. For more information, see [Limit the rate of stream creation](https://docs.cloud.google.com/bigquery/docs/write-api-best-practices#limit_the_rate_of_stream_creation) .

### Rust

1.  Call `create_stream` on the `Write` client and build a `CommittedWriter` to create one or more streams in committed type.
2.  For each stream, call `append` in a loop to write batches of records.
3.  Call `finalize` for each stream to release the stream. After you call this method, you can't write any more rows to the stream. This step is optional in committed type, but helps to prevent exceeding the limit on active streams. For more information, see [Limit the rate of stream creation](https://docs.cloud.google.com/bigquery/docs/write-api-best-practices#limit_the_rate_of_stream_creation) .

You cannot delete a stream explicitly. Streams follow the system-defined time to live (TTL):

- A committed stream has a TTL of three days if there is no traffic on the stream.
- A buffered stream by default has a TTL of seven days if there is no traffic on the stream.

The following code shows how to use committed type:

### Java

To learn how to install and use the client library for BigQuery, see [BigQuery client libraries](https://docs.cloud.google.com/bigquery/docs/reference/storage/libraries) . For more information, see the [BigQuery Java API reference documentation](https://docs.cloud.google.com/java/docs/reference/google-cloud-bigquerystorage/latest/com.google.cloud.bigquery.storage.v1) .

To authenticate to BigQuery, set up Application Default Credentials. For more information, see [Set up authentication for client libraries](https://docs.cloud.google.com/bigquery/docs/authentication#client-libs) .

```java
import com.google.api.core.ApiFuture;
import com.google.api.core.ApiFutureCallback;
import com.google.api.core.ApiFutures;
import com.google.api.gax.retrying.RetrySettings;
import com.google.cloud.bigquery.storage.v1.AppendRowsResponse;
import com.google.cloud.bigquery.storage.v1.BigQueryWriteClient;
import com.google.cloud.bigquery.storage.v1.CreateWriteStreamRequest;
import com.google.cloud.bigquery.storage.v1.Exceptions;
import com.google.cloud.bigquery.storage.v1.Exceptions.StorageException;
import com.google.cloud.bigquery.storage.v1.FinalizeWriteStreamResponse;
import com.google.cloud.bigquery.storage.v1.JsonStreamWriter;
import com.google.cloud.bigquery.storage.v1.TableName;
import com.google.cloud.bigquery.storage.v1.WriteStream;
import com.google.common.util.concurrent.MoreExecutors;
import com.google.protobuf.Descriptors.DescriptorValidationException;
import java.io.IOException;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Phaser;
import javax.annotation.concurrent.GuardedBy;
import org.json.JSONArray;
import org.json.JSONObject;
import org.threeten.bp.Duration;

public class WriteCommittedStream {

  public static void runWriteCommittedStream()
      throws DescriptorValidationException, InterruptedException, IOException {
    // TODO(developer): Replace these variables before running the sample.
    String projectId = "MY_PROJECT_ID";
    String datasetName = "MY_DATASET_NAME";
    String tableName = "MY_TABLE_NAME";

    writeCommittedStream(projectId, datasetName, tableName);
  }

  public static void writeCommittedStream(String projectId, String datasetName, String tableName)
      throws DescriptorValidationException, InterruptedException, IOException {
    BigQueryWriteClient client = BigQueryWriteClient.create();
    TableName parentTable = TableName.of(projectId, datasetName, tableName);

    DataWriter writer = new DataWriter();
    // One time initialization.
    writer.initialize(parentTable, client);

    try {
      // Write two batches of fake data to the stream, each with 10 JSON records.  Data may be
      // batched up to the maximum request size:
      // https://cloud.google.com/bigquery/quotas#write-api-limits
      long offset = 0;
      for (int i = 0; i < 2; i++) {
        // Create a JSON object that is compatible with the table schema.
        JSONArray jsonArr = new JSONArray();
        for (int j = 0; j < 10; j++) {
          JSONObject record = new JSONObject();
          record.put("col1", String.format("batch-record %03d-%03d", i, j));
          jsonArr.put(record);
        }
        writer.append(jsonArr, offset);
        offset += jsonArr.length();
      }
    } catch (ExecutionException e) {
      // If the wrapped exception is a StatusRuntimeException, check the state of the operation.
      // If the state is INTERNAL, CANCELLED, or ABORTED, you can retry. For more information, see:
      // https://grpc.github.io/grpc-java/javadoc/io/grpc/StatusRuntimeException.html
      System.out.println("Failed to append records. \n" + e);
    }

    // Final cleanup for the stream.
    writer.cleanup(client);
    System.out.println("Appended records successfully.");
  }

  // A simple wrapper object showing how the stateful stream writer should be used.
  private static class DataWriter {

    private JsonStreamWriter streamWriter;
    // Track the number of in-flight requests to wait for all responses before shutting down.
    private final Phaser inflightRequestCount = new Phaser(1);

    private final Object lock = new Object();

    @GuardedBy("lock")
    private RuntimeException error = null;

    void initialize(TableName parentTable, BigQueryWriteClient client)
        throws IOException, DescriptorValidationException, InterruptedException {
      // Initialize a write stream for the specified table.
      // For more information on WriteStream.Type, see:
      // https://googleapis.dev/java/google-cloud-bigquerystorage/latest/com/google/cloud/bigquery/storage/v1/WriteStream.Type.html
      WriteStream stream = WriteStream.newBuilder().setType(WriteStream.Type.COMMITTED).build();

      CreateWriteStreamRequest createWriteStreamRequest =
          CreateWriteStreamRequest.newBuilder()
              .setParent(parentTable.toString())
              .setWriteStream(stream)
              .build();
      WriteStream writeStream = client.createWriteStream(createWriteStreamRequest);

      // Configure in-stream automatic retry settings.
      // Error codes that are immediately retried:
      // * ABORTED, UNAVAILABLE, CANCELLED, INTERNAL, DEADLINE_EXCEEDED
      // Error codes that are retried with exponential backoff:
      // * RESOURCE_EXHAUSTED
      RetrySettings retrySettings =
          RetrySettings.newBuilder()
              .setInitialRetryDelay(Duration.ofMillis(500))
              .setRetryDelayMultiplier(1.1)
              .setMaxAttempts(5)
              .setMaxRetryDelay(Duration.ofMinutes(1))
              .build();

      // Use the JSON stream writer to send records in JSON format.
      // For more information about JsonStreamWriter, see:
      // https://googleapis.dev/java/google-cloud-bigquerystorage/latest/com/google/cloud/bigquery/storage/v1/JsonStreamWriter.html
      streamWriter =
          JsonStreamWriter.newBuilder(writeStream.getName(), writeStream.getTableSchema(), client)
              .setRetrySettings(retrySettings)
              .build();
    }

    public void append(JSONArray data, long offset)
        throws DescriptorValidationException, IOException, ExecutionException {
      synchronized (this.lock) {
        // If earlier appends have failed, we need to reset before continuing.
        if (this.error != null) {
          throw this.error;
        }
      }
      // Append asynchronously for increased throughput.
      ApiFuture<AppendRowsResponse> future = streamWriter.append(data, offset);
      ApiFutures.addCallback(
          future, new DataWriter.AppendCompleteCallback(this), MoreExecutors.directExecutor());
      // Increase the count of in-flight requests.
      inflightRequestCount.register();
    }

    public void cleanup(BigQueryWriteClient client) {
      // Wait for all in-flight requests to complete.
      inflightRequestCount.arriveAndAwaitAdvance();

      // Close the connection to the server.
      streamWriter.close();

      // Verify that no error occurred in the stream.
      synchronized (this.lock) {
        if (this.error != null) {
          throw this.error;
        }
      }

      // Finalize the stream.
      FinalizeWriteStreamResponse finalizeResponse =
          client.finalizeWriteStream(streamWriter.getStreamName());
      System.out.println("Rows written: " + finalizeResponse.getRowCount());
    }

    public String getStreamName() {
      return streamWriter.getStreamName();
    }

    static class AppendCompleteCallback implements ApiFutureCallback<AppendRowsResponse> {

      private final DataWriter parent;

      public AppendCompleteCallback(DataWriter parent) {
        this.parent = parent;
      }

      public void onSuccess(AppendRowsResponse response) {
        System.out.format("Append %d success\n", response.getAppendResult().getOffset().getValue());
        done();
      }

      public void onFailure(Throwable throwable) {
        synchronized (this.parent.lock) {
          if (this.parent.error == null) {
            StorageException storageException = Exceptions.toStorageException(throwable);
            this.parent.error =
                (storageException != null) ? storageException : new RuntimeException(throwable);
          }
        }
        System.out.format("Error: %s\n", throwable.toString());
        done();
      }

      private void done() {
        // Reduce the count of in-flight requests.
        this.parent.inflightRequestCount.arriveAndDeregister();
      }
    }
  }
}
```

### Node.js

To learn how to install and use the client library for BigQuery, see [BigQuery client libraries](https://docs.cloud.google.com/bigquery/docs/reference/storage/libraries) .

To authenticate to BigQuery, set up Application Default Credentials. For more information, see [Set up authentication for client libraries](https://docs.cloud.google.com/bigquery/docs/authentication#client-libs) .

```
const {adapt, managedwriter} = require('@google-cloud/bigquery-storage');
const {WriterClient, JSONWriter} = managedwriter;

async function appendJSONRowsCommittedStream() {
  /**
   * TODO(developer): Uncomment the following lines before running the sample.
   */
  // projectId = 'my_project';
  // datasetId = 'my_dataset';
  // tableId = 'my_table';

  const destinationTable = `projects/${projectId}/datasets/${datasetId}/tables/${tableId}`;
  const streamType = managedwriter.CommittedStream;
  const writeClient = new WriterClient({projectId});

  try {
    const writeStream = await writeClient.createWriteStreamFullResponse({
      streamType,
      destinationTable,
    });
    const streamId = writeStream.name;
    console.log(`Stream created: ${streamId}`);

    const protoDescriptor = adapt.convertStorageSchemaToProto2Descriptor(
      writeStream.tableSchema,
      'root',
    );

    const connection = await writeClient.createStreamConnection({
      streamId,
    });

    const writer = new JSONWriter({
      streamId,
      connection,
      protoDescriptor,
    });

    let rows = [];
    const pendingWrites = [];

    // Row 1
    let row = {
      row_num: 1,
      customer_name: 'Octavia',
    };
    rows.push(row);

    // Row 2
    row = {
      row_num: 2,
      customer_name: 'Turing',
    };
    rows.push(row);

    // Send batch.
    let pw = writer.appendRows(rows);
    pendingWrites.push(pw);

    rows = [];

    // Row 3
    row = {
      row_num: 3,
      customer_name: 'Bell',
    };
    rows.push(row);

    // Send batch.
    pw = writer.appendRows(rows);
    pendingWrites.push(pw);

    const results = await Promise.all(
      pendingWrites.map(pw => pw.getResult()),
    );
    console.log('Write results:', results);

    const {rowCount} = await connection.finalize();
    console.log(`Row count: ${rowCount}`);
  } catch (err) {
    console.log(err);
  } finally {
    writeClient.close();
  }
}
```

### Rust

```rust
use anyhow::Result;
use arrow::datatypes::{DataType, Field, Schema};
use arrow::ipc::writer::StreamWriter;
use arrow_json::ReaderBuilder;
use google_cloud_bigquery::client::Write;
use google_cloud_bigquery::model::{ArrowRecordBatch, ArrowSchema};
use google_cloud_bigquery::write::CommittedWriter;
use google_cloud_bigquery::write::format::Arrow;
use std::sync::Arc;
use tokio::task::JoinSet;

// The client library doesn't support a built-in JSON API surface.
//
// This example demonstrates how to write JSON data to BigQuery by first
// converting it to Arrow record batches using the `arrow-json` crate.

pub async fn sample(project_id: &str, dataset_id: &str, table_id: &str) -> anyhow::Result<()> {
    let client = Write::builder().build().await?;

    // Define the table schema
    let schema = Arc::new(Schema::new(vec![
        Field::new("string", DataType::Utf8, false),
        Field::new("int", DataType::Int64, false),
    ]));

    // Initialize an IPC stream writer and extract the serialized schema
    let mut ipc_writer = StreamWriter::try_new(Vec::new(), &schema)?;
    let schema_buf = std::mem::take(ipc_writer.get_mut());

    let table = format!("projects/{project_id}/datasets/{dataset_id}/tables/{table_id}");
    // Create a writer for a committed stream
    let writer: CommittedWriter<Arrow> = client
        .create_stream(table)
        .build_arrow(ArrowSchema::new().set_serialized_schema(schema_buf))
        .await?;

    // Create a decoder to convert JSON to Arrow
    let mut decoder = ReaderBuilder::new(schema).build_decoder()?;

    let mut writes = JoinSet::new();
    for i in 0..100 {
        for j in 0..10 {
            // Generate example data (JSON bytes)
            let json = format!(r#"{{"string": "batch {i}", "int": {}}}"#, 10 * i + j);
            let _ = decoder.decode(json.as_bytes())?;
        }
        if let Some(batch) = decoder.flush()? {
            // Serialize the batch
            let batch = {
                ipc_writer.write(&batch)?;
                let batch_bytes = std::mem::take(ipc_writer.get_mut());
                ArrowRecordBatch::new().set_serialized_record_batch(batch_bytes)
            };
            // Write the batch to the committed stream
            writes.spawn(writer.append(batch).set_offset(10 * i).send());
        }
    }
    let results: Result<Vec<_>, _> = writes.join_all().await.into_iter().collect();
    let _ = results?;

    // Finalize the stream, preventing further writes
    let resp = writer.finalize().await?;
    println!(
        "Successfully finalized the stream with {} rows",
        resp.row_count
    );

    Ok(())
}
```

## Use the Apache Arrow format to ingest data

The following code shows how to ingest data using the Apache Arrow format.

### Python

This example shows how to ingest a serialized PyArrow table using the default stream. For a more detailed, end-to-end example, see the [PyArrow example on GitHub](https://github.com/googleapis/google-cloud-python/tree/main/packages/google-cloud-bigquery-storage/samples/pyarrow) .

```
from google.cloud.bigquery_storage_v1 import types as gapic_types
from google.cloud.bigquery_storage_v1.writer import AppendRowsStream
from google.cloud import bigquery_storage_v1

def append_rows_with_pyarrow(
  pyarrow_table: pyarrow.Table,
  project_id: str,
  dataset_id: str,
  table_id: str,
):
  bqstorage_write_client = bigquery_storage_v1.BigQueryWriteClient()

  # Create request_template.
  request_template = gapic_types.AppendRowsRequest()
  request_template.write_stream = (
      f"projects/{project_id}/datasets/{dataset_id}/tables/{table_id}/_default"
  )
  arrow_data = gapic_types.AppendRowsRequest.ArrowData()
  arrow_data.writer_schema.serialized_schema = (
      pyarrow_table.schema.serialize().to_pybytes()
  )
  request_template.arrow_rows = arrow_data

  # Create AppendRowsStream.
  append_rows_stream = AppendRowsStream(
      bqstorage_write_client,
      request_template,
  )

  # Create request with table data.
  request = gapic_types.AppendRowsRequest()
  request.arrow_rows.rows.serialized_record_batch = (
      pyarrow_table.to_batches()[0].serialize().to_pybytes()
  )

  # Send request.
  future = append_rows_stream.send(request)

  # Wait for result.
  future.result()
```

### Java

To learn how to install and use the client library for BigQuery, see [BigQuery client libraries](https://docs.cloud.google.com/bigquery/docs/reference/storage/libraries) . For more information, see the [BigQuery Java API reference documentation](https://docs.cloud.google.com/java/docs/reference/google-cloud-bigquerystorage/latest/com.google.cloud.bigquery.storage.v1) .

To authenticate to BigQuery, set up Application Default Credentials. For more information, see [Set up authentication for client libraries](https://docs.cloud.google.com/bigquery/docs/authentication#client-libs) .

```java
import com.google.api.core.ApiFuture;
import com.google.api.core.ApiFutureCallback;
import com.google.api.core.ApiFutures;
import com.google.api.gax.core.FixedExecutorProvider;
import com.google.api.gax.retrying.RetrySettings;
import com.google.cloud.bigquery.BigQuery;
import com.google.cloud.bigquery.BigQueryOptions;
import com.google.cloud.bigquery.QueryJobConfiguration;
import com.google.cloud.bigquery.TableResult;
import com.google.cloud.bigquery.storage.v1.AppendRowsRequest;
import com.google.cloud.bigquery.storage.v1.AppendRowsResponse;
import com.google.cloud.bigquery.storage.v1.BigQueryWriteClient;
import com.google.cloud.bigquery.storage.v1.BigQueryWriteSettings;
import com.google.cloud.bigquery.storage.v1.Exceptions;
import com.google.cloud.bigquery.storage.v1.Exceptions.AppendSerializationError;
import com.google.cloud.bigquery.storage.v1.Exceptions.MaximumRequestCallbackWaitTimeExceededException;
import com.google.cloud.bigquery.storage.v1.Exceptions.StorageException;
import com.google.cloud.bigquery.storage.v1.Exceptions.StreamWriterClosedException;
import com.google.cloud.bigquery.storage.v1.StreamWriter;
import com.google.cloud.bigquery.storage.v1.TableName;
import com.google.common.collect.ImmutableList;
import com.google.common.util.concurrent.MoreExecutors;
import com.google.protobuf.Descriptors.DescriptorValidationException;
import java.io.IOException;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.Phaser;
import java.util.concurrent.atomic.AtomicInteger;
import javax.annotation.concurrent.GuardedBy;
import org.apache.arrow.memory.BufferAllocator;
import org.apache.arrow.memory.RootAllocator;
import org.apache.arrow.vector.BigIntVector;
import org.apache.arrow.vector.VarCharVector;
import org.apache.arrow.vector.VectorSchemaRoot;
import org.apache.arrow.vector.VectorUnloader;
import org.apache.arrow.vector.compression.CompressionCodec;
import org.apache.arrow.vector.compression.CompressionUtil;
import org.apache.arrow.vector.compression.NoCompressionCodec;
import org.apache.arrow.vector.ipc.message.ArrowRecordBatch;
import org.apache.arrow.vector.types.pojo.ArrowType;
import org.apache.arrow.vector.types.pojo.Field;
import org.apache.arrow.vector.types.pojo.FieldType;
import org.apache.arrow.vector.types.pojo.Schema;
import org.threeten.bp.Duration;

/**
 * This class demonstrates how to ingest data using Arrow format into BigQuery via the default
 * stream. It initiates a DataWriter to establish a connection to BigQuery and reuses this
 * connection to continuously ingest data.
 */
public class WriteToDefaultStreamWithArrow {
  public static void main(String[] args)
      throws DescriptorValidationException, InterruptedException, IOException {
    if (args.length < 3) {
      System.out.println(
          "Usage: WriteToDefaultStreamWithArrow <projectId> <datasetName> <tableName>");
      return;
    }
    String projectId = args[0];
    String datasetName = args[1];
    String tableName = args[2];
    // Table schema should contain 3 fields:
    // ['test_string': STRING, 'test_int': INTEGER, 'test_geo':GEOGRAPHY]
    writeToDefaultStreamWithArrow(projectId, datasetName, tableName);
  }

  private static Schema createArrowSchema() {
    List<Field> fields =
        ImmutableList.of(
            new Field("test_string", FieldType.nullable(new ArrowType.Utf8()), null),
            new Field("test_int", FieldType.nullable(new ArrowType.Int(64, true)), null),
            new Field("test_geo", FieldType.nullable(new ArrowType.Utf8()), null));
    return new Schema(fields, null);
  }

  // Create an ArrowRecordBatch object that is compatible with the table schema.
  private static ArrowRecordBatch buildRecordBatch(VectorSchemaRoot root, int rowCount) {
    VarCharVector testString = (VarCharVector) root.getVector("test_string");
    BigIntVector testInt = (BigIntVector) root.getVector("test_int");
    VarCharVector testGeo = (VarCharVector) root.getVector("test_geo");

    testString.allocateNew(rowCount);
    testInt.allocateNew(rowCount);
    testGeo.allocateNew(rowCount);

    for (int i = 0; i < rowCount; i++) {
      testString.set(i, ("A" + i).getBytes());
      testInt.set(i, i + 100);
      testGeo.set(
          i,
          "POLYGON((-124.49 47.35,-124.49 40.73,-116.49 40.73,-113.49 47.35,-124.49 47.35))"
              .getBytes());
    }
    root.setRowCount(rowCount);

    CompressionCodec codec =
        NoCompressionCodec.Factory.INSTANCE.createCodec(CompressionUtil.CodecType.NO_COMPRESSION);
    VectorUnloader vectorUnloader =
        new VectorUnloader(root, /* includeNullCount= */ true, codec, /* alignBuffers= */ true);
    return vectorUnloader.getRecordBatch();
  }

  public static void writeToDefaultStreamWithArrow(
      String projectId, String datasetName, String tableName)
      throws DescriptorValidationException, InterruptedException, IOException {
    TableName parentTable = TableName.of(projectId, datasetName, tableName);
    Schema arrowSchema = createArrowSchema();
    DataWriter writer = new DataWriter();
    // One time initialization for the worker.
    writer.initialize(parentTable, arrowSchema);
    long initialRowCount = getRowCount(parentTable);
    try (BufferAllocator allocator = new RootAllocator()) {
      // A writer should be used to ingest as much data as possible before teardown.
      // Append 100 batches.
      for (int i = 0; i < 100; i++) {
        try (VectorSchemaRoot root = VectorSchemaRoot.create(arrowSchema, allocator)) {
          // Each batch has 10 rows.
          ArrowRecordBatch batch = buildRecordBatch(root, 10);

          // Asynchronous append.
          writer.append(new ArrowData(arrowSchema, batch));
        }
      }
    }
    // Final cleanup for the stream during worker teardown.
    // It's blocked until all append requests' response are received.
    writer.cleanup();

    verifyExpectedRowCount(parentTable, initialRowCount + 1000);
    System.out.println("Appended records successfully.");
  }

  private static long getRowCount(TableName parentTable) throws InterruptedException {
    String queryRowCount =
        "SELECT COUNT(*) FROM `"
            + parentTable.getProject()
            + "."
            + parentTable.getDataset()
            + "."
            + parentTable.getTable()
            + "`";
    QueryJobConfiguration queryConfig = QueryJobConfiguration.newBuilder(queryRowCount).build();
    BigQuery bigquery =
        BigQueryOptions.newBuilder().setProjectId(parentTable.getProject()).build().getService();
    TableResult results = bigquery.query(queryConfig);
    return Long.parseLong(results.getValues().iterator().next().get("f0_").getStringValue());
  }

  private static void verifyExpectedRowCount(TableName parentTable, long expectedRowCount)
      throws InterruptedException {
    String queryRowCount =
        "SELECT COUNT(*) FROM `"
            + parentTable.getProject()
            + "."
            + parentTable.getDataset()
            + "."
            + parentTable.getTable()
            + "`";
    QueryJobConfiguration queryConfig = QueryJobConfiguration.newBuilder(queryRowCount).build();
    BigQuery bigquery =
        BigQueryOptions.newBuilder().setProjectId(parentTable.getProject()).build().getService();
    TableResult results = bigquery.query(queryConfig);
    long countRowsActual =
        Long.parseLong(results.getValues().iterator().next().get("f0_").getStringValue());
    if (countRowsActual != expectedRowCount) {
      throw new RuntimeException(
          "Unexpected row count. Expected: " + expectedRowCount + ". Actual: " + countRowsActual);
    }
  }

  private static class ArrowData {
    Schema arrowSchema;
    ArrowRecordBatch data;

    ArrowData(Schema arrowSchema, ArrowRecordBatch data) {
      this.arrowSchema = arrowSchema;
      this.data = data;
    }
  }

  private static class DataWriter {

    private static final int MAX_RECREATE_COUNT = 3;

    private BigQueryWriteClient client;

    // Track the number of in-flight requests to wait for all responses before shutting down.
    private final Phaser inflightRequestCount = new Phaser(1);
    private final Object lock = new Object();

    private Schema arrowSchema;
    private StreamWriter streamWriter;

    @GuardedBy("lock")
    private RuntimeException error = null;

    private final AtomicInteger recreateCount = new AtomicInteger(0);

    private StreamWriter createStreamWriter(String streamName, Schema arrowSchema)
        throws IOException {
      // Configure in-stream automatic retry settings.
      // Error codes that are immediately retried:
      // * ABORTED, UNAVAILABLE, CANCELLED, INTERNAL, DEADLINE_EXCEEDED
      // Error codes that are retried with exponential backoff:
      // * RESOURCE_EXHAUSTED
      RetrySettings retrySettings =
          RetrySettings.newBuilder()
              .setInitialRetryDelay(Duration.ofMillis(500))
              .setRetryDelayMultiplier(1.1)
              .setMaxAttempts(5)
              .setMaxRetryDelay(Duration.ofMinutes(1))
              .build();

      // Use the Stream writer to send records in Arrow format. Specify the table name to write
      // to the default stream.
      // For more information about StreamWriter, see:
      // https://cloud.google.com/java/docs/reference/google-cloud-bigquerystorage/latest/com.google.cloud.bigquery.storage.v1.StreamWriter
      return StreamWriter.newBuilder(streamName, client)
          .setExecutorProvider(FixedExecutorProvider.create(Executors.newScheduledThreadPool(10)))
          .setChannelProvider(
              BigQueryWriteSettings.defaultGrpcTransportProviderBuilder()
                  .setKeepAliveTime(org.threeten.bp.Duration.ofMinutes(1))
                  .setKeepAliveTimeout(org.threeten.bp.Duration.ofMinutes(1))
                  .setKeepAliveWithoutCalls(true)
                  .setChannelsPerCpu(2)
                  .build())
          .setEnableConnectionPool(true)
          // If value is missing in ArrowRecordBatch and there is a default value configured on
          // bigquery column, apply the default value to the missing value field.
          .setDefaultMissingValueInterpretation(
              AppendRowsRequest.MissingValueInterpretation.DEFAULT_VALUE)
          .setMaxRetryDuration(java.time.Duration.ofSeconds(5))
          // Set the StreamWriter with Arrow Schema, this would only allow the StreamWriter to
          // append data in Arrow format.
          .setWriterSchema(arrowSchema)
          .setRetrySettings(retrySettings)
          .build();
    }

    public void initialize(TableName parentTable, Schema arrowSchema)
        throws DescriptorValidationException, IOException, InterruptedException {
      // Initialize client without settings, internally within stream writer a new client will be
      // created with full settings.
      client = BigQueryWriteClient.create();

      streamWriter = createStreamWriter(parentTable.toString() + "/_default", arrowSchema);
    }

    public void append(ArrowData arrowData)
        throws DescriptorValidationException, IOException, InterruptedException {
      synchronized (this.lock) {
        if (!streamWriter.isUserClosed()
            && streamWriter.isClosed()
            && recreateCount.getAndIncrement() < MAX_RECREATE_COUNT) {
          streamWriter = createStreamWriter(streamWriter.getStreamName(), arrowData.arrowSchema);
          this.error = null;
        }
        // If earlier appends have failed, we need to reset before continuing.
        if (this.error != null) {
          throw this.error;
        }
      }
      // Append asynchronously for increased throughput.
      ApiFuture<AppendRowsResponse> future = streamWriter.append(arrowData.data);
      ApiFutures.addCallback(
          future, new AppendCompleteCallback(this, arrowData), MoreExecutors.directExecutor());

      // Increase the count of in-flight requests.
      inflightRequestCount.register();
    }

    public void cleanup() {
      // Wait for all in-flight requests to complete.
      inflightRequestCount.arriveAndAwaitAdvance();

      client.close();
      // Close the connection to the server.
      streamWriter.close();

      // Verify that no error occurred in the stream.
      synchronized (this.lock) {
        if (this.error != null) {
          throw this.error;
        }
      }
    }

    static class AppendCompleteCallback implements ApiFutureCallback<AppendRowsResponse> {

      private final DataWriter parent;
      private final ArrowData arrowData;

      public AppendCompleteCallback(DataWriter parent, ArrowData arrowData) {
        this.parent = parent;
        this.arrowData = arrowData;
      }

      public void onSuccess(AppendRowsResponse response) {
        System.out.format("Append success\n");
        this.parent.recreateCount.set(0);
        done();
      }

      public void onFailure(Throwable throwable) {
        System.out.format("Append failed: " + throwable.toString());
        if (throwable instanceof AppendSerializationError) {
          AppendSerializationError ase = (AppendSerializationError) throwable;
          Map<Integer, String> rowIndexToErrorMessage = ase.getRowIndexToErrorMessage();
          if (rowIndexToErrorMessage.size() > 0) {
            System.out.format("row level errors: " + rowIndexToErrorMessage.toString());
            // The append returned failure with indices for faulty rows.
            // Fix the faulty rows or remove them from the appended data and retry the append.
            done();
            return;
          }
        }

        boolean resendRequest = false;
        if (throwable instanceof MaximumRequestCallbackWaitTimeExceededException) {
          resendRequest = true;
        } else if (throwable instanceof StreamWriterClosedException) {
          if (!parent.streamWriter.isUserClosed()) {
            resendRequest = true;
          }
        }
        if (resendRequest) {
          // Retry this request.
          try {
            this.parent.append(new ArrowData(arrowData.arrowSchema, arrowData.data));
          } catch (DescriptorValidationException e) {
            throw new RuntimeException(e);
          } catch (IOException e) {
            throw new RuntimeException(e);
          } catch (InterruptedException e) {
            throw new RuntimeException(e);
          }
          // Mark the existing attempt as done since we got a response for it
          done();
          return;
        }

        synchronized (this.parent.lock) {
          if (this.parent.error == null) {
            StorageException storageException = Exceptions.toStorageException(throwable);
            this.parent.error =
                (storageException != null) ? storageException : new RuntimeException(throwable);
          }
        }
        done();
      }

      private void done() {
        // Reduce the count of in-flight requests.
        this.parent.inflightRequestCount.arriveAndDeregister();
      }
    }
  }
}
```

### Rust

```rust
use anyhow::Result;
use arrow::array::{Int64Array, StringArray};
use arrow::datatypes::{DataType, Field, Schema};
use arrow::ipc::writer::StreamWriter;
use arrow::record_batch::RecordBatch;
use google_cloud_bigquery::client::Write;
use google_cloud_bigquery::model::{ArrowRecordBatch, ArrowSchema};
use std::sync::Arc;
use tokio::task::JoinSet;

pub async fn sample(project_id: &str, dataset_id: &str, table_id: &str) -> anyhow::Result<()> {
    let client = Write::builder().build().await?;

    // Define the table schema
    let schema = Arc::new(Schema::new(vec![
        Field::new("string", DataType::Utf8, false),
        Field::new("int", DataType::Int64, false),
    ]));

    // Initialize an IPC stream writer and extract the serialized schema
    let mut ipc_writer = StreamWriter::try_new(Vec::new(), &schema)?;
    let schema_buf = std::mem::take(ipc_writer.get_mut());

    let table = format!("projects/{project_id}/datasets/{dataset_id}/tables/{table_id}");
    // Create a writer for the default stream
    let writer = client
        .open_default_stream(table)
        .build_arrow(ArrowSchema::new().set_serialized_schema(schema_buf))
        .await?;

    let mut writes = JoinSet::new();
    for i in 0..100 {
        // Generate example data (`arrow::RecordBatch`).
        let batch = {
            let string = StringArray::from(vec![format!("batch {i}"); 10]);
            let int = Int64Array::from_iter_values(10 * i..10 * (i + 1));
            RecordBatch::try_new(schema.clone(), vec![Arc::new(string), Arc::new(int)])?
        };
        // Serialize the batch
        let batch = {
            ipc_writer.write(&batch)?;
            let batch_bytes = std::mem::take(ipc_writer.get_mut());
            ArrowRecordBatch::new().set_serialized_record_batch(batch_bytes)
        };
        // Write the batch to BigQuery
        writes.spawn(writer.append(batch).send());
    }
    let results: Result<Vec<_>, _> = writes.join_all().await.into_iter().collect();
    let _ = results?;
    println!("Successfully wrote 100 record batches of 10 rows each.");

    Ok(())
}
```
