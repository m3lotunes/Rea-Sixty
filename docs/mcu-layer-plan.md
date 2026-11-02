# MCU-Layer auf dem UF8: Analyse und Plan

Stand 02.11.2026, erste Fassung. **Nichts gebaut**, nur gelesen, recherchiert und geplant. Vorlage
ist `docs/midi-cc-layer-plan.md`. Zeilennummern beziehen sich auf Commit `9455d76` (upstream
`main`, 02.11.2026). Herkunft jeder Aussage:

- **[Code]**: im Repo gelesen, mit Datei und Zeile.
- **[Hb S. X]**: SSL UF8 User Guide Rev 11 (`docs/docs/SSL UF8 User Guide_Rev11.pdf`). PDF-Seite =
  gedruckte Seite.
- **[Web]**: öffentliche Quelle, Liste am Ende.
- **[Mac]**: auf dem Mac des Nutzers gelesen (Inspektion vom 02.11., Abschnitt 2.5).
- **[vermutet]**: abgeleitet, nicht gemessen. Steht unter „Zuerst messen“ (Abschnitt 8).

---

## 1. Was der Layer ist

Einer der drei UF8-Layer bekommt die Art **MCU (external app)**. Steht die UF8 auf diesem Layer, ist
sie für ein fremdes Programm eine Mackie Control Unit (MCU) an eigenen virtuellen CoreMIDI-Ports
„Rea-Sixty MCU“. Erster Abnehmer: **UA Midi Control** (Fonoflow) im Mackie-Modus. Das Programm
steuert ein UA Apollo Twin über UAD Console: Fader, Mute/Solo, Sends, Namen auf den Displays.

So lief es früher mit SSL 360°: UF8-Layer 3 auf einem MCU-DAW-Profil, UA Midi Control an
„SSL V-MIDI Port 9“. Rea-Sixty ersetzt SSL 360° (beide wollen das USB-Gerät), deshalb muss
Rea-Sixty diese Rolle selbst übernehmen.

Erste Version: nur macOS, nur eine UF8. LAYER 1–3, 360 und die Rea-Sixty-eigenen Aktionen bleiben
bei Rea-Sixty, damit man jederzeit zu REAPER zurückkommt.

---

## 2. Wer braucht was

### 2.1 UA Midi Control (UAMC) im Mackie-Modus [Web]

Es gibt keine veröffentlichte Spezifikation. Die Befunde stammen aus Release Notes,
Entwickler-Posts und Demo-Videos [W1–W13].

| Thema | Befund |
|---|---|
| Lizenz | „Mackie“ ist eine eigene Lizenz (der Nutzer hat Midi+Mackie, Screenshot: 5.11.2) |
| Einrichtung | Settings → Midi: Ein- und Ausgang wählen, **Mode = „Mackie“** pro Port. Ohne Mode tut sich nichts [W3]. Ab 5.8.0 werden *manche* Mackie-Geräte automatisch erkannt [W2] |
| Mehrere Geräte | Jedes Mackie-Gerät hat eine **„Order“** (1–8, 9–16 …). X-Touch + Extender laufen [W2, W6, W7] |
| Virtuelle Ports | Werden seit 5.0 erkannt. Laut Entwickler wechseln ihre IDs bei jedem Neustart, UAMC gleicht das aus [W8]. IAC echot alles, UAMC hat seit 5.11 eine Schleifenerkennung |
| Zustand | Bei Programmstart und bei jedem (Wieder-)Verbinden schickt UAMC den ganzen Console-Zustand. Fest 100 ms Verzögerung, dazu einstellbar „Initialization Delay“. „Refresh Device/All“ lässt sich auf eine MIDI-Nachricht legen [W2, W9] |
| Was es nutzt | Fader 1–8 = aktuelle Console-Bank, **Master-Fader = Master/Monitor**. Mute, Solo, Rec, Select, Fader-Touch (schaltet die untere LCD-Zeile auf Volume), V-Pots, V-Pot-Druck, FLIP, BANK/CHANNEL, Global-View-Tasten [W2, W10, W11] |
| Assign-Tasten | TRACK = Namen + Volume, PAN = Pan, SEND = Sends der gewählten Spur, PLUG-IN = Inserts (blättern mit Zoom/Jog), EQ = Preamp + Unison, INSTRUMENT = Gains [W2, W10–W12] |
| Weitere Tasten | SCRUB = Console-Fenster, ZOOM = Stereo, F1 = DAW-Modus. **Transport wird für Monitor-Funktionen genutzt** (2025: STOP = Monitor Mute, REW/FF = ALT 1/2), die Belegung wechselt je nach Version [W2, W7, W10] |
| Displays | Obere Zeile Kanalnamen, untere Zeile Pan bzw. beim Fader-Touch dB [W6, W10] |
| Meter | Teil der Mackie-Lizenz. Ein Fix in 5.8.0 („Master-Meter auf Kanal 1“) zeigt: UAMC sendet auch ein Master-Meter [W2] |
| DAW-Modus | UAMC legt **pro Mackie-Gerät einen weiteren virtuellen Port „<Port> DAW“** an [W2] |
| Farben | Steuert UAMC nicht (Stand Nov. 2023) [W13] |
| Handshake | **Nicht belegt.** Läuft mit Stream Deck, FaderPort und EuControl im Mackie-Modus. Ein Handshake ist also wahrscheinlich nicht nötig [vermutet] |

### 2.2 SSL 360° und die V-MIDI-Ports [Hb, Web]

- **12 Ports.** Port = 4 × (Layer − 1) + Nummer der UF8 (1–4). Layer 3 auf der ersten UF8 = **Port 9**
  [Hb S. 20, 25].
- **Namen auf dem Mac:** „SSL V-MIDI Port N Source“ / „… Destination“. Unter Windows nur
  „SSL V-MIDI Port N“ [Hb, W14–W16].
- **Wer die Ports anlegt:** Kein Treiber unter `/Library/Audio/MIDI Drivers`. Sehr wahrscheinlich
  virtuelle Endpunkte des laufenden Dienstes `SSL360Core` [vermutet, W17]. Ob sie ohne SSL 360°
  existieren, siehe 2.5.
- **Kein generisches MCU-Profil.** Profile gibt es nur pro DAW (Logic, Cubase/Nuendo, Live,
  Studio One, LUNA, REAPER …; Pro Tools = HUI) [Hb S. 6, 25]. Für andere MCU-Software empfiehlt SSL
  das **Logic-Profil**. UAMC-Nutzer nehmen ebenfalls Logic [W18, W19].
- **Mehrere UF8:** Die erste ist „Mackie Control“, jede weitere ein „Mackie Control Extender“ auf
  ihrem eigenen Port (Bank-Offset 8/16/24) [Hb S. 39, 114].
- **Handshake / Geräte-ID:** Das Handbuch schweigt. MCU-Layer zeigen kein „Waiting for DAW“, nur
  Pro Tools (HUI) tut das [Hb S. 184]. SSL 360° wartet also wohl nicht auf einen Host [vermutet].
