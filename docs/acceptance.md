# Accettazione v1

Le sette prove del piano (Task 11, passo 3), fatte davvero: cuffie HyperX sul jack combo del Mac,
Discord, e un secondo account o una seconda persona in ascolto.

Chi le esegue:

- **11b**: l'agente, sul Mac di sviluppo e senza nessuno alla tastiera. Misura con registrazioni e
  con i comandi da sviluppatore della build Debug (`--status`, `--snapshot`).
- **Utente**: serve una persona, per Discord con un secondo ascoltatore, per l'ascolto a orecchio,
  per sleep/wake e per staccare le cuffie.

Le celle *Esito* restano vuote finché la prova non è stata fatta. Scrivi ✅ o ❌, la data, e cosa
hai misurato o sentito.

## Preparazione

1. fixomic installato da `dist/fixomic-<versione>.zip` (vedi il [README](../README.md)), driver
   installato, permessi per microfono e audio di sistema concessi. Per 11b: build Debug in `build/`.
2. Nelle impostazioni di fixomic: microfono *Microfono esterno* (la HyperX sul jack), buffer 128.
   Nel pannello: cancellazione eco attiva, bypass globale spento.
3. In Discord: dispositivo di ingresso *fixomic Mic*. Disattiva la cancellazione dell'eco e la
   riduzione del rumore di Discord: la prova riguarda quella di fixomic.
4. Per le prove con un ascoltatore: un secondo account Discord su un altro dispositivo, o una
   persona in chiamata, che ascolta con le cuffie.

Comandi utili per 11b (build Debug). Il log dell'app va dove dice `--stderr`; i comandi lanciati di
nuovo con `open -n` passano all'app già aperta e scrivono lì:

```sh
APP=build/fixomic_artefacts/Debug/fixomic.app
open --stdout /tmp/fixomic.log --stderr /tmp/fixomic.log "$APP"
open -n "$APP" --args --status          # stato, latenza, catena, picchi ed ERLE nel log
ffmpeg -f avfoundation -i ":fixomic Mic" -t 30 -ac 1 -ar 48000 mic.wav   # registra fixomic Mic
open -n "$APP" --args --set-aec 0       # cancellazione eco spenta (1: accesa), come l'interruttore
```

> **ffmpeg va bene come client, non come registratore di misura.** Su questo Mac l'ingresso
> `avfoundation` di ffmpeg 9.0.2 perde circa un buffer su cinque senza avvisare: con `-t 75` ha
> scritto 60 s di audio in 76,6 s, anche dal microfono fisico, quindi non è fixomic. In più `-ac 1`
> somma i due canali uguali di fixomic Mic (+3 dB). Per livelli e click 11b ha usato un registratore
> Core Audio che scrive ogni frame (`carec`, vedi `task-11b-report.md`).

## Riepilogo

