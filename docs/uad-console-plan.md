# UAD-Console-Side-Car: Plan

Stand 02.11.2026, erste Fassung.

- **Gebaut:** das Protokoll (`extension/src/UadProto.h` + Test) und die Lese-Sonde
  (`extension/tools/uad_engine_probe.cpp`).
- **Gemessen:** der Baum der UA Mixer Engine auf dem Mac des Nutzers, nur lesend, ohne
  angeschlossenes Apollo.
- **Protokoll-Referenz:** `docs/uad-mixer-engine-protocol.md`.

Herkunft:

- **[Mac]**: gemessen am 02.11.
- **[Ref]**: aus der Protokoll-Referenz, also aus offenen Projekten.
- **[vermutet]**: nicht gemessen.

---

## 1. Ziel

Entscheid des Nutzers vom 02.11.: Die UF8 steuert das **Apollo Twin MkII direkt** über die UA Mixer
Engine (den Hintergrundprozess der UAD Console). UA Midi Control entfällt, ebenso MIDI-Ports und die
Mackie-Lizenz. Vorbild ist der **TotalMix-Side-Car** (`Uf8Rme`, `RmeUf1`, ORC): Ein fremdes
Mischpult übernimmt die ganze Fläche, mit Namen, Pegeln, Metern und Mute/Solo.

- **UF8 = Apollo-Mixer:** Console-Kanäle auf den Fadern, Namen, Meter, Mute/Solo, Pan, Sends,
  Preamp.
- **UF1 = Monitor-Controller** (eigener Baustein danach): Monitor-Pegel, ALT/DIM/MONO/MUTE,
  Talkback, Monitor-Meter auf dem großen Display. So macht es ORC heute für RME.
- Der **MCU-Layer** ist geparkt, auf Branch `mcu-layer` (Plan, Kern, virtuelle UF8, alles getestet).
  Die virtuelle UF8 (`Uf8Sim.h`) kommt in diesen Branch.

---

## 2. Die Schnittstelle in Kürze [Ref]

- **Verbindung:** TCP `127.0.0.1:4710`, ohne Anmeldung. Jede Nachricht endet mit NUL.
- **Befehle:** `get`, `set`, `subscribe` auf Pfade. Antworten kommen als JSON
  `{"path", "data": {"properties", "children"}}`.
- **Schreiben** zielt immer auf `…/value`. Werte als JSON: Zahlen nackt, `true`/`false`, Texte in
  Anführungszeichen.
- **Fader:** `FaderLevelTapered` 0..1 ist die Stellung des Console-Faders, `FaderLevel` der Wert in
  dB (−144..+12). Oberhalb −12,6 dB gilt `t = (dB + 43) / 55`, also 0 dB = 0,7818.
- **Monitor- und Preamp-Pegel** laufen beim Twin in festen Stufen [Ref; am Twin prüfen].
- **Die Engine prüft „readonly“ nicht selbst.** Rea-Sixty schreibt nur Eigenschaften aus einer
  eigenen Erlaubt-Liste (Abschnitt 5).

---

## 3. Gemessen am Mac (02.11.2026, ohne Apollo) [Mac]

1052 Abfragen, 1052 Antworten, in 378 ms, nur `get`.

- **Engine:** `UA Mixer Engine`, lauscht auf **`*:4710`**, also auf allen Netzwerkschnittstellen und
  ohne Anmeldung. Rea-Sixty verbindet sich nur über 127.0.0.1.
- **Gerät:** `/devices/0` = „Apollo Twin MkII“, `DeviceOnline=false`, `CueBusCount` 2.
  - **Ohne Apollo bleibt das Gerät mit allen alten Werten im Baum.** `DeviceOnline` ist das
    einzige Zeichen, dass es fehlt.
  - Folge: Ist es `false`, zeigt die UF8 „offline“ und schreibt nichts.