- **Inaktive Layer laufen weiter:** Im Hybrid-Modus wird die MCU-Bank eines nicht angezeigten
  Layers „nicht synchronisiert“ [Hb S. 182 f.]. Die MCU-Sitzung jedes Layers lebt also im
  Hintergrund weiter. Das ist dasselbe Bild wie der Schatten-Zustand in diesem Plan.
- **Bekannte UAMC-Probleme mit SSL 360°:**
  - Ein Nutzer hatte leere Displays auf Layer 3 / Port 9, auf Layer 1 / Port 1 aber Text [W3].
  - Das Monitor-Meter landete auf dem jeweils linken Strip jeder Bank [W20].
  - UAMC stürzte einmal beim Layerwechsel ab [W19].

### 2.3 Die Logic-Belegung von SSL [Hb S. 40–49]

Das ist die Vorlage für die Tastenbelegung (Abschnitt 4). Die MCU-Notennummern stehen nicht im
Handbuch, sie stammen aus der MCU-Spezifikation [W21].

- **SEL:** im Modus NORM = Select, im Modus REC = Rec-Arm, die SEL-Lampe leuchtet dann rot [S. 41].
- **FINE / SHIFT:** Fine, solange gedrückt, und zugleich der **Cmd/Alt**-Modifier [S. 44].
- **PAN** = Pan-Assign, **PLUGIN** = Plug-In-Assign. Nochmal drücken schaltet zwischen Mixer View
  und Channel View um. Halten zeigt ein Untermenü auf den LCDs [S. 45].
- **SEND/PLUG-IN-Lampen** zeigen den gewählten Send-/Plug-in-Slot, die CHANNEL-Lampe die Channel
  View [S. 46]. Beim Drücken unter Logic: nicht beschrieben. Unter Cubase: MCU-Fader-Bank 1–8
  [S. 57].
- **PAGE ◄ ►:** unter Logic ohne Funktion, unter Cubase MCU „CHANNEL ◄ ►“ [S. 47, 57].
- **BANK ◄ ►:** springt um 8 × Anzahl der Units [S. 42].
- **CHANNEL-Encoder:** Drehen verschiebt um einen Kanal. NAV = Abspielkopf. NUDGE ist frei belegbar
  [S. 42].
- **CHANNEL-Encoder gedrückt halten:** Die Cursor-Tasten werden zum Transport. ↑ PLAY, ↓ STOP,
  ← REW, → FF, ◯ REC [S. 16, 43].
- **Automation:** OFF/READ/WRITE/TRIM/LATCH/TOUCH wirken auf die gewählte Spur, mit Opt auf alle
  [S. 44].
- **Soft-Key-Bänke (Logic-Werkseinstellung)** [S. 47 f.]:

| Bank | Tasten 1–8 |
|---|---|
| V-POT | SEND, INSTRUMENT, EQ, TRACK, GROUP, MARKER, NUDGE, – |
| 1 | REWIND, FORWARD, STOP, PLAY, RECORD, CYCLE, –, – |
| 2 | CLICK, DROP, REPLACE, MARKER, SOLO, –, NUDGE, SCRUB |
| 3 | Ctrl, Opt, Alt, Shift, SAVE, UNDO, CANCEL, ENTER |
| 4 | MIDI TRACKS, INPUTS, AUDIO TRACKS, AUDIO INST, AUX, BUSSES, OUTPUTS, USER |
| 5 | F1–F8 |

- **LUNA (UAs eigene DAW)** führt als „ohne Funktion“: Track, Plugin, Global, Name/Value, F3–F8,
  die acht View-Tasten, Save, Cancel, Enter, Drop, Fussschalter [S. 94]. Das ist ein Hinweis darauf,
  was UA-Software überhaupt auswertet.

### 2.3a Die Bitwig-Belegung von SSL [Hb S. 124–133]: die, die der Nutzer hatte (2.5)

- **V-POT-Bank = Assign-Tasten:** 1 PAN, 2 SEND, 3 PLUGIN, 4 EQ+, 5 TRACK REMOTES, 6 PROJECT REMOTES,
  7 CLIP LAUNCHER, 8 NAME/VALUE [S. 129]. Die PAN-Taste rechts ist ebenfalls PAN.
- **SEND/PLUG-IN 1–8** wählen im SEND-Modus den Send, im PLUGIN-Modus das Plug-in, bei EQ+ und den
  Remotes die Seite. Im PAN-Modus haben sie keine Funktion. **CHANNEL** im SEND-Modus: alle Sends der
  gewählten Spur [S. 129 f.].
- **PAGE ◄ ►:** vorherige/nächste Parameterseite bzw. Send. Im PAN-Modus wählen sie die
  vorherige/nächste Spur [S. 128].
- **FINE/SHIFT = SHIFT** (mehrere Spuren solo, Gruppe auf/zu mit SEL) [S. 128].
- **Automation:**
  - Es gelten OFF, READ, WRITE, LATCH und TOUCH. TRIM gilt nicht.
  - OFF = Automation wiederherstellen [S. 128].
- **Cursor:**
  - ↑/← vorherige, ↓/→ nächste Spur. ZOOM schaltet den Zoom.
  - Bei Sends und Plug-ins blättern ↑/↓ [S. 126].
  - Transportmodus wie bei Logic (CHANNEL halten) [S. 127].
- **Encoder:** NAV = Abspielkopf (mit SHIFT feiner). NUDGE ist frei. FOCUS = Mausrad [S. 126].
- **Quick 1–3:** Arranger/Mix umschalten, Global View, SSL-Plug-in einfügen [S. 128].
- **Selection Mode:** NORM = SEL wählt, REC = SEL ist Rec-Arm, AUTO ohne Funktion [S. 125].
- **User Bank 1:** REWIND, FORWARD, STOP, PLAY, RECORD, LOOP, PUNCH IN, PUNCH OUT.
- **User Bank 2:** METRONOME, TEMPO, CUE MARKER, PREV MARKER, NEXT MARKER, GROOVE, INS. SSL PLUGIN,
  UNDO.
- **User Bank 3–5:** frei [S. 132].
- **LCD:** UpLCD = 6 Zeichen Spurname, LowLCD = Wert. Beim Fader-Bewegen zeigt LowLCD dB [S. 124 f.].
- **Welche MCU-Note** SSL für EQ+, TRACK REMOTES, PROJECT REMOTES, CLIP LAUNCHER und für
  SEND/PLUG-IN 1–8 sendet, steht nicht im Handbuch.
  - **Aus keinem Handbuch zu lesen:** Bitwig wird über **Bitwigs eigene Erweiterung „SSL UF8/UF1
    by Bitwig v1.0“** angebunden, nicht über ein offenes Skript [S. 123]. Das Handbuch nennt nur,
    was eine Taste *in Bitwig* tut. Welche Note SSL 360° dafür sendet, legen SSL und Bitwig unter
    sich fest. Das SSL-Log nennt das Protokoll „McuBitwig“, also MCU mit SSLs Belegung.
  - **Wir brauchen diese Noten aber nicht** (Entscheid 02.11.). UAMC versteht Standard-MCU, denn es
    läuft mit X-Touch, FaderPort und anderen. Die Labels des Nutzers (INPUTS, GAIN, AUX, ALT, 2.5)
    sagen, welche UAMC-Funktion er auf welcher Taste benutzt hat. Wir senden für genau diese
    Funktion die Standard-MCU-Note. Der einzige Fall, der an SSLs Note hing (Alt-Monitor = Note 70),
    ist gelöst: Die ALT-Taste sendet Note 70.