| # | Prova | Chi | Esito |
|---|-------|-----|-------|
| 1 | Video in cuffia, utente in silenzio: l'altro non sente il video | 11b (misura) + Utente (Discord) | 11b ✅ 13/11: eco −31,6 dB, residuo sotto il rumore di fondo · Discord: da fare dall'utente |
| 2 | Utente che parla sopra il video: voce chiara, video non udibile | 11b (misura dell'eco) + Utente (voce, Discord) | misura dell'eco: vedi 1 · voce: da fare dall'utente |
| 3 | Catena Gate + Nectar attiva: voce trattata | Utente (a orecchio) | da fare dall'utente |
| 4 | Latenza mostrata ≤ 16 ms (AEC on) e ≤ 5 ms (AEC off), esclusi i plugin | 11b | ❌ 13/11: 29,1 ms (AEC on) e 10,2 ms (AEC off) con buffer 128; 7,5 ms AEC off con buffer 64 |
| 5 | Sessione di 2 ore: nessun click, latenza mostrata invariata | 11b (registrazione) | da rifare: interrotto su richiesta dell'utente dopo 28,8 min (nessun click né buco in quei minuti, latenza 30,1 ms invariata) |
| 6 | Sleep/wake e cuffie staccate e riattaccate a metà chiamata: riprende da solo, silenzio durante il distacco | Utente | da fare dall'utente |
| 7 | Discord chiuso: pallino arancione spento entro ~3 s | 11b (indicatore di richiesta) + Utente (pallino) | 11b ✅ 13/11: microfono chiuso dopo 3,18–3,32 s · pallino: da fare dall'utente |
| — | Client VoiceProcessingIO (FaceTime, Discord): richiesta e audio | 11b | ✅ 13/11: Attivo in 0,25–0,27 s, audio ricevuto, In attesa 3,2–3,3 s dopo lo stop |

Ambiente delle misure 11b (13/11/2026): MacBook Air, macOS 27.2, build Debug di `feat/v1`. Cuffie
HyperX sul jack combo, *appoggiate sulla scrivania* accanto al Mac: l'eco è diafonia elettrica più il
suono che esce dai padiglioni ed entra nel microfono, più forte che a cuffie indossate. Volumi di
ingresso e uscita al massimo. Microfono *Microfono esterno*, buffer 128, catena *Nectar 4 Gate*.

## 1. Video in cuffia, utente in silenzio

**Come.**
1. Avvia un video YouTube in cuffia a volume normale. L'utente resta in silenzio, a cuffie
   indossate.
2. *11b:* registra fixomic Mic per 30 s con la cancellazione eco attiva, poi altri 30 s con la
   cancellazione spenta, sempre con lo stesso video. Confronta il livello (RMS) delle due
   registrazioni e annota l'*Eco rimossa* del pannello (o `ERLE` di `--status`).
3. *Utente:* il secondo ascoltatore in Discord dice se sente il video.

**Atteso.** L'ascoltatore non sente il video. Con la cancellazione attiva la registrazione resta
vicina al rumore di fondo, e il video non si riconosce all'ascolto. Con la cancellazione spenta il
video si sente. Annota la differenza di livello in dB.

**Esito.**

| Misura (11b) | Discord (Utente) |
|---|---|
| ✅ 13/11/2026: con la cancellazione attiva l'eco scende di 31,6 dB e resta 22 dB sotto il rumore di fondo; in RMS fixomic Mic è 1 dB sotto il rumore di prima del programma | da fare dall'utente |

Misure 11b (programma di 60 s in cuffia, dettagli e metodo sotto):

| Registrazione | Rumore di fondo | Durante il programma | Eco (parte coerente col programma) |
|---|---|---|---|
| Microfono crudo (*Microfono esterno*, senza fixomic) | −41,7 dBFS | −31,8 dBFS | −32,3 dB |
| fixomic Mic, cancellazione spenta | −41,9 dBFS | −31,9 dBFS | −32,4 dB |
| fixomic Mic, cancellazione attiva, gate escluso | −42,0 dBFS | **−43,0 dBFS** | **−64,0 dB** |
| fixomic Mic, cancellazione attiva, catena attiva | −42,0 dBFS | **−43,0 dBFS** | **−65,2 dB** |

- L'eco c'è davvero: nel microfono crudo il programma sta circa 10 dB sopra il rumore della stanza
  (−41,7 dBFS: è il rumore della stanza, lo stesso con e senza fixomic).
- Con la cancellazione spenta fixomic Mic è il microfono crudo (stesso livello, stessa eco).
- Con la cancellazione attiva il livello durante il programma (−43,0 dBFS) è 11,1 dB sotto quello a
  cancellazione spenta e 1 dB *sotto* il rumore di fondo: in RMS non si può scendere oltre, il resto
  è la stanza. L'eco vera, separata dal rumore, scende di 31,6 dB (32,8 dB con la catena), fino a
  4 dB dal limite di misura (−68 dB).
- *Eco rimossa* (`ERLE` di `--status`) a +20, +40 e +55 s: 12, 6, 9 dB a gate escluso, 11, 6, 8 dB
  con la catena. Il pannello calcola energia del microfono / energia dell'uscita: con un'eco solo
  10 dB sopra il rumore non può leggere molto più di 10 dB, anche se l'eco è tolta quasi tutta.

**Metodo (11b).** Programma di 60 s generato sul Mac: voce `say -v Alice` su un letto musicale di
sintesi (ffmpeg `aevalsrc`), −14,7 LUFS come YouTube, suonato in cuffia con `afplay`. Quattro
riprese: cancellazione attiva e spenta (`--set-aec 1|0`), con il gate escluso (`--bypass 0 1`) e
con la catena attiva. In ogni ripresa `carec` registra insieme fixomic Mic (canale 0) e il microfono
crudo: 6 s di silenzio, il programma, 6 s di silenzio. Il microfono crudo si allinea al programma per
correlazione; fixomic Mic si allinea al microfono crudo seguendo la deriva tra i due orologi (33 ppm:
fixomic Mic va col clock del driver, il microfono col codec). L'eco coerente è la potenza della
registrazione prevedibile linearmente dal programma (spettri incrociati di Welch): il rumore della
stanza non è coerente col programma, quindi resta fuori. `--status` a +20, +40 e +55 s per l'ERLE.

## 2. Utente che parla sopra il video

**Come.**
1. Con lo stesso video in cuffia, l'utente parla normalmente per circa un minuto, anche mentre il
   video ha voci o musica forti (double-talk).
2. *11b:* registra fixomic Mic durante il video: è la stessa misura dell'eco del punto 1, fatta
   mentre il video cambia. La parte con la voce richiede una persona che parli.
3. *Utente:* l'ascoltatore giudica la voce: completa, senza sillabe mangiate, senza "effetto
   radio". E dice se il video si sente.

**Atteso.** La voce arriva chiara e intera. Il video non è udibile, o lo è appena nei momenti di
double-talk: meglio un po' di eco residua che una voce mangiata.

**Esito.**

| Misura (11b) | Voce in Discord (Utente) |
|---|---|
| L'eco del video senza voce è quella del punto 1 (−31,6 dB, sotto il rumore). La parte con la voce richiede una persona | da fare dall'utente (passi in *Da fare dall'utente*) |

## 3. Catena Gate + Nectar

**Come.** Nel pannello, *+ Aggiungi plugin*: un gate (per esempio *Nectar 4 Gate*) e poi Nectar.
Regolali dalle loro finestre. L'utente parla in Discord, prima con la catena attiva e poi con il
bypass globale.

**Atteso.** Con la catena attiva la voce è trattata (il gate chiude nelle pause, si sente l'effetto
di Nectar). Con il bypass globale arriva il microfono crudo. Alla riapertura di fixomic la catena e
le regolazioni sono quelle di prima.

**Esito.**

| Utente |
|---|
| da fare dall'utente (passi in *Da fare dall'utente*) |

## 4. Latenza mostrata

**Come.** *11b:* con un client che legge fixomic Mic (Discord, o `ffmpeg` come sopra), leggi la riga
*Latenza* del pannello e il dettaglio *Buffer · eco · plugin · dispositivi* (o `latency` di
`--status`). Prima con la cancellazione eco attiva, poi spenta. Ripeti con buffer 64 e 128.
Togli la parte *plugin* dal totale.

**Atteso (piano).** ≤ 16 ms con la cancellazione attiva, ≤ 5 ms con la cancellazione spenta.

> **Già noto, da confermare.** La cancellazione eco (WebRTC AEC3) aggiunge da sola 910 campioni a
> 48 kHz, cioè 19,0 ms: un blocco da 10 ms per disaccoppiarla dal buffer, più il ritardo dell'AEC3
> stesso. Con buffer 128 il buffer conta 2 × 2,7 = 5,3 ms. Totale misurato con la cancellazione
> attiva: **24,3 ms** più i dispositivi, oltre l'obiettivo di 16 ms. Con la cancellazione spenta:
> **5,3 ms** con buffer 128 e **2,7 ms** con buffer 64, più i dispositivi.

**Esito.** ❌ 13/11/2026: gli obiettivi del piano non sono raggiunti, come già noto. Con un client
in lettura (stato *Attivo*), dal `--status`:

| Buffer | AEC on (senza plugin) | AEC off (senza plugin) | Dispositivi |
|---|---|---|---|
| 64 | ❌ 26,5 ms (mostrati 27,4 ms) | ❌ 7,5 ms (mostrati 8,5 ms) | 4,8 ms (ingresso 168 + uscita 64 campioni) |
| 128 | ❌ 29,1 ms (mostrati 30,1 ms) | ❌ 10,2 ms (mostrati 11,2 ms) | 4,8 ms (ingresso 168 + uscita 64 campioni) |

- Le parti, in campioni a 48 kHz: buffer 2 × il buffer scelto (128 con buffer 64, 256 con buffer
  128), eco 910 (19,0 ms), plugin 47
  (1,0 ms, *Nectar 4 Gate*), dispositivi 232 (4,8 ms). Senza i dispositivi i totali sono quelli già
  noti: 24,3 ms (AEC on, 128), 5,3 ms (AEC off, 128), 2,7 ms (AEC off, 64).
- La sola cancellazione eco (19,0 ms) supera l'obiettivo di 16 ms. A cancellazione spenta i 5 ms si
  rispettano solo con buffer 64 e senza contare i dispositivi.
- Il pannello mostra le stesse cifre: *Latenza 30,1 ms*, *Buffer 5,3 · eco 19,0 · plugin 1,0 ·
  dispositivi 4,8 ms* (snapshot con AEC on, buffer 128).

**Metodo (11b).** `carec` legge fixomic Mic per tutta la prova. Per ogni combinazione
`--set-aec 1|0 --set-buffer 128|64`, 3 s di assestamento e poi `--status`, che ora stampa anche il
dettaglio in campioni (`latency … : buffer, aec, plugins, input device, output device`). Ogni cambio
riapre l'IO: il client riceve 0,35–0,39 s di silenzio (zeri), senza click. Poi `--snapshot` del
pannello.

## 5. Sessione di 2 ore

**Come.** *11b:* per 2 ore un client legge fixomic Mic mentre in cuffia suona audio con parti in
silenzio, e la registrazione di fixomic Mic va su file. Leggi la latenza con `--status` all'inizio,
ogni 30 minuti e alla fine. Poi cerca nella registrazione i click: salti tra campioni consecutivi,
buchi di zeri dentro il segnale, ripetizioni di blocchi.

**Atteso.** Nessun click, nessun buco e nessun drop-out nella registrazione. La latenza mostrata
resta identica dall'inizio alla fine. Lo stato resta *Attivo*.

**Esito.** Da rifare: la sessione è stata interrotta su richiesta dell'utente (13/11/2026, 03:19:
niente suono in cuffia di notte). Risultato parziale dei minuti registrati:

| Click / buchi trovati | Latenza: inizio → fine | Note |
|---|---|---|
| Nessuno in 28,8 min (27,2 min con il programma in cuffia): 0 buchi di zeri, 0 blocchi ripetuti, 0 salti nella linea del tempo. I 4 picchi segnalati dal rilevatore sono suoni presenti anche nel microfono crudo nello stesso istante | 30,1 ms → 30,1 ms (inizio, +10 min, +20 min; stesse parti: buffer 256, eco 910, plugin 47, dispositivi 168 + 64 campioni) | Stato *Attivo* per tutta la sessione, nessun riavvio dell'IO; da rifare per 2 ore intere |

- Contatori dell'IO (`ioDiagnostics()`): stesso aggregato (376) dall'inizio, callback × 128 = frame a
  ogni lettura, 48000,1 e 47999,9 frame/s tra le letture: nessuna callback persa, nessuna
  riapertura, nessun riavvio di Core Audio.
- Il registratore su fixomic Mic ha ricevuto 82 939 392 frame (1727,9 s) in 1727,8 s, senza
  discontinuità nel tempo dei campioni e senza sovraccarichi.
- Lo scarto tra fixomic Mic e il microfono crudo segue la deriva tra i due orologi (circa 33 ppm) con
  un residuo di 1,0 campioni RMS (massimo 2,4). Un buffer perso o ripetuto si vedrebbe come un salto
  di almeno 128 campioni.
