# S3 Serial protocol v1

All HTTP API responses are JSON. Errors are `{error:"code",message:"..."}` with appropriate HTTP status.
Session cookie `s3sid` is HttpOnly, SameSite=Strict, Path=/ (HTTP local network).

* `POST /api/v1/session` body `{password}` -> `{ok:true}` and cookie.
* `DELETE /api/v1/session` -> revoke cookie and all its sockets/control.
* `GET /api/v1/status` -> `{device,boot_id,uptime_ms,free_heap,network,serial,controller_id,client_count}`.
* `GET /api/v1/network` -> `{ssid,sta_enabled,connected,sta_ip,ap_ssid,ap_ip,error}` (never passwords).
* `PUT /api/v1/network` body `{client_id,ssid,password,enabled}` -> `{ok:true}`; controller only.
* `PUT /api/v1/password` body `{client_id,password}` -> `{ok:true,reconnect_required:true}`; controller only, 12..63 ASCII characters; changes AP/login password and revokes sessions.

WebSocket `/api/v1/ws` requires the session cookie and same origin.
Client JSON: `{v:1,type,id,payload:{...}}`; ids are nonempty <=40 ASCII characters. Never replay commands on reconnect.
Server JSON: `{v:1,type,id?,payload:{...}}`. Server sequence timestamps apply to UART history records, not all WS messages.
On open: `hello` payload `{client_id,boot_id,controller_id,serial,latest_seq}`.
Client sends `heartbeat` every 5 seconds; server expires control/socket after 15 seconds without heartbeat.

| Command | Payload | Result |
|---|---|---|
| `heartbeat` | `{}` | `ack` |
| `control.claim` | `{}` | `ack`, `control.changed` broadcast |
| `control.release` | `{}` | cancels pending work, `ack`, broadcast |
| `uart.configure` | `{baud_rate,data_bits,parity:"N/E/O",stop_bits,gap_us}` | `ack` accepted then `config.applied` or `command.failed` |
| `uart.save` | `{}` | `ack` accepted then `config.saved` or `command.failed` |
| `tx.send` | `{data_b64,crc:"none/modbus"}` | `ack` queued, `tx.started`, `tx.done` / `tx.failed` |
| `repeat.start` | `{data_b64,crc:"none/modbus",interval_ms}` | `ack`, `repeat.changed` |
| `repeat.stop` | `{}` | `ack`, `repeat.changed`; stops unsent repeat jobs |
| `history.get` | `{after_seq}` | sets this socket cursor; subsequent `history` batches |

`ack` payload `{ok:true,...}`. `error` payload `{code,message}`. All command responses/events keep their request id when applicable.
`history` payload `{boot_id,records,oldest_seq,latest_seq,gap,evicted_bytes}`; record shape: `{seq,t_us,direction:"rx/tx",data_b64,length,config_revision,id?}`.
RX timestamps are software block-read time. TX timestamps are UART-completion time. History batches are not device protocol frames.
`status` payload has the same shape as GET status (once/second). `control.changed` -> `{controller_id}`.
Serial status fields: `{config:{baud_rate,data_bits,parity,stop_bits,gap_us},config_revision,owner_id,tx_busy,queued,repeat:{active,interval_ms,skipped},stats:{rx_bytes,tx_bytes,uart_overflows,frame_errors,parity_errors,history_evicted_bytes}}`.

Limits: 3 sockets, one explicit controller; WS incoming message <=4096 bytes total, TX <=1024 raw bytes including CRC. 1200..115200 baud, data bits 5..8, parity N/E/O, stop 1/2, gap 0..100000 us. Repeat 20..3600000 ms and at least wire time + gap. CRC only with 8 data bits. All callers validate limits independently; server is authoritative.
Unknown outcome after connection loss stays unknown. Never resend or restart repeat automatically. Disconnect, logout, password change, explicit release and heartbeat expiry revoke queued work. An already started frame may finish.

Frontend demo mode is explicit (`?demo=1`) and never claims connection to real hardware.