### 2.4 Die UF8 und Rea-Sixty heute [Code]

**Die UF8 hat kein eigenes USB-MIDI.** Sie hängt nur am Hersteller-Interface (VID 0x31E9 /
PID 0x0021, EP 0x02/0x81, `docs/protocol-notes.md`). Alles MCU macht SSL 360° bzw. künftig
Rea-Sixty.

**Was die UF8 anzeigen kann** (pro Strip, `Protocol.h:128-139`):

| Zelle | Format | Frame | Für MCU |
|---|---|---|---|
| Scribble oben / unten | 2 × 8 Zeichen | `buildStripTextUpper` / `…Lower`, `Protocol.h:78-81` | MCU-Zeile 1 / 2 (je 7 Zeichen) |
| Wertzeile | 19 Zeichen | `buildValueLine`, `Protocol.h:311` | frei |
| dB-Anzeige | 6 Zeichen + Einheit | `buildFaderDbReadout` | frei |
| CS-Typ-Zelle | 4 (bis 12) Zeichen | `buildChannelStripType` | z. B. „MCU“ |
| Kanalnummer | 1–3 Zeichen | `buildChannelNumber` | frei |
| V-Pot-Balken | Position + Modus unipolar/bipolar/aus | `buildVPotReadoutBar`, `buildVPotMode`, `Protocol.h:325-339` | MCU-Ring |
| VU-Meter | 0..31, je Strip „in“ + „out“ | `buildVuMeter`, `Protocol.h:375-381`; dB→Byte `dbToVuByte_` (−55..0 dB), `main.cpp:35152` | MCU-Meter |
| SOLO/CUT/SEL-LED | Farbpaar | `buildLedColourPair` | MCU-LEDs; REC = SEL rot |
| Soft-Key oben | Text + LED | `buildPluginSlotName`, `buildTopSoftKeyLed` | Beschriftung der MCU-Taste |
| Globale LEDs | 47 Lampen | `kUf8GlobalLedTable`, `Protocol.cpp:759` | Assign, FLIP, Automation, Zoom, Transport |

- **Die LED-Zellen** stimmen in `Protocol.cpp:759`. `docs/uf8-global-led-map.md` ist für Layer 1/2
  und PAGE ◄ ► veraltet.
- **Blinken kann die Firmware nicht** (`GlobalLedState` = Off/Dim/Bright, `Protocol.h:180`).
  Rea-Sixty blinkt im Takt seiner eigenen Uhr, z. B. `main.cpp:46392`. MCU-Velocity 1 („blinkt“)
  braucht also unseren Takt.

**Was die UF8 meldet** (`docs/buttons-leds-quickref.md`):

- **Fader:** `FF 21 03`, 0..15583 (`kUf8FaderPbMax`, `main.cpp:20364`). Zwei Datenströme und
  Endanschläge, beides gelesen über `uf8fader::read` (`Uf8Fader.h`).
- **Touch:** `FF 20 02`.
- **V-Pot:** `FF 24 02`, 6 Bit mit Vorzeichen; Pot 0x08 = CHANNEL-Encoder.
- **Tasten:** `FF 22 03 <id> 00 <0/1>`.

**Ein Layer ist heute REAPER oder MIDI CC.**

- Typ: `enum class LayerKind { Reaper, MidiCc }`, `Bindings.h:853`.
- JSON:
  - Geschrieben wird `"kind": "midi_cc"` (`Bindings.cpp:2142-2147`).
  - Gelesen wird mit `strcmp(s, "midi_cc")`, alles andere gilt als REAPER (`Bindings.cpp:3215-3222`).
  - **Folge:** Ein älteres Rea-Sixty lädt eine Datei mit `"kind": "mcu"` ohne Fehler als
    REAPER-Layer. Speichert es danach, ist die Art weg. Der MCU-Block selbst bliebe nur erhalten,
    wenn er wie `midi_cc` als eigener Schlüssel steht und das alte Rea-Sixty unbekannte Schlüssel
    durchreicht. Das tut es nicht, es schreibt nur, was es kennt.
- Merker für den aktiven CC-Layer: `g_ccActiveLayer`. Atomar, vom USB-Thread lesbar, gesetzt in
  `refreshCcActiveLocked_` (`Bindings.cpp:10066-10071`) auf dem Layer `g_cfg.activeLayer`.

**Die alten virtuellen Ports „reaper_uf8 in/out“** (`MidiBridge.cpp`, gestartet in `initDevices_`,
`main.cpp:26609-26618`) sind ein Rest aus der CSI-Zeit. Für MCU taugen sie nicht, aus drei
Gründen:

1. `onMidiFromReaper` (`main.cpp:39620-39689`) schreibt **direkt aus dem Core-MIDI-Thread an die
   UF8** (`g_dev->send`). Das stört jeden Maler.
2. Nicht behandelte UF8-Tasten gehen als `90 <UF8-Tasten-ID>` hinaus (`main.cpp:28598-28601`). Das
   sind keine MCU-Nummern: SOLO 1 ist auf der UF8 0x20, in MCU 0x08.
3. `openUf8Output` sucht ein Ziel mit „UF8“ im Namen (`MidiBridge.cpp:59-103`). Die SSL-Ports heißen
   „SSL V-MIDI Port N …“, die Suche greift also nie.

Der MCU-Layer bekommt deshalb eigene Endpunkte. Die alten bleiben unverändert.

**Threads:**

- **USB-Eingang:** pro Unit `onUf8InputUnit_` (`main.cpp:27464`) unter `g_uf8InputMx`.
- **Core MIDI:** `readProc_`. Dort keine REAPER-API.
- **Main-Thread:** `onTimer`. In der Reihenfolge `ccTick_` → `drainInputQueue` → Maler
  (`main.cpp:50026`, `50172-50181`).

**ORC** (eigene Mac-App, `extension/orc/`) übernimmt UF8 und UF1, wenn REAPER beendet wird
(`docs/orc-uf8-plan.md`). Die MCU-Ports leben im REAPER-Prozess. Ohne REAPER gibt es also auch kein
„Rea-Sixty MCU“, siehe Entscheid 9.

### 2.5 Auf dem Mac des Nutzers [Mac]

Nur lesende Inspektion am 02.11.2026 über Remote Control (Bericht `~/ReaSixty-MCU/inspection-report.md`
auf dem Mac). Die Umgebung:

- macOS 14.7.2, arm64.
- Es laufen REAPER, UA Midi Control 5.11.2 und UAD Console.
- **SSL 360° läuft nicht.** Der Dienst `com.sslcontrolroom.daemon` wurde beendet. Die Version ist
  2.1.12.
- ORC ist nicht installiert.