- coreaudiod durante la sessione: 5,7% di CPU in media (il lavoro dell'IO), con un picco breve del
  13–22% a +20 min; fixomic 5,3%, ControlCenter 0,6%.

**Metodo (11b).**
- `carec` registra per tutta la sessione fixomic Mic e il microfono crudo, mentre in cuffia gira in
  loop un programma di 240 s: voce, musica, silenzi e rumore rosa. Cancellazione eco attiva, catena
  *Nectar 4 Gate*.
- Ogni 10 minuti: `--status` e `top`.
- Analisi della registrazione:
  - picchi della derivata seconda oltre 12 volte il suo RMS locale, ciascuno confrontato con il
    microfono crudo nello stesso istante;
  - buchi di zeri esatti;
  - blocchi di 64–512 campioni ripetuti;
  - lo scarto da fixomic Mic al microfono crudo, finestra per finestra.

**Da rifare.** Rifare la sessione intera di 2 ore quando il suono in cuffia non disturba nessuno:
`run_soak.sh 7200` di 11b (in `task-11b-report.md`), oppure due ore di video in cuffia con Discord
(o `ffmpeg`) che legge fixomic Mic, poi le stesse analisi.

## 6. Sleep/wake e cuffie staccate a metà chiamata

**Come.** *Utente:* in chiamata Discord con fixomic Mic:
1. Metti il Mac in stop per almeno un minuto, poi risveglialo. Parla.
2. Stacca le cuffie dal jack per circa 10 s, poi riattaccale. Parla.

**Atteso.**
- Dopo il risveglio la voce torna da sola, senza riaprire fixomic né cambiare il microfono in
  Discord.
- Mentre le cuffie sono staccate l'ascoltatore sente silenzio, mai rumore, mai il microfono del Mac
  (fixomic non passa da solo a un altro microfono). L'icona nella barra dei menu segnala il
  microfono mancante.
- Quando le cuffie tornano, la voce riprende da sola.

**Esito.**

| Sleep/wake | Cuffie staccate e riattaccate |
|---|---|
| da fare dall'utente | da fare dall'utente |

## 7. Discord chiuso, pallino arancione

**Come.**
- *11b:* avvia un client che legge fixomic Mic (`ffmpeg`), verifica con `--status` che lo stato sia
  *Attivo*, chiudi il client e leggi `--status` ogni mezzo secondo per 5 s. Misura dopo quanto lo
  stato torna *In attesa* e il microfono viene chiuso.
- *Utente:* chiudi Discord del tutto (⌘Q) e guarda il pallino arancione del microfono nella barra
  dei menu.

**Atteso.** Entro circa 3 s fixomic chiude il microfono e il tap dell'audio di sistema: lo stato
torna *In attesa* e il pallino arancione si spegne. Se un'app riapre il microfono entro quei 3 s,
fixomic resta acceso.

**Esito.**

| Indicatore di richiesta (11b) | Pallino arancione (Utente) |
|---|---|
| ✅ 13/11/2026, circa 3 s: il microfono delle cuffie si chiude 3,18–3,32 s dopo la chiusura del client (4 prove su 4) | da fare dall'utente |

Tempi dal Ctrl-C (SIGINT) del client che legge fixomic Mic:

| Prova | Client uscito | fixomic Mic senza client | fixomic *In attesa* | Microfono cuffie spento |
|---|---|---|---|---|
| ffmpeg 1 | +0,075 s | +0,016 s | +3,240 s | +3,235 s |
| ffmpeg 2 | +0,071 s | +0,025 s | +3,318 s | +3,312 s |
| ffmpeg 3 | +0,061 s | +0,013 s | +3,186 s | +3,182 s |
| carec | +0,034 s | +0,008 s | +3,275 s | +3,271 s |

- La richiesta cade subito (8–25 ms); poi fixomic aspetta la grazia di progetto di 3,0 s
  (`EngineController::kStopGraceSeconds`), controllata dal timer di 200 ms, e chiude l'IO: il
  microfono fisico smette di girare 4–6 ms prima del log *In attesa*.
- È "circa 3 s" come chiede il piano. Un limite stretto di 3,0 s non si può rispettare con questa
  grazia: si va oltre di 0,18–0,32 s ogni volta.
- All'avvio del client fixomic va *Attivo* in 0,52–0,58 s con ffmpeg, 0,31 s con carec.

**Metodo (11b).** `devtool poll` (Core Audio, ogni 5 ms, orologio del log di fixomic) segue
`kAudioDevicePropertyDeviceIsRunningSomewhere` di fixomic Mic e del microfono delle cuffie: è la
proprietà che accende il pallino arancione. Client: `ffmpeg -f avfoundation -i ":fixomic Mic"`, 8 s,
poi `kill -INT`; una quarta prova con `carec`. Lo stato *In attesa* viene dal log dell'app
(`engine state -> Idle`).

## Client VoiceProcessingIO (FaceTime, Discord)

**Come.** *11b:* un piccolo programma apre fixomic Mic con l'unità audio `VoiceProcessingIO` di Apple
(quella che usano FaceTime e molte app VoIP), con fixomic Mic scelto come dispositivo d'ingresso
(`kAudioOutputUnitProperty_CurrentDevice`), e registra 10 s. Una volta in silenzio, una volta con il
programma del punto 1 in cuffia.

**Atteso.** fixomic va *Attivo*, la registrazione ha segnale, e torna *In attesa* circa 3 s dopo la
chiusura del programma.

**Esito.** ✅ 13/11/2026.

| Prova | Dispositivo dell'unità | *Attivo* dopo | Audio ricevuto | *In attesa* dopo lo stop |
|---|---|---|---|---|
| silenzio | fixomic Mic | +0,25 s | 10,01 s, RMS −51,8 dBFS, picco −31,2 dBFS | +3,16 s |
| programma in cuffia | fixomic Mic | +0,27 s | 10,01 s, RMS −54,8 dBFS, picco −36,7 dBFS | +3,27 s |

- L'unità accetta fixomic Mic come ingresso (non è servito cambiare l'ingresso predefinito del
  sistema, che è rimasto *Microfono MacBook Air*).
