/*
  DROP_LIB v3 — bibliothèque de fichiers locale sur ESP32 Super Mini
  v3 : présentation minimaliste « écran d'ordinateur ambre » (texte seul, une couleur, crochets,
       vidéo inversée), sans image ni dessin : page plus légère.
  v2 : « casier à disquettes » (archive dans _dev/DROP_LIB_v2), jauge de place, fiche détaillée,
       explication claire quand la fenêtre de connexion automatique du téléphone ne sait pas
       ouvrir le sélecteur de fichiers (voir plus bas).
  - Point d'accès Wi-Fi ouvert "DROP_LIB", DNS captif, page sur http://4.3.2.1
    (ou http://192.168.4.1 si USE_PUBLIC_IP vaut 0)
  - On y dépose des fichiers texte (.txt .md .csv .log .json) et des PDF légers
    (MAX_FILE_KB par fichier), ou on tape / colle un texte directement dans la page :
    il est alors enregistré comme fichier .txt. Marche sur téléphone et sur ordinateur
    (sur ordinateur, on peut aussi glisser-déposer les fichiers dans la page).
  - Tout reste dans la flash (LittleFS) et survit au redémarrage. Chaque fichier se télécharge
    sur l'appareil ; les textes peuvent aussi être lus et copiés dans la page.
  - Seul l'auteur (jeton secret gardé par son téléphone) peut supprimer son fichier.
    Le gardien (GUARD_USER / GUARD_PASS, comme DROP-BOOK) peut en sélectionner et effacer
    à tout moment. Quand la place manque, la bibliothèque indique qu'elle est pleine :
    rien n'est effacé automatiquement.
  Bibliothèques : WiFi.h, WebServer.h, DNSServer.h, LittleFS.h (incluses dans le cœur Arduino ESP32)

  Arduino IDE : Type de carte "ESP32C3 Dev Module", USB CDC On Boot "Enabled".
  Place pour les fichiers : le fichier partitions.csv, à garder dans le dossier du croquis, donne
  2,7 Mo aux fichiers (le programme garde 1,19 Mo) quel que soit le « Partition Scheme ». Sans lui :
  « No OTA (2MB APP/2MB SPIFFS) » donne 1,9 Mo, le schéma par défaut seulement 1,3 Mo.
  Changer la table de partitions efface la bibliothèque (la carte formate sa mémoire au démarrage).

  IMPORTANT — dépôt de fichiers : la petite fenêtre de connexion que le téléphone ouvre tout seul
  (portail captif) ne sait pas ouvrir le sélecteur de fichiers. Elle sert à parcourir, lire,
  télécharger et écrire des textes. Pour déposer un PDF, ouvrir http://4.3.2.1 dans un vrai
  navigateur (Chrome, Safari…), sur téléphone ou ordinateur. Sur Android, il faut couper la 4G
  (mode avion puis Wi-Fi seul : vérifié sur un téléphone) pour que le navigateur atteigne la
  carte : le téléphone envoie sinon le trafic vers la 4G. Sans navigateur du tout (ordinateur, script) : le dépôt marche aussi avec curl, par exemple
    curl -F "f=@cours.pdf" "http://4.3.2.1/up?tok=0123456789abcdef&n=Moi&t=$(date +%s)"
  (tok : 16 caractères hexadécimaux au choix, ils servent à retrouver ses propres dépôts).
  Sans Wi-Fi du tout : _dev/DROP_LIB/flash_lib.py fabrique une image de la bibliothèque à partir d'un
  dossier de l'ordinateur et l'écrit par le câble USB dans la partition de données (le programme
  n'est pas touché ; --keep conserve ce qui est déjà sur la carte).

  Sources : gardien et stockage sur le modèle de DROP-BOOK, structure de l'envoi de la page et du
  portail captif reprises de DROPCHAT (mêmes auteurs). Idées de portail captif reprises de
  CDFER/Captive-Portal-ESP32 (licence MIT) : https://github.com/CDFER/Captive-Portal-ESP32
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>

// ---------- Réglages ----------
const char* AP_SSID        = "DROP_LIB";
const char* AP_PASS        = "";   // vide = réseau ouvert (sinon 8 caractères minimum)
const int   AP_CHANNEL     = 1;
const int   AP_MAX_CLIENTS = 8;    // 10 maximum sur ESP32

#define USE_PUBLIC_IP 1            // 1 = 4.3.2.1 (portail mieux détecté), 0 = 192.168.4.1

#define MAX_ENTRIES     100        // fichiers gardés au maximum
#define MAX_FILE_KB     512        // taille maximale d'un fichier (Ko)
#define MAX_FILE_BYTES  ((size_t)MAX_FILE_KB * 1024)
#define FNAME_MAX       80         // octets UTF-8, extension comprise
#define NICK_MAX        32         // octets UTF-8
#define FREE_MARGIN     32768      // place gardée libre dans la flash (octets) : en dessous, c'est "plein"

// Gardien de la bibliothèque : peut sélectionner et effacer n'importe quel fichier
const char* GUARD_USER = "marguerite";
const char* GUARD_PASS = "1226";
#define GUARD_MAX_FAILS 5          // après 5 erreurs, connexion bloquée...
#define GUARD_LOCK_MS   60000      // ...pendant 1 minute
#define POST_COOLDOWN   1000       // délai mini entre deux dépôts d'un même appareil (ms)
#define SENDER_SLOTS    12

// ---------- Données ----------
struct Item {
  uint32_t id;
  uint32_t rev;                    // change à chaque modification
  uint32_t created;                // date (secondes, heure du téléphone) ou 0
  uint32_t size;
  uint32_t own;                    // empreinte du jeton de l'auteur
  char     kind;                   // 'P' = PDF, 'T' = texte
  char     name[FNAME_MAX + 1];
  char     nick[NICK_MAX + 1];
};

Item     items[MAX_ENTRIES];       // du plus ancien au plus récent
int      ecount = 0;
uint32_t lastId = 0;
char     gen[9] = "00000000";

struct Sender { uint32_t ip; uint32_t t; };
Sender senders[SENDER_SLOTS];

// Dépôt en cours (l'envoi arrive par morceaux, directement écrit dans la flash)
struct Upload {
  File     f;
  bool     started;
  int      err;                    // 0 = bon, sinon code HTTP à renvoyer
  char     kind;
  bool     first;
  size_t   bytes;
  size_t   limit;
  uint32_t t;
  char     tok[17];
  char     nick[NICK_MAX + 1];
  char     name[FNAME_MAX + 1];
};
Upload up;

WebServer server(80);
DNSServer dns;
#if USE_PUBLIC_IP
const IPAddress apIP(4, 3, 2, 1);
#else
const IPAddress apIP(192, 168, 4, 1);
#endif
String apHost;
String portalUrl;

// ---------- Page ----------
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#0d0900">
<title>DROP_LIB</title>
<link rel="icon" href="data:,">
<style>
/* Écran d'ordinateur ambre des années 80 : une seule couleur, de la police à chasse fixe, des crochets,
   la vidéo inversée pour ce qui est actif. Aucune image : tout est du texte. */
:root{
  --bg:#0d0900; --fg:#ffb000; --hi:#ffd98a; --dim:#a06a00; --line:#6b4700; --err:#ff7a45;
  --mono:ui-monospace,"SF Mono",Menlo,Consolas,"Liberation Mono","DejaVu Sans Mono","Courier New",monospace;
}
*{box-sizing:border-box;margin:0;padding:0}
[hidden]{display:none!important}
/* Page web normale : c'est le document qui défile (indispensable dans la fenêtre de connexion Android) */
html{overflow-anchor:none;overscroll-behavior-y:none;background:var(--bg);-webkit-text-size-adjust:100%}
body{min-height:calc(100vh + 2px);background:var(--bg);color:var(--fg);font:15px/1.45 var(--mono);overscroll-behavior-y:none;text-shadow:0 0 5px rgba(255,176,0,.3)}
button,input,textarea,a{font:inherit;color:inherit;text-shadow:inherit}
button{background:none;border:0;cursor:pointer;-webkit-tap-highlight-color:transparent}
::selection{background:var(--fg);color:var(--bg)}

/* Boutons entre crochets ; vidéo inversée quand on appuie ou quand c'est actif */
.bt{display:inline-block;min-height:44px;padding:10px 6px;color:var(--fg);text-decoration:none;text-transform:uppercase;letter-spacing:.04em;text-align:center}
.bt::before{content:"[\00a0"}
.bt::after{content:"\00a0]"}
.bt:active,.bt.on,.bt.go{background:var(--fg);color:var(--bg);text-shadow:none}
.bt:disabled{opacity:.4}
.bt.go{font-weight:700}
.bt.big{display:block;width:100%;min-height:54px;font-size:17px}
.inv{background:var(--fg);color:var(--bg);text-shadow:none}

.mast{position:sticky;top:0;z-index:5;background:var(--bg);border-bottom:1px solid var(--fg)}
.mast .in{display:flex;align-items:center;justify-content:space-between;gap:12px;max-width:820px;margin:0 auto;padding:12px max(16px,env(safe-area-inset-right)) 10px max(16px,env(safe-area-inset-left))}
h1{font-size:20px;font-weight:700;letter-spacing:.08em}
.cur{display:inline-block;width:.55em;height:1em;margin-left:3px;vertical-align:-.12em;background:var(--fg);animation:bl 1.1s steps(1) infinite}
@keyframes bl{50%{opacity:0}}
@media (prefers-reduced-motion:reduce){.cur{animation:none}}
.st{display:flex;flex-direction:column;align-items:flex-end;gap:5px;font-size:13px}
#gauge{display:flex;gap:2px}
#gauge i{width:7px;height:11px;border:1px solid var(--dim)}
#gauge i.on{background:var(--fg);border-color:var(--fg)}
#gauge.hot i.on{background:var(--err);border-color:var(--err)}