**Die SSL-V-MIDI-Ports gibt es nur, solange SSL 360° läuft.** Ohne SSL 360° existiert kein
„SSL V-MIDI Port 1–12“, und unter `/Library/Audio/MIDI Drivers` liegt nichts. Das bestätigt 2.2: Es
sind virtuelle Endpunkte von SSL360Core. Deshalb findet UAMC heute keinen Mackie-Partner mehr.

**Das alte Profil war Bitwig, nicht Logic.**

- In SSL 360° stehen die V-MIDI-Ports 1–8 auf „Reaper MCU“ und **9–12 auf „Bitwig MCU“**.
- Layer 3 = „DAW 3“, also Port 9.
- Für Layer 3 sind die Bitwig-Soft-Key-Modi für Automation und Send/Plug-in eingeschaltet.
- Die Bitwig-Belegung steht im Handbuch auf S. 124–133 (Abschnitt 2.3a).

**UAMC hängt an einer festen Port-ID und an einer MCU-Note.**

- Das Preset `MELOTUNES.ua` enthält genau ein eigenes Mapping: **Alt-Monitor-Auswahl ← Note On 70
  (0x46), Velocity 127, Kanal 1, von „SSL V-MIDI Port 9 Source“, Quell-ID 1821581804.**
- SSL 360° hat für Port 9 dieselbe ID gespeichert. SSL vergibt also feste IDs, und UAMC merkt sich
  Mappings über diese ID.
- 0x46 ist im MCU-Standard **SHIFT**. **Welche UF8-Taste die Note 70 geschickt hat, ist offen:**
  - **Kandidat 1:** Die Soft-Key-Taste „ALT“ (Bank 1, Taste 1). Ihr Label deutet auf die
    Alt-Monitor-Taste. Belegt ist sie mit dem SSL-Befehl `AssignPanSur`, und der sendet im
    MCU-Standard 0x2A, nicht 0x46. Dann hätte SSLs Bitwig-Profil eine eigene Notenzuordnung.
  - **Kandidat 2:** FINE/SHIFT, die im Bitwig-Profil SHIFT sendet [Hb S. 128].
  - Klären lässt sich das nur mit dem Mitschnitt (8 A).
- **Folgen:**
  - Die Taste, die der Nutzer als „ALT“ bedient (Soft-Key Bank 1 Taste 1), **sendet auf dem
    MCU-Layer Note 70**. Dann greift sein UAMC-Mapping, egal was SSL intern tat.
  - „Rea-Sixty MCU“ braucht eine eigene feste ID.
  - Das Alt-Monitor-Mapping muss in UAMC einmal auf den neuen Port umgehängt werden. Die SSL-ID zu
    übernehmen wäre falsch, sie kollidiert, sobald SSL 360° läuft.

**Die Layer-3-Belegung des Nutzers in SSL 360°** (Port 9, Bitwig, `LayerOption 14`, AutoSave =
`Melostalker.xml`; Tabellen auf dem Mac unter `~/ReaSixty-MCU/collected/ssl/`):

| Bereich | Taste | SSL-Befehl | Label des Nutzers | Bedeutung in UAMC (2.1) [vermutet] |
|---|---|---|---|---|
| V-POT-Bank | 1 | AssignPanSur | – (orange) | PAN |
| | 2 | AssignSend | – | SEND |
| | 3 | AssignPlugin | – | PLUG-IN = Inserts |
| | 4 | AssignEQ | **INPUTS** | EQ = Preamp + Unison |
| | 5 | Bitwig.TrackRemotes | **GAIN** | wohl INSTRUMENT = Gains |
| | 6–8 | – (aus) | | |
| Bank 1 | 1 | AssignPanSur | **ALT** | Alt-Monitor (Mapping auf Note 70) |
| | 2–8 | – (aus) | | |
| Bank 2–5 | alle | – (aus/leer) | | |
| Quick | 1 | GlobalView | – | GLOBAL |
| | 2 | Bitwig.InsSslPlugin | **AUX** | wohl Global View AUX |
| | 3 | – (aus) | | |
| Fussschalter | 1 / 2 | Stop/Play, Rec | – | Werk |

- SEND/PLUG-IN und Automation stehen auf User-Modus, aber ohne Belegung. Auf Layer 3 waren sie
  also ohne Funktion.
- Pan/Send auto-fine sind aus.
- Im SSL-Log steht nichts zu Handshake oder Device Query. Port 9 lief als „Bitwig Master“, die
  Ports 10–12 als „Bitwig Extender“.
- **Das ist die eigentliche Vorlage für die erste Version:** Der Nutzer hatte die UF8 schon auf
  UAMC zugeschnitten (INPUTS, GAIN, AUX, ALT). Wir bauen dieselbe Bedienung, mit den MCU-Noten, die
  UAMC für diese Funktionen erwartet. SSLs interne Bitwig-Noten brauchen wir dafür nicht.

**UAMC-Ports heute** (alle aktiv, `order` 0):

| Eingang | Ausgang | `mode` |
|---|---|---|
| IAC Bus 2 | IAC Apollo_IN | 0 |
| IAC Apollo_IN | IAC Bus 1 DAW | 1 |
| reaper_uf8 out | – | 0 |
| KONTROL S49 (4 Ports) | dieselben | 0 |

- „SSL V-MIDI Port 9“ steht nicht mehr in der Liste. Ob `mode` 1 „Mackie“ bedeutet, ist offen.
- Die „… DAW“-Ports (z. B. `reaper_uf8 out DAW`, `IAC-Treiber Apollo_IN DAW`) legt UAMC selbst an
  (2.1).
- **Der IAC-Treiber ist offline.** Die echten IAC-Busse existieren gerade nicht.

**Rea-Sixty heute** (`bindings.json` Version 57, `reaper_rea-sixty.dylib` vom 30.10.):

| Layer | Art | Bemerkung |
|---|---|---|
| 1 | REAPER | |
| 2 | REAPER | mit leerem CC-Rest |
| 3 | **MIDI CC** | Ausgang „IAC-Treiber – Apollo_IN“, 6 Seiten, Werkseinstellung (Fader CC 1, 11, 2, 21, 7, 10, 74, 71) |

Der Rückfall über den CC-Layer (Entscheid 10) ist also schon eingerichtet. Er kann aber nicht
gehen, solange der IAC-Treiber offline ist (Audio-MIDI-Setup → IAC-Treiber → „Gerät ist online“).

**SSL-360°-Log:**

- Die UF8 erscheint als „V-UF8 (1)“, also als virtuelle Kachel, weil Rea-Sixty das USB-Gerät hält.
- Heute bis 18:28 kamen **7.830 SysEx-Fehler**: „Ran out of data reading sequence of sys. ex. UMP
  data messages“ / „unable to convert it to a valid MidiPacket“. Abgeschnittene SysEx sind an einem
  SSL-Port angekommen. Port und Absender stehen nicht im Log.
- Für uns heißt das: SysEx über Paketgrenzen sauber zusammensetzen und Bruchstücke verwerfen, ohne
  den Zustand zu stören.

Aus dem Screenshot vom 02.11.:

