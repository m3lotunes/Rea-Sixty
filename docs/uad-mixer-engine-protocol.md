# UA Mixer Engine (TCP 4710): protocol notes for Rea-Sixty

The interface UAD Console's background process offers on the local machine, as far as it is
known. **Universal Audio does not document it.** Everything here comes from three open-source
projects that use it (read on 02.11.2026), plus our own read-only walk of the user's engine,
recorded in `docs/uad-console-plan.md`. Plan and measurements: `docs/uad-console-plan.md`;
code: `extension/src/UadProto.h`, `extension/tools/uad_engine_probe.cpp`.

**Sources** (file:line references below point into these repositories):

- **UMC** = UA Midi Control by Radu Varga, https://github.com/raduvarga/UA-Midi-Control.
  The author's README describes the protocol.
- **MCP** = uad-console-mcp, https://github.com/fomoPhil/uad-console-mcp. Verified live on an
  Apollo x8 Gen 2 with UAD 11.7.1 on macOS (docs/PROTOCOL.md:3-4).
- **OA** = open-apollo, https://github.com/rolotrealanis98/open-apollo. Captured engines 11.8.2
  and 12.0.0 read-only, plus Console 1.2.x/1.3.1 packet captures. `ue/` =
  `console-rs/crates/ua-engine/`.
- **Fixtures:** **x8** = MCP `tests/fixtures/apollo_x8_gen2_tree.json` (11.7.1); **x4** =
  `ue/tests/fixtures/apollo-x4-12.0.0.snapshot.json`.

*Inferred* marks a reading of the evidence that no project states.

## 1. Connection

| Topic | Fact | Evidence |
|---|---|---|
| Endpoint | TCP `127.0.0.1:4710`, served by the "UA Mixer Engine" process, which keeps running after Console quits | UMC README.md:13,44; AppDelegate.swift:14-15; MCP PROTOCOL.md:13-14 |
| Exposure | Engine 12.0.0 answered a LAN client. Bonjour `_uamixer._tcp`. No authentication or encryption | ue/README.md:49,84-88; MCP README.md:340 |
| Framing | UTF-8, one `\0` after every frame in both directions, no length prefix. A whole tree is up to about 1.8 MB | UMC README.md:45; MCP PROTOCOL.md:17-21; ue/README.md:88 |
| Several clients | Supported. Console is just another client and sees your changes live | MCP PROTOCOL.md:15-16,22-24 |
| Handshake | None is required: UMC and MCP send none. Console first sends `{"path":"networkID","data":"<32 hex>"}` and `{"path":"client_id","data":"console"}`, then `set /Sleep false`, `subscribe /MixerState/value`, `subscribe /errors?recursive=1` and `get /errors` | ue/src/connection.rs:97-103,120-123,759-776 |
| `/Sleep` | A hint for one socket, not mixer state. `true` slows that socket's pushes to 100 ms and stops metering it; the default is `false` (reverse-engineered). UMC sends `false` every 3 s and saw no effect. MCP never sends it. The real tree has no `/Sleep` node | ue/src/command.rs:135-137; UMC TCPListener.swift:233-243, README.md:46; MCP PROTOCOL.md:177-179; ue/README.md:75,124 |
| Keepalive / reconnect | No application keepalive is needed: OA uses TCP keepalive, and `get /ping` is for the 4720 helper only. UMC retries every 3 s. MCP backs off 0.5 to 8 s and re-sends every subscription after reconnecting | ue/src/connection.rs:104-106,750-753; UMC TCPListener.swift:14,30-43; MCP client.py:257-272,483-495 |

## 2. Grammar

Requests have the form `<verb> <path>[ <value>]\0` with verbs `get`, `set`, `subscribe`, `unsubscribe` and `post`. **Unknown verbs are dropped silently, with no reply** (MCP PROTOCOL.md:28-40).

- **Paths.** A property path looks like `/devices/0/inputs/0/Mute`, and its value leaf is `…/Mute/value` (ue/src/path.rs:1-5).
  - `set` always targets `…/value` (MCP PROTOCOL.md:35).
  - `get` also resolves the sub-leaves `/value`, `/values`, `/default` and `/enabled` (ue/src/mock.rs:4-6; ue/README.md:97-98).
  - UMC shipped trailing-slash forms such as `set …/FaderLevelTapered/value/ 0.5` and `subscribe …/FaderLevelTapered/` (TCPListener.swift:115,119; README.md:67,73). Use the canonical form without the slash.
