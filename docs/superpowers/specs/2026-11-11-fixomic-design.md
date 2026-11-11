# fixomic — Design

Data: 2026-11-11
Stato: bozza da approvare

## Intento

fixomic è una piccola app per macOS che si mette tra il microfono fisico e le app che lo usano (Discord, Zoom, Meet…). Prende il microfono, rimuove l'eco di ciò che il Mac sta riproducendo, lo fa passare in una catena ordinata di plugin VST3/AU scelti dall'utente, e pubblica il risultato come un microfono virtuale chiamato **"fixomic Mic"**, che le app selezionano come un microfono qualsiasi.

### Perché

L'autore usa cuffie HyperX collegate al jack combo del Mac. Su Discord gli altri sentono i video di YouTube/Instagram che lui guarda, anche a volume basso. Con cuffie chiuse la causa più probabile non è acustica ma elettrica: sul jack combo TRRS cuffia e microfono condividono la massa e il segnale d'uscita "sporca" il microfono (diafonia). Finora la soluzione è stata aprire Reaper, mettere un gate e Nectar (iZotope) e instradare l'uscita con BlackHole verso Discord: funziona ma è pesante e scomodo.

Il gate da solo non basta: taglia il rientro solo quando l'utente tace; quando parla, il gate si apre e il video passa insieme alla voce. Serve una vera **cancellazione dell'eco (AEC)** che usi come riferimento l'audio che il Mac sta riproducendo.

### Successo

- Su Discord, con un video YouTube in cuffia a volume normale, l'utente parla e gli altri sentono solo la sua voce, trattata dalla sua catena (gate, Nectar…), senza il video in sottofondo, né mentre parla né mentre tace.
- Nessun ritardo percepibile in chiamata.
- Gira per ore (sessioni di gioco/chiamate lunghe), sopravvive a sleep/wake e a cuffie scollegate e ricollegate, senza click né buchi.
- Si configura una volta; poi parte da sola all'accesso e "c'è e basta".
- Pubblicabile su GitHub per altri utenti Mac.

## Obiettivi (v1)

