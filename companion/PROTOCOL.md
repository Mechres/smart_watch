# Hikaboshi Companion Protocol

Device name: **Hikaboshi**

## Service

`1d8a503d-e931-369f-164b-6f10596a178d`

## Characteristics

| Char | UUID | Props |
|------|------|-------|
| Notification | `2480757d-4f07-9fa5-0f48-e4125a9bdab8` | Read, Write, Notify |
| Control | `b31cb75e-410c-29ba-0b45-9da7834df66e` | Read, Write, Notify |
| Battery | `12345678-90ab-cdef-1234-567890abcdef` | Read, Notify |
| Steps | `fedcba98-7654-3210-fedc-ba9876543210` | Read, Notify |

### Notification write (phone → watch)

UTF-8 text: `Title|Body` or `Title\nBody`

Example: `Message|Hello World`

### Notification read (watch → phone)

Explicit wire format (not a raw struct):

```
byte 0: flags (bit0=has_data, bit1=has_unread)
byte 1: title_len (0..31)
byte 2: body_len  (0..127)
bytes 3..: title bytes, then body bytes
```

### Battery

`uint8` percentage 0–100.

### Steps

`uint32` native endian (little-endian on ESP32).

## Control commands (phone → watch write)

| Command | Effect |
|---------|--------|
| `wifi_on` / `wifi_off` | Toggle WiFi |
| `screen_on` / `screen_off` | Toggle display |
| `time=<unix_ts>` | Set system time (seconds) |
| `weather=<temp>,<code>` | e.g. `weather=24.5,1` |
| `ota=<https-url>` | HTTPS OTA download + reboot |

## Control notifications (watch → phone)

| Message | Meaning |
|---------|---------|
| `find_phone` / `find_phone_stop` | Ring / stop ringing the phone |
| `music_toggle` / `music_next` / `music_prev` | Media control |
| `ota_status=start\|wifi_fail\|fail\|done` | OTA lifecycle |
| `ota_progress=NN` | OTA percent (0,10,...,100) |

The watch also echoes written payloads as Notify on the same characteristic (write ACK).

## Advertising

- Fast advertising while active; stops after ~5 min without a connection.
- Restarts when the user wakes the watch (raise wrist / button).
- Wake the watch before scanning if connect fails.