.wrap{max-width:820px;margin:0 auto;padding:0 max(16px,env(safe-area-inset-right)) 0 max(16px,env(safe-area-inset-left))}
.pr{display:flex;align-items:center;gap:10px;min-height:46px;margin-top:16px;padding:0 12px;border:1px solid var(--fg)}
.pr:focus-within{background:rgba(255,176,0,.08)}
#q{flex:1;min-width:0;background:none;border:0;outline:0;color:var(--hi);font-size:16px;caret-color:var(--fg);-webkit-appearance:none;appearance:none}
#q::placeholder{color:var(--dim)}
.chips{display:flex;flex-wrap:wrap;gap:0 6px;margin-top:6px}
.chip{min-height:40px;font-size:13px}
#sort{margin-left:auto;color:var(--dim)}

#full{display:none;margin-top:14px;padding:10px 12px;background:var(--fg);color:var(--bg);text-shadow:none;font-weight:700}
#list{padding:8px 0 10px}
.f{display:grid;grid-template-columns:4.6em 1fr;gap:0 10px;width:100%;min-height:54px;padding:9px 4px;border-bottom:1px dashed var(--line);text-align:left}
.f .ty{grid-row:1/3;color:var(--dim)}
.f .ty::before{content:"["}
.f .ty::after{content:"]"}
.f .nm{color:var(--hi);font-weight:700;overflow-wrap:anywhere}
.f .mt{font-size:13px;color:var(--dim);overflow-wrap:anywhere}
.f .mt b{color:var(--fg)}
.f:active,.f.sel{background:var(--fg);color:var(--bg);text-shadow:none}
.f:active .ty,.f:active .nm,.f:active .mt,.f:active .mt b,.f.sel .ty,.f.sel .nm,.f.sel .mt,.f.sel .mt b{color:var(--bg)}
.f.sel .nm::before{content:"* "}
#empty{padding:16vh 6px;text-align:center;color:var(--dim);line-height:1.8}

.foot{padding:22px 0 26vh;font-size:13px;line-height:1.6;color:var(--dim);text-align:center}
.foot p{margin:8px 0 10px}
.foot b{color:var(--hi)}
.foot .bt{min-height:38px;font-size:12px}
#space{color:var(--fg)}

#add{position:fixed;left:50%;bottom:calc(14px + env(safe-area-inset-bottom));transform:translateX(-50%);z-index:6;width:min(86vw,360px);background:var(--fg);color:var(--bg);text-shadow:none;box-shadow:0 0 0 6px var(--bg)}
body.guard #add{bottom:calc(76px + env(safe-area-inset-bottom))}
body.guard #tray{bottom:calc(140px + env(safe-area-inset-bottom))}
body.guard #toast{bottom:calc(140px + env(safe-area-inset-bottom))}