- **Baum:**

  | Zweig | Anzahl |
  |---|---|
  | `inputs` | 22 |
  | `preamps` | auf Eingang 0 und 1 |
  | `sends` | je 6: `aux1`, `aux2`, `cue1`–`cue4` |
  | `meters` | je 2: L, R |
  | `auxs` | 2 |
  | `outputs` | 7 |
  | `effects` | 8 Slots je Kanal |
  | `asioinputs` / `asiooutputs` | 24 / 20 |

- **Kanäle**, sichtbar sind die mit `Active` und `EnabledByUser`; `ChannelHidden` ist überall
  false:

  | # | Name | Typ | sichtbar |
  |---|---|---|---|
  | 0 | UAD 1 | Mic | ja |
  | 1 | UAD 2 | Line | ja |
  | 2 | AVALON | ADAT | ja |
  | 3–9 | ADAT 2–8 | ADAT | ja |
  | 10/11 | S/PDIF | S/PDIF, Stereo | nein (Active false) |
  | 12/13 | **REAPER** (StereoName von 12) | Virtual, Stereo | ja (12) |
  | 14/15 | **VoX Return** | Virtual, Stereo | ja (14) |
  | 16–19 | VIRTUAL 5–8 | Virtual | nein (EnabledByUser false) |
  | 20 | TALKBACK | TalkbackMic | nein (Active false) |
  | 21 | N/A 1 | – | nein |

  Bei einem Stereo-Paar trägt die **linke Hälfte** alle Regler und den Namen (`StereoName`).
  Dazu kommen AUX 1 und AUX 2. Das ergibt dieselbe Liste wie in UA Midi Control.

- **Monitor:**

  | Funktion | Pfad |
  |---|---|
  | Pegel (dB −96..0) | `/devices/0/outputs/4/CRMonitorLevel`, dazu `CRMonitorLevelTapered` |
  | Mute | `/devices/0/outputs/4/Mute` |
  | Dim | `/devices/0/outputs/4/DimOn`; Stärke `/devices/0/DimAttenuation` (0..60, Werk 17) |
  | Mono | `/devices/0/outputs/4/MixToMono` |
  | Quelle | `/devices/0/outputs/4/MixInSource` |
  | ALT | `/devices/0/AltMonSelection` (0..2); `outputs/6/AltMonEnabled`, `AltMonTrim` |
  | Talkback | `/devices/0/TalkbackMaster`, Eingang 20, `TalkbackEnabled` je Kanal |
  | Kopfhörer | nur die Quelle (`outputs/5`, `outputs/6` → `MixInSource`: default, cue1–cue4, none) |

  **Die Kopfhörer-Lautstärke ist nicht steuerbar**, sie kommt vom Hardware-Knopf.
- **Meter:** `…/meters/0|1/MeterLevel` und `MeterPeakLevel` in dB, −77..0, dazu `MeterClip`.
- **Fallen:**
  - Manche Eigenschaften haben gar kein `value` (Aux-`Gain`, Pan eines Aux-Sends, IOType von
    Eingang 21).
  - Eine Eingangs-Antwort ist rund 11 KB groß, wegen des Preset-Baums.
  - `ActiveSpans` ist uninitialisiert (der Text „PARA“).
  - Preamp-`GainTapered` passt offline nicht zu `Gain`.
  - Jede Antwort kam unter genau dem gefragten Pfad zurück.

---

## 4. Bedienung an der UF8 (Vorschlag)

Angelehnt an den alten Layer 3 des Nutzers (PAN, SEND, PLUGIN, INPUTS, GAIN, ALT, AUX). Was
offen ist, steht in Abschnitt 6.

**Kanalliste:**

- Die sichtbaren Eingänge, ein Stereo-Paar ist ein Strip. Dahinter AUX 1 und AUX 2.
- Beim Nutzer sind das 14 Strips:

  | Bank | Strips |
  |---|---|
  | 1 | UAD 1, UAD 2, AVALON, ADAT 2–6 |
  | 2 | ADAT 7, ADAT 8, REAPER, VoX Return, AUX 1, AUX 2 |

- BANK ◄ ► springt um 8 Strips.

