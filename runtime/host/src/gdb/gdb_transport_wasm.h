#ifndef BLYT_GDB_TRANSPORT_WASM_H
#define BLYT_GDB_TRANSPORT_WASM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Connect to the relay WebSocket at ws://127.0.0.1:<relay_port>/gdb.
 * Sets the active transport in gdb_stub. */
void fc_gdb_transport_wasm_open(int relay_port);

/* Close the WebSocket connection. */
void fc_gdb_transport_wasm_shutdown(void);

/* Returns 1 if the WebSocket is connected. */
int fc_gdb_transport_wasm_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* BLYT_GDB_TRANSPORT_WASM_H */
