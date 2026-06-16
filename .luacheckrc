-- Runtime-provided globals all carts may read (blyt) or read-write (S).
-- S is in globals (not read_globals) because carts write to its fields
-- (e.g. S.globals[0].player = ...).
read_globals = { "blyt" }
globals = { "S" }

-- Example carts deliberately demonstrate global state patterns:
-- - allow_defined_top: lifecycle callbacks (init, update, …) are defined at
--   the top level of the chunk and called by the runtime, not within the file.
-- - ignore W131: suppress "unused global variable" for those callbacks since
--   they are intentionally defined for the runtime to call, never in-file.
files["examples/"] = {
    allow_defined_top = true,
    ignore = { "131" },
}
