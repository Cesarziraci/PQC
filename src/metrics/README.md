# PQC metrics

This folder is the planning area for all PQC metrics work.

Current phase: node metrics reports to root.

The first goal is to define which values each side can measure locally without
changing the protocol too much. After that, node-side reports can be sent to the
root using `PQC_MSG_METRICS`, and the root can export the combined view as CSV.

## Phases

1. Local counters
   - Define counters and timestamps that live in each node/session.
   - Keep the schema stable enough to print or serialize later.
   - Instrument handshake overhead without changing handshake behavior.

2. Root CSV output
   - Root prints structured `PQC_CSV,...` lines over serial.
   - PC-side tooling captures those lines and writes `.csv`.
   - Human logs stay separate from machine-readable CSV lines.

3. Node metrics reports
   - Nodes send compact `PQC_MSG_METRICS` reports once the session is established.
   - Reports should include `node_id`, `seq`, and `uptime_ticks`.
   - Counters should be cumulative so one lost report does not destroy the data.

4. Session summary
   - Root combines root-observed metrics and node-reported metrics.
   - Root emits one summary row for each handshake success/failure.

5. Watchdog/reset reporting
   - Low priority roadmap item.
   - If a node resets while it had an established session, inspect reset cause.
   - If the reset is not just first boot/join, notify the root so it can reset
     the session for that node.
   - This should become useful after the basic metrics path is stable.

6. Root restart broadcast
   - Low priority roadmap item.
   - If the root restarts, it has lost all session material.
   - Broadcast an invalid-session notification so nodes discard old AES/session
     state and restart the handshake.
   - This avoids nodes continuing to send TEMP with keys the new root process
     cannot use.

## Metric groups

- Handshake
  - attempts, success, failure
  - GETPK, PK, CT, ACK stage transitions
  - keypair, encapsulation, decapsulation timing

- Fragmentation
  - fragments sent/received
  - duplicates and rejected fragments
  - NACK sent/received
  - fragments recovered by NACK

- Temperature traffic
  - TEMP sent
  - TEMP decrypted
  - TEMP decrypt failures
  - RX counter resyncs

- Queue/resource behavior
  - TX queue drops
  - TX queue high-water mark
  - RX slots used
  - retry counts

## CSV direction

Root serial output should keep a stable prefix:

```text
PQC_CSV_HEADER,record,root_seconds,node_id,event,value0,value1,value2,value3
PQC_CSV,EVENT,12.345,42,GETPK_ACCEPT,0,0,0,0
PQC_CSV,EVENT,12.380,42,NODE_HS,0.300,1,0,1
PQC_CSV,EVENT,12.390,42,SESSION_HS,0.310,0.300,0,0
PQC_CSV,TEMP,12.400,42,TEMP_RX,17,45123,24567,0
```

The PC logger should ignore every line that does not start with `PQC_CSV`.
Time columns are exported as decimal seconds. The firmware keeps Contiki ticks
internally and converts them only when printing CSV.

## Compact report groups

`PQC_MSG_METRICS` uses one single-frame payload per group:

```text
version,group,seq,uptime_ticks,value0,value1,value2,value3
```

Current groups:

- `NODE_HS`: handshake_seconds_last, success, failure, attempts
- `NODE_PK`: pk_tx_seconds_last, pk_rx_seconds_last, pk_tx, pk_rx_complete
- `NODE_CT`: ct_tx_seconds_last, ct_rx_seconds_last, ct_tx, ct_rx_complete
- `NODE_CRYPTO`: keypair_seconds_last, encaps_seconds_last, decaps_seconds_last, ack_wait_seconds_last
- `NODE_FRAG`: fragments_tx, fragments_rx, nack_tx, nack_rx
- `NODE_TEMP`: temp_tx, temp_tx_fail, temp_rx_ok, temp_counter_resync
- `NODE_RES`: retries, tx_queue_full, rx_slot_full, rx_slot_timeout

## Combined session rows

After the root has an established session and has received all node report
groups for that handshake, it emits one combined summary:

- `SESSION_HS`: hs_seconds_root, hs_seconds_node, retries_root, retries_node
- `SESSION_PK`: pk_tx_seconds_root, pk_rx_seconds_node, pk_tx_root, pk_rx_node
- `SESSION_CT`: ct_tx_seconds_node, ct_rx_seconds_root, ct_tx_node, ct_rx_root
- `SESSION_CRYPTO`: keypair_seconds_root, encaps_seconds_node, decaps_seconds_root, ack_wait_seconds_node
- `SESSION_FRAG`: nack_tx_root, nack_rx_root, nack_tx_node, nack_rx_node
- `SESSION_TEMP`: temp_rx_ok_root, temp_rx_fail_root, temp_tx_node, temp_tx_fail_node

## Handshake impact window

When the root accepts a `GETPK`, it snapshots TEMP counters for all nodes that
were already established. When that handshake succeeds or fails, it emits:

- `IMPACT_START`: established_snapshot_count, 0, 0, 0
- `IMPACT_TEMP`: hs_node, temp_rx_ok_delta, temp_rx_fail_delta, counter_gap
- `IMPACT_RESYNC`: hs_node, temp_resync_delta, rx_counter_delta, still_established
- `IMPACT_END`: hs_duration_seconds, snapshot_count, success, 0
- `IMPACT_OVERLAP`: new_hs_node, snapshot_count, 0, 0

`counter_gap` estimates skipped TEMP counters during the HS window. It is useful
for spotting TEMP packets that were lost while the root was busy with HQC.