- **Values.** One space, then JSON text: numbers bare, `true`/`false`, strings JSON-quoted (`set …/Name/value "UG 1"`), integers with no fraction (MCP client.py:582; ue/src/value.rs:104-109; OA console-rs/WIRING.md:69). The literal `"default"` resets a property to its default (ue/src/writer.rs:23-25).
- **Console extras (optional).**
  - Console appends `?func_id=N` to writes, plus `context_type=main` on fader, pan, mute and monitor writes (ue/src/command.rs:78-101,189-201).
  - A `get` sent with `func_id` gets it echoed back in `parameters` (ue/README.md:46,99). Without it there is no correlation ID, so route replies by `path` (MCP PROTOCOL.md:44-47).
  - Console brackets each edit with `set /UndoRecording/value true` … `false` so it is one undo step (ue/src/writer.rs:8-10).

| Request | Reply | Evidence |
|---|---|---|
| `get <node>` | `{"path":…,"data":{"properties":{<name>:{meta}},"children":{"0":{},…}}}`. Every scalar property comes inline with its metadata | UMC README.md:56-58,64; MCP PROTOCOL.md:49-54,100 |
| `get <prop>` | `data` = `{type, value, [default, min, max, readonly, enabled, values]}`. Types are string, bool, int, int64 and float. An enum is a plain list or a list of `{value,string,enabled}` | MCP PROTOCOL.md:56-62,86-98 |
| `set` | Ack `{"path":"<prop>/value","data":<applied>}`. The engine clamps and quantizes silently, so read back to verify | MCP PROTOCOL.md:64-69,180-181; client.py:572-599 |
| `subscribe <prop>` | `{"path":"<prop>/value","data":<current>}` at once, then a push only when the value changes | UMC README.md:62; MCP PROTOCOL.md:78-84 |
| Errors | `{"path","error":"Unable to resolve path for get."}` (also `set.` and `subscribe.`), or `"Unable to set value."` when the hardware refuses | MCP PROTOCOL.md:71-76; ue/src/message.rs:128 |
| `subscribe mode=multi` | Pushes then arrive as `{"path":"multi","data":[{path,data},…]}`. `subscribe {"paths":[…]}` is answered with one `multi` | ue/src/command.rs:18-22; ue/src/message.rs:88-101 |
| `unsubscribe` | Single paths only; a batch unsubscribe is ignored | ue/src/command.rs:16; ue/README.md:62 |
| `get /?recursive=1` | On 11.7.1: top-level stubs only, so walk node by node (1,941 nodes in about 11 s). On 12.0.0: the full tree | MCP PROTOCOL.md:164-165; ue/README.md:88; ue/src/connection.rs:1079 |

## 3. Tree

**Root** (x8:9ff):
- Clock: `SampleRate` and `ClockSource` (enums), `ClockLocked`.
- Talkback: `TalkbackOn`, `TalkbackMaster` (int −1..100), `TalkbackInPhysicalCR`.
- Metering: `PeakHold`, `ClipHold`, `PostFaderMetering`, `MeterPulse` (the meter tick counter).
- Sessions and undo: `Session`, `Dirty`, `UndoRecording`.
- Version: `Version`, `ProtocolVersion` ("1.0.0").
- `AppCommand`: opaque. Never write it (MCP PROTOCOL.md:125-128).