| Bedienelement | Wirkung |
|---|---|
| Fader | `FaderLevelTapered` direkt. Der UF8-Fader steht also wie der Console-Fader. Gesendet wird nur bei Berührung. Nach eigenem Schreiben hat die Hand 350 ms Vorrang vor dem Echo [Ref] |
| Scribble oben | Name (bei Stereo `StereoName`) |
| dB-Anzeige | `FaderLevel` |
| Wertzeile | was der V-Pot gerade regelt (Pan, Send, Gain) |
| SOLO / CUT | `Solo` / `Mute` |
| SEL | wählt den Kanal (lokal) für die Preamp-Tasten |
| V-Pot, Modus PAN | `Pan`. Bei Stereo vorerst `Pan` der linken Hälfte [am Gerät prüfen: `Pan2`] |
| V-Pot, Modus SEND | `sends/{n}/GainTapered`. SEND/PLUG-IN 1–6 wählen den Send: AUX1, AUX2, CUE1–4. Druck = `Bypass` |
| V-Pot, Modus INPUTS | Preamp-`Gain` in ganzen dB, nur auf Kanälen mit Preamp; die anderen bleiben ohne Ring |
| Soft-Keys, Bank 1 „Monitor“ | ALT (`AltMonSelection` 0↔1), DIM, MONO, MUTE, TALKBACK |
| Soft-Keys, Bank 2 „Preamp“ (gewählter Kanal) | 48V (**halten**), PAD, PHASE, LOW CUT, HI-Z |
| CHANNEL-Encoder | Monitor-Pegel, schrittweise. Druck = Monitor Mute [Entscheid 4] |
| Meter | `MeterLevel` L/R in die zwei UF8-Spalten (`dbToVuByte_`), `MeterClip` = oberste Stufe |
| Farbe | fest nach Kanaltyp (Mic, Line, ADAT, Virtual, Aux); die Console hat keine Farben |
| LAYER 1–3, 360 | bleiben Rea-Sixty |

---

## 5. Sicherheit

1. **Erlaubt-Liste.** Geschrieben werden nur:
   - Eingänge: `FaderLevelTapered`, `Mute`, `Solo`, `Pan`, `Pan2`.
   - Sends: `GainTapered`, `Bypass`.
   - Preamp: `Gain`, `48V`, `Pad`, `Phase`, `LowCut`, `HiZ`.
   - Aux: `FaderLevelTapered`, `Mute`.
   - Monitor: `CRMonitorLevel`, `Mute`, `DimOn`, `MixToMono`, `MixInSource`.
   - Gerät: `AltMonSelection`, `TalkbackMaster`.

   Nie geschrieben werden: `DeviceHwID`, `AppCommand`, `Session`, Clock und Sample-Rate, Plug-ins
   und Presets.
2. **48V** nur mit Halten der Taste (½ s). Phantomspeisung kann Bändchenmikrofone beschädigen.
3. **Monitor-Pegel:** pro Encoder-Raste höchstens eine Stufe. Kein Sprung beim Einschalten.
   Unmute und Un-Dim schreiben nur den Schalter, nie einen Pegel.
4. **Offline** (`DeviceOnline=false`): Die UF8 zeigt „Apollo offline“ und schreibt nichts.
5. **Schreib-Bündelung:** höchstens ein Wert pro Pfad alle 16 ms, der neueste gewinnt [Ref].
6. **Nur 127.0.0.1.** Kein Bonjour, keine fremden Rechner.

---

## 6. Entscheide (offen)