/* Dépôts en cours : une ligne par fichier */
#tray{position:fixed;left:max(12px,env(safe-area-inset-left));right:max(12px,env(safe-area-inset-right));bottom:calc(84px + env(safe-area-inset-bottom));z-index:9;display:flex;flex-direction:column;gap:6px;max-width:460px;margin:0 auto}
.job{display:flex;justify-content:space-between;gap:12px;padding:8px 10px;border:1px solid var(--fg);background:var(--bg);font-size:13px}
.job .jn{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
.job .js{flex:none;white-space:pre}
.job.ok{background:var(--fg);color:var(--bg);text-shadow:none}
.job.err{border-color:var(--err);color:var(--err)}

/* Fiche d'un fichier : une fenêtre à double bordure */
#rd{position:fixed;inset:0;z-index:12;display:none;align-items:flex-end;justify-content:center;background:rgba(0,0,0,.82);touch-action:none;overscroll-behavior:contain}
.rdp{width:100%;max-width:560px;max-height:90vh;overflow-y:auto;overscroll-behavior:contain;touch-action:pan-y;padding:0 0 calc(16px + env(safe-area-inset-bottom));border:4px double var(--fg);border-bottom:0;background:var(--bg);display:flex;flex-direction:column;gap:14px}
.ttl{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:2px 4px 2px 12px;background:var(--fg);color:var(--bg);text-shadow:none;font-weight:700}
.ttl .bt{color:var(--bg);min-height:44px}
.ttl .bt:active{background:var(--bg);color:var(--fg)}
#rdName{overflow-wrap:anywhere;min-width:0}
#rdMeta{padding:0 14px;font-size:13px;color:var(--dim)}
.rdp>.bt,.rdp>.rda,.rdp>#rdPv{margin:0 14px}
#rdDl{width:auto}
.rda{display:flex;flex-wrap:wrap;align-items:center;gap:0 6px}
.rda span{color:var(--hi)}
#rdPv{display:none;max-height:44vh;overflow:auto;overscroll-behavior:contain;padding:10px;border:1px solid var(--line);color:var(--hi);font-size:13px;line-height:1.55;white-space:pre-wrap;overflow-wrap:anywhere;-webkit-user-select:text;user-select:text}

/* Feuille de dépôt */
#sheet{position:fixed;inset:0;z-index:10;display:none;flex-direction:column;background:var(--bg);overscroll-behavior:none}
#sheet .bar{display:flex;justify-content:space-between;align-items:center;gap:6px;padding:4px max(10px,env(safe-area-inset-right)) 4px max(10px,env(safe-area-inset-left));background:var(--fg);color:var(--bg);text-shadow:none;font-weight:700}
#sheet .bar .bt{color:var(--bg)}
#sheet .bar .bt:active{background:var(--bg);color:var(--fg)}
#sheet .bar span{text-transform:uppercase;letter-spacing:.1em;font-size:13px}
#sheet .body{flex:1;overflow-y:auto;overscroll-behavior:contain;padding:18px max(18px,env(safe-area-inset-right)) calc(26px + env(safe-area-inset-bottom)) max(18px,env(safe-area-inset-left))}
#sheet .col{max-width:40em;margin:0 auto;display:flex;flex-direction:column;gap:16px}
.seg{display:flex;gap:6px}
.seg .bt{flex:1;border:1px solid var(--fg);min-height:48px}
.seg .bt::before,.seg .bt::after{content:none}
.field{display:flex;flex-direction:column;gap:4px}
.field label,.lab label{font-size:12px;letter-spacing:.12em;text-transform:uppercase;color:var(--dim)}
.lab{display:flex;justify-content:space-between;align-items:center}
.lab .bt{min-height:36px;padding:4px 6px;font-size:12px}
#sheet input[type=text],#sheet textarea{width:100%;padding:11px 12px;border:1px solid var(--fg);border-radius:0;background:none;color:var(--hi);font-size:16px;outline:none;caret-color:var(--fg)}
#sheet input[type=text]:focus,#sheet textarea:focus{background:rgba(255,176,0,.08)}
#sheet ::placeholder{color:var(--dim)}
#tName{font-weight:700}
#tText{min-height:38vh;resize:vertical;font-size:15px;line-height:1.55}
#tCount{font-size:12px;color:var(--dim);text-align:right}
.hint{font-size:13px;color:var(--dim);line-height:1.5}
#pick{display:block;padding:22px 12px;border:1px dashed var(--fg);text-align:center;cursor:pointer}
#pick b{display:block;font-size:16px;text-transform:uppercase;letter-spacing:.06em}
#pick .s{font-size:13px;color:var(--dim)}
#file{position:absolute;width:1px;height:1px;opacity:0;pointer-events:none}
#chosen{display:flex;flex-direction:column}
.ch{display:flex;align-items:center;gap:10px;padding:2px 0 2px 2px;border-bottom:1px dashed var(--line)}
.ch .cn{flex:1;min-width:0;overflow-wrap:anywhere;color:var(--hi)}
.ch .cs{flex:none;font-size:12px;color:var(--dim)}
.ch.bad .cn{color:var(--dim);text-decoration:line-through}
.ch.bad .cs{color:var(--err)}
.ch .bt{min-height:40px;padding:4px 0}
#wvnote{padding:12px;border:1px solid var(--fg);display:flex;flex-direction:column;gap:8px;font-size:14px;line-height:1.5}
#wvnote strong{align-self:flex-start;padding:0 8px;background:var(--fg);color:var(--bg);text-shadow:none;text-transform:uppercase;letter-spacing:.06em}
#wvnote b{color:var(--hi)}
#wvnote ol{padding-left:1.6em}
#wvnote .sm{font-size:13px;color:var(--dim)}
#wvnote .bt{align-self:flex-start;min-height:40px}
#sErr{min-height:1.2em;color:var(--err);font-weight:700}

/* Gardien */
#login{position:fixed;inset:0;z-index:15;display:none;align-items:center;justify-content:center;padding:22px;background:rgba(0,0,0,.88)}
#login form{width:100%;max-width:320px;padding:16px;border:4px double var(--fg);background:var(--bg);display:flex;flex-direction:column;gap:14px}
#login h3{padding:2px 8px;background:var(--fg);color:var(--bg);text-shadow:none;font-size:13px;letter-spacing:.1em;text-transform:uppercase}
#login input{padding:11px 12px;border:1px solid var(--fg);border-radius:0;background:none;color:var(--hi);font-size:16px;outline:none}
#lErr{min-height:1.2em;color:var(--err);font-weight:700;text-align:center}
.lrow{display:flex;justify-content:space-between;gap:10px}
#gbar{position:fixed;left:0;right:0;bottom:0;z-index:7;display:none;align-items:center;justify-content:space-between;gap:10px;padding:6px max(14px,env(safe-area-inset-right)) calc(6px + env(safe-area-inset-bottom)) max(14px,env(safe-area-inset-left));background:var(--fg);color:var(--bg);text-shadow:none;font-weight:700;font-size:14px}
#gbar .gacts{display:flex;gap:2px}
#gbar .bt{color:var(--bg);font-size:13px}
#gbar .bt:active{background:var(--bg);color:var(--fg)}

#drop{position:fixed;inset:0;z-index:30;display:none;align-items:center;justify-content:center;background:rgba(0,0,0,.88);pointer-events:none}
body.dragging #drop{display:flex}
#drop div{padding:50px 36px;border:2px dashed var(--fg);font-size:18px;letter-spacing:.08em;text-transform:uppercase}
#toast{position:fixed;left:50%;bottom:calc(84px + env(safe-area-inset-bottom));transform:translateX(-50%);z-index:20;max-width:92vw;padding:7px 14px;background:var(--fg);color:var(--bg);text-shadow:none;font-weight:700;font-size:14px;text-align:center;opacity:0;pointer-events:none;transition:opacity .15s}
#toast.on{opacity:1}

@media (max-width:380px){
  #sheet .bar span{display:none}
}
@media (min-width:720px){
  #add{left:auto;right:max(24px,env(safe-area-inset-right));transform:none;width:auto;padding:10px 22px}
  #rd{align-items:center}
  .rdp{border-bottom:4px double var(--fg)}
}
</style>
</head>
<body>
<header class="mast"><div class="in">
  <h1>DROP_LIB<i class="cur"></i></h1>
  <div class="st"><span id="count">ouverture…</span><span id="gauge" aria-hidden="true"></span></div>
</div></header>

<div class="wrap">
  <label class="pr"><span>&gt;</span><input id="q" type="search" placeholder="chercher un fichier" autocomplete="off" autocapitalize="off" spellcheck="false" enterkeyhint="search"></label>
  <div class="chips">
    <button class="bt chip on" type="button" data-k="">tous</button>
    <button class="bt chip" type="button" data-k="T">textes</button>
    <button class="bt chip" type="button" data-k="P">pdf</button>
    <button class="bt chip" id="sort" type="button">tri:récents</button>
  </div>
  <div id="full">RÉPERTOIRE PLEIN. Libère de la place avant de déposer un nouveau fichier.</div>
  <main id="list"></main>
  <footer class="foot">
    <div id="space"></div>
    <p>Pour déposer un PDF depuis Android : mode avion, Wi-Fi seul,<br>puis ouvre <b id="addr"></b> dans Chrome.</p>
    <div><button class="bt" id="cpAddr" type="button">copier l’adresse</button><button class="bt" id="gOpen" type="button">gardien</button></div>
  </footer>
</div>

<button id="add" class="bt big" type="button">+ déposer</button>
<div id="tray"></div>

<div id="rd" role="dialog" aria-modal="true" aria-label="Fichier">
  <div class="rdp">
    <div class="ttl"><span id="rdName"></span><button id="rdClose" class="bt" type="button" aria-label="Fermer">x</button></div>
    <div id="rdMeta"></div>
    <a id="rdDl" class="bt go big">↓ télécharger</a>
    <div class="rda" id="rdAct">
      <button id="rdRead" class="bt" type="button">lire</button>
      <button id="rdCopy" class="bt" type="button">copier</button>
      <button id="rdDel" class="bt" type="button">supprimer</button>
    </div>
    <div class="rda" id="rdConf" hidden>
      <span>Supprimer ce fichier ?</span>
      <button id="rdYes" class="bt" type="button">oui</button>
      <button id="rdNo" class="bt" type="button">non</button>
    </div>
    <div id="rdPv"></div>
  </div>
</div>

<div id="sheet" role="dialog" aria-label="Déposer">
  <div class="bar">
    <button id="sCancel" class="bt" type="button">annuler</button>
    <span>Nouveau dépôt</span>
    <button id="sGo" class="bt" type="button">déposer</button>
  </div>
  <div class="body"><div class="col">
    <div class="seg">
      <button id="tabF" class="bt on" type="button">fichier</button>
      <button id="tabT" class="bt" type="button">texte</button>
    </div>

    <section id="pF" class="col">
      <div id="wvnote" hidden>
        <strong>! pas d’envoi de fichier ici</strong>
        <span>Cette fenêtre de connexion ne peut pas ouvrir tes fichiers. Pour déposer un PDF :</span>
        <ol>
          <li>active le mode avion, puis rallume seulement le Wi-Fi (ou coupe les données mobiles) ;</li>
          <li>ouvre <b id="nAddr"></b> dans Chrome (Safari sur iPhone) ;</li>
          <li>dépose ton fichier.</li>
        </ol>
        <span class="sm">Autre essai, sans couper les données : menu ⋮ de cette fenêtre, « Utiliser ce réseau tel quel », puis Chrome.</span>
        <span class="sm">Écrire ou coller un texte marche très bien ici : onglet TEXTE.</span>
        <button id="nCopy" class="bt" type="button">copier l’adresse</button>
      </div>
      <label id="pick" for="file"><b>&gt; choisir des fichiers</b><span class="s" id="pickHint"></span></label>
      <input id="file" type="file" multiple accept=".pdf,.txt,.md,.csv,.log,.json,application/pdf,text/plain,text/markdown,text/csv,application/json">
      <div id="chosen"></div>
      <p class="hint">PDF et textes (.txt .md .csv .log .json). <span id="lim"></span></p>
    </section>

    <section id="pT" class="col" hidden>
      <div class="field">
        <label for="tName">Nom du fichier</label>
        <input id="tName" type="text" maxlength="60" placeholder="ex. recette" autocomplete="off" autocapitalize="off" spellcheck="false">
      </div>
      <div class="field">
        <div class="lab"><label for="tText">Texte</label><button id="fPaste" class="bt" type="button">coller</button></div>
        <textarea id="tText" maxlength="60000" placeholder="écris ou colle ton texte ici…"></textarea>
        <div id="tCount">0 caractère</div>
      </div>
      <p class="hint">Enregistré en .txt, sauf si tu tapes toi-même .md, .csv, .log ou .json à la fin du nom.</p>
    </section>

    <div class="field">
      <label for="sig">Signature (facultatif)</label>
      <input id="sig" type="text" maxlength="24" placeholder="ton pseudo" autocomplete="off">
    </div>
    <div id="sErr"></div>
  </div></div>
</div>

<div id="login">
  <form id="lForm" autocomplete="off">
    <h3>Gardien du répertoire</h3>
    <input id="lUser" placeholder="pseudo" autocapitalize="off" autocorrect="off" spellcheck="false">
    <input id="lPass" placeholder="mot de passe" type="password" inputmode="numeric">
    <div id="lErr"></div>
    <div class="lrow">
      <button id="lCancel" class="bt" type="button">annuler</button>
      <button class="bt go" type="submit">entrer</button>
    </div>
  </form>
</div>

<div id="gbar">
  <span id="gInfo">Aucun fichier choisi</span>
  <span class="gacts" id="gActs"></span>
</div>

<div id="drop"><div>Dépose tes fichiers ici</div></div>
<div id="toast"></div>

<script>
const $ = id => document.getElementById(id);
const listEl = $('list'), countEl = $('count'), toast = $('toast'), tray = $('tray');
const sheet = $('sheet'), sErr = $('sErr'), sig = $('sig'), tName = $('tName'), tText = $('tText');
const mast = document.querySelector('.mast');
const EXT_RE = /\.(pdf|txt|md|csv|log|json)$/i;
const PV_MAX = 20000;
const MONTHS = ['janv.','févr.','mars','avr.','mai','juin','juil.','août','sept.','oct.','nov.','déc.'];
const SORTS = ['récents','nom','taille'];
// Fenêtre de connexion automatique (WebView Android, navigateur intégré iOS) : pas de sélecteur de fichiers
const UA = navigator.userAgent;
const EMBED = /; wv\)/.test(UA) || (/iPhone|iPad|iPod/.test(UA) && !/Safari\//.test(UA));

// ---------- Mémoire du téléphone ----------
function load(k){ try { return localStorage.getItem(k) || ''; } catch(e){ return ''; } }
function save(k,v){ try { localStorage.setItem(k, v); } catch(e){} }
function sload(k){ try { return sessionStorage.getItem(k) || ''; } catch(e){ return ''; } }
function ssave(k,v){ try { sessionStorage.setItem(k, v); } catch(e){} }
function cookie(k){ const m = document.cookie.match(new RegExp('(?:^|; )' + k + '=([0-9a-f]{16})')); return m ? m[1] : ''; }

// Jeton d'auteur : il prouve qu'un fichier est le tien (gardé dans le téléphone)
function newTok(){
  const a = new Uint8Array(8);
  try { crypto.getRandomValues(a); } catch(e){ for (let i = 0; i < 8; i++) a[i] = Math.random() * 256 | 0; }
  return Array.from(a, b => b.toString(16).padStart(2,'0')).join('');
}
let tok = load('lib_tok');
if (!/^[0-9a-f]{16}$/.test(tok)) tok = cookie('lt');
if (!/^[0-9a-f]{16}$/.test(tok)) tok = newTok();
save('lib_tok', tok);
document.cookie = 'lt=' + tok + '; max-age=31536000; path=/; SameSite=Lax';

function post(url, params){
  const body = Object.keys(params).map(k => k + '=' + encodeURIComponent(params[k])).join('&');
  return fetch(url, { method:'POST', headers:{ 'Content-Type':'application/x-www-form-urlencoded' }, body });
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

let tmo = null;
function say(t){
  toast.textContent = t;
  toast.classList.add('on');
  clearTimeout(tmo);
  tmo = setTimeout(() => toast.classList.remove('on'), 2200);
}

// La fenêtre de connexion Android recharge la page si on tire vers le bas tout en haut.
// On garde donc la page décalée d'1 pixel : elle ne se croit jamais « tout en haut ».
let liftT = null;
function lift(){ if (window.scrollY < 1) window.scrollTo(0, 1); }
window.addEventListener('scroll', () => { clearTimeout(liftT); liftT = setTimeout(lift, 150); }, { passive: true });

// ---------- Mise en forme ----------
function fmtSize(n){
  if (n < 1024) return n + ' o';
  if (n < 10240) return (n / 1024).toFixed(1).replace('.', ',') + ' Ko';
  if (n < 1048576) return Math.round(n / 1024) + ' Ko';
  return (n / 1048576).toFixed(1).replace('.', ',') + ' Mo';
}
function fmtDate(t){
  if (!t) return '';
  const d = new Date(t * 1000), now = new Date();
  const s = d.getDate() + ' ' + MONTHS[d.getMonth()];
  return d.getFullYear() === now.getFullYear() ? s : s + ' ' + d.getFullYear();
}
function fold(s){ return s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, ''); }
function extOf(n){ const m = /\.([^.]+)$/.exec(n); return m ? m[1].toUpperCase().slice(0, 4) : 'TXT'; }

// Nom de fichier propre : sans caractères interdits, pas trop long
function fileName(s){
  s = s.replace(/[\\/:*?"<>|\u0000-\u001f]+/g, '_').replace(/\s+/g, ' ').trim();
  const m = /(\.[A-Za-z0-9]{1,5})$/.exec(s);
  let ext = m ? m[1] : '', stem = m ? s.slice(0, -ext.length) : s;
  if (stem.length > 60) stem = stem.slice(0, 60).trim();
  return stem + ext;
}

// ---------- Garder sa place pendant que la liste change ----------
function anchor(){
  const lim = mast.offsetHeight;
  for (const el of listEl.children) {
    if (el.getBoundingClientRect().bottom > lim) return el;
  }
  return null;
}
function keepView(fn){
  if (window.scrollY <= 2) { fn(); return; }
  const a = anchor();
  const before = a ? a.getBoundingClientRect().top : 0;
  fn();
  if (a && a.isConnected) {
    const diff = a.getBoundingClientRect().top - before;
    if (Math.abs(diff) > 0.5) window.scrollBy(0, diff);
  }
}

// ---------- Fichiers ----------
const items = new Map();            // id -> { id, rev, mine, kind, size, created, nick, name, el, txt }
const picked = new Set();
let gen = null, busy = false, again = false, goTop = false, lastSig = '';
let isFull = false, roomB = 0, capB = 0, maxFile = 524288;
let fKind = '', sortMode = 0, qStr = '';
let guard = null;
try { const g = JSON.parse(sload('lib_guard') || 'null'); if (g && g.u && g.p) guard = g; } catch(e){}

function btn(label, fn, cls){
  const b = document.createElement('button');
  b.type = 'button'; b.className = 'bt' + (cls ? ' ' + cls : ''); b.textContent = label;
  b.addEventListener('click', fn);
  return b;
}

function span(cls, txt){
  const s = document.createElement('span');
  s.className = cls; s.textContent = txt;
  return s;
}

function makeRow(o){
  const b = document.createElement('button');
  b.type = 'button'; b.className = 'f';
  const mt = span('mt', [fmtSize(o.size), fmtDate(o.created), 'par ' + o.nick].filter(Boolean).join(' · '));
  if (o.mine) { const m = document.createElement('b'); m.textContent = ' · à toi'; mt.append(m); }
  b.append(span('ty', extOf(o.name)), span('nm', o.name), mt);
  b.classList.toggle('sel', picked.has(o.id));
  b.addEventListener('click', () => { if (guard) togglePick(o); else openReader(o); });
  o.el = b;
}

// ---------- Fiche d'un fichier ----------
let cur = null;

async function readText(o){
  if (o.txt !== null) return o.txt;
  const r = await fetch('/f?id=' + o.id + '&r=' + o.rev, { cache: 'no-store' });
  if (!r.ok) throw r.status;
  o.txt = await r.text();
  return o.txt;
}

function openReader(o){
  cur = o;
  $('rdName').textContent = o.name;
  $('rdMeta').textContent = [fmtSize(o.size), fmtDate(o.created), 'par ' + o.nick].filter(Boolean).join(' · ');
  const dl = $('rdDl');
  dl.href = '/f?id=' + o.id + '&d=1&r=' + o.rev;
  dl.setAttribute('download', o.name);
  $('rdRead').hidden = $('rdCopy').hidden = o.kind !== 'T';
  $('rdDel').hidden = !o.mine;
  $('rdAct').hidden = o.kind !== 'T' && !o.mine;
  $('rdConf').hidden = true;
  $('rdRead').textContent = 'lire';
  $('rdPv').style.display = 'none'; $('rdPv').textContent = '';
  $('rd').style.display = 'flex';
}
function closeReader(){ $('rd').style.display = 'none'; cur = null; }

$('rdClose').addEventListener('click', closeReader);
$('rd').addEventListener('click', e => { if (e.target === $('rd')) closeReader(); });

$('rdRead').addEventListener('click', async () => {
  const o = cur, pv = $('rdPv');
  if (!o) return;
  if (pv.style.display === 'block') { pv.style.display = 'none'; $('rdRead').textContent = 'lire'; return; }
  pv.style.display = 'block'; pv.textContent = 'chargement…'; $('rdRead').textContent = 'masquer';
  try {
    const t = await readText(o);
    if (cur !== o) return;
    pv.textContent = !t ? '(fichier vide)' : t.length > PV_MAX ? t.slice(0, PV_MAX) + '\n\n… (la suite est dans le fichier téléchargé)' : t;
  } catch(e) {
    if (cur === o) pv.textContent = 'Lecture impossible, réessaie.';
  }
});

// Copier (marche aussi sans HTTPS)
function copyText(s){
  const ta = document.createElement('textarea');
  ta.value = s;
  ta.setAttribute('readonly', '');
  ta.style.position = 'fixed'; ta.style.top = '0'; ta.style.left = '0'; ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.focus({ preventScroll: true }); ta.select();
  try { ta.setSelectionRange(0, s.length); } catch(e){}
  let ok = false;
  try { ok = document.execCommand('copy'); } catch(e){}
  ta.remove();
  return ok;
}
async function copyString(s, okMsg){
  let ok = false;
  if (navigator.clipboard && window.isSecureContext) {
    try { await navigator.clipboard.writeText(s); ok = true; } catch(e){}
  }
  if (!ok) ok = copyText(s);
  say(ok ? okMsg : 'Copie impossible : sélectionne le texte à la main');
}
$('rdCopy').addEventListener('click', async () => {
  const o = cur;
  if (!o) return;
  try { await copyString(await readText(o), 'Texte copié'); }
  catch(e) { say('Lecture impossible, réessaie.'); }
});
const copyAddr = () => copyString('http://' + location.host + '/', 'Adresse copiée');
$('cpAddr').addEventListener('click', copyAddr);
$('nCopy').addEventListener('click', copyAddr);
$('addr').textContent = 'http://' + location.host;
$('nAddr').textContent = 'http://' + location.host;

$('rdDel').addEventListener('click', () => { $('rdAct').hidden = true; $('rdConf').hidden = false; });
$('rdNo').addEventListener('click', () => { $('rdConf').hidden = true; $('rdAct').hidden = false; });
$('rdYes').addEventListener('click', async () => { if (cur && await del(cur)) closeReader(); });

async function del(o){
  try {
    const r = await post('/del', { tok, id: o.id });
    if (r.ok) { say('Fichier supprimé'); poll(); return true; }
    say(r.status === 403 ? 'Seul l’auteur peut supprimer ce fichier.' : 'Échec, réessaie.');
  } catch(e) { say('Connexion perdue, réessaie.'); }
  return false;
}

// ---------- Liste : filtres, tri, affichage ----------
function visible(){
  const q = fold(qStr.trim());
  const a = [...items.values()].filter(o => (!fKind || o.kind === fKind) && (!q || fold(o.name + ' ' + o.nick).includes(q)));
  if (sortMode === 1) a.sort((x, y) => x.name.localeCompare(y.name, 'fr', { numeric: true, sensitivity: 'base' }));
  else if (sortMode === 2) a.sort((x, y) => y.size - x.size || y.id - x.id);
  else a.sort((x, y) => y.id - x.id);
  return a;
}

function layout(){
  const a = visible();
  if (a.length) listEl.replaceChildren(...a.map(o => o.el));
  else {
    const e = document.createElement('div');
    e.id = 'empty';
    e.innerHTML = items.size ? 'Aucun fichier ne correspond.' : 'Le répertoire est vide.<br>Dépose le premier fichier.';
    listEl.replaceChildren(e);
  }
  const n = items.size;
  let t = n === 0 ? 'aucun fichier' : n === 1 ? '1 fichier' : n + ' fichiers';
  if (n && a.length !== n) t = a.length + ' sur ' + t;
  countEl.textContent = t;
}

const GAUGE = 12;
function renderSpace(){
  const gauge = $('gauge');
  if (!gauge.children.length) for (let i = 0; i < GAUGE; i++) gauge.appendChild(document.createElement('i'));
  const used = Math.max(0, capB - roomB);
  const frac = isFull ? 1 : capB ? Math.min(1, used / capB) : 0;
  const on = Math.round(frac * GAUGE);
  [...gauge.children].forEach((s, i) => s.classList.toggle('on', i < on));
  gauge.classList.toggle('hot', frac > 0.85 || isFull);
  gauge.title = fmtSize(used) + ' utilisés sur ' + fmtSize(capB);
  $('space').textContent = isFull ? 'répertoire plein' : fmtSize(roomB) + ' libres';
  $('full').style.display = isFull ? 'block' : 'none';
  $('lim').textContent = fmtSize(maxFile) + ' maximum par fichier.';
}

document.querySelectorAll('.chip[data-k]').forEach(b => b.addEventListener('click', () => {
  fKind = b.dataset.k;
  document.querySelectorAll('.chip[data-k]').forEach(x => x.classList.toggle('on', x === b));
  layout();
}));
$('sort').addEventListener('click', () => {
  sortMode = (sortMode + 1) % SORTS.length;
  $('sort').textContent = 'tri:' + SORTS[sortMode];
  layout();
});
$('q').addEventListener('input', e => { qStr = e.target.value; layout(); });

// Format : "H \t gen \t nombre \t max \t plein(0/1) \t place libre \t taille max \t capacité"
//          puis "id \t version \t mien(0/1) \t type(P/T) \t taille \t date \t signature \t nom"
function sync(body){
  const lines = body.split('\n');
  const h = lines[0].split('\t');
  if (h[0] !== 'H') return;

  if (gen !== null && h[1] !== gen) { items.clear(); picked.clear(); lastSig = ''; }
  gen = h[1];
  isFull = h[4] === '1'; roomB = +h[5]; maxFile = +h[6] || maxFile; capB = +h[7] || capB;

  const seen = new Set();
  for (let i = 1; i < lines.length; i++) {
    if (!lines[i]) continue;
    const p = lines[i].split('\t');
    if (p.length < 8) continue;
    const id = +p[0], rev = +p[1], mine = p[2] === '1';
    seen.add(id);
    const old = items.get(id);
    if (old && old.rev === rev && old.mine === mine) continue;
    const o = { id, rev, mine, kind: p[3], size: +p[4], created: +p[5], nick: p[6], name: p.slice(7).join('\t'), txt: null };
    items.set(id, o);
    makeRow(o);
    lastSig = '';
  }
  items.forEach((o, id) => { if (!seen.has(id)) { items.delete(id); picked.delete(id); lastSig = ''; if (cur === o) closeReader(); } });

  const sg = [...items.values()].map(o => o.id + ':' + o.rev + ':' + (o.mine ? 1 : 0)).join(',');
  if (sg !== lastSig) { lastSig = sg; keepView(layout); }
  if (goTop) { goTop = false; if (sortMode === 0 && !fKind && !qStr) window.scrollTo(0, 1); }
  renderSpace();
  renderGuard();
}

async function poll(){
  if (busy) { again = true; return; }
  busy = true;
  try {
    const r = await fetch('/list?tok=' + tok, { cache: 'no-store' });
    if (!r.ok) throw 0;
    sync(await r.text());
  } catch(e) {
    countEl.textContent = 'hors ligne…';
  }
  busy = false;
  if (again) { again = false; poll(); }
}

// ---------- Mode gardien : on touche les fichiers à effacer ----------
function refreshActs(){
  items.forEach(o => o.el.classList.toggle('sel', picked.has(o.id)));
  renderGuard();
}
function togglePick(o){
  if (picked.has(o.id)) picked.delete(o.id); else picked.add(o.id);
  refreshActs();
}
function renderGuard(){
  document.body.classList.toggle('guard', !!guard);
  const bar = $('gbar'), acts = $('gActs');
  bar.style.display = guard ? 'flex' : 'none';
  $('gOpen').textContent = guard ? 'quitter le mode gardien' : 'gardien';
  if (!guard) return;
  const n = picked.size;
  $('gInfo').textContent = n === 0 ? 'Touche les fichiers à effacer' : n === 1 ? '1 fichier choisi' : n + ' fichiers choisis';
  if (!n) { acts.replaceChildren(btn('quitter', leaveGuard)); return; }
  acts.replaceChildren(
    btn('effacer', () => {
      $('gInfo').textContent = 'Effacer ' + (n === 1 ? 'ce fichier' : 'ces ' + n + ' fichiers') + ' ?';
      acts.replaceChildren(btn('oui', guardDelete), btn('non', renderGuard));
    }),
    btn('annuler', () => { picked.clear(); refreshActs(); })
  );
}
async function guardDelete(){
  try {
    const r = await post('/mdel', { u: guard.u, p: guard.p, ids: [...picked].join(',') });
    if (r.ok) {
      const n = picked.size;
      picked.clear();
      say(n === 1 ? 'Fichier effacé' : n + ' fichiers effacés');
      poll();
    } else if (r.status === 403) { say('Identifiants refusés'); leaveGuard(); }
    else if (r.status === 429) say('Trop d’essais, patiente une minute');
    else say('Échec, réessaie.');
  } catch(e) { say('Connexion perdue, réessaie.'); }
  refreshActs();
}
function leaveGuard(){
  guard = null; picked.clear(); ssave('lib_guard', '');
  refreshActs();
}
$('gOpen').addEventListener('click', () => {
  if (guard) { leaveGuard(); return; }
  $('lErr').textContent = ''; $('lUser').value = ''; $('lPass').value = '';
  $('login').style.display = 'flex';
});
$('lCancel').addEventListener('click', () => { $('login').style.display = 'none'; });
$('lForm').addEventListener('submit', async e => {
  e.preventDefault();
  const u = $('lUser').value.trim(), p = $('lPass').value;
  if (!u || !p) return;
  try {
    const r = await post('/mlogin', { u, p });
    if (r.ok) {
      guard = { u, p }; ssave('lib_guard', JSON.stringify(guard));
      $('login').style.display = 'none';
      refreshActs();
      say('Mode gardien');
      return;
    }
    $('lErr').textContent = r.status === 429 ? 'Trop d’essais, patiente une minute.' : 'Pseudo ou mot de passe incorrect.';
  } catch(err) { $('lErr').textContent = 'Connexion perdue, réessaie.'; }
});

// ---------- Dépôts : file d'attente, un fichier à la fois ----------
const jobs = [];
let upBusy = false;

function errText(st){
  return st === 413 ? 'trop lourd' : st === 415 ? 'format refusé' : st === 429 ? 'trop vite' :
         st === 507 ? 'plus de place' : st === 400 ? 'envoi invalide' : st === 500 ? 'erreur de mémoire' :
         st === 0 ? 'connexion perdue' : 'erreur ' + st;
}

function drawJob(j){
  if (!j.el) {
    j.el = document.createElement('div');
    j.el.innerHTML = '<span class="jn"></span><span class="js"></span>';
    j.el.addEventListener('click', () => { if (j.state === 'err' || j.state === 'ok') dropJob(j); });
    tray.appendChild(j.el);
  }
  const pct = Math.round(j.pct * 100), n = Math.round(j.pct * 10);
  j.el.className = 'job ' + j.state;
  j.el.querySelector('.jn').textContent = j.name;
  j.el.querySelector('.js').textContent = j.state === 'wait' ? 'en attente' : j.state === 'up' ? '[' + '#'.repeat(n) + '-'.repeat(10 - n) + '] ' + pct + ' %' :
                                          j.state === 'ok' ? 'ok' : j.msg;
}
function dropJob(j){
  const i = jobs.indexOf(j);
  if (i >= 0) jobs.splice(i, 1);
  if (j.el) j.el.remove();
}

function sendOne(j){
  return new Promise(res => {
    const xhr = new XMLHttpRequest();
    const q = 'tok=' + tok + '&nm=' + encodeURIComponent(j.name) + '&n=' + encodeURIComponent(j.nick) + '&t=' + Math.floor(Date.now() / 1000);
    xhr.open('POST', '/up?' + q);
    xhr.timeout = 180000;
    xhr.upload.onprogress = e => { if (e.lengthComputable) { j.pct = e.loaded / e.total; drawJob(j); } };
    xhr.onload = () => res(xhr.status);
    xhr.onerror = () => res(0);
    xhr.ontimeout = () => res(0);
    const fd = new FormData();
    fd.append('f', j.blob, 'f.bin');       // le vrai nom voyage dans l'adresse (accents compris)
    xhr.send(fd);
  });
}

async function runUp(){
  if (upBusy) return;
  upBusy = true;
  for (;;) {
    const j = jobs.find(x => x.state === 'wait');
    if (!j) break;
    j.state = 'up'; j.pct = 0; drawJob(j);
    const st = await sendOne(j);
    if (st === 200) { j.state = 'ok'; j.pct = 1; goTop = true; poll(); }
    else if (st === 429 && j.tries++ < 4) { j.state = 'wait'; drawJob(j); await sleep(1600); continue; }
    else { j.state = 'err'; j.msg = errText(st); }
    drawJob(j);
    setTimeout(() => dropJob(j), j.state === 'ok' ? 2500 : 9000);
    if (jobs.some(x => x.state === 'wait')) await sleep(1200);   // le serveur espace les dépôts
  }
  upBusy = false;
}

function enqueue(list){
  list.forEach(j => { j.state = 'wait'; j.pct = 0; j.tries = 0; jobs.push(j); drawJob(j); });
  runUp();
}

window.addEventListener('beforeunload', e => {
  if (jobs.some(j => j.state === 'wait' || j.state === 'up')) { e.preventDefault(); e.returnValue = ''; }
});

// ---------- Feuille de dépôt ----------
let tab = 'F', chosen = [], pickT = null, pickAt = 0;

function setTab(t){
  tab = t;
  $('tabF').classList.toggle('on', t === 'F');
  $('tabT').classList.toggle('on', t === 'T');
  $('pF').hidden = t !== 'F';
  $('pT').hidden = t !== 'T';
  sErr.textContent = '';
  ssave('lib_ed', t);
}

function renderChosen(){
  $('chosen').replaceChildren(...chosen.map((c, i) => {
    const row = document.createElement('div');
    row.className = 'ch' + (c.err ? ' bad' : '');
    const x = btn('x', () => { chosen.splice(i, 1); renderChosen(); });
    x.setAttribute('aria-label', 'Retirer');
    row.append(span('cn', c.name), span('cs', c.err || fmtSize(c.size)), x);
    return row;
  }));
}

function addChosen(files){
  for (const f of files) {
    const name = fileName(f.name || 'fichier');
    let err = '';
    if (!EXT_RE.test(name)) err = 'format refusé';
    else if (f.size === 0) err = 'vide';
    else if (f.size > maxFile) err = 'trop lourd';
    chosen.push({ file: f, name, size: f.size, err });
  }
  sErr.textContent = '';
  renderChosen();
}

function getDraft(){ try { return JSON.parse(sload('lib_draft') || '{}') || {}; } catch(e){ return {}; } }
function saveDraft(){ ssave('lib_draft', JSON.stringify({ n: tName.value, x: tText.value, s: sig.value })); }
function updCount(){
  const n = tText.value.length;
  $('tCount').textContent = n + (n > 1 ? ' caractères' : ' caractère');
}

function openSheet(t, files){
  if (isFull) { say('Le répertoire est plein'); window.scrollTo(0, 1); return; }
  const d = getDraft();
  tName.value = d.n || ''; tText.value = d.x || '';
  sig.value = (d.n || d.x) && typeof d.s === 'string' ? d.s : load('lib_nick');
  updCount();
  $('wvnote').hidden = !EMBED;
  setTab(t || (EMBED ? 'T' : 'F'));
  if (files) addChosen(files);
  sheet.style.display = 'flex';
}
function closeSheet(clear){
  sheet.style.display = 'none';
  chosen = []; renderChosen();
  clearTimeout(pickT);
  ssave('lib_ed', '');
  if (clear) { ssave('lib_draft', ''); tName.value = ''; tText.value = ''; updCount(); }
}

$('add').addEventListener('click', () => openSheet());
$('sCancel').addEventListener('click', () => closeSheet(tab === 'T'));   // « annuler » abandonne le texte en cours
$('tabF').addEventListener('click', () => setTab('F'));
$('tabT').addEventListener('click', () => setTab('T'));
$('file').addEventListener('change', e => { clearTimeout(pickT); addChosen([...e.target.files]); e.target.value = ''; });
// Dans la fenêtre de connexion, le sélecteur est refusé aussitôt (annulation immédiate) : on le dit.
// Une annulation tardive est celle de la personne qui renonce : rien à signaler.
$('file').addEventListener('cancel', () => {
  clearTimeout(pickT);
  if (Date.now() - pickAt < 700) $('wvnote').hidden = false;
});
tText.addEventListener('input', () => { updCount(); saveDraft(); });
tName.addEventListener('input', saveDraft);
sig.addEventListener('input', saveDraft);
$('pickHint').textContent = matchMedia('(pointer:fine)').matches ? 'ou les glisser ici' : 'depuis la mémoire du téléphone';

// Si on touche « choisir » et que rien ne s'ouvre ni ne s'annule, on explique pourquoi
$('pick').addEventListener('click', () => {
  clearTimeout(pickT);
  pickAt = Date.now();
  pickT = setTimeout(() => { $('wvnote').hidden = false; }, 1500);
});
window.addEventListener('blur', () => clearTimeout(pickT));
document.addEventListener('visibilitychange', () => clearTimeout(pickT));

// Coller : direct quand le navigateur l'autorise, sinon on guide vers l'appui long / Ctrl+V
$('fPaste').addEventListener('click', async () => {
  let txt = null;
  if (navigator.clipboard && navigator.clipboard.readText && window.isSecureContext) {
    try { txt = await navigator.clipboard.readText(); } catch(e){ txt = null; }
  }
  if (txt) {
    const s = tText.selectionStart ?? tText.value.length, e2 = tText.selectionEnd ?? s;
    tText.value = (tText.value.slice(0, s) + txt + tText.value.slice(e2)).slice(0, 60000);
    updCount(); saveDraft();
    return;
  }
  tText.focus();
  say('Appui long dans le texte puis « Coller » (ou Ctrl+V)');
});

$('sGo').addEventListener('click', () => {
  const nick = sig.value.trim();
  save('lib_nick', nick);
  sErr.textContent = '';
  if (tab === 'F') {
    const ok = chosen.filter(c => !c.err);
    if (!ok.length) { sErr.textContent = chosen.length ? 'Aucun fichier valide dans ta sélection.' : 'Choisis au moins un fichier.'; return; }
    const total = ok.reduce((s, c) => s + c.size, 0);
    if (total > roomB) { sErr.textContent = 'Pas assez de place : il reste ' + fmtSize(roomB) + '.'; return; }
    enqueue(ok.map(c => ({ blob: c.file, name: c.name, size: c.size, nick })));
    closeSheet(false);
    return;
  }
  let name = fileName(tName.value);
  const text = tText.value;
  if (!name) { sErr.textContent = 'Il manque le nom du fichier.'; tName.focus(); return; }
  if (!text.trim()) { sErr.textContent = 'Il manque le texte.'; tText.focus(); return; }
  if (!EXT_RE.test(name) || /\.pdf$/i.test(name)) name = name.replace(/\.pdf$/i, '') + '.txt';
  const blob = new Blob([text], { type: 'text/plain' });
  if (blob.size > maxFile) { sErr.textContent = 'Texte trop long (' + fmtSize(maxFile) + ' maximum).'; return; }
  if (blob.size > roomB) { sErr.textContent = 'Pas assez de place : il reste ' + fmtSize(roomB) + '.'; return; }
  enqueue([{ blob, name, size: blob.size, nick }]);
  closeSheet(true);
});

// Ctrl+Entrée (ou Cmd+Entrée) dépose le texte, depuis le clavier d'un ordinateur
tText.addEventListener('keydown', e => { if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); $('sGo').click(); } });

// Glisser-déposer des fichiers sur la page (ordinateur)
let dragN = 0;
const hasFiles = e => e.dataTransfer && [...(e.dataTransfer.types || [])].includes('Files');
window.addEventListener('dragenter', e => { if (hasFiles(e)) { e.preventDefault(); dragN++; document.body.classList.add('dragging'); } });
window.addEventListener('dragover', e => { if (hasFiles(e)) e.preventDefault(); });
window.addEventListener('dragleave', e => { if (hasFiles(e) && --dragN <= 0) { dragN = 0; document.body.classList.remove('dragging'); } });
window.addEventListener('drop', e => {
  if (!hasFiles(e)) return;
  e.preventDefault();
  dragN = 0; document.body.classList.remove('dragging');
  const fs = [...e.dataTransfer.files];
  if (!fs.length) return;
  closeReader();
  if (sheet.style.display === 'flex' && tab === 'F') addChosen(fs);
  else { if (sheet.style.display === 'flex') closeSheet(false); openSheet('F', fs); }
});

// ---------- Démarrage ----------
if ('scrollRestoration' in history) history.scrollRestoration = 'manual';
document.addEventListener('visibilitychange', () => { if (!document.hidden) poll(); });
function tick(){ if (!document.hidden) poll(); setTimeout(tick, 10000); }
renderGuard();
renderSpace();
layout();
lift();
poll().then(() => { if (sload('lib_ed') === 'T') openSheet('T'); });
setTimeout(tick, 10000);
</script>
</body>
</html>
)rawliteral";