| Node | Properties (type and range from the fixtures) |
|---|---|
| `/devices/D` | `DeviceName`, `DeviceTypeID`, `DeviceOnline` (bool), `CueBusCount`, `DimAttenuation` (int 0..60, default 17; the hardware snaps it to 7 steps), `AltMonSelection` (int 0..2) (x8:808; ue/src/topology.rs:618) |
| `…/inputs/I` | `Name` {default, value}, `StereoName`, `IOType`, `FaderLevel` (float −144..+12 dB), `FaderLevelTapered` (0..1), `Mute`, `Solo`, `SoloSafe`, `Pan`/`Pan2` (−1..1), `Stereo`, `Active` (read-only), `ChannelHidden`, `EnabledByUser`, `CustomIntegratedPreampInputMeter` (−77..0) (x8:79294) |
| Stereo pair | The left (even) input holds every control: `Pan` is the L leg (default −1) and `Pan2` the R leg (default +1). The right input reads `Active:false`. A strip is visible when Active && EnabledByUser && !ChannelHidden (x8 inputs/16-18; ue/src/topology.rs:375-376,406-413,714-722) |
| `…/preamps/0` | `Gain` (float; metadata says −144..65, the Mic range is 10..65), `GainTapered`, `48V`, `Pad`, `HiZ`, `LowCut`, `Phase`. `48V/enabled` and `Pad/enabled` go false in Line mode. Unison slot: `effects/0` (x8:111607; ue/src/topology.rs:281-299) |
| `…/sends/S` | `ID` (aux1, aux2, cue1..cue4), `Gain` (−144..+12; −144 = off), `GainTapered`, `Pan`, `Bypass` (the send mute; there is no `Mute`), `PostFaderOverride` (−1, 0 or 1). The cue3/cue4 nodes exist even when CueBusCount is 2 (x8:111666; ue/src/topology.rs:3-4,71-87; MCP AGENTS.md:169) |
| `…/effects/K` | `EffectName` ("" = empty; setting it loads a plug-in), `Power`, `Preset`. `parameters/N/{Name, NormalizedValue, StringValue, StepValue}` exist only while a plug-in is loaded (MCP PROTOCOL.md:166-168; ue/src/topology.rs:181-207) |
| `/devices/D/auxs/A` | `Name`, `FaderLevel`, `FaderLevelTapered`, `Mute`, `MixToMono`, `SendPostFader`. `sends/0..3` go to the cues (x8:77824) |
| Monitor output | `CRMonitorLevel` (−96..0 dB), `CRMonitorLevelTapered`, `Mute`, `DimOn`, `MixToMono`, `MixInSource` (mon, cue1..4). Find it by the presence of `CRMonitorLevel`, never by index (x8:104464; MCP PROTOCOL.md:153-155) |
| HP outputs | Only `MixInSource`, `Stereo` and `Active`: **no level property**. The level is a hardware knob (x8:104583; MCP README.md:335; ue/README.md:90) |
| `…/meters/K` | K=0 is L, K=1 is R. `MeterLevel` and `MeterPeakLevel` are dBFS floats −77..0 (floor −77, with float noise), `MeterClip` is a bool. The hold times follow `/PeakHold` and `/ClipHold` (x8:111557; ue/src/meters.rs:18-26) |
| Sessions | `set /Session/value "<name>"` loads a session; the other operations are `post` commands such as `post /SaveSession {}`. Their effects were never observed (ue/README.md:131-147). MCP blocks writes to `Session` (safety.py:116) |

**Meter rate.**
- On engine 12.0.0 the meter tick (`/MeterPulse`) runs at about 31 Hz, one tick per 30.7 ms (ue/README.md:92; OA console-rs/crates/rack/src/ballistics.rs:14,30).
- A plain subscription pushes only on change (MCP PROTOCOL.md:83).
- MCP polls `get …/MeterLevel` at about 3 Hz on a second socket instead (PROTOCOL.md:22-24,169-170).
- To avoid flooding (*inferred*): subscribe only `meters/0|1/MeterLevel` and `MeterClip` for the 8 strips in the current bank, and unsubscribe them when the bank changes.

## 4. Writes and warnings

**UMC** writes tapered values: input `FaderLevelTapered` (CC/127), `Pan` (−1..1), `Solo`, `Mute`, preamp `GainTapered`, `48V`, `Pad`, `Phase`, `LowCut`, and `GainTapered` on sends 0-5. It only reads `DeviceName`, `DeviceOnline` and `Name` (TCPListener.swift:58-135).

**Console 1.3.1** (from pcap) writes the tapered properties (`FaderLevelTapered`, `GainTapered`, `CRMonitorLevelTapered`), plus `Mute`, `Solo`, `Pan`, `Pan2`, `Bypass`, `DimOn`, `DimAttenuation`, `MixInSource`, `AltMonSelection` and `/TalkbackOn` (OA console-rs/WIRING.md:80-142).

**MCP** writes dB values, from an allowlist of 35 names (safety.py:59-97). Its rule is to write the dB property, not the tapered one, because tapered values quantize (PROTOCOL.md:103-106). Its live tests write `Name` and preamp `Gain` (tests/live/test_live_smoke.py:75-93).

**OA** never sent a `set` to a real engine (ue/README.md:75-80).

**Warnings.**
- **The engine does not enforce `readonly`.** A write to `DeviceHwID` was applied (MCP PROTOCOL.md:108-119).
- Never write `AppCommand`, `MixerState`, `DeviceHwID`, `MultiUnitOrder`, `ActiveSpans`, `DeviceTypeID`, `Type`, or anything under `/pluginscenes` (safety.py:103-121).
- 48V can destroy ribbon mics.
- Do not change the clock or sample rate while audio is flowing.
- MCP's monitor rail: ceiling −6 dB, at most +6 dB per write and +10 dB per 30 s. It treats unmute and un-dim as level jumps, and allows at most 10 writes/s (SPEC.md:215-238; safety.py:42-47).
- OA coalesces writes into 16 ms windows and ignores engine echoes for 350 ms after a local write, so a moving fader is not pulled back (ue/src/writer.rs:6-7; ue/README.md:64).