1. **App nella barra dei menu**, leggera, senza finestra principale e senza icona nel Dock.
2. **Selezione del microfono** in ingresso (dispositivo e canale) e parametri audio (sample rate, dimensione buffer).
3. **Cancellazione dell'eco** come primo stadio, attivabile/disattivabile, con un indicatore di quanta eco sta togliendo.
4. **Catena di plugin**: aggiungere, rimuovere, riordinare, bypassare singolarmente plugin **AU e VST3**; aprire la finestra di editing di ogni plugin come in un DAW. Deve funzionare con iZotope Nectar e con plugin gate comuni.
5. **Microfono virtuale "fixomic Mic"** fornito da un driver HAL incluso nel progetto (nessuna dipendenza da BlackHole installato).
6. **Persistenza**: catena e stato completo di ogni plugin salvati e ripristinati all'avvio; avvio automatico all'accesso (opzionale, attivabile dall'utente).
7. **Latenza bassissima e visibile**: l'interfaccia mostra la latenza totale aggiunta (buffer + AEC + latenza dichiarata dai plugin).
8. **Bypass globale**: un interruttore che manda il microfono "crudo" su fixomic Mic, per confronto rapido o emergenza.
9. **Elaborazione solo su richiesta** (deciso con l'autore): finché nessuna app usa fixomic Mic, fixomic non apre il microfono, non crea il tap dell'audio di sistema e non elabora nulla, quindi nessun pallino arancione del microfono e nessun consumo. Il driver avvisa l'app quando un client inizia o smette di leggere fixomic Mic (a partire dal suo StartIO/StopIO); l'app accende microfono, tap e AEC il più velocemente possibile, con i plugin **già caricati e inizializzati in memoria**, e li spegne quando l'ultimo client si ferma (con un breve ritardo, per non riavviare tutto quando un'app chiude e riapre il microfono di seguito). Durante l'accensione fixomic Mic dà silenzio, mai rumore. Obiettivo: audio pronto entro poche centinaia di millisecondi.

## Non-obiettivi (v1)

- Windows (previsto in futuro: vedi Vincoli → Portabilità).
- Cancellazione dell'eco ottimizzata per **altoparlanti e microfono del Mac**: la combinazione integrata funziona già bene (deciso con l'autore). Il bersaglio sono **cuffie e headset**, dove l'audio in uscita rientra nel microfono per diafonia elettrica (jack combo) o a distanza ravvicinata. Regolazione e test dell'AEC si concentrano su questo caso; con gli altoparlanti l'AEC può restare quella predefinita di AEC3, senza garanzie.
- Più preset/catene commutabili (in v1 c'è una sola catena; i preset arrivano dopo).
- Registrazione, timeline, tracce, mixer, MIDI, instrument plugin.
- Hosting dei plugin in un processo separato (sandbox anti-crash).
- Elaborazione dell'audio di sistema in uscita (fixomic tocca solo il microfono).
- Firma e notarizzazione Apple (l'autore si iscriverà al programma Apple Developer più avanti; il codice non deve cambiare per questo, solo il processo di rilascio).
- App Store.

## Anti-obiettivi (cose che lo renderebbero un fallimento anche se "funziona")

- Latenza percepibile o variabile, click, crepitii, drop-out, deriva che peggiora dopo ore.
- Diventare un DAW: complessità d'interfaccia alla Reaper.
- Richiedere all'utente BlackHole, Loopback, Reaper o configurazioni manuali di routing.
- Alterare ciò che l'utente ascolta (il tap dell'audio di sistema non deve mai silenziare o modificare l'uscita).
- Estensioni del kernel.
- Rovinare la voce quando l'utente parla sopra l'audio (double-talk): meglio un po' di eco residua che una voce mangiata.

## Vincoli

- **Piattaforma**: macOS, Apple Silicon e Intel (Universal 2). Minimo macOS 14.4 (*mia scelta*: i Core Audio process tap esistono da 14.2, ma diversi progetti riportano problemi prima di 14.4). La macchina dell'autore è su macOS 27, Xcode 27.
- **Linguaggio**: C++ (deciso con l'autore). Motivo: JUCE per l'hosting VST3/AU e per l'interfaccia, AEC3 è C++, il driver HAL è C/C++. Niente Swift o Rust sul percorso audio. Piccole parti Objective-C++ dove servono API Apple (process tap, SMAppService, permessi).
- **Framework**: JUCE (versione 8 corrente) — host plugin, gestione dispositivi, UI, icona nella barra dei menu.
- **Motore AEC**: WebRTC AEC3 (BSD-3), tramite la libreria autonoma `webrtc-audio-processing` (freedesktop) o un'estrazione solo-AEC3. A 48 kHz.
- **Driver virtuale**: AudioServerPlugIn (driver HAL in user-space, caricato da `coreaudiod`), derivato da BlackHole (GPL-3.0) con attribuzione.
- **Riferimento per l'AEC**: Core Audio process tap globale (`CATapDescription`, non muto, privato), che esclude il processo di fixomic. Include tutto ciò che suona sul Mac, quindi anche le voci degli altri su Discord (anch'esse rientrano nel microfono e vanno tolte).
- **Licenze**: JUCE open source è AGPL-3.0; BlackHole è GPL-3.0. *Mia scelta*: app sotto AGPL-3.0-or-later, cartella del driver sotto GPL-3.0 (derivata da BlackHole), con NOTICE che cita BlackHole, JUCE, WebRTC e VST3 SDK.
- **Portabilità futura su Windows**: tutto ciò che è specifico di macOS (driver virtuale, tap dell'audio di sistema, avvio all'accesso, installazione driver) sta dietro interfacce piccole e separate, così che host, catena, AEC e UI possano essere riusati.
- **Distribuzione v1**: release su GitHub, non firmate. Al primo avvio l'app offre "Installa driver" e chiede la password di amministratore per copiarlo in `/Library/Audio/Plug-Ins/HAL` e riavviare `coreaudiod`; offre anche la disinstallazione. README con le istruzioni per aprire un'app non firmata.
- **Identificatore bundle**: `it.manolozocco.fixomic` (*mia scelta*, in linea con le altre app dell'autore).

## Percorso del segnale (deciso)

```
Microfono fisico (es. HyperX su jack)
        │
        ▼
[1] Cancellazione eco AEC3  ◀── riferimento: process tap globale (tutto ciò che suona sul Mac)
        │
        ▼
[2] Catena plugin utente (ordinata, es. Gate → Nectar)
        │
        ▼
[3] "fixomic Mic" (driver HAL) ──▶ Discord / Zoom / qualsiasi app
```

L'AEC **deve** stare prima della catena: gate, compressori e saturazioni rendono il segnale non lineare e la cancellazione non funzionerebbe più.

## Latenza (budget)

A 48 kHz, stime:

| Punto | Latenza |
|---|---|
| Buffer dispositivo (64–128 campioni) | 1,3–2,7 ms |
| AEC3 (lavora a blocchi fissi di 10 ms) | ~10 ms |
| Passaggio nel driver virtuale | ~1–3 ms |
| Plugin | quella che dichiarano (0 per molti; Nectar dipende dai moduli) |

Obiettivi: **≤ 16 ms con AEC attiva** e **≤ 5 ms con AEC spenta**, esclusa la latenza dichiarata dai plugin. Per confronto Discord aggiunge da solo ~20 ms di codifica più la rete.

## Interfaccia (lasciata al builder, con indicazioni)

L'autore si fida del gusto del builder; il design va fatto usando le skill di design/UX disponibili (es. `impeccable`). Indicazioni:

- Clic sull'icona nella barra dei menu → pannello compatto con, in ordine di importanza:
  - stato generale (attivo / bypass) e interruttore di bypass globale;
  - indicatori di livello ingresso e uscita;
  - interruttore AEC con indicatore dell'eco rimossa;
  - la catena: lista ordinabile con trascinamento, bypass per plugin, clic per aprire l'editor, rimozione, "+ Aggiungi plugin" (lista dei plugin installati, con ricerca);
  - latenza totale;
  - accesso alle impostazioni (dispositivo d'ingresso, sample rate, buffer, avvio all'accesso, driver installato/da installare).
- Gli editor dei plugin si aprono in finestre separate.
- Primo avvio guidato: permesso microfono → permesso "registrazione audio di sistema" (per l'AEC) → installazione driver → "ora scegli *fixomic Mic* in Discord".
- Icona della barra dei menu che riflette lo stato (attivo, bypass, errore/dispositivo mancante).

## Robustezza (requisiti)

- **Thread audio real-time safe**: nessuna allocazione, lock o I/O sul thread audio; modifiche alla catena (aggiunta, rimozione, riordino) applicate senza bloccare l'audio.
- **Deriva dei clock**: microfono, uscita di sistema (riferimento AEC) e fixomic Mic possono avere clock diversi (es. microfono USB e altoparlanti interni). Serve compensazione (es. ricampionamento adattivo) così che dopo ore non si accumulino ritardi, click o errori dell'AEC.
- **Microfono scomparso** (*mia scelta*): se il microfono scelto viene scollegato, fixomic Mic dà silenzio e l'icona segnala il problema finché lo stesso microfono non torna; fixomic non passa mai da sola a un altro microfono (per esempio quello del Mac), per non mandare in chiamata un microfono che l'utente non sa essere attivo.
- **Cambi di dispositivo**: microfono scollegato/ricollegato, cambio dell'uscita predefinita (il tap va ricreato), cambio di sample rate, sleep/wake → ripresa automatica.
- **Quando l'app non gira o si blocca**: fixomic Mic deve dare silenzio, mai rumore o loop di buffer vecchi.
- **Scansione dei plugin** fuori dal processo principale (JUCE lo supporta), così che un plugin difettoso non faccia crashare l'app durante la scansione.
- Permessi gestiti con messaggi chiari se negati (`NSMicrophoneUsageDescription`, `NSAudioCaptureUsageDescription`).

## Driver virtuale (indicazioni)

- Preferito: l'utente vede **"fixomic Mic" solo tra gli ingressi**, non tra gli altoparlanti. Un modo plausibile: lo stesso AudioServerPlugIn pubblica due dispositivi che condividono un ring buffer interno: "fixomic Mic" (solo ingresso, visibile) e un dispositivo d'uscita nascosto (`kAudioDevicePropertyIsHidden`) che l'app apre tramite UID. Da verificare in fase di build.
- Accettabile come ripiego per la v1: dispositivo loopback duplex in stile BlackHole, con un nome che renda ovvio che non è un altoparlante.
- Canali: ingresso stereo con L=R (le app si aspettano spesso 1 o 2 canali); sample rate 48 kHz di base, 44,1 kHz supportato.

## Riferimenti studiati

- [Curve](https://github.com/tomderham/curve) (AGPL, JUCE 8): host AU/VST3 nella barra dei menu, gestione dispositivi e loopback via tap. Il miglior riferimento per la parte host.
- [LightHost](https://github.com/opencma/LightHost) e fork (Drahonn, LightHostReforge, MicVSTHost): host JUCE minimale nella barra dei menu.
- [BlackHole](https://github.com/ExistentialAudio/BlackHole) (GPL-3.0): base del driver.
- [mac-virtual-mic-bridge](https://github.com/marcohefti/mac-virtual-mic-bridge): microfono virtuale con ripresa da sleep e riconnessione.
- [webrtc-audio-processing](https://github.com/Sqhh99/webrtc-audio-processing), [Enaium/webrtc-aec3](https://github.com/Enaium/webrtc-aec3): AEC3 autonomo con build per macOS.
- [aec-client](https://github.com/samudinzul/aec-client) (Windows): stessa idea (AEC → microfono virtuale), confronto fra motori AEC.
- [AudioCap](https://github.com/insidegui/AudioCap), [macos-audio-tap](https://github.com/firedanceio/macos-audio-tap): esempi di Core Audio process tap.

## Lasciato al builder

- Struttura del repository, sistema di build (preferibilmente CMake; integrazione di AEC3 senza rendere la build fragile).
- Disposizione interna dei canali della catena (mono vs stereo) purché Nectar e i gate comuni funzionino.
- Formato di salvataggio della configurazione.
- Design visivo dell'interfaccia (con le skill di design).
- Come misurare e testare latenza e qualità dell'AEC (es. banco di prova offline con registrazioni microfono + riferimento, e test con il caso reale HyperX su jack).
- Scelta dello strumento di installazione del driver (script con privilegi amministratore, pacchetto .pkg, ecc.), purché resti un'esperienza guidata dall'app.