// ---------- Fonctions pures (sans matériel : vérifiées aussi sur ordinateur) ----------

// Nettoie un nom de fichier envoyé par le téléphone : renvoie 'P' (PDF), 'T' (texte)
// ou 0 si l'extension n'est pas acceptée. Le résultat tient dans FNAME_MAX octets,
// extension comprise, sans casser un caractère UTF-8.
char cleanName(const char* in, char* out) {
  const char* base = in;
  for (const char* p = in; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;

  const char* dot = strrchr(base, '.');
  if (!dot) return 0;
  size_t el = strlen(dot);                         // point compris
  if (el < 2 || el > 6) return 0;

  char ext[8];
  for (size_t i = 0; i < el; i++) ext[i] = (char)tolower((uint8_t)dot[i]);
  ext[el] = 0;

  char kind = 0;
  if (!strcmp(ext, ".pdf")) kind = 'P';
  else if (!strcmp(ext, ".txt") || !strcmp(ext, ".md") || !strcmp(ext, ".csv") ||
           !strcmp(ext, ".log") || !strcmp(ext, ".json")) kind = 'T';
  else return 0;

  size_t maxStem = FNAME_MAX - el;
  size_t n = 0;
  const char* p = base;
  for (; p < dot && n < maxStem; p++) {
    uint8_t c = (uint8_t)*p;
    if (c < 32 || c == 127) continue;
    if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    if (c == ' ' && (n == 0 || out[n - 1] == ' ')) continue;
    out[n++] = (char)c;
  }
  if (p < dot && ((uint8_t)*p & 0xC0) == 0x80) {   // coupé au milieu d'un caractère
    while (n > 0 && ((uint8_t)out[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0) n--;
  }
  while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.')) n--;
  if (n == 0) { memcpy(out, "fichier", 7); n = 7; }

  memcpy(out + n, ext, el + 1);
  return kind;
}

// Deux écritures du nom pour l'en-tête Content-Disposition : ASCII de secours (ascii,
// FNAME_MAX + 1 octets) et UTF-8 encodé en pourcentage (pct, 3 * FNAME_MAX + 1 octets).
void dispNames(const char* name, char* ascii, char* pct) {
  static const char U[] = "0123456789ABCDEF";
  size_t a = 0, p = 0;
  for (const uint8_t* s = (const uint8_t*)name; *s; s++) {
    uint8_t c = *s;
    ascii[a++] = (c < 32 || c > 126 || c == '"' || c == '\\' || c == ';' || c == '%') ? '_' : (char)c;
    bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 c == '!' || c == '#' || c == '$' || c == '&' || c == '+' || c == '-' ||
                 c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
    if (plain) pct[p++] = (char)c;
    else { pct[p++] = '%'; pct[p++] = U[c >> 4]; pct[p++] = U[c & 15]; }
  }
  ascii[a] = 0;
  pct[p] = 0;
}

// ---------- Outils ----------

// Copie nettoyée d'une ligne (pseudo) : supprime tabulations/retours ligne, coupe sans casser un caractère UTF-8
void cleanLine(const String& in, char* out, size_t maxBytes) {
  const char* s = in.c_str();
  size_t len = in.length();
  size_t i = 0, n = 0;

  while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;

  for (; i < len && n < maxBytes; i++) {
    char c = s[i];
    if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    else if ((uint8_t)c < 32) continue;
    out[n++] = c;
  }

  if (i < len && ((uint8_t)s[i] & 0xC0) == 0x80) {
    while (n > 0 && ((uint8_t)out[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0) n--;
  }

  while (n > 0 && out[n - 1] == ' ') n--;
  out[n] = 0;
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool validTok(const String& t) {
  if (t.length() != 16) return false;
  for (size_t i = 0; i < 16; i++) if (hexVal(t[i]) < 0) return false;
  return true;
}

uint32_t tokHash(const char* s) {
  uint32_t h = 2166136261UL;       // FNV-1a
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619UL; }
  return h ? h : 1;
}

bool allowSender(uint32_t ip, uint32_t now) {
  int oldest = 0;
  for (int i = 0; i < SENDER_SLOTS; i++) {
    if (senders[i].ip == ip) {
      if (now - senders[i].t < POST_COOLDOWN) return false;
      senders[i].t = now;
      return true;
    }
    if (senders[i].t < senders[oldest].t) oldest = i;
  }
  senders[oldest].ip = ip;
  senders[oldest].t = now;
  return true;
}

bool isPost() {
  if (server.method() == HTTP_POST) return true;
  server.send(405, "text/plain", "POST uniquement");
  return false;
}

uint32_t clientTime() {
  uint32_t t = strtoul(server.arg("t").c_str(), nullptr, 10);
  return (t < 1600000000UL || t > 4000000000UL) ? 0 : t;
}

// ---------- Stockage ----------
// Deux fichiers par dépôt : /l/<id>.f (le contenu tel quel) et /l/<id>.m (la fiche) :
//   ligne 1 : jeton de l'auteur
//   ligne 2 : création \t version \t taille \t type
//   ligne 3 : signature
//   ligne 4 : nom du fichier
// Le contenu est écrit d'abord dans /l/tmp, puis renommé : un dépôt coupé en route n'apparaît jamais.

String filePath(uint32_t id) { return "/l/" + String(id) + ".f"; }
String metaPath(uint32_t id) { return "/l/" + String(id) + ".m"; }

uint32_t freeAt = 0;
size_t   freeCache = 0;
size_t freeBytes() {                // le calcul parcourt toute la flash : on le garde 3 secondes
  uint32_t now = millis();
  if (freeAt == 0 || now - freeAt > 3000) {
    size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
    freeCache = used < total ? total - used : 0;
    freeAt = now ? now : 1;
  }
  return freeCache;
}

size_t capBytes() {                 // capacité totale utilisable (hors marge), pour la jauge de la page
  size_t t = LittleFS.totalBytes();
  return t > FREE_MARGIN ? t - FREE_MARGIN : 0;
}

size_t roomBytes() {                // place utilisable pour un nouveau fichier
  size_t f = freeBytes();
  return f > FREE_MARGIN ? f - FREE_MARGIN : 0;
}

bool libFull() {
  return ecount >= MAX_ENTRIES || roomBytes() < 4096;
}

int findItem(uint32_t id) {
  for (int i = 0; i < ecount; i++) if (items[i].id == id) return i;
  return -1;
}

void removeAt(int k) {
  LittleFS.remove(filePath(items[k].id));
  LittleFS.remove(metaPath(items[k].id));
  for (int i = k; i < ecount - 1; i++) items[i] = items[i + 1];
  ecount--;
  freeAt = 0;
}

bool writeMeta(uint32_t id, const char* tok, uint32_t created, uint32_t rev, uint32_t size,
               char kind, const char* nick, const char* name) {
  File f = LittleFS.open("/l/tmp.m", "w");
  if (!f) return false;
  char line[48];
  snprintf(line, sizeof(line), "%lu\t%lu\t%lu\t%c\n", (unsigned long)created, (unsigned long)rev, (unsigned long)size, kind);
  size_t want = strlen(tok) + 1 + strlen(line) + strlen(nick) + 1 + strlen(name) + 1;
  size_t done = f.print(tok);  done += f.print('\n');
  done += f.print(line);
  done += f.print(nick);       done += f.print('\n');
  done += f.print(name);       done += f.print('\n');
  f.close();
  if (done != want) { LittleFS.remove("/l/tmp.m"); return false; }
  String p = metaPath(id);
  LittleFS.remove(p);
  return LittleFS.rename("/l/tmp.m", p);
}

// Lit la fiche d'un dépôt : remplit l'élément (et le jeton si demandé). Faux si la fiche est abîmée.
bool readMeta(uint32_t id, Item& it, String* tokOut) {
  File f = LittleFS.open(metaPath(id), "r");
  if (!f) return false;
  String tok  = f.readStringUntil('\n');
  String hd   = f.readStringUntil('\n');
  String nick = f.readStringUntil('\n');
  String name = f.readStringUntil('\n');
  f.close();

  unsigned long created = 0, rev = 1, size = 0;
  char kind = 0;
  if (sscanf(hd.c_str(), "%lu\t%lu\t%lu\t%c", &created, &rev, &size, &kind) != 4) return false;
  if ((kind != 'P' && kind != 'T') || !name.length() || tok.length() != 16) return false;

  it.id = id; it.rev = rev; it.created = created; it.size = size; it.kind = kind;
  it.own = tokHash(tok.c_str());
  strlcpy(it.nick, nick.c_str(), sizeof(it.nick));
  strlcpy(it.name, name.c_str(), sizeof(it.name));
  if (tokOut) *tokOut = tok;
  return true;
}

void loadGen() {
  File f = LittleFS.open("/lgen", "r");
  if (f && f.size() == 8) { f.readBytes(gen, 8); gen[8] = 0; f.close(); return; }
  if (f) f.close();
  snprintf(gen, sizeof(gen), "%08lx", (unsigned long)esp_random());
  f = LittleFS.open("/lgen", "w");
  if (f) { f.print(gen); f.close(); }
}

// Relit l'index au démarrage. Les dépôts incomplets (coupure pendant un envoi) sont supprimés.
void scanFiles() {
  if (!LittleFS.exists("/l")) LittleFS.mkdir("/l");
  LittleFS.remove("/l/tmp");
  LittleFS.remove("/l/tmp.m");

  ecount = 0; lastId = 0;
  static uint32_t mids[MAX_ENTRIES + 8], fids[MAX_ENTRIES + 8];
  int mn = 0, fn = 0;

  File dir = LittleFS.open("/l");
  if (!dir) return;
  File f = dir.openNextFile();
  while (f) {
    String name = f.name();
    f.close();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    char* end = nullptr;
    unsigned long id = strtoul(name.c_str(), &end, 10);
    if (end != name.c_str() && id > 0 && end[0] == '.' && end[1] && end[2] == 0) {
      if (id > lastId) lastId = id;
      if (end[1] == 'm' && mn < MAX_ENTRIES + 8) mids[mn++] = id;
      else if (end[1] == 'f' && fn < MAX_ENTRIES + 8) fids[fn++] = id;
    }
    f = dir.openNextFile();
  }
  dir.close();

  for (int i = 0; i < mn; i++) {
    Item it;
    File g = LittleFS.open(filePath(mids[i]), "r");
    bool ok = g && readMeta(mids[i], it, nullptr);
    if (g) { if (ok && g.size() != it.size) ok = false; g.close(); }
    if (!ok) {
      LittleFS.remove(filePath(mids[i]));
      LittleFS.remove(metaPath(mids[i]));
      continue;
    }
    if (ecount >= MAX_ENTRIES) continue;       // en trop : laissé tel quel sur la flash, sans l'effacer
    int k = ecount++;
    while (k > 0 && items[k - 1].id > it.id) { items[k] = items[k - 1]; k--; }
    items[k] = it;
  }
  for (int i = 0; i < fn; i++) {
    if (!LittleFS.exists(metaPath(fids[i]))) LittleFS.remove(filePath(fids[i]));
  }
}

// ---------- Routes : portail ----------

void handleRedirect() {
  server.sendHeader("Location", portalUrl, true);
  server.sendHeader("Cache-Control", "no-store");
  server.send(302, "text/plain", "");
}

void handlePage() {
  if (server.hostHeader().length() && server.hostHeader() != apHost) { handleRedirect(); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", PAGE);
}

void handleNothing() { server.send(404, "text/plain", ""); }

// ---------- Routes : bibliothèque ----------

// GET /list?tok=
void handleList() {
  String tok = server.arg("tok");
  uint32_t mine = validTok(tok) ? tokHash(tok.c_str()) : 0;

  String out;
  out.reserve(70 + ecount * 140);
  out += "H\t"; out += gen; out += '\t';
  out += ecount; out += '\t';
  out += MAX_ENTRIES; out += '\t';
  out += libFull() ? '1' : '0'; out += '\t';
  out += (unsigned long)roomBytes(); out += '\t';
  out += (unsigned long)MAX_FILE_BYTES; out += '\t';
  out += (unsigned long)capBytes(); out += '\n';
  for (int i = 0; i < ecount; i++) {
    const Item& it = items[i];
    out += (unsigned long)it.id;      out += '\t';
    out += (unsigned long)it.rev;     out += '\t';
    out += (mine && it.own == mine) ? '1' : '0'; out += '\t';
    out += it.kind;                   out += '\t';
    out += (unsigned long)it.size;    out += '\t';
    out += (unsigned long)it.created; out += '\t';
    out += it.nick;                   out += '\t';
    out += it.name;                   out += '\n';
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", out);
}

// GET /f?id=   lecture dans la page (texte)     GET /f?id=&d=1   téléchargement
void handleFile() {
  uint32_t id = strtoul(server.arg("id").c_str(), nullptr, 10);
  int k = findItem(id);
  if (k < 0) { server.send(404, "text/plain", "absent"); return; }
  File f = LittleFS.open(filePath(id), "r");
  if (!f) { server.send(404, "text/plain", "absent"); return; }

  const Item& it = items[k];
  bool dl = server.arg("d") == "1";
  char ascii[FNAME_MAX + 1], pct[FNAME_MAX * 3 + 1];
  dispNames(it.name, ascii, pct);

  String cd = dl ? "attachment" : "inline";
  cd += "; filename=\""; cd += ascii; cd += "\"; filename*=UTF-8''"; cd += pct;
  server.sendHeader("Content-Disposition", cd);
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(f, it.kind == 'P' ? "application/pdf" : "text/plain; charset=utf-8");
  f.close();
}

// POST /up?tok=&nm=&n=&t=   corps multipart avec un fichier : écrit directement dans la flash
void upFail(int code) {
  Serial.printf("envoi interrompu apres %u octets : code %d\n", (unsigned)up.bytes, code);
  up.err = code;
  if (up.f) up.f.close();
  LittleFS.remove("/l/tmp");
}

void handleUpData() {
  HTTPUpload& u = server.upload();

  if (u.status == UPLOAD_FILE_START) {
    if (up.f) up.f.close();
    up.started = true; up.err = 0; up.bytes = 0; up.limit = 0; up.first = true;
    up.t = clientTime();
    up.nick[0] = 0; up.tok[0] = 0;

    String tok = server.arg("tok");
    String nm  = server.arg("nm");
    if (!nm.length()) nm = u.filename;
    up.kind = cleanName(nm.c_str(), up.name);
    cleanLine(server.arg("n"), up.nick, NICK_MAX);
    if (!up.nick[0]) strcpy(up.nick, "anonyme");

    if (!validTok(tok))                                                   up.err = 400;
    else if (!up.kind)                                                    up.err = 415;
    else if (!allowSender((uint32_t)server.client().remoteIP(), millis())) up.err = 429;
    else if (libFull())                                                   up.err = 507;
    else if (server.clientContentLength() > (int)(MAX_FILE_BYTES + 4096)) up.err = 413;

    Serial.printf("envoi \"%s\" : %d octets annonces, code %d\n", nm.c_str(), server.clientContentLength(), up.err);

    if (!up.err) {
      strcpy(up.tok, tok.c_str());
      size_t room = roomBytes();
      up.limit = room < MAX_FILE_BYTES ? room : MAX_FILE_BYTES;
      up.f = LittleFS.open("/l/tmp", "w");
      if (!up.f) up.err = 500;
    }
  }
  else if (u.status == UPLOAD_FILE_WRITE) {
    if (up.err || !up.f) return;                  // on lit la suite sans l'écrire, puis on répond l'erreur
    if (up.first) {
      up.first = false;
      if (up.kind == 'P' && (u.currentSize < 5 || memcmp(u.buf, "%PDF-", 5) != 0)) { upFail(415); return; }
    }
    if (up.kind == 'T' && memchr(u.buf, 0, u.currentSize)) { upFail(415); return; }
    if (up.bytes + u.currentSize > up.limit) { upFail(up.limit < MAX_FILE_BYTES ? 507 : 413); return; }
    if (up.f.write(u.buf, u.currentSize) != u.currentSize) { upFail(507); return; }
    up.bytes += u.currentSize;
  }
  else if (u.status == UPLOAD_FILE_END) {
    if (up.f) up.f.close();
    if (!up.err && up.bytes == 0) { up.err = 400; LittleFS.remove("/l/tmp"); }
  }
  else {                                          // UPLOAD_FILE_ABORTED
    if (up.f) up.f.close();
    LittleFS.remove("/l/tmp");
    up.err = 400;
  }
}

void handleUpDone() {
  if (up.f) up.f.close();                         // normalement déjà fermé à la fin de l'envoi
  int err = up.started ? up.err : 400;
  uint32_t id = 0;

  if (!err && ecount >= MAX_ENTRIES) err = 507;
  if (!err) {
    id = lastId + 1;
    String fp = filePath(id);
    LittleFS.remove(fp);
    if (!LittleFS.rename("/l/tmp", fp)) err = 500;
    else {
      uint32_t rev = (esp_random() % 900000UL) + 1;      // au hasard : un numéro réutilisé après redémarrage n'est jamais confondu
      if (!writeMeta(id, up.tok, up.t, rev, (uint32_t)up.bytes, up.kind, up.nick, up.name)) {
        LittleFS.remove(fp);
        err = 500;
      } else {
        Item& it = items[ecount++];
        it.id = id; it.rev = rev; it.created = up.t; it.size = (uint32_t)up.bytes; it.kind = up.kind;
        it.own = tokHash(up.tok);
        strlcpy(it.nick, up.nick, sizeof(it.nick));
        strlcpy(it.name, up.name, sizeof(it.name));
        lastId = id;
        freeAt = 0;
        Serial.printf("depot #%lu \"%s\" (%u octets) par %s - %d fichiers, %u octets libres\n",
                      (unsigned long)id, it.name, (unsigned)up.bytes, it.nick, ecount, (unsigned)freeBytes());
      }
    }
  }

  if (err) { Serial.printf("depot refuse : code %d\n", err); LittleFS.remove("/l/tmp"); LittleFS.remove("/l/tmp.m"); }
  up.started = false;
  if (err) server.send(err, "text/plain", "refus");
  else     server.send(200, "text/plain", String(id));
}

// POST /del  tok, id
void handleDel() {
  if (!isPost()) return;
  String tok = server.arg("tok");
  uint32_t id = strtoul(server.arg("id").c_str(), nullptr, 10);
  int k = findItem(id);
  if (k < 0) { server.send(404, "text/plain", "absent"); return; }

  Item it; String owner;
  if (!validTok(tok) || !readMeta(id, it, &owner) || owner != tok) { server.send(403, "text/plain", "pas l'auteur"); return; }

  removeAt(k);
  Serial.printf("fichier #%lu supprime par son auteur\n", (unsigned long)id);
  server.send(200, "text/plain", "ok");
}

// ---------- Gardien ----------

int      guardFails = 0;
uint32_t guardLockAt = 0;

// 200 = ok, 403 = refusé, 429 = bloqué après trop d'erreurs
int guardCheck() {
  uint32_t now = millis();
  if (guardFails >= GUARD_MAX_FAILS) {
    if (now - guardLockAt < GUARD_LOCK_MS) return 429;
    guardFails = 0;
  }
  String u = server.arg("u"); u.trim(); u.toLowerCase();
  if (u == GUARD_USER && server.arg("p") == GUARD_PASS) { guardFails = 0; return 200; }
  guardFails++;
  if (guardFails >= GUARD_MAX_FAILS) guardLockAt = now;
  return 403;
}

// POST /mlogin  u, p
void handleGuardLogin() {
  if (!isPost()) return;
  int c = guardCheck();
  server.send(c, "text/plain", c == 200 ? "ok" : "refus");
}

// POST /mdel  u, p, ids = "12,15,18"
void handleGuardDelete() {
  if (!isPost()) return;
  int c = guardCheck();
  if (c != 200) { server.send(c, "text/plain", "refus"); return; }
  String ids = server.arg("ids");
  int n = 0, start = 0;
  while (start < (int)ids.length()) {
    int comma = ids.indexOf(',', start);
    if (comma < 0) comma = ids.length();
    uint32_t id = strtoul(ids.substring(start, comma).c_str(), nullptr, 10);
    int k = findItem(id);
    if (k >= 0) { removeAt(k); n++; }
    start = comma + 1;
  }
  Serial.printf("gardien : %d fichier(s) efface(s), %u octets libres\n", n, (unsigned)freeBytes());
  server.send(200, "text/plain", String(n));
}

// ---------- Setup / Loop ----------

void setup() {
  Serial.begin(115200);
  delay(200);

  if (!LittleFS.begin(true)) {     // true = formate si la flash n'est pas encore préparée
    Serial.println("ERREUR : LittleFS indisponible (verifie le Partition Scheme)");
  }
  loadGen();
  scanFiles();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, AP_MAX_CLIENTS);

  // Certaines cartes ESP32-C3 Super Mini ont une antenne faible :
  // si le réseau n'apparaît pas ou décroche, décommenter la ligne suivante.
  // WiFi.setTxPower(WIFI_POWER_8_5dBm);

  apHost = apIP.toString();
  portalUrl = "http://" + apHost + "/";

  // Option DHCP 114 (RFC 8910) : annonce directement l'adresse du portail aux téléphones récents.
  // Disponible seulement avec un cœur ESP32 récent (ESP-IDF 5.4.2 ou plus) : sinon ce bloc est ignoré.
#if defined(ESP_IDF_VERSION_VAL) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
  if (WiFi.AP.enableDhcpCaptivePortal()) Serial.println("Option DHCP 114 active");
#endif

  dns.start(53, "*", apIP);

  server.on("/", handlePage);

  // Détection de portail captif : une redirection 302 est la réponse la mieux reconnue
  server.on("/generate_204", handleRedirect);              // Android
  server.on("/gen_204", handleRedirect);                   // Android
  server.on("/hotspot-detect.html", handleRedirect);       // iOS / macOS
  server.on("/library/test/success.html", handleRedirect); // iOS ancien
  server.on("/ncsi.txt", handleRedirect);                  // Windows
  server.on("/connecttest.txt", handleRedirect);           // Windows
  server.on("/redirect", handleRedirect);                  // Windows
  server.on("/success.txt", handleRedirect);               // Firefox
  server.on("/canonical.html", handleRedirect);            // Firefox
  server.on("/favicon.ico", handleNothing);
  server.on("/wpad.dat", handleNothing);

  // Bibliothèque
  server.on("/list", HTTP_GET, handleList);
  server.on("/f",    HTTP_GET, handleFile);
  server.on("/up",   HTTP_POST, handleUpDone, handleUpData);
  server.on("/del",  handleDel);
  server.on("/mlogin", handleGuardLogin);
  server.on("/mdel",   handleGuardDelete);

  server.onNotFound(handleRedirect);
  server.begin();

  Serial.println();
  Serial.print("DROP_LIB pret : ");
  Serial.println(portalUrl);
  Serial.printf("%d fichiers, flash : %u octets libres sur %u\n",
                ecount, (unsigned)freeBytes(), (unsigned)LittleFS.totalBytes());
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
}