## 5. Tapers

- **Faders** (input, aux and send `GainTapered`):
  - Above t = 0.5528 (about −12.6 dB): `dB = 55·t − 43`, so unity is 43/55 = 0.7818182 and t = 1 is +12 dB.
  - Below that: a 361-point lookup table, with t = 0 at −144 dB (−∞) (ue/src/tapers.rs:7-65,175-204).
  - I checked every pair in both fixtures (−9.37 to +0.05 dB): all match within 0.001 dB. The lookup table itself was captured on the x4 only.
- **Monitor:**
  - It uses a step table per model, with t = step/steps. The engine computes `step = (uint)(t·steps + 0.5)`; for dB → t it takes the first step at or above the dB (OA mixer-engine/model_features.py:162-176).
  - Desktop models have 54 steps, rack models 48 (OA mixer-engine/tests/test_monitor_taper.py:1-2,25-28).
  - Twin MkII table (reverse-engineered, unverified): −96, −95.5, −75, −60, −55, −52, −49 … −18, −17, then 0.5 dB steps up to 0 (OA devices/apollo-twin-mkii.json:153).
  - The fixtures agree with this: x4 −52 dB = 5/54, x8 −22 dB = 17/48. The x4-only lookup table in `tapers.rs` mispredicts the x8 as −18 dB.
- **Preamp:**
  - OA assumes `dB = round(10 + 55·t)` (tapers.rs:67-81; mixer-engine/hardware.py:1211-1223).
  - The fixtures disagree: x4 27 dB = 17/54 and x8 15 dB = 5/48. *Inferred:* the preamp uses the same per-model step tables as the monitor.
  - Write `Gain` in whole dB.
- **Pan:** linear, −1 = L100, 0 = C, +1 = R100 (tapers.rs:130-150).

## 6. Versions

- **UMC:** written in 2018, macOS only (TCPListener.swift:5-6; README.md:32-34). Its 22-input example (README.md:64) matches the 22 mixer inputs of OA's Twin MkII descriptor (devices/apollo-twin-mkii.json:115), so the author probably used a Twin (*inferred*).
- **MCP:** UAD 11.7.1 on an x8 Gen 2 under macOS. It expects the Twin to work but has not tested it, nor Windows (README.md:21-25).
- **OA:** engines 11.8.2, 11.9.0 and 12.0.0 all report `ProtocolVersion` 1.0.0. The Windows traces agree with the Mac write format (ue/README.md:84-99; ue/src/command.rs:78-79).
- **11.7.1 vs 12.0.0:** root property names are identical between the 11.7.1 x8 and 12.0.0 x4 trees (my own diff). What changed is the recursive `get` (stubs vs the full tree).
- **Twin MkII** (OA descriptor, marked unverified): 2 preamps, 1 Hi-Z, talkback, S/PDIF or ADAT over optical, at most 2 cue buses, 54-step monitor (devices/apollo-twin-mkii.json:7-18,153).
- **UA Connect and LUNA:** no project mentions them in a protocol context.

**What `extension/src/UadProto.h` does with this (02.11.2026):**
- `set()` targets the `/value` leaf and takes JSON text. `setString` quotes, `setNumber` and
  `setBool` write bare.
- `replyPath()` returns `"multi"` for batched pushes. We do not send `subscribe mode=multi`, so
  none arrive.

## Open questions to measure on the user's Mac (all read-only)

1. `get /Version`, `/ProtocolVersion`, `/devices/0/DeviceTypeID`: which engine version ships with UA Connect?
2. Does `get /?recursive=1` return the full tree or only stubs?
3. Twin MkII layout: the input and output indices, which output carries `CRMonitorLevel`, and whether the HP output has any level property.
4. Turn the hardware knobs and read `Gain`/`GainTapered` and `CRMonitorLevel`/`Tapered` pairs, to confirm the step tables.
5. Move a fader in Console below −12.6 dB and read FaderLevel/Tapered pairs, to check the lookup table.
6. Does every `set` without `func_id` get an ack on this version, and does Console receive the push?
7. Meter push rate and bytes/s, with and without `subscribe mode=multi`.
8. Is port 4710 bound on all interfaces? **Measured 02.11.2026 on the user's Mac: yes,
   `*:4710`.** Rea-Sixty connects to 127.0.0.1 only.
9. During fast fader moves, does the engine echo intermediate values back (so an echo hold is needed)?
