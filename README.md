# ESPHome external_component: Hilo/Aktiia Blutdruckmanschette

Diese `external_component` steuert eine Hilo (früher Aktiia) BLE-Blutdruckmanschette
von einem ESP32 (Zielboard: ODROID-GO, `board: odroid_esp32`, Framework `esp-idf`).
Protokoll, UUIDs und Timing sind 1:1 aus dem Referenz-Client
[antirez/bplog](https://github.com/antirez/bplog/blob/main/python/bplog.py) übernommen.

## Ordnerstruktur

```
my_components/hilo_cuff/
  __init__.py        # Basis-Komponente, verknüpft mit ble_client
  sensor.py           # Plattform: systolic, diastolic, map, heart_rate, battery_level
  text_sensor.py       # Plattform: measurement_status
  button.py             # Plattform: Start-Button (auch für Home Assistant nutzbar)
  hilo_cuff.h / .cpp     # C++-Implementierung (BLE GATT-Client, esp-idf)
hilo-cuff.yaml            # Gesamtkonfiguration: BLE, Messintervalle, ILI9341-Display
partitions.csv             # Eigene Partitionstabelle (App/OTA + Log-Partition "hilo_log")
secrets.yaml.example      # Vorlage für secrets.yaml
```

## Protokoll (aus bplog.py übernommen)

| Zweck | UUID |
|---|---|
| Service | `b1e71568-047b-47c4-88c9-0f90e397acf7` |
| Messwert-Notify | `a6b40002-003d-4e65-9208-08f4db958863` |
| Status | `a6b40003-003d-4e65-9208-08f4db958863` |
| Akku (Standard-BLE) | `0x180F` / `0x2A19` |

Notification-Payload (≥4 Byte): `[diastolic, systolic, map, heart_rate]`.
Statuswert `2` = erfolgreiche Messung, alles andere = Fehlercode.
Timeout entspricht `MEASUREMENT_TIMEOUT = 120s` im Original-Skript.

**Wichtig:** Laut der aktuellen Protokoll-Dokumentation im Original-Repo startet die
Manschette das Aufpumpen bereits **automatisch beim reinen BLE-Connect** (solange
ihr physischer Netzschalter an ist) - nicht erst durch einen Tastendruck an der
Manschette selbst. Die Komponente verbindet sich deshalb bewusst nur bei einem
expliziten `start_measurement()`-Aufruf (Taste/HA) und trennt danach wieder, statt
dauerhaft verbunden zu bleiben - sonst würde jeder automatische Reconnect
unbemerkt eine neue Messung auslösen.

**Bekannter, behobener Bug:** `esp_ble_gattc_register_for_notify()` allein reicht
nicht aus, damit die Manschette tatsächlich Notifications sendet - dafür muss
zusätzlich der Client Characteristic Configuration Descriptor (CCCD, `0x2902`)
der Messwert-Characteristic mit `0x0001` beschrieben werden. `bleak`s
`start_notify()` in `bplog.py` erledigt das intern automatisch, die Komponente
macht es jetzt explizit im `ESP_GATTC_REG_FOR_NOTIFY_EVT`-Handler.

## Setup

1. **MAC-Adresse der Manschette ermitteln** (das Original scannt nach dem
   Advertising-Namenspräfix `AKTIIA C`; ESPHomes `ble_client` benötigt dagegen eine
   feste MAC-Adresse). Möglichkeiten: `esp32_ble_tracker`-Debug-Log eines
   Minimal-Sketches, nRF Connect App, oder `bluetoothctl scan on` auf einem
   Linux-Rechner.
2. Die MAC-Adresse wird **nicht** über `secrets.yaml` gesetzt, sondern über die
   Substitution `mac_address` ganz oben in `hilo-cuff.yaml`:
   ```yaml
   substitutions:
     mac_address: "AA:BB:CC:DD:EE:FF"
   ```
   Das erlaubt z. B. mehrere Geräte über ESPHome-Packages/Dashboard-Substitutionen
   mit derselben Basiskonfiguration, aber unterschiedlicher MAC zu betreiben.
3. `secrets.yaml.example` nach `secrets.yaml` kopieren und ausfüllen
   (WLAN, API-Key, OTA-Passwort).
4. Kompilieren/Flashen:
   ```
   esphome run hilo-cuff.yaml
   ```

## Auslösen der Messung

- **Physisch:** START-Taste des ODROID-GO an GPIO39 (`binary_sensor` ruft
  `button.press: start_measurement_button` auf).
- **Home Assistant:** Der Button `Blutdruckmessung starten` erscheint automatisch
  als Entity (z. B. `button.hilo_cuff_monitor_blutdruckmessung_starten`) und kann
  dort direkt gedrückt oder per Automatisierung aufgerufen werden.
- **Automatisch, Tag/Nacht-gesteuert:** Über die Number-Entities
  `Messintervall Tag`, `Messintervall Nacht`, `Tagbeginn (Stunde)` und
  `Tagende (Stunde)` sowie den Schalter `Automatische Messungen` lässt sich
  ein periodischer Messrhythmus konfigurieren - z. B. alle 60 min tagsüber,
  nachts deaktiviert (`0` = aus). Ein `interval:`-Block prüft minütlich über
  `time:` (Zeitquelle: Home Assistant), ob laut aktueller Uhrzeit und konfiguriertem Intervall eine
  Messung fällig ist, und löst sie über denselben Button aus wie GPIO39/HA.
  Alle vier Werte sind auch aus Home Assistant heraus änderbar.

## Display (ILI9341 über `mipi_spi`)

Die ODROID-GO-typische SPI-Verkabelung (`CLK=18, MOSI=23, MISO=19, CS=5, DC=21,
RESET=4, Backlight=14`) ist bereits eingetragen (`rotation: 270`,
`update_interval: 15s`). Angezeigt werden:

- Aktuelles Datum + Uhrzeit (Zeitquelle: Home Assistant über `time:
  platform: homeassistant` - erfordert eine aktive `api:`-Verbindung zu
  Home Assistant, sonst bleibt die Zeile leer, bis `now().is_valid()` wird)
- Systolisch/Diastolisch groß und **farbcodiert nach WHO/ISH-Blutdruckkategorien**
  (Optimal/Normal = grün, Hoch-normal = gelb, Hypertonie Grad 1 = orange,
  Grad 2 = rot, Grad 3 = dunkelrot, niedriger Blutdruck = blau)
- Kategorie-Bezeichnung als Text
- Herzfrequenz, Akkustand der Manschette, aktueller Messstatus
- konfiguriertes Tag-/Nacht-Messintervall

Die Grenzwerte liegen direkt in der Display-Lambda in `hilo-cuff.yaml` und lassen
sich dort bei Bedarf anpassen.

### Hintergrundbeleuchtung: SELECT-Taste + automatisches Timeout

- **SELECT-Taste (GPIO27)** schaltet die Hintergrundbeleuchtung manuell um
  (`light.toggle`). Anders als GPIO39 (START) unterstützt GPIO27 interne
  Pull-Widerstände, daher `pullup: true` dort bewusst gesetzt.
- **Number-Entity `Display Timeout`** (Minuten, Standard 5, auch aus Home
  Assistant änderbar): Ein `interval:`-Block prüft alle 5 s, ob seit dem
  letzten Tastendruck (START oder SELECT) länger als der eingestellte
  Timeout vergangen ist, und schaltet die Beleuchtung dann automatisch aus -
  auch während einer laufenden Automatik-Messung. `0` deaktiviert die
  automatische Abschaltung.
- Die Beleuchtung wird **nicht** automatisch wieder eingeschaltet - das
  passiert nur über SELECT oder einen Druck auf START.

## CSV-Logging + Download über den Webserver

Jede abgeschlossene Messung (erfolgreich oder mit Fehlercode) wird als Zeile an
`/logfs/hilo_log.csv` angehängt (Spalten: `timestamp, systolic_mmHg,
diastolic_mmHg, map_mmHg, heart_rate_bpm, cuff_battery_pct, status`). Die
Datei lässt sich unter `http://<gerätename>/hilo_log.csv` direkt über den in
`hilo-cuff.yaml` aktivierten ESPHome-Webserver herunterladen.

**Speicherort:** eine eigene FAT/Wear-Leveling-Partition im **internen
Flash** des ESP32 (Partitionslabel `hilo_log`, definiert in
`partitions.csv`, gemountet unter `/logfs`) - bewusst **keine SD-Karte**:
Die TF-Karte des ODROID-GO hängt am selben physischen SPI-Bus wie das
Display, was in einer früheren Version dieses Projekts zu einem
Laufzeit-Absturz führte (`spi_bus_initialize` schlug mit
`ESP_ERR_NOT_FOUND` fehl, da beide Komponenten unabhängig voneinander den
Bus/DMA-Kanal beanspruchten). Internes Flash-Logging umgeht dieses Problem
vollständig, da kein zusätzlicher SPI-Teilnehmer hinzukommt.

Konfigurierbar über die `hilo_cuff:`-Sektion:
- `csv_logging: true|false` - Logging komplett an/aus
- `csv_log_path` - Pfad der CSV-Datei (Standard `/logfs/hilo_log.csv`)
- `csv_download_url` - Pfad des Download-Endpunkts (Standard `/hilo_log.csv`)
- `web_server_base_id` / `time_id` - Verknüpfung mit `web_server_base:` bzw.
  dem Home-Assistant-Zeitgeber für Zeitstempel in der CSV

Notwendige Voraussetzungen in `hilo-cuff.yaml`, bereits eingetragen:
- `esp32.partitions: partitions.csv` - eigene Partitionstabelle mit
  zusätzlicher `hilo_log`-Partition (Typ `data`, Subtyp `fat`, 256 KB)
- `esp32.framework.advanced.include_builtin_idf_components: ["fatfs",
  "wear_levelling"]` - diese Standard-ESP-IDF-Komponenten bindet ESPHome
  sonst nicht automatisch mit ein

**Wichtige, nicht vollständig verifizierte Annahme (bitte beim ersten
Testlauf prüfen):** `partitions.csv` geht von mindestens 4 MB Flash aus
(Standard-Layout: NVS + OTA-Daten + zwei App-Partitionen à 1,75 MB +
256 KB Log-Partition ≈ 3,8 MB von 4 MB). Falls der Build mit
`"partitions do not fit in flash size"` fehlschlägt: entweder die Größe der
`hilo_log`-Partition in `partitions.csv` reduzieren, oder falls das Board
tatsächlich mehr Flash hat, die App-Partitionsgrößen entsprechend anpassen.

Der Download-Endpunkt ist als eigene `AsyncWebHandler`-Subklasse
(`HiloLogDownloadHandler`) implementiert und per `server->addHandler(...)`
registriert - der ESP-IDF-Webserver-Shim von ESPHome kennt kein
`AsyncWebServer::on(url, method, lambda)` wie ESPAsyncWebServer, sondern
nur `addHandler()` mit einer Handler-Klasse (`canHandle()` /
`handleRequest()`). Die restliche Response-API (`beginResponse`,
`addHeader`, `request->send(...)`) kann sich zwischen ESPHome-Versionen
dennoch leicht unterscheiden - bei Compile-Fehlern im Download-Handler
gegen die tatsächlich installierte 2026.7-Version
(`esphome/components/web_server_idf/web_server_idf.h`) prüfen.

**Setup-Reihenfolge/Absturz beim Boot behoben:** `HiloCuffComponent::setup()`
läuft mit Priorität `AFTER_BLUETOOTH` typischerweise VOR dem `setup()` von
`web_server_base`, in dem der eigentliche `AsyncWebServer` erst angelegt
wird. Ein direkter `get_server()`-Aufruf in `setup()` konnte deshalb einen
noch nicht einsatzbereiten Server liefern und beim anschließenden
`addHandler(...)` abstürzen. Die Registrierung erfolgt jetzt über
`register_download_handler_()` mit Retry (bis zu 20-mal im 500-ms-Abstand),
statt sich auf eine exakte Priority-Reihenfolge zwischen den Komponenten zu
verlassen.

## Akustisches Feedback

Über den eingebauten Lautsprecher des ODROID-GO (per `output: platform: ledc`
+ `rtttl:`-Komponente) gibt es drei kurze Signaltöne:

- **GPIO25 = `SPEAKER_ENABLE`**, nicht der Audio-Ausgang selbst - muss auf
  HIGH stehen, damit der Verstärker überhaupt ein Signal durchlässt. Als
  `switch: platform: output` (Entity "Lautsprecher aktiv") umgesetzt, die
  standardmäßig AUS ist (`restore_mode: ALWAYS_OFF`) und nur unmittelbar vor
  jeder Wiedergabe eingeschaltet wird - über `rtttl.on_finished_playback` schaltet
  sie sich automatisch wieder aus, sobald die jeweilige Melodie fertig ist.
  So bleibt der Verstärker nicht dauerhaft aktiv.
- **GPIO26** trägt das eigentliche PWM-Audiosignal (zweiter ESP32-DAC-Pin
  neben GPIO25/DAC1, das hier als Enable-Leitung belegt ist), begrenzt auf
  `max_power: 50%` (Lautstärke).

Signaltöne:
- **Bereit nach Neustart:** `esphome.on_boot` spielt einen aufsteigenden
  Dreiklang, sobald der Boot-Vorgang abgeschlossen ist.
- **Messung gestartet:** ein kurzer Piepton, ausgelöst über `on_press` am
  `start_measurement_button` - greift damit einheitlich für GPIO39,
  Home Assistant und den Tag/Nacht-Automatik-Intervall, da alle drei
  denselben Button auslösen.
- **Messung fertig:** ein zweitöniger Piep, ausgelöst per `on_value` am
  Messstatus-Text-Sensor, sobald dieser einen Endzustand erreicht
  (`ok`, `timeout` oder ein `error`-Präfix) - nicht bei Zwischenzuständen
  wie `connecting`/`waiting`.

Die RTTTL-Melodiestrings (`"start:d=8,o=6,b=160:c6"` usw.) lassen sich direkt
in `hilo-cuff.yaml` anpassen. Falls weiterhin kein Ton zu hören ist, bitte
GPIO25/26 gegen das tatsächliche ODROID-GO-Schaltbild verifizieren - die
Zuordnung "GPIO26 = Audiosignal" ist meine plausibelste Ableitung aus der
bestätigten Enable-Leitung auf GPIO25, aber nicht durch eine offizielle
Pinbelegungs-Quelle gegengeprüft.

## HA-Button "CSV-Log herunterladen"

**Technische Einschränkung:** Ein in Home Assistant gedrückter Button löst
eine Aktion auf dem ESP32 aus - er kann aber grundsätzlich **keinen
Browser-Download** auf dem Handy/PC der Person auslösen, die den Button
gedrückt hat. Das ist eine reine Server→Client-Grenze (der ESP32 kennt den
Browser/das Gerät der Person gar nicht) und lässt sich nicht durch
ESPHome-Tricks umgehen.

Der ergänzte Button `CSV-Log herunterladen` protokolliert beim Drücken die
vollständige Download-URL (`http://<IP>${csv_download_url}`) ins ESPHome-Log.
Für einen echten "antippen → Download öffnet sich"-Ablauf in Home Assistant
empfiehlt sich eine kleine HA-seitige Automation (nicht Teil dieser
ESPHome-YAML), die auf das `press`-Event dieser Button-Entity reagiert und
z. B. eine Mobile-App-Benachrichtigung mit einer Tap-Aktion (`url`) auf die
Download-URL verschickt, oder ein Dashboard-Card-Link mit `tap_action: url`
direkt auf `http://<geraet>${csv_download_url}` anlegt.

## BLE-Pairing/Verschlüsselung (Fix für Status 5 "Insufficient Authentication")

Die Manschette lehnt den CCCD-Write für Notifications ohne verschlüsselten
Link mit GATT-Status `5` (`ESP_GATT_INSUF_AUTHENTICATION`) ab. Die Komponente
richtet daher beim Start global BLE-Security-Parameter ein (Just-Works-Pairing,
keine PIN nötig, Secure-Connections + Bonding) und stößt direkt nach dem
Connect aktiv `esp_ble_set_encryption()` an. Der CCCD-Write wird bei
Fehlschlag automatisch bis zu 5-mal im 400-ms-Abstand wiederholt, während die
Verschlüsselung im Hintergrund abgeschlossen wird.

Falls beim ersten Pairing-Versuch dennoch Probleme auftreten:
- Manschette ggf. vorher aus etwaigen alten Bluetooth-Kopplungen an Handy/PC
  entfernen, damit sie mit dem ESP32 neu und sauber bondet.
- Bonding-Informationen liegen im NVS-Flash des ESP32; nach einem
  Werksreset/Neuflash kann ein erneutes Pairing beim ersten Connect etwas
  länger dauern.

**Technischer Hinweis:** GAP-Sicherheitsereignisse (Pairing-Anfrage,
Auth-Ergebnis) werden über die von `BLEClientNode` geerbte virtuelle Methode
`gap_event_handler()` behandelt (analog zu `gattc_event_handler()`) - ein
eigenes, globales `esp_ble_gap_register_callback()` ist dafür nicht nötig
und würde mit der Basisklasse kollidieren (`... cannot be declared`, wenn
`gap_event_handler` versehentlich als `static` deklariert wird, wie in einer
früheren Version dieser Komponente).

## Doppelter Start während des Verbindungsaufbaus

`start_measurement()` prüfte ursprünglich nur, ob der Client `ESTABLISHED`
war, und rief bei jedem anderen Zustand erneut `connect()` auf. Wurde der
Start-Button (GPIO39/HA) ein zweites Mal gedrückt, während bereits eine
Verbindung aufgebaut wurde, kollidierte das mit dem laufenden Verbindungsversuch:
Die erste Verbindung wurde mit `reason 0x100` (`ESP_GATT_CONN_CONN_CANCEL`)
abgebrochen, ein verspätetes `ESP_GATTC_OPEN_EVT` traf danach auf einen
Client im falschen Zustand (`status=133`), und die Messung kam nie zustande.

`start_measurement()` prüft jetzt den tatsächlichen `ClientState`: Läuft
bereits ein Verbindungsauf- oder -abbau (`CONNECTING`/`DISCONNECTING`), wird
der erneute Aufruf ignoriert statt einen zweiten `connect()` anzustoßen.
Zusätzlich wird der Messstatus während des Verbindungsaufbaus auf
`connecting` gesetzt (sichtbar auf dem Display und in Home Assistant), damit
ein zweiter Tastendruck aus Ungeduld gar nicht erst nötig erscheint.

## Hinweise / Einschränkungen

- Das Protokoll basiert auf reverse-engineerten UUIDs aus einer dekompilierten APK
  (siehe Kommentare in `bplog.py`) - keine offizielle/dokumentierte Schnittstelle
  von Hilo/Aktiia. Änderungen an Firmware oder App können die UUIDs/das Verhalten
  brechen.
- Die exakten Parameter der `mipi_spi`-Plattform (z. B. weitere Pflichtfelder wie
  Auflösung/`dimensions`) können sich zwischen ESPHome-Versionen unterscheiden -
  gegen die tatsächlich installierte Version 2026.7 prüfen, falls der Build
  Parameterfehler meldet.
- Ebenso sollten die internen `ble_client`-API-Aufrufe (`get_characteristic`,
  `BLECharacteristic::handle` usw.) gegen 2026.7 verifiziert werden, falls der
  C++-Build fehlschlägt.
- Diese Integration ersetzt keine medizinische Beratung; die Manschette bleibt ein
  Consumer-Gerät, keine zertifizierte Medizinprodukt-Anzeige über Home Assistant.
  Die WHO/ISH-Farbcodierung dient nur zur groben Einordnung, nicht als Diagnose.