- UAMC 5.11.2, Lizenz Midi+Mackie, Session „MELOTUNES REC“.
- MIDI-Mapping auf IAC „Apollo…“: Volume = CC 1, Mute = CC 22 (Momentary).
- Kanäle: UAD 1/2, AVALON, ADAT 2–8, REAPER, VoX Return, AUX 1/2, TALKBACK, CUE OUTPUT, MONITOR.

---

## 3. Karte: wo der CC-Layer eingreift, und was der MCU-Layer dort tut

Der Auftrag nannte rund 13 Stellen. Es sind 20 in `main.cpp` und 7 außerhalb. Jede braucht ein
MCU-Gegenstück. Vorschlag: ein gemeinsamer Merker „die UF8 gehört einem fremden Layer“ (CC oder
MCU) für die Stellen, die nur REAPER stilllegen. Eigene Zweige gibt es nur dort, wo MCU etwas
anderes tut.

| # | Stelle | Was der CC-Layer tut | MCU-Layer |
|---|---|---|---|
| 1 | `main.cpp:668-674` `uf8CcOwnsUnit_` | Gehört die Eingabe dieser Unit dem Layer? (jeder Thread) | `uf8McuOwnsUnit_`: nur Unit 0; Unit 1 dunkel (v1) |
| 2 | `main.cpp:6846-6852` `PendingInput::Cc*` + Drain `23305` | Rohdaten → Main-Thread | **Nicht nötig für die Ausgabe**: MCU-Bytes gehen direkt vom USB-Thread an Core MIDI (wie heute `main.cpp:28600`). Nur Touch-Loslassen kommt vom Main-Thread |
| 3 | `main.cpp:23146-23290` Laufzeit, `ccSend_`, `ccApplyInput_` | Werte, `StuffMIDIMessage` | eigener Block: Port, Schatten, Ausgabe; reine Logik in `McuLayer.h` |
| 4 | `main.cpp:26257` `sendLed` | früh raus | ebenso (REAPER-Ereignisse dürfen die LEDs nicht malen) |
| 5 | `main.cpp:27581-27600` Fader `FF 21 03` | `CcFader` + Echo als Motorziel | Pitchbend Kanal n senden (nur bei Touch, wie dort), Echo wie dort |
| 6 | `main.cpp:27768-27816` Tasten `FF 22 03` | eigene Tasten, Liste der REAPER-Tasten | Tastentabelle UF8 → MCU (Abschnitt 4), nur LAYER 1–3 und 360 fallen durch |
| 7 | `main.cpp:28628-28648` Touch `FF 20 02` | Motor schlaff, keine Spurwahl | ebenso, dazu Note 0x68+n 7F |
| 8 | `main.cpp:28880-28885` V-Pot `FF 24 02` | `CcVPot`, Encoder 0x08 fällt durch | CC 0x10+n relativ. Encoder 0x08: CHANNEL ◄ ► bzw. Jog (Abschnitt 4) |
| 9 | `main.cpp:29079-29090` `onUf8Input2` | rechte Unit verwerfen | ebenso (v1) |
| 10 | `main.cpp:40827-40866` `ccTick_` | Layer erkennen, Betreten/Verlassen, Caches | `mcuTick_`: dazu Core-MIDI-Queue leeren, Schatten pflegen, **auch wenn der Layer nicht aktiv ist** |
| 11 | `main.cpp:41044-41053` `ccColorForStrip_` + `50172` | Farbbalken | feste Layer-Farbe |
| 12 | `main.cpp:41077-41200` `ccPaintSurface_` + `50180` | Maler statt `pushZonesForVisibleSlots` | `mcuPaintSurface_`: LCD, LEDs, Motor, Ring, Soft-Key-Beschriftung |
| 13 | `main.cpp:41636` Side-Car-Soft-Keys | aus bei CC | aus bei MCU |
| 14 | `main.cpp:44644` `commitDebouncedTouchReleases` | Touch endet, sonst nichts | dazu Note 0x68+n 00, danach Motor auf den Schattenwert |
| 15 | `main.cpp:45012` `pushVuMeter` | Meter aus | MCU-Meter einspeisen (Abschnitt 4.3), Muster TotalMix `45057` |
| 16 | `main.cpp:46012-46240` `pushUf8GlobalLeds` | Seiten-LEDs, Rest dunkel, SEND/PLUG-IN | globale LEDs aus dem MCU-Schatten |
| 17 | `main.cpp:50026` Aufruf `ccTick_` | vor dem Drain | `mcuTick_` daneben |
| 18 | `main.cpp:50574` GR-Meter | rechte Unit dunkel | GR-Zeile ganz aus (MCU kennt kein GR) |
| 19 | `main.cpp:55823` `reasixty_ccLivePage` | Seite für Settings | `reasixty_mcuStatus` für die Statusanzeige |
| 20 | `main.cpp:26609` `initDevices_` / `27156` Abbau | MidiBridge | Ports „Rea-Sixty MCU“ anlegen/abbauen, dauerhaft |
| 21 | `Bindings.h:853`, `Bindings.cpp:2142, 3217, 10066-10110` | Art, JSON, Merker | `LayerKind::Mcu`, `"kind": "mcu"`, `g_mcuActiveLayer` |
| 22 | `SettingsScreen_MidiCc.cpp:390-423` `drawUf8LayerKindRow` | Dropdown REAPER / MIDI CC | dritter Eintrag „MCU (external app)“ + MCU-Block |
| 23 | `SettingsScreen.cpp:7735, 7823` | CC-Editor statt Schaltplan | MCU-Block statt Schaltplan |
| 24 | `MixerWindow.cpp:332` Suchindex | „Layer type“ | „MCU“, „Mackie“ dazu |
| 25 | `CMakeLists.txt:897-899` | `test_midi_cc_layer` | `test_mcu_layer` |
| 26 | `docs/user-manual.md:3491` „Keys on a MIDI CC layer“ | Tastenliste | eigener Abschnitt „MCU layer (UF8)“ |

---

## 4. Abbildung UF8 ↔ MCU (Vorschlag)

Vorlage ist das Logic-Profil von SSL (2.3), angepasst an das, was UAMC nutzt (2.1). Alles, was
nicht aus dem Handbuch kommt, ist ein Vorschlag und steht unter den Entscheiden (Abschnitt 7).

**Korrektur nach der Mac-Inspektion (2.5):** Der Nutzer hatte das **Bitwig**-Profil (2.3a) und
darauf seine eigene UAMC-Belegung (Tabelle in 2.5). Die erste Version folgt **dieser** Belegung:

- V-POT-Bank: PAN, SEND, PLUG-IN, INPUTS (= EQ 0x2C), GAIN (= INSTRUMENT 0x2D).
- Soft-Key Bank 1 Taste 1 „ALT“ = Note 70.
- QUICK 1 = GLOBAL 0x33, QUICK 2 „AUX“ = Global View AUX 0x42.
- FINE = SHIFT 0x46 (Bitwig).

Die Logic-Spalten unten bleiben als Rückfall stehen. Was davon UAMC wirklich auswertet, zeigt der
Mitschnitt oder die Sonde (8 A / B2).

### 4.1 Strips (UF8 → MCU)

