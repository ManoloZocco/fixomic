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
```

## Riepilogo

| # | Prova | Chi | Esito |
|---|-------|-----|-------|
| 1 | Video in cuffia, utente in silenzio: l'altro non sente il video | 11b (misura) + Utente (Discord) | |
| 2 | Utente che parla sopra il video: voce chiara, video non udibile | 11b (misura dell'eco) + Utente (voce, Discord) | |
| 3 | Catena Gate + Nectar attiva: voce trattata | Utente (a orecchio) | |
| 4 | Latenza mostrata ≤ 16 ms (AEC on) e ≤ 5 ms (AEC off), esclusi i plugin | 11b | |
| 5 | Sessione di 2 ore: nessun click, latenza mostrata invariata | 11b (registrazione) | |
| 6 | Sleep/wake e cuffie staccate e riattaccate a metà chiamata: riprende da solo, silenzio durante il distacco | Utente | |
| 7 | Discord chiuso: pallino arancione spento entro ~3 s | 11b (indicatore di richiesta) + Utente (pallino) | |

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
| | |

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
| | |

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
| |

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

**Esito.**

| Buffer | AEC on (senza plugin) | AEC off (senza plugin) | Dispositivi |
|---|---|---|---|
| 64 | | | |
| 128 | | | |

## 5. Sessione di 2 ore

**Come.** *11b:* per 2 ore un client legge fixomic Mic mentre in cuffia suona audio con parti in
silenzio, e la registrazione di fixomic Mic va su file. Leggi la latenza con `--status` all'inizio,
ogni 30 minuti e alla fine. Poi cerca nella registrazione i click: salti tra campioni consecutivi,
buchi di zeri dentro il segnale, ripetizioni di blocchi.

**Atteso.** Nessun click, nessun buco e nessun drop-out nella registrazione. La latenza mostrata
resta identica dall'inizio alla fine. Lo stato resta *Attivo*.

**Esito.**

| Click / buchi trovati | Latenza: inizio → fine | Note |
|---|---|---|
| | | |

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
| | |

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
| | |
