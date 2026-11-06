# Display groups (multi-display streaming, prototype)

Status: experimental client prototype for the approved Option G design. It needs a
Foundation Sunshine host with display group support (host slices GW-H2 to GW-H5,
branch `asafmah-multi-monitor-p1-coordinator`). Against any other host it does nothing.

## Model

A display group streams several host display heads. Each head is an ordinary,
unchanged single-track session in its own Moonlight process and window, because
moonlight-common-c keeps connection state per process.

- **Leader**: the Moonlight process the user starts. It probes `/serverinfo` for
  `DisplayGroupMaxHeads`, sends `/launch ...&displayHeads=N`, and reads
  `<displayGroup>` (32 lowercase hex characters) from the response. After PLAY the
  host sends the layout (control opcode `0x55F0`). The leader then starts one member
  process per extra head and places them on its free monitors in host desktop order.
  If the host sends no layout within 3 s, members use the leader's stream size.
- **Member**: `Moonlight stream <hostUuid> <app> --display-group <id> --display-head <i>
  --display-group-ipc <socket> --display-group-size WxH --display-group-fps F
  --display-group-display <k> [--display-group-fullscreen]`. It joins with
  `/resume ...&displayGroup=<id>&displayHead=<i>`, with a gamepad bitmap of 0 and
  `mode=` set to its head's size.

| Concern | Owner |
| --- | --- |
| Host app launch and quit | Leader. A member never calls `/cancel`. |
| Keyboard | Leader. Members forward keys and text over a local socket (`KeyboardSink`). The leader releases a member's held keys if its link drops. |
| Gamepads, microphone, clipboard, file mapping | Leader only. |
| Absolute mouse, touch, pen | Each window's own session (the host maps them through that head's `touch_port`). Members force absolute mouse mode. |
| Audio | Host side: members get no audio. The client is unchanged. |

Lifetime:
- A member that quits leaves its head on the host until the group ends.
- When the leader's stream ends for any reason, including a reconnect, the host ends
  the group. The leader also tells its members to quit, and kills any that don't exit
  within 5 s.
- A member whose link to the leader drops exits on its own.
- A leader that reconnects resumes as an ordinary single-display session.

## Opting in

- Settings key `displaygroupheads` (`StreamingPreferences::displayGroupHeads`): 0 = off
  (the default), 2..8 = heads to request. There is no UI yet.
- CLI: `Moonlight stream <host> <app> --display-heads 2`.

With the feature off, or against a host without the capability, requests and
behaviour are unchanged. `tests/display_group/test_display_group_gate.py` enforces this.

## Wire contract

Agreed with the host session and recorded in moonlight-common-c
`docs/protocol-extension-registry.md`:
- `LI_FF_DISPLAY_GROUP = 0x4000` (diagnostic only).
- Opcode `0x55F0` layout v1. Little-endian, `20 + 24*N` bytes. Parsed by
  `src/DisplayGroupStream.h`.

Member `/resume` errors:

| Status | Meaning |
| --- | --- |
| 404 | Unknown or ended group |
| 403 | Different client certificate |
| 409 | Head already streamed |
| 503 | Admission refused or mode change failed |

## Code

- `app/streaming/displaygroup.{h,cpp}`: pure logic (negotiation, layout, planning,
  IPC framing, `KeyRelay`).
- `app/streaming/displaygroupipc.{h,cpp}`: `LeaderHub` and `MemberLink`, local sockets
  on private threads.
- `app/streaming/sessiondisplaygroup.cpp`: Session glue.
- `app/streaming/input/keyboardsink.{h,cpp}`: the single exit point for keyboard input.
- Tests: `tests/display_group` (C++ with a fake host layout and fake member processes,
  plus Python source gates); restart-recovery harness cases for leader and member
  reconnects.

## Not done yet

- Settings UI.
- Re-placing windows when the layout changes after launch, and relaunching a member
  whose head is admitted late.
- End-to-end run against a real group-capable host. It's blocked on host GW-H2/H3.
