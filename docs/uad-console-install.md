# UAD-Console-Layer: Installieren, testen, zurück

Für den Branch `uad-console` im Fork `m3lotunes/Rea-Sixty`, macOS Apple Silicon. Stand 02.11.2026.

**Wichtig vorweg:**

- Das ist eine Testversion. Gegen eine virtuelle UF8 und eine virtuelle UAD Console ist sie getestet
  (alle Tests grün), an echter Hardware noch nicht.
- Solange **kein** Layer auf „UAD Console“ steht, verhält sich Rea-Sixty wie die offizielle
  Version.
- Die Erweiterung wird von GitHub gebaut (dieselbe Pipeline wie Franks Releases).

---

## 1. Bauen lassen (GitHub, einmalig pro Version)

1. https://github.com/m3lotunes/Rea-Sixty → Reiter **Actions**. Beim ersten Mal den grünen Knopf
   „I understand my workflows, go ahead and enable them“ drücken.
2. Links **Build** wählen, rechts **Run workflow**, als Branch `uad-console`, dann **Run workflow**.
3. Warten, bis der Lauf grün ist (etwa 15–25 Minuten). Unten auf der Seite des Laufs unter
   **Artifacts** auf **reaper_rea-sixty.dylib** klicken. Das lädt ein ZIP; im Finder doppelklicken.
   Darin liegt `reaper_rea-sixty.dylib`.

## 2. Installieren

1. **REAPER beenden.**
2. Sichern, was jetzt installiert ist:

   ```bash
   mkdir -p ~/ReaSixty-Backup
   ```

   ```bash
   cp -p ~/Library/Application\ Support/REAPER/UserPlugins/reaper_rea-sixty.dylib ~/ReaSixty-Backup/
   ```

   ```bash
   cp -p ~/Library/Application\ Support/REAPER/rea_sixty/bindings.json ~/ReaSixty-Backup/
   ```

3. Die neue Datei einsetzen (Pfad anpassen, falls das ZIP woanders entpackt wurde):

   ```bash
   cp ~/Downloads/reaper_rea-sixty.dylib ~/Library/Application\ Support/REAPER/UserPlugins/
   ```

   ```bash
   xattr -d com.apple.quarantine ~/Library/Application\ Support/REAPER/UserPlugins/reaper_rea-sixty.dylib
   ```

   (`xattr` meldet „No such xattr“, wenn es nichts zu entfernen gibt; das ist in Ordnung.)
4. Prüfen, dass libusb und hidapi daneben liegen. Die kommen von der ReaPack-Installation und
   bleiben:

   ```bash
   ls ~/Library/Application\ Support/REAPER/UserPlugins/ | grep -E "libusb|hidapi"
   ```

## 3. Einrichten und erster Test

1. UAD Console starten (das Fenster darf danach zu sein), Apollo angeschlossen.
2. REAPER starten → Rea-Sixty Settings → **Bindings → UF8** → mit dem LAYER-Knopf auf Layer 3
   gehen → **Layer type** = **UAD Console**. Darunter steht der Status, z. B. „Connection: UAD
   Console, Apollo Twin MkII, online, 14 channels“. Dein bisheriger MIDI-CC-Satz auf Layer 3
   bleibt gespeichert.
3. An der UF8 **LAYER 3** drücken. Erwartet:
   - Namen „UAD 1 … ADAT 6“ und die Pegel der Kanäle.
   - Die Fader stehen wie in der Console.
4. **Erster Schreibtest, vorsichtig:**
   1. Mit **BANK ►** auf Bank 2 wechseln.
   2. **Fader ADAT 8** (Strip 2) ein Stück bewegen. Der Fader in der Console muss folgen.
   3. Dann in der Console einen Fader bewegen: Der UF8-Fader muss mitfahren.
5. Danach der Reihe nach: SOLO/CUT, V-Pots (PAN), SEND + SEND/PLUG-IN 1–6, Soft-Key-Bank 1
   (ALT/DIM/MONO/MUTE), CHANNEL-Encoder (Monitor-Pegel, **leise anfangen**), INPUTS, 48V nur, wenn
   kein Bändchenmikrofon dranhängt.
6. **LAYER 1** drücken: REAPER wie gewohnt. **LAYER 3**: die Console sofort wieder da.

**Was mir hilft, wenn etwas nicht stimmt:** ein Foto der UF8, der Status-Text aus den Settings und
was du gedrückt hast.

## 4. Zurück zur offiziellen Version

1. **REAPER beenden.**
2. Sicherung zurückspielen:

   ```bash
   cp -p ~/ReaSixty-Backup/reaper_rea-sixty.dylib ~/Library/Application\ Support/REAPER/UserPlugins/
   ```

   ```bash
   cp -p ~/ReaSixty-Backup/bindings.json ~/Library/Application\ Support/REAPER/rea_sixty/
   ```

3. REAPER starten. Alternativ in ReaPack Rea-Sixty neu installieren.

**Hinweis:**

- ReaPack ersetzt diese Testversion beim nächsten Rea-Sixty-Update durch Franks offizielle.
- Die offizielle Version kennt „UAD Console“ nicht: Sie liest diesen Layer als REAPER-Layer und
  speichert ihn auch so. Dein CC-Satz und alle anderen Bindings bleiben erhalten.
