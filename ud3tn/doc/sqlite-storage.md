# SQLite Storage

The SQLite-based persistent storage back-end is implemented in μD3TN as an ordinary CLA. Bundles sent via this CLA are written to a SQLite database instead of being transferred to another node. By default, a volatile in-memory database with shared cache is used (`sqlite:file::memory:?cache=shared`). As several database connections are open, the `cache=shared` option is mandatory. To save bundles beyond the lifetime of the μD3TN, a dedicated database file must be specified (e.g. `sqlite:~/ud3tn.dtn`). Bundles can also be fed back into the μD3TN. From the point of view of the μD3TN, returning bundles is no different from receiving bundles via a CLA.

To interact with the SQLite storage, for example to delete bundles or send them to μD3TN, special commands can be sent to a local application agent that is connected to the SQLiteCLA. The [protocol spoken by SQLiteAgent is based on Protobuf](../references/protobuf/#componentsagentsstoragestorage_agentproto) and allows to apply an operation to a set of packages selected by the destination EID.
Note that only local bundles with the AAP 2.0 flag `BUNDLE_ADU_WITH_BDM_AUTH` will be accepted for security reasons.

## Storage Interaction

### Option 1: Using the external next-hop routing BDM

This is the easiest option to use the persistent storage. Just launch µD3TN with the external BDM and it will make use of the storage system automatically.

1. Run μD3TN with enabled MTCP and SQLite CLA and external dispatch flag

    ```sh
    ./build/posix/ud3tn --cla "sqlite:ud3tn.sqlite;mtcp:*,4224" --external-dispatch
    ```

2. Run the BDM

    ```sh
    aap2-bdm-ud3tn-routing -v
    ```

3. Run a second μD3TN instance as receiver

    ```sh
    ./build/posix/ud3tn --node-id dtn://ud3tn2.dtn/ --aap2-socket ud3tn2.aap2.socket --cla "mtcp:*,4225"
    ```

4. Connect a receiver to the second μD3TN instance

    ```sh
    aap2-receive --socket ud3tn2.aap2.socket --agentid bundlesink
    ```

5. Configure a contact starting a bit later

    ```sh
    aap2-config --schedule 30 60 100000 dtn://ud3tn2.dtn/ mtcp:localhost:4225
    ```

6. Send a bundle to `dtn://ud3tn2.dtn`

    ```sh
    aap2-send dtn://ud3tn2.dtn/bundlesink "Hello"
    ```

7. Query the database to verify the bundle is stored (before contact starts)

    ```sh
    sqlite3 ud3tn.sqlite "SELECT source, destination, creation_timestamp, hex(bundle) FROM bundles;"
    ```

8. Wait until the contact starts

9. Verify the bundle is received by `aap2-receive`

10. Query the database to verify the bundle has been deleted automatically

    ```sh
    sqlite3 ud3tn.sqlite "SELECT source, destination, creation_timestamp, hex(bundle) FROM bundles;"
    ```

!!! note

    If the BDM restarts while bundles are in persistent storage, the bundles have to be recalled manually to re-schedule them, _after applying all necessary configuration_ (e.g. scheduling contacts):

    `aap2-storage-agent push --dest-eid-glob "*"`

### Option 2: Using the external static BDM

1. Run μD3TN with enabled MTCP and SQLite CLA and external dispatch flag

    ```sh
    ./build/posix/ud3tn --cla "sqlite:ud3tn.sqlite;mtcp:*,4224"
    ```

2. Run the BDM with a rule to forward the bundles to storage

    ```sh
    aap2-bdm-static <(echo '{"dtn://ud3tn2.dtn/": "dtn:storage"}')
    ```

3. Run a second μD3TN instance as receiver

    ```sh
    ./build/posix/ud3tn --node-id dtn://ud3tn2.dtn/ --aap2-socket ud3tn2.aap2.socket --cla "mtcp:*,4225"
    ```

4. Connect a receiver to the second μD3TN instance

    ```sh
    aap2-receive --socket ud3tn2.aap2.socket --agentid bundlesink
    ```

5. Send a bundle to `dtn://ud3tn2.dtn`

    ```sh
    aap2-send dtn://ud3tn2.dtn/bundlesink "Hello"
    ```

6. Query the database to verify the bundle is stored

    ```sh
    sqlite3 ud3tn.sqlite "SELECT source, destination, creation_timestamp, hex(bundle) FROM bundles;"
    ```

7. Terminate the BDM and re-run it with a rule to forward bundles to the second node

    ```sh
    ^C
    aap2-bdm-static <(echo '{"dtn://ud3tn2.dtn/": "mtcp:localhost:4225"}')
    ```

8. Instruct µD3TN to connect to the second node

    ```sh
    aap2-configure-link dtn://ud3tn2.dtn/ mtcp:localhost:4225
    ```

9. Send a command to the SQLiteAgent to inject the stored bundle back to μD3TN

    ```sh
    aap2-storage-agent push --dest-eid-glob "dtn://ud3tn2.dtn*"
    ```

10. Verify the bundle is received by `aap2-receive`

11. Send a command to the SQLiteAgent to delete the stored bundle

    ```sh
    aap2-storage-agent delete --dest-eid-glob "dtn://ud3tn2.dtn*"
    ```

12. Query the database to verify the bundle is deleted

    ```sh
    sqlite3 ud3tn.sqlite "SELECT source, destination, creation_timestamp, hex(bundle) FROM bundles;"
    ```

### Option 3: Using the integrated minimal next-hop routing approach

In real scenarios, the storage is accessed via a BDM. The BDM decides when bundles should be sent to the storage and when they should be retrieved. Nevertheless, for simple tests, you want to interact with the storage without a BDM. This requires a workaround in which a "virtual" contact to the storage is configured.

1. Run μD3TN with enabled MTCP and SQLite CLA

    ```sh
    ./build/posix/ud3tn --cla "sqlite:ud3tn.sqlite;mtcp:*,4224"
    ```

2. Run a second μD3TN instance as receiver

    ```sh
    ./build/posix/ud3tn --node-id dtn://ud3tn2.dtn/ --aap2-socket ud3tn2.aap2.socket --cla "mtcp:*,4225"
    ```

3. Connect a receiver to the second μD3TN instance

    ```sh
    aap2-receive --socket ud3tn2.aap2.socket --agentid bundlesink
    ```

4. Configure a "virtual" contact to the destination node via the SQLiteCLA

    ```sh
    aap2-config --schedule 1 10 100000 dtn://ud3tn2.dtn/ sqlite:
    ```

5. Send a bundle to `dtn://ud3tn2.dtn`

    ```sh
    aap2-send dtn://ud3tn2.dtn/bundlesink "Hello"
    ```

6. Query the database to verify the bundle is stored

    ```sh
    sqlite3 ud3tn.sqlite "SELECT source, destination, creation_timestamp, hex(bundle) FROM bundles;"
    ```

7. Configure a contact

    ```sh
    aap2-config --schedule 1 60 100000 dtn://ud3tn2.dtn/ mtcp:localhost:4225
    ```

8. Send a command to the SQLiteAgent to inject the stored bundle back to μD3TN

    ```sh
    aap2-storage-agent push --dest-eid-glob "dtn://ud3tn2.dtn*"
    ```

9. Verify the bundle is received by `aap2-receive`

10. Send a command to the SQLiteAgent to delete the stored bundle

    ```sh
    aap2-storage-agent delete --dest-eid-glob "dtn://ud3tn2.dtn*"
    ```

11. Query the database to verify the bundle is deleted

    ```sh
    sqlite3 ud3tn.sqlite "SELECT source, destination, creation_timestamp, hex(bundle) FROM bundles;"
    ```