- Dopo i primi 0,50–0,56 s di zeri (l'avvio dell'IO) il segnale è continuo, senza buchi. Il livello è
  più basso del microfono crudo perché `VoiceProcessingIO` applica la sua riduzione del rumore e
  abbassa l'altro audio (il programma) mentre è attiva.

## Da fare dall'utente

Queste prove richiedono una persona: un secondo ascoltatore in Discord, l'ascolto a orecchio, lo
stop del Mac e le cuffie da staccare. 11b non le ha eseguite.

**Preparazione (una volta).**
1. Esci da ogni copia di fixomic aperta (icona nella barra dei menu → *Esci*). Apri la copia
   installata da `dist/`.
2. Nel pannello di fixomic → *Impostazioni…*: microfono *Microfono esterno*, buffer 128. Nel
   pannello: *Cancellazione eco* attiva, *Bypass* spento, catena con *Nectar 4 Gate*.
3. Discord → Impostazioni utente → *Voce e video*: dispositivo di ingresso *fixomic Mic*, uscita
   *Cuffie esterne*. Spegni la cancellazione dell'eco, la soppressione del rumore e il controllo
   automatico del guadagno di Discord.
4. Avvia una chiamata con un secondo account Discord su un altro dispositivo (o con una persona) che
   ascolta in cuffia. Indossa le cuffie HyperX.

