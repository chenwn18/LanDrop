# LanDrop Protocol v1

LanDrop uses one TCP connection per outgoing transfer job. The receiver is also
a TCP server, so both peers can send and receive at the same time. Discovery is
separate and runs over UDP broadcast.

## Ports

| Transport | Default port | Purpose |
|-----------|--------------|---------|
| TCP | 47653 | File transfer |
| UDP | 47654 | LAN peer discovery (optional) |

The TCP protocol is private to LanDrop. It is not a compatible replacement for
FTP, SMB, rsync, HTTP, or any other protocol.

## Framing

Every message has a 12-byte header followed by a payload:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | Magic `LDX1` |
| 4 | 2 | Protocol version (currently `1`), big-endian |
| 6 | 2 | Message type, big-endian |
| 8 | 4 | Payload length, big-endian, maximum 1 MiB |

All integers are big-endian. A protocol string is encoded as a `uint16` byte
length followed by UTF-8 bytes. Relative paths use `/`, are at most 4096 bytes,
and may not be absolute, contain empty components, `.`/`..`, backslashes, or
control characters.

The default data payload is 256 KiB plus the four-byte file index. Relative
paths are validated again on the receiving side before any write.

## Message types

| Value | Name | Payload |
|-------|------|---------|
| 1 | `Hello` | `u16 version`, `string deviceName` |
| 2 | `HelloAck` | `u8 status`, `string serverName`, `string message` |
| 3 | `Start` | `16-byte transfer id`, `u32 fileCount`, `u32 dirCount`, `u64 totalBytes` |
| 4 | `Entry` | `u32 index`, `u8 kind` (0 dir, 1 file), `u64 size`, `32-byte SHA-256`, `string path` |
| 5 | `ManifestEnd` | `u32 itemCount` |
| 6 | `Resume` | `u32 index`, `u64 offset` |
| 7 | `ResumeEnd` | `u32 itemCount` |
| 8 | `FileBegin` | `u32 index`, `u64 offset` |
| 9 | `FileData` | `u32 index`, raw file bytes |
| 10 | `FileEnd` | `u32 index` |
| 11 | `FileResult` | `u32 index`, `u8 success`, `u16 errorCode`, `string message` |
| 12 | `Done` | `u64 totalBytes`, `u32 fileCount` |
| 13 | `DoneAck` | `string message` |
| 14 | `Error` | `u16 errorCode`, `string message` |
| 15 | `Cancel` | `string message` |
| 16 | `Progress` | `u32 index`, `u64 bytesDone`, `string message` |
| 17 | `Ping` | `u64 token` |
| 18 | `Pong` | `u64 token` |

Hello acknowledgement status: `0` OK, `1` busy, `2` rejected, `3` version
mismatch. Error codes: `1` protocol, `2` receiving disabled, `3` busy, `4`
disk I/O, `5` path rejected, `6` internal, `7` cancelled, `8` no space, `9`
integrity failure.

## Transfer flow

1. The sender scans selected files and folders, builds stable relative paths,
   and streams SHA-256 over every file. The result is sent in `Start` and
   `Entry` messages.
2. The receiver validates the entire manifest (path safety, duplicate paths,
   file/directory conflicts, counts, total bytes) and checks partial files.
3. The receiver replies with one `Resume` message per manifest item, then
   `ResumeEnd`. Directories use offset `0`. A file offset is the length of the
   receiver's metadata-matching partial prefix. If a final destination file
   already exists with the expected size and SHA-256, the receiver reports an
   offset equal to the file size and the file is skipped. A resumed partial
   file is checked again end-to-end with SHA-256 before it is committed.
4. For each file, the sender sends `FileBegin`, zero or more `FileData`
   messages, and `FileEnd`. It skips the bytes already present on the receiver.
   The receiver appends to its partial file and checks the declared limit on
   every frame.
5. A newly received file is hashed while it is written. A resumed file is
   re-read and hashed end-to-end before it is accepted. `FileResult` is sent
   only after the digest matches and the partial file has been atomically
   renamed into place.
6. After every file, the sender sends `Done`; the receiver verifies the totals,
   sends `DoneAck`, and marks the job complete.

If the connection breaks, the sender automatically reconnects up to three
times. The receiver keeps already-written partial data. When the sender
re-sends the same files, the receiver returns the existing byte counts and the
transfer continues. Network retries are automatic only within the same send
job; after an application restart, re-select the same files to resume.

## Partial-file state

Temporary data lives under:

```
<receive-directory>/.landrop-parts/
```

Each file has a deterministic key derived from the SHA-256 of the absolute
destination path:

```
<key>.part
<key>.part.meta
```

The 48-byte metadata file contains:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | Magic `LDP1` |
| 4 | 1 | Metadata version (`1`) |
| 5 | 3 | Reserved, zero |
| 8 | 8 | Expected final file size, big-endian |
| 16 | 32 | Expected SHA-256 |

A partial prefix is reused only when metadata and expected digest match. If
they do not, the stale pair is deleted and the file restarts from zero. After
successful verification the part file is atomically renamed to its final
destination; the sidecar is removed.

## Discovery beacon

UDP datagrams sent to port 47654 use:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | Magic `LDSC` |
| 4 | 2 | Version (`1`), big-endian |
| 6 | 2 | TCP transfer port |
| 8 | 16 | Random 128-bit instance id |
| 24 | 2 | Name length, big-endian |
| 26 | N | UTF-8 device name (maximum 120 bytes) |

Peers expire after 8 seconds without a beacon. Discovery is convenience only;
manual `IPv4` or `IPv4:port` entry always works.

## Security model

LanDrop is designed for a trusted local network:

- No authentication, pairing PIN, or encryption is implemented.
- Anyone who can reach TCP port 47653 may offer files when receiving is
  enabled.
- Path traversal and symlink escapes are blocked; existing destination
  symlinks are rejected.
- Completed same-name files are replaced only after SHA-256 verification.
- Do not expose the listening port to the public internet or an untrusted
  network. Use the operating-system firewall to restrict access when needed.
