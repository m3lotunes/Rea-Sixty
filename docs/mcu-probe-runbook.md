# MCU-Layer: Messungen am Mac (Runbook)

Gehört zu `docs/mcu-layer-plan.md`, Abschnitt 8. Zwei Messungen, beide ohne Änderung an Rea-Sixty.

- **B2:** Was schickt UA Midi Control (UAMC) einer Mackie-Fläche? Gemessen mit der Sonde
  `mcu_port_probe`, ohne SSL 360°.
- **A:** Was haben SSL 360° und UAMC über „SSL V-MIDI Port 9“ gesprochen? Mitschnitt mit MIDI Monitor
  (Snoize).

Die Ergebnisse kommen nach `~/ReaSixty-MCU/` auf dem Mac. Von dort lese ich sie.

---

## B2: Sonde `mcu_port_probe`

**Was sie tut:**

- Sie legt die virtuellen Ports **„Rea-Sixty MCU Probe“** (Source + Destination) mit fester ID an.
- Sie schreibt jede Nachricht von UAMC entschlüsselt mit, über denselben Parser wie der MCU-Layer
  (`extension/src/McuLayer.h`).
- **Sie sendet von sich aus nichts.** Kein Fader, keine Taste, kein Pegel in der Console ändert sich.
- Mit `--answer` beantwortet sie den MCU-Handshake, so wie es der Layer später tun wird.

**Bauen** (einmal, aus dem Repo-Ordner, Branch `mcu-layer`):

```bash
clang++ -std=c++20 -O1 -Iextension/src extension/tools/mcu_port_probe.cpp -framework CoreMIDI -framework CoreFoundation -o ~/ReaSixty-MCU/mcu_port_probe
```

**Ablauf:**

1. REAPER darf laufen, Rea-Sixty stört nicht. SSL 360° bleibt aus.
2. Lauf 1, **ohne** Handshake-Antwort (120 s):

   ```bash
   cd ~/ReaSixty-MCU && ./mcu_port_probe --log probe-1-silent.log
   ```

3. In UAMC → Settings → Midi die neuen Ports wählen: Eingang und Ausgang „Rea-Sixty MCU Probe“,
   **Mode = Mackie**.
4. Diese Liste der Reihe nach abarbeiten, mit je zwei Sekunden Pause:
   1. UAMC beenden und neu starten. Das zeigt, was beim Verbinden kommt.
   2. In der Console Fader von Kanal 1 bewegen, dann den MONITOR-Fader.
   3. Mute, Solo und Select an Kanal 1.
   4. Pan an Kanal 1 drehen.
   5. Einen Kanal umbenennen, falls das geht.
   6. In UAMC zwischen den Bänken blättern, falls das dort geht.
   7. Etwas abspielen, damit Pegel anliegen (Meter).
5. Warten, bis die Sonde nach 120 s ihre Zusammenfassung ausgibt, oder vorher Ctrl+C drücken.
6. Lauf 2, **mit** Handshake-Antwort, dieselbe Liste:

   ```bash
   ./mcu_port_probe --answer --log probe-2-answer.log
   ```

7. Danach in UAMC den Probe-Port wieder entfernen.

**Was ich daraus lese:**

- Fragt UAMC nach dem Gerät (`DEVICE QUERY`), und verhält es sich ohne Antwort anders?
- Welche Geräte-ID steht in den SysEx (0x14 / 0x10)?
- Was steht in LCD-Zeile 1 und 2?
- Kommen Ringe (welcher Modus), Meter (`D0`, `D1` für Master?), Timecode oder Assignment?
- Welche LEDs setzt UAMC, und blinkt etwas?
- Kommen Werte für den Master-Fader (`FADER MASTER`)?

---

## A: Mitschnitt der alten Lösung (SSL 360° + UAMC an Port 9)

Zeigt zusätzlich, welche MCU-Note SSLs Bitwig-Profil für jede UF8-Taste gesendet hat. Offen ist vor
allem, welche Taste Note 70 schickte, auf die das Alt-Monitor-Mapping hört.

1. **REAPER beenden**, denn Rea-Sixty hält die UF8.
2. SSL 360° starten. Warten, bis die UF8 verbunden ist. Layer 3 steht noch auf DAW 3 (Bitwig, Port 9).
3. In UAMC → Settings → Midi „SSL V-MIDI Port 9 Source/Destination“ wieder eintragen,
   **Mode = Mackie**.
4. MIDI Monitor starten:
   - Bei *Sources* „SSL V-MIDI Port 9 Source“ anhaken (das, was die UF8 sendet).
   - Bei *Spy on output to destinations* „SSL V-MIDI Port 9 Destination“ anhaken (das, was UAMC
     sendet). Fehlt der Punkt, fragt MIDI Monitor beim ersten Mal nach der Installation seines
     Spy-Treibers.
5. UAMC neu starten (Verbindungsaufbau mitschneiden).
6. Auf **Layer 3** der UF8 nacheinander, mit Pausen:
   1. Fader 1 anfassen und bewegen.
   2. V-Pot 1 drehen und drücken.
   3. SOLO, CUT, SEL 1.
   4. NORM/REC umschalten und SEL 1 drücken.
   5. BANK ◄ ►, PAGE ◄ ►, CHANNEL-Encoder drehen, drücken und halten.
   6. Alle Tasten der V-POT-Bank.
   7. Bank 1 Taste 1 „ALT“.
   8. FINE/SHIFT.
   9. QUICK 1–3, SEND/PLUG-IN 1–8, FLIP.
   10. Automation-Tasten.
   11. Zoom-Kreuz normal und im Transportmodus.
   12. Fussschalter, falls angeschlossen.
7. In der Console Fader 1, Mute und Solo klicken.
8. In MIDI Monitor *File → Save* als `~/ReaSixty-MCU/ssl-port9.mmon`. Zusätzlich die Liste markieren
   und als Text kopieren (`ssl-port9.txt`).
9. SSL 360° beenden, REAPER wieder starten.

**Hinweis:** Danach steht der Port-9-Eintrag in UAMC, ohne dass es den Port gibt. Das stört nicht.