| UF8 | MCU | Bemerkung |
|---|---|---|
| Fader n | Pitchbend Kanal n, 14 bit | 0..15583 ↔ 0..16383 linear. **Nur bei Touch**, sonst Motor-Echo-Schleife. Nur Änderungen senden |
| Touch n | Note 0x68+n, 7F / 00 | 7F sofort (USB-Thread), 00 nach der Entprellung (Main-Thread, Stelle 14) |
| V-Pot n drehen | CC 0x10+n relativ | +k → `k` (1..63), −k → `0x40 \| k`. Die UF8 liefert −32..+31 pro Frame |
| V-Pot n drücken | Note 0x20+n | |
| SOLO n | Note 0x08+n | |
| CUT n | Note 0x10+n (Mute) | |
| SEL n | Note 0x18+n (NORM) bzw. 0x00+n (REC) | Modus siehe Entscheid 5 |

### 4.2 Globale Tasten (UF8 → MCU)

Die UF8-IDs stammen aus `docs/buttons-leds-quickref.md`, die MCU-Nummern aus [W21].

| UF8-Taste (ID) | MCU (Note) | Quelle |
|---|---|---|
| BANK ◄ ► (0x78/79) | BANK ◄ ► 0x2E/2F | Hb S. 42 |
| PAGE ◄ ► (0x52/53) | CHANNEL ◄ ► 0x30/31 | Cubase-Profil Hb S. 57 (Logic: ohne Funktion) |
| FLIP (0x54) | FLIP 0x32 | Hb S. 47 |
| PAN (0x6E) | PAN 0x2A | Hb S. 45 |
| PLUGIN (0x50) | PLUG-IN 0x2B | Hb S. 45 |
| CHANNEL (0x51) | ? Vorschlag TRACK 0x28 | Entscheid 4 |
| SEND/PLUG-IN 1–8 (0x48–4F) | ? Vorschlag Global View 0x3E–45 | Entscheid 3 |
| FINE (0x6F) | **SHIFT 0x46** (Bitwig, Hb S. 128). Logic hätte ALT/CMD 0x49 | Hb, Mac 2.5 |
| Automation OFF/READ/WRITE/TRIM/LATCH/TOUCH (0x58–5D) | –/0x4A/0x4B/0x4C/0x4E/0x4D | Hb S. 44; OFF hat in MCU keine eigene Note [vermutet: READ/OFF teilen sich 0x4A] |
| Zoom-Kreuz ↑ ↓ ← → (0x7A/7E/7B/7D) | Cursor 0x60/61/62/63 | Hb S. 45 |
| Zoom-Mitte ◯ (0x7C) | ZOOM 0x64 | Hb S. 43 |
| dito im Transportmodus | ↑ PLAY 0x5E, ↓ STOP 0x5D, ← REW 0x5B, → FF 0x5C, ◯ REC 0x5F | Hb S. 43 |
| CHANNEL-Encoder drehen | CHANNEL ◄ ► 0x30/31 je Rastung; im NAV-Modus Jog CC 0x3C | Hb S. 42 |
| CHANNEL-Encoder drücken / halten | zurück zu CHANNEL / Transportmodus an-aus | Hb S. 16, 43; Option in Settings |
| NAV / NUDGE / FOCUS (0x73–75) | Encoder-Modus, lokal; NUDGE → NUDGE 0x55? | Entscheid 4 |
| NORM / REC / AUTO (0x70–72) | SEL-Modus des MCU-Layers | Entscheid 5 |
| Soft-Key-Bank V-POT, 1–5 (0x68–6D) | wählt die Soft-Key-Bank, lokal | Hb S. 47 |
| Soft-Keys 1–8 (0x18–1F) | je nach Bank, Tabelle 2.3; V-POT-Bank Taste 8 → NAME/VALUE 0x34 (Vorschlag) | Hb S. 47 f. |
| QUICK 1–3 (0x43–45) | Vorschlag: SCRUB 0x65 (= UAMC Console-Fenster), GLOBAL 0x33, frei | Entscheid 4 |
| Fussschalter 1/2 (0x00/01) | Vorschlag USER A/B 0x66/67 | Logic: Play/Record |
| LAYER 1–3 (0x40–42), 360 (0x46) | **bleiben Rea-Sixty** | Auftrag |

Die Beschriftung der Soft-Keys zeigt die MCU-Funktion, wie SSL es tut („labels in the LCD top
zone“, Hb S. 25).

### 4.3 MCU → UF8

| MCU | UF8 | Bemerkung |
|---|---|---|
| Pitchbend Kanal 1–8 | Motorfader | nicht bewegen, solange berührt; nach dem Loslassen auf den letzten Schattenwert |
| Pitchbend Kanal 9 (Master) | kein Fader | Entscheid 1 |
| Note 0x08–0F / 0x10–17 | SOLO-LED gelb / CUT-LED orange | 7F an, 01 blinkt (eigener Takt), 00 aus |
| Note 0x18–1F / 0x00–07 | SEL-LED weiss / rot | NORM zeigt Select, REC zeigt Rec-Arm (wie SSL) |
| Note 0x28–2D (Assign) | PAN-, PLUGIN-LED; SEND/INSTR/EQ/TRACK auf den Soft-Key-LEDs der V-POT-Bank | |
| Note 0x32 FLIP | FLIP-LED | |
| Note 0x3E–45 (Global View) | SEND/PLUG-IN-LEDs 1–8 | wenn Entscheid 3 = Global View |
| Note 0x4A–4F (Automation) | Automation-LEDs | |
| Note 0x5B–5F (Transport) | Zoom-Kreuz-LEDs im Transportmodus | |
| Note 0x46–49 (Modifier) | FINE-LED bei 0x49 | |
| SysEx `12` LCD | Puffer 2 × 56 Zeichen, Teil-Updates. Zeile 1 → Scribble oben, Zeile 2 → Scribble unten | je 7 Zeichen, Zelle hat 8 (`kUf8ScribbleChars`) |
| CC 0x30–37 (Ring) | V-Pot-Balken | Dot → unipolar (eine Linie, wie Pan `Protocol.h:334-337`), Boost/Cut → bipolar, Wrap → unipolar, Spread → bipolar [vermutet, am Gerät prüfen]. Position 0 = aus |
| Kanaldruck `D0 hv` (Meter) | VU-Meter, beide Spalten | Stufe 0..12 → dB-Tabelle → `dbToVuByte_`. **Abklingen macht das Gerät**, also wir (~300 ms pro Segment [W21]). E/F = Clip an/aus |
| `D1` (Master-Meter?) | – | [vermutet, W21]; nicht auf Strip 1 legen (vgl. W20) |
| CC 0x40–4B (Timecode/Assignment) | ignorieren | Die UF8 hat keine 7-Segment-Anzeige. Ausnahme: die Assignment-Zeichen 0x4A/4B in die CS-Typ-Zelle (Vorschlag) |
| SysEx `00` Device Query | antworten `01 <Seriennummer 7> <Challenge 4>` | billig, schadet nicht; UAMC erkennt „manche“ Geräte automatisch |
| SysEx `02` Host Reply | antworten `03 <Seriennummer>` | jede Antwort annehmen |
| SysEx `13` Version | antworten `14 "V1.00"` | |
| SysEx `0F`, `61`–`63` | offline / Fader runter / LEDs aus / Reset → Schatten leeren | |
| andere SysEx (`0A`–`0E`, `20`/`21`, `72` Farben) | ignorieren | Farben der X-Touch (`72`) evtl. später |