| # | Frage | Vorschlag |
|---|---|---|
| 1 | **Wie hinein?** Side-Car mit Shift + LAYER n wie TotalMix, Hue und DynaMount, oder Layer-Typ „UAD Console“ (wie der CC-Layer: LAYER 3 = Apollo) | Layer-Typ, weil Layer 3 beim Nutzer schon „das Apollo“ ist. Der Kern ist von dieser Wahl unabhängig |
| 2 | MONITOR als eigener Fader hinter den Aux-Kanälen? | nein, Monitor über Encoder und Soft-Keys (und später die UF1) |
| 3 | Belegung der V-POT-Bank | PAN, SEND, INPUTS (Preamp); 4–8 frei |
| 4 | CHANNEL-Encoder = Monitor-Pegel? | ja; NAV schaltet auf Bank blättern |
| 5 | Was die alten Labels GAIN und PLUGIN werden | GAIN entfällt (Preamp = INPUTS). PLUGIN später: Plug-ins des gewählten Kanals (Bypass), nur lesen und an/aus |
| 6 | Mapping-Editor | Stufe 1 sofort: Apollo-Builtins (ALT, DIM, Talkback, 48V …) auf jede Taste, wie `RmeBuiltins`. Stufe 2: Kanalliste und Modi im Editor. Stufe 3: Lernen über Abo |

---

## 7. Im Studio messen (mit `uad_engine_probe`, nur lesend)

- `get /`: Version und ProtocolVersion.
- 15 s Abo bei laufendem Audio, auf diese Pfade:
  - `/devices/0/DeviceOnline`
  - `/devices/0/inputs/0/FaderLevelTapered`, `/devices/0/inputs/0/FaderLevel`
  - `/devices/0/inputs/0/Mute`, `/devices/0/inputs/0/Solo`
  - `/devices/0/inputs/0/meters/0/MeterLevel`, `/devices/0/inputs/0/meters/0/MeterPeakLevel`,
    `/devices/0/inputs/0/meters/1/MeterLevel`
  - `/devices/0/inputs/0/preamps/0/Gain`
  - `/devices/0/outputs/4/CRMonitorLevel`, `/devices/0/outputs/4/Mute`,
    `/devices/0/outputs/4/DimOn`, `/devices/0/outputs/4/meters/0/MeterLevel`

  Das zeigt Meter-Rate und Datenmenge, die Stufen am Preamp- und Monitor-Knopf und ob ein Fader in
  der Console Zwischenwerte schickt.
- **Erster Schreibtest erst danach**, mit Zustimmung des Nutzers: ein Fader von ADAT 8, dann
  zurück.

---

## 8. Aufbau und Bausteine

Wie beim TotalMix-Side-Car: Protokoll, Zustand und Regeln ohne REAPER und ohne Gerät, mit Tests.
Socket und REAPER kommen zuletzt dazu.

| Baustein | Inhalt | Stand |
|---|---|---|
| A | `UadProto.h`, `uad_engine_probe`, Protokoll-Referenz | **gebaut 02.11.** |
| B | `UadModel`: Baum und Updates lesen, Kanalliste, Monitor. Dazu eine virtuelle Engine (`UadSim.h`), gefüttert mit dem gemessenen Baum | **gebaut 02.11.**, `test_uad_model` |
| C | `UadSurface`: UF8 ↔ Modell und Schreibbefehle, Erlaubt-Liste, Bündelung, Echo-Sperre. Getestet gegen virtuelle UF8 und virtuelle Engine. Das ist der Abnahmetest ohne Studio | **gebaut 02.11.**, `test_uad_surface` (11 Szenarien) |
| D | `UadClient`: Socket-Thread, Wiederverbinden, Abos je Bank | |
| E | Rea-Sixty-Anschluss: Eintritt (Entscheid 1), Settings, Apollo-Builtins, Handbuch | |
| F | UF1 als Monitor-Controller | |
| G | Editor und Lernen (Entscheid 6) | |
| H | ORC (Apollo ohne REAPER) | |
| I | Pull Request an acklin83/Rea-Sixty, **nur nach OK** | |

---

## Quellen

- `docs/uad-mixer-engine-protocol.md` und die dort genannten Projekte (UA Midi Control,
  uad-console-mcp, open-apollo).
- Messung auf dem Mac des Nutzers, 02.11.2026: `~/ReaSixty-MCU/uad-report.md`,
  `uad_tree.log` (bleiben auf dem Mac).
