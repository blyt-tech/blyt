#ifndef BLYT_GDB_TRANSPORT_TCP_H
#define BLYT_GDB_TRANSPORT_TCP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Start the TCP listener on 127.0.0.1:<port>.
 * If port is 0, the OS assigns a free port written to *port_out.
 * Sets the active transport in gdb_stub.  Returns 0 on success. */
int fc_gdb_transport_tcp_listen(int port, int *port_out);

/* Stop the listener and close any active client connection. */
void fc_gdb_transport_tcp_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* BLYT_GDB_TRANSPORT_TCP_H */
