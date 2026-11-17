# kneil_proto — KNEIL live protocol, copied unchanged

`kneil_proto.h` and `kneil_proto.c` are a **verbatim copy** from the KNEIL firmware repository
(`MSaviorTR/kneil_firmware`), `protocol/` folder, at **main `718301d`** ("Merge agent/E2: model V4
(library 2.0), BLE protocol v2, PC app v2").

Do not edit them here: the KNEIL board, the XIAO bridge and the PC app use the same code, and a forked
decoder would drift. To update, copy the two files again from the KNEIL repository and change the commit
hash in `CMakeLists.txt`.

Contents (protocol v2):

- `kneil_live_t` + `kneil_live_encode/decode` (LIVE 22 bytes; v1 20-byte packets are also decoded),
- `kneil_raw_t` + raw packet codec (unused by the watch),
- `kneil_summary_t` + `kneil_summary_encode/decode` (daily summary, format 2, 40 bytes),
- `kneil_info_encode/to_line` (INFO text),
- the text-line writers `kneil_live_to_line`, `kneil_summary_to_line`, `kneil_info_to_line`,
- `kneil_cmd_to_ctrl()` (S/P/T/Q/Z → CONTROL bytes).

`KNEIL_UUID_ENCODE()` uses `BT_UUID_128_ENCODE()` from Zephyr; it is only a macro definition and is never
expanded on the watch, which builds its own NimBLE UUIDs in `main/kneil_central.c`.
