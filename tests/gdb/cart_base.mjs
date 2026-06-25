/*
 * tests/gdb/cart_base.mjs — resolve the cart's guest load base from the GDB
 * stub's qXfer:libraries-svr4 list.
 *
 * Issue #119: a debug session presents the cart purely as a shared library and
 * (fix A2) relocates it off guest base 0 to BLYT_RELOAD_BASE_A so it never
 * overlaps the lldb-dap stub `program`.  A real debugger (lldb) reads the svr4
 * library list and resolves a symbol's runtime address as (link-time vaddr +
 * library l_addr).  These raw-RSP test drivers set breakpoints at link-time
 * symbol vaddrs (from `nm`/readelf), so they must add the cart library's l_addr
 * to land on the relocated code — exactly what a debugger does.
 *
 * The cart is the one library whose name is not a runtime lib (libblyt*.so);
 * returns its l_addr, or 0 if the cart sits at base 0 / no such entry.
 *
 * `exchange` is a function that sends an RSP packet payload and resolves to the
 * server's response payload (the transport each driver already defines).
 */
export async function cartLoadBase(exchange) {
	const resp = await exchange('qXfer:libraries-svr4:read::0,8000');
	if (!resp) return 0;
	/* Strip the leading qXfer status byte ('l' = last chunk, 'm' = more). */
	const xml = /^[lm]/u.test(resp) ? resp.slice(1) : resp;
	const re = /<library\s+name="([^"]*)"[^>]*\bl_addr="0x([0-9a-fA-F]+)"/gu;
	let m;
	while ((m = re.exec(xml)) !== null) {
		const name = m[1];
		const base = name.split('/').pop() || name;
		if (!base.startsWith('libblyt')) return parseInt(m[2], 16);
	}
	return 0;
}