**Punto 1, l'ascoltatore e il video.**
1. Avvia un video YouTube con parlato e musica, volume di sistema normale. Resta in silenzio per
   30 s. L'ascoltatore dice se sente il video.
2. Spegni *Cancellazione eco* nel pannello per 30 s (controprova: ora il video deve sentirsi), poi
   riaccendila.
3. Scrivi nella cella *Discord (Utente)* del punto 1: ✅ se a cancellazione attiva il video non si
   sente, ❌ altrimenti, con la data.

**Punto 2, voce sopra il video.**
1. Con lo stesso video, parla normalmente per circa un minuto, anche sopra i passaggi forti.
2. L'ascoltatore giudica: voce intera (nessuna sillaba mangiata), niente "effetto radio", video
   udibile o no. Scrivilo nella cella *Voce in Discord*.

**Punto 3, catena Gate + Nectar.**
1. Nel pannello, *+ Aggiungi plugin* → Nectar (dopo *Nectar 4 Gate*). Regola gate e Nectar dalle
   loro finestre.
2. Parla per 30 s con la catena attiva, poi per 30 s con *Bypass* acceso. L'ascoltatore deve
   sentire la differenza: il gate chiude nelle pause e Nectar tratta la voce; con il bypass arriva il
   microfono crudo.
3. Spegni *Bypass*, esci da fixomic e riaprilo: catena e regolazioni devono essere quelle di prima.

**Punto 6, sleep/wake e cuffie staccate.**
1. In chiamata, menu Apple → *Stop*. Aspetta almeno un minuto, risveglia il Mac e parla: la voce deve
   tornare da sola, senza toccare fixomic né Discord.
2. Stacca le cuffie dal jack per circa 10 s mentre parli. L'ascoltatore deve sentire silenzio (mai
   rumore, mai il microfono del Mac); l'icona di fixomic segnala il microfono mancante.
3. Riattacca le cuffie e parla: la voce deve riprendere da sola.

**Punto 7, il pallino arancione.**
1. In chiamata il pallino arancione del microfono è acceso nella barra dei menu.
2. Chiudi Discord del tutto con ⌘Q e guarda il pallino: deve spegnersi entro circa 3 s (11b ha
   misurato 3,2–3,3 s per la chiusura del microfono).