**Kopfzeile:** `F0 00 00 66 <id>` mit id 0x14 (MCU), 0x15 (XT), 0x10/0x11 (Logic Control).
Alle vier annehmen. Wenn wir selbst senden: 0x14.

**Farben:** MCU hat keine. Feste ruhige Farbe pro Strip oder eine Layer-Farbe aus den Settings.

---

## 5. Architektur (Vorschlag)

1. **`McuLayer.h`** (reine Logik, ctest `test_mcu_layer`, gleiche Form wie `MidiCcLayer.h`):
   - Kodieren und Dekodieren: Pitchbend ↔ Faderposition, V-Pot relativ, Ring-Byte.
   - SysEx-Zusammenbau über Paketgrenzen.
   - LCD-Puffer 2 × 56 mit Teil-Updates.
   - Tastentabelle UF8 ↔ MCU samt Soft-Key-Bänken.
   - Handshake-Antworten.
   - Meter-Abklingen, Blinktakt.
   - `Shadow`: der ganze MCU-Zustand. Fader, LEDs, LCD, Ringe, Meter, Assign.
2. **`McuPort.{h,cpp}`** (Core MIDI, eigene Klasse neben `MidiBridge`, die unverändert bleibt):
   - Source + Destination „Rea-Sixty MCU“.
   - Feste `kMIDIPropertyUniqueID`, gespeichert in ExtState. Apple rät dazu, sonst gilt der Port
     für Apps nach jedem Start als neu [W22].
   - Manufacturer und Model gesetzt.
   - Lebt von `initDevices_` bis zum Abbau, egal welcher Layer aktiv ist.
   - Windows: leere Stubs wie `MidiBridge`.
3. **Eingang (Core-MIDI-Thread):**
   - `readProc_` setzt SysEx zusammen und schiebt die Nachrichten in eine Queue.
   - Handshake-Antworten gehen sofort zurück. `MIDIReceived` ist threadsicher, und es ist keine
     REAPER-API im Spiel.
   - Sonst kein Zugriff auf UF8 oder REAPER.
4. **`mcuTick_` (Main-Thread, jeder Tick, auch ohne aktiven Layer):**
   - Leert die Queue in den `Shadow`.
   - Lässt Meter abklingen.
   - Erkennt Betreten und Verlassen des Layers (wie `ccTick_`).
5. **Ausgang (USB-Thread):**
   - Fader, Touch an, V-Pots und Tasten werden sofort als MCU-Bytes gesendet, wenn
     `g_mcuActiveLayer >= 0`.
   - Kein Umweg über die 30-Hz-Queue. Das Muster gibt es schon (`main.cpp:28600`).
6. **Maler `mcuPaintSurface_`** statt `pushZonesForVisibleSlots`:
   - Läuft über `uf8strips::paintStrip`. Dessen `View` hat noch kein Feld für die untere
     Scribble-Zeile.
   - `Uf8Strips.h` ist mit ORC geteilt. Das Feld muss dort additiv dazukommen (gesetzt, aber
     ungenutzt, bleibt alles wie heute).
7. **Merker `g_mcuActiveLayer`** in Bindings, wie `g_ccActiveLayer`:
   - Gefragt wird der **rohe** Layer, denn ein MCU-Layer übernimmt immer die ganze Fläche.
   - Nur **ein** MCU-Layer zur Zeit (ein Portpaar). Der zweite Eintrag im Dropdown ist gesperrt.
8. **JSON:**
   - `"kind": "mcu"` und `"mcu": { "port": "Rea-Sixty MCU", "transport_hold": true, … }` im
     Layer-Objekt, nur geschrieben, wenn gesetzt.
   - Version bleibt.
   - Einschränkung siehe 2.4: Ein altes Rea-Sixty liest den Layer als REAPER.
9. **Zwei UF8:** v1 lässt die rechte dunkel und verwirft ihre Eingabe (wie CC hinter der letzten
   Seite). Später ein Port „Rea-Sixty MCU XT“ (0x15) für sie.

---

## 6. Was gleich bleibt

- Hardware-Init, USB-Protokoll, `MidiBridge` und die „reaper_uf8“-Ports bleiben unverändert.
- REAPER-Layer und CC-Layer verhalten sich wie bisher. Die gemeinsamen Stellen (Abschnitt 3)
  bekommen nur eine Bedingung dazu.
- `Uf8Strips.h` / `Uf8Fader.h` (mit ORC geteilt) ändern sich nur additiv.

---

## 7. Entscheide (offen)

| # | Frage | Vorschlag |
|---|---|---|
| 1 | **Master-Fader:** UAMC legt Master/Monitor darauf, die UF8 hat keinen | **Entschieden 02.11.:** kein Sonderweg. Laut Nutzer lag Master/Monitor früher als normaler Fader in einer späteren Bank (MONITOR ist ein Kanal in UAMCs Liste). Werte auf Kanal 9 (`E8`) landen nur im Schatten |
| 2 | Welches SSL-Profil lag auf Layer 3? | **Geklärt: Bitwig MCU** (2.5). Vorlage wird Bitwig, die fehlenden Noten liefert der Mitschnitt |
| 3 | SEND/PLUG-IN 1–8 | Global View 0x3E–45 (UAMC nutzt Inputs/Aux/Outputs …). Alternative: frei |
| 4 | CHANNEL-Taste, QUICK 1–3, NAV/NUDGE/FOCUS, Fussschalter | wie in 4.2; v1 fest, später einstellbar wie beim CC-Layer |
| 5 | SEL-Modus NORM/REC auf dem MCU-Layer | eigener Zustand des MCU-Layers, damit REAPERs Modus unberührt bleibt |
| 6 | Meter in v1? | **Entschieden 02.11.: ja**, nach Fader/LCD/LEDs. Die Helfer passen (`dbToVuByte_`, TotalMix-Muster) |
| 7 | Portname | fest „Rea-Sixty MCU“, in Settings änderbar. **Nicht** „SSL V-MIDI Port 9“ nachbauen: Kollision, falls SSL360Core läuft, und es sähe aus wie SSLs Port |
| 8 | CHANNEL halten = Transportmodus | Option in Settings, Standard an (wie SSL) |
| 9 | Ohne REAPER (ORC hat die UF8) | v1: kein MCU. Später könnte ORC denselben Kern hosten (Apollo ohne REAPER) |
| 10 | Bis dahin | Der vorhandene CC-Layer kann UAMC im **MIDI-Mapping-Modus** über IAC schon heute bedienen, ohne Namen und Bänke. Als Rückfall behalten |

---

## 8. Zuerst messen

**A. Mitschnitt der alten Lösung (optional, seit 02.11.).** Die Tastenbelegung kommt aus den
Labels des Nutzers (2.3a), nicht aus SSLs Noten. Der Mitschnitt bestätigt nur noch. Was UAMC
sendet, zeigt auch die Sonde (B2) oder das Protokoll des Layers selbst beim ersten Test.
SSL 360° mit Layer 3 auf dem alten Profil,
UAMC im Mackie-Modus an Port 9, MIDI Monitor (Snoize):

1. *Sources*: „SSL V-MIDI Port 9 Source“ anhaken.
2. *Spy on output to destinations*: „SSL V-MIDI Port 9 Destination“ anhaken.
3. UAMC starten.
4. Fader 1, Touch, V-Pot 1 drehen und drücken, SEL/SOLO/CUT 1, BANK ◄ ►, PAN, PLUGIN, FLIP, jeden
   Soft-Key der V-POT-Bank, Automation, Zoom-Kreuz, CHANNEL halten + Kreuz.
5. In der Console Fader 1 und Mute klicken.
6. Speichern als `.mmon`.

Das klärt:

- Handshake ja/nein.
- Geräte-ID (0x14/0x10).
- Was UAMC an LCD, Ring, Meter, Master und Timecode sendet.
- Welche UF8-Taste unter dem SSL-Profil welche MCU-Note sendet.
- Ob SSL 360° Blinken selbst taktet.

Für den Mitschnitt muss REAPER zu sein, denn Rea-Sixty hält die UF8. SSL 360° muss laufen, sonst
gibt es Port 9 nicht (2.5). In UAMC muss Port 9 wieder im Mackie-Modus eingetragen werden.

**B. Ports ohne SSL 360°:** **gemessen [Mac]:** Ohne SSL 360° gibt es keinen V-MIDI-Port.

**B2. Sonde ohne SSL (Alternative zu A für die UAMC-Seite):** ein kleines eigenständiges
Programm auf dem Mac, keine Änderung an Rea-Sixty.

- Es legt „Rea-Sixty MCU Probe“ (Source + Destination) mit fester ID an.
- Es schreibt 60 s lang alles mit, was UAMC im Mackie-Modus sendet.
- Auf eine Device Query antwortet es wahlweise.

Das zeigt Handshake, Geräte-ID, LCD-, Ring-, Meter- und Master-Verkehr. Die Tastenbelegung von SSL
zeigt es nicht. Nebenwirkung: UAMC trägt neue virtuelle Ports selbst in seine Liste ein.

**C. Am Gerät, nach dem ersten Bau:**

- Ring-Modi auf dem V-Pot-Balken.
- Meter-Tabelle.
- Ob UAMC den Port nach einem REAPER-Neustart wiederfindet (Name vs. ID).

---

## 9. Bausteine und Reihenfolge

1. **A** Messungen (Abschnitt 8 A und B2, **beide**, Entscheid 02.11.). Werkzeug und Anleitung:
   `extension/tools/mcu_port_probe.cpp`, `docs/mcu-probe-runbook.md` (gebaut 02.11.).
2. **B** `McuLayer.h` + `test_mcu_layer` (CMake wie `test_midi_cc_layer`). **Gebaut 02.11.**,
   Test grün; unter Linux 43/45 Tests grün, die zwei übrigen bauen dort schon vorher nicht.
3. **C** `LayerKind::Mcu`, JSON, `g_mcuActiveLayer`, Settings-Zeile (Typ, Portname, Status
   „Port offen / letzte Nachricht vor X s“, Option CHANNEL halten).
4. **D** `McuPort`: Endpunkte, feste ID, Status, Handshake. Ab hier mit MIDI Monitor sichtbar.
5. **E** UF8 → MCU: Fader, Touch, V-Pots, Tasten. Prüfen mit MIDI Monitor.
6. **F** MCU → UF8: Schatten, Maler (LCD, LEDs, Motor, Ring), Betreten/Verlassen. Prüfen mit UAMC.
7. **G** Meter.
8. **H** Soft-Key-Bänke, Beschriftung, Transportmodus, Master-Fader (Entscheid 1).
9. **I** Handbuch „MCU layer (UF8)“, dieser Plan mit „gemessen / vermutet“.
10. **J** Pull Request an acklin83/Rea-Sixty vorbereiten, **erst nach OK**.

---

## Quellen

- [Hb] SSL UF8 User Guide Rev 11, `docs/docs/SSL UF8 User Guide_Rev11.pdf`, S. 6, 14–25, 38–49, 53–62,
  87–94, 114–122, 135, 163–169, 182–184.
- [W1] https://fonoflow.com/products/ua-midi-control
- [W2] https://fonoflow.com/download/ua-midi-control/UA%20Midi%20Control.html (Release Notes bis 5.13.3)
- [W3] https://www.reddit.com/r/UaMidiControl/comments/188qe5k/midi_mapping_gone_with_uf8/
- [W6] https://www.reddit.com/r/UaMidiControl/comments/1jgv74v/calibrating_streamdeck_knobs_for_console_faders/
- [W7] https://www.reddit.com/r/UaMidiControl/comments/pqppa3/ua_midi_control_5_beta_release/ und
  https://www.reddit.com/r/UaMidiControl/comments/1j6chpo/uf8_stream_deck_wua_midi_mackie_version/
- [W8] https://www.reddit.com/r/UaMidiControl/comments/uv86t9/iac_driver_bus_not_recognised_after_os_reinstall/
- [W9] https://www.reddit.com/r/UaMidiControl/comments/1qv8qzh/midi_controller_not_syncing_on_startup/
- [W10] https://www.youtube.com/watch?v=-rKfKjD1NgQ (MCU-Demo 2021)
- [W11] https://www.youtube.com/watch?v=Ezj-r833G6s (FaderPort im MCU-Modus)
- [W12] https://uadforum.com/community/index.php?threads/best-compact-controller-for-luna.56293/
- [W13] https://www.reddit.com/r/UaMidiControl/comments/180i4jk/colours_on_xtouch/
- [W14] https://forums.steinberg.net/t/midi-remote-controller-disappearing-on-launch/951248/40
- [W15] https://forums.steinberg.net/t/ssl-uc1-uf1-or-x-touch-one/872460?page=2
- [W16] https://forum.rme-audio.de/viewtopic.php?pid=198362
- [W17] https://formulae.brew.sh/api/cask/ssl-360.json
- [W18] https://support.solidstatelogic.com/hc/en-gb/articles/4410043451537-UF8-Compatibility
- [W19] https://uadforum.com/community/index.php?threads/ssl-uf8-studio-controller-looks-interesting-and-appears-to-have-mcu-support.52575/page-4
- [W20] https://www.reddit.com/r/UaMidiControl/comments/19bone2/
- [W21] Logic Control Manual, Anhang B/C: https://www.fullcompass.com/common/files/4705-LogicControlManual.pdf;
  gegengeprüft an Ardour `libs/surfaces/mackie` (https://github.com/Ardour/ardour/tree/master/libs/surfaces/mackie)
  und https://github.com/NicoG60/TouchMCU/blob/main/doc/mackie_control_protocol.md
- [W22] https://forum.juce.com/t/retaining-virtual-midi-ports-kmidipropertyuniqueid-to-improve-compatibility-with-studioone/24586

Reddit- und SSL-Support-Seiten wurden teils nur über Suchauszüge gelesen (direkter Abruf gesperrt).
