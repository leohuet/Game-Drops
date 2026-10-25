/*
  DROPCHAT v7 — messagerie éphémère locale sur ESP32 Super Mini
  v7 : corrections iPhone — le clavier s'ouvre dans le geste de l'utilisateur (pseudo, envoi),
       la barre d'envoi suit le clavier, l'éditeur de dessin s'ajuste à l'écran (paysage compris),
       un seul doigt dessine, secours tactile pour les iOS sans événements pointeur,
       CSS sans « inset » ni « gap » flex (iOS 13 à 14.4 les ignorent)
  v6 : la page défile comme un site normal (plus de rechargement intempestif dans la
       fenêtre de connexion Android quand on glisse vers le bas)
  v4 : 150 messages en mémoire, 8 derniers dessins, blocage du "tirer pour recharger",
       brouillons (texte + dessin) restaurés si la page est rechargée par erreur
  - Point d'accès Wi-Fi ouvert "DROPCHAT", DNS captif, page sur http://4.3.2.1
    (ou http://192.168.4.1 si USE_PUBLIC_IP vaut 0)
  v5 : portail captif plus fiable sur Android (adresse publique, redirections 302,
       option DHCP 114 si le cœur ESP32 la supporte, favicon sans requête)
       Idées reprises de CDFER/Captive-Portal-ESP32 (licence MIT) :
       https://github.com/CDFER/Captive-Portal-ESP32
  - Messages texte OU dessins (64x64, 16 couleurs) envoyés dans la conversation
  - Pseudos réservés pendant la session : un pseudo pris est refusé aux autres appareils
  - Liste des présents + "X écrit…" / "X dessine…"
  - Réactions emoji, son + vibration à l'arrivée d'un message
  Éphémère : rien n'est écrit en flash. Au redémarrage de l'ESP32, messages, dessins et
  pseudos disparaissent, et les téléphones oublient aussi leur pseudo.
  Bibliothèques : WiFi.h, WebServer.h, DNSServer.h (incluses dans le cœur Arduino ESP32)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

// ---------- Réglages ----------
const char* AP_SSID        = "DROPCHAT";
const char* AP_PASS        = "";   // vide = réseau ouvert (sinon 8 caractères minimum)
const int   AP_CHANNEL     = 1;
const int   AP_MAX_CLIENTS = 8;    // 10 maximum sur ESP32

// Adresse du portail. Certains Android (Samsung notamment) n'ouvrent pas la page captive
// quand l'adresse est privée (192.168.x.x). 1 = adresse 4.3.2.1 (recommandé),
// 0 = ancienne adresse 192.168.4.1. C'est aussi l'adresse à taper à la main dans le navigateur.
#define USE_PUBLIC_IP 1

#define MAX_MSG           150      // messages conservés (texte + dessins)
#define NICK_MAX          32       // octets UTF-8
#define TEXT_MAX          200      // octets UTF-8 (la page limite à 150 caractères)
#define MAX_REAC          8        // réactions max par message
#define NUM_EMO           6        // nombre d'emoji proposés (liste côté page)
#define MAX_USERS         16
#define ONLINE_MS         12000    // "présent" si vu depuis moins de 12 s
#define ACT_MS            4000     // durée de l'indicateur "écrit…/dessine…"
#define EVICT_MS          300000UL // place inactive depuis 5 min réattribuable si tout est plein
#define SEND_COOLDOWN_MS  800
#define DRAW_COOLDOWN_MS  2500

#define DW         64
#define DH         64
#define DRAW_BYTES (DW * DH / 2)   // 2 pixels par octet = 2048 octets par dessin
#define MAX_DRAW   8               // dessins gardés en mémoire : au-delà, les plus anciens expirent

// ---------- Données ----------
struct Reac { uint16_t uid; uint8_t e; };

struct Msg {
  uint32_t id;
  uint32_t rev;                    // change à chaque nouveauté (message ou réaction)
  uint32_t ms;
  uint16_t uid;
  uint8_t  kind;                   // 0 = texte, 1 = dessin
  uint8_t  nr;
  Reac     r[MAX_REAC];
  char nick[NICK_MAX + 1];
  char text[TEXT_MAX + 1];
};

struct User {
  bool     used;
  char     tok[17];
  char     nick[NICK_MAX + 1];
  uint16_t uid;
  uint8_t  act;                    // 1 = écrit, 2 = dessine
  uint32_t actUntil;
  uint32_t seen;
  uint32_t lastSend;
  uint32_t lastDraw;
};

struct Drawing { uint32_t msgId; uint8_t px[DRAW_BYTES]; };

Msg      msgs[MAX_MSG];
uint32_t lastId = 0;
uint32_t msgRev = 0;

User     users[MAX_USERS];
uint16_t nextUid = 0;

Drawing  draws[MAX_DRAW];
uint32_t drawCount = 0;

char bootTag[9];
const char HEXCH[] = "0123456789abcdef";
String pbuf;                       // tampon réutilisé pour envoyer /poll par morceaux

WebServer server(80);
DNSServer dns;
#if USE_PUBLIC_IP
const IPAddress apIP(4, 3, 2, 1);
#else
const IPAddress apIP(192, 168, 4, 1);
#endif
String apHost;                     // "4.3.2.1"
String portalUrl;                  // "http://4.3.2.1/"

// ---------- Page ----------
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DROPCHAT</title>
<link rel="icon" href="data:,">
<style>
*{box-sizing:border-box;margin:0;padding:0}
/* Page web normale : c'est le document qui défile (évite les rechargements dans la fenêtre de connexion Android) */
html{background:#000;overscroll-behavior-y:none;-webkit-text-size-adjust:100%;text-size-adjust:100%;-webkit-tap-highlight-color:transparent;touch-action:manipulation}
body{min-height:calc(100vh + 2px);background:#000;color:#eee;font-family:monospace;overscroll-behavior-y:none}
/* Pas de "gap" sur les boîtes flex ni de "inset" : iOS 13 à 14.4 les ignorent (espaces perdus, fenêtres mal placées) */
header{position:sticky;top:0;z-index:4;background:#000;display:flex;align-items:center;justify-content:space-between;padding:10px 12px;border-bottom:1px solid #333}
h1{font-size:16px;letter-spacing:2px;margin-right:8px}
.right{display:flex;align-items:center}
.right>*+*{margin-left:6px}
.hb{background:none;border:1px solid #555;color:#fff;border-radius:8px;padding:6px 9px;font:inherit;font-size:13px;white-space:nowrap}
#me{max-width:32vw;overflow:hidden;text-overflow:ellipsis}
#pres.off{color:#f87171;border-color:#7f1d1d}
#who{display:none;position:absolute;right:12px;top:100%;margin-top:4px;background:#111;border:1px solid #444;border-radius:10px;padding:8px 12px;z-index:5;min-width:150px;max-height:50vh;overflow:auto}
#who.show{display:block}
#who div{padding:4px 0;font-size:14px}
#list{padding:10px 12px;display:flex;flex-direction:column}
#list>*+*{margin-top:8px}
#dock{position:fixed;left:0;right:0;bottom:0;z-index:4;background:#000}
.m{max-width:85%;align-self:flex-start;background:#111;border:1px solid #2a2a2a;border-radius:12px;padding:6px 10px;cursor:pointer}
.m.mine{align-self:flex-end;background:#0b2a4a;border-color:#1d4ed8}
.h{font-size:11px;color:#888;margin-bottom:4px}
.n{font-weight:bold}
.t{font-size:15px;line-height:1.35;white-space:pre-wrap;word-break:break-word;overflow-wrap:anywhere}
.exp{color:#666;font-style:italic;font-size:13px}
.dw{display:block;width:min(160px,60vw);height:min(160px,60vw);image-rendering:pixelated;image-rendering:crisp-edges;border-radius:8px;border:1px solid #333;background:#000}
.rx{display:flex;flex-wrap:wrap;margin:4px 0 -4px}
.rx:empty{display:none}
.chip{background:#1a1a1a;border:1px solid #333;color:#fff;border-radius:999px;padding:2px 7px;font-size:13px;font-family:inherit;margin:0 4px 4px 0}
.chip.on{border-color:#3b82f6;background:#172554}
#picker{display:none;margin:6px 0 -4px;flex-wrap:wrap}
#picker.show{display:flex}
#picker button{background:#1a1a1a;border:1px solid #333;border-radius:10px;font-size:20px;padding:4px 6px;margin:0 4px 4px 0}
#empty{color:#666;text-align:center;margin:30vh auto;font-size:13px;line-height:1.6}
#st{min-height:18px;padding:0 12px;font-size:12px;color:#9ca3af}
#bar{display:flex;padding:8px 12px;padding-bottom:calc(10px + env(safe-area-inset-bottom));border-top:1px solid #333}
#bar>*+*{margin-left:8px}
input{flex:1;min-width:0;background:#111;color:#fff;border:1px solid #444;border-radius:10px;padding:12px;font:inherit;font-size:16px}
button.go{background:#1d4ed8;color:#fff;border:none;border-radius:10px;padding:0 16px;font:inherit;font-size:15px;font-weight:bold}
button.go:disabled{opacity:.5}
button.ic{background:#111;border:1px solid #444;border-radius:10px;font-size:20px;padding:0 10px}
/* Éditeur : la taille du dessin est ajustée par le script (fitEditor) pour que tout tienne à l'écran, même en paysage */
#ed{position:fixed;top:0;left:0;right:0;bottom:0;background:#000;display:none;flex-direction:column;align-items:center;padding:10px 12px;padding-bottom:calc(12px + env(safe-area-inset-bottom));z-index:8;overflow:hidden;overscroll-behavior:none;touch-action:none;-webkit-user-select:none;user-select:none;-webkit-touch-callout:none}
.edtop{display:flex;align-items:center;justify-content:space-between;width:100%}
.edtop span{font-size:14px;color:#bbb}
.edtop .go{padding:10px 16px}
#edBody{display:flex;flex-direction:column;align-items:center;margin-top:10px;max-width:100%;width:100%}
#cv{width:min(94vw,52vh);height:min(94vw,52vh);image-rendering:pixelated;image-rendering:crisp-edges;border:2px solid #333;touch-action:none;background:#000;flex:none;-webkit-user-select:none;user-select:none;-webkit-touch-callout:none}
#edSide{width:min(94vw,52vh);margin-top:10px}
#pal{display:grid;grid-template-columns:repeat(8,1fr);gap:6px;margin-top:10px}
#pal button{height:34px;border:2px solid #333;border-radius:6px}
#pal button.sel{border-color:#fff;box-shadow:0 0 0 2px #1d4ed8}
.tools{display:flex;margin-top:10px}
.tools button{flex:1;min-width:0;background:#111;color:#fff;border:1px solid #444;border-radius:10px;padding:10px 4px;font:inherit;font-size:13px}
.tools button+button{margin-left:8px}
#edSt{min-height:16px;margin-top:8px;font-size:12px;color:#f87171;text-align:center}
@media (orientation:landscape) and (max-height:520px){
  #edBody{flex-direction:row;align-items:flex-start;justify-content:center}
  #edSide{flex:1;min-width:0;max-width:460px;margin:0 0 0 14px}
}
/* Fenêtre du pseudo : "margin:auto" centre sans jamais couper le haut quand le clavier réduit la place */
#modal{position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,.85);display:none;padding:20px;z-index:10;overflow:auto;overscroll-behavior:contain}
#nickForm{background:#111;border:1px solid #444;border-radius:12px;padding:18px;width:100%;max-width:320px;display:flex;flex-direction:column;margin:auto}
#nickForm>*+*{margin-top:12px}
@media (max-height:520px){
  #modal{padding:8px 20px}
  #nickForm{padding:12px}
  #nickForm>*+*{margin-top:6px}
}
#nickForm p{font-size:14px;text-align:center}
#nickErr{color:#f87171;font-size:13px;min-height:16px;text-align:center}
#nickForm button{padding:12px}
</style>
</head>
<body>
<header>
  <h1>DROPCHAT</h1>
  <div class="right">
    <button class="hb" id="pres" type="button">● 0</button>
    <button class="hb" id="snd" type="button">🔔</button>
    <button class="hb" id="me" type="button">pseudo</button>
  </div>
  <div id="who"></div>
</header>

<div id="list"><div id="empty">Aucun message pour l'instant.<br>Écris ou dessine quelque chose !</div></div>
<div id="dock">
<div id="st"></div>
<form id="bar" autocomplete="off">
  <button class="ic" id="drawBtn" type="button" title="Dessiner">✏️</button>
  <input id="txt" maxlength="150" placeholder="Message…" enterkeyhint="send">
  <button class="go" id="sendBtn">Envoyer</button>
</form>
</div>

<div id="ed">
  <div class="edtop" id="edTop">
    <button class="hb" id="edClose" type="button">✕ Fermer</button>
    <span>Dessin</span>
    <button class="go" id="edSend" type="button">Envoyer</button>
  </div>
  <div id="edBody">
    <canvas id="cv" width="64" height="64"></canvas>
    <div id="edSide">
      <div id="pal"></div>
      <div class="tools">
        <button id="tSize" type="button">Trait : moyen</button>
        <button id="tUndo" type="button">↶ Annuler</button>
        <button id="tClear" type="button">Effacer</button>
      </div>
      <div id="edSt"></div>
    </div>
  </div>
</div>

<div id="picker"></div>

<div id="modal">
  <form id="nickForm" autocomplete="off">
    <p>Choisis ton pseudo</p>
    <input id="nickIn" maxlength="16" placeholder="Pseudo">
    <div id="nickErr"></div>
    <button class="go">OK</button>
  </form>
</div>

<script>
const $ = id => document.getElementById(id);
const EMO = ['👍','❤️','😂','😮','😢','🔥'];
const PAL = ['#000000','#ffffff','#7f7f7f','#c3c3c3','#ef4444','#f97316','#facc15','#22c55e',
             '#15803d','#06b6d4','#3b82f6','#1e3a8a','#a855f7','#ec4899','#92400e','#fcd5b4'];
const PAL_RGB = PAL.map(h => [parseInt(h.substr(1,2),16), parseInt(h.substr(3,2),16), parseInt(h.substr(5,2),16)]);
const HEXS = '0123456789abcdef';
const DW = 64, DH = 64, MAX_DOM = 300;

const list = $('list'), txt = $('txt'), st = $('st'), sendBtn = $('sendBtn');
const pres = $('pres'), who = $('who'), snd = $('snd'), meBtn = $('me');
const modal = $('modal'), nickIn = $('nickIn'), nickErr = $('nickErr');
const picker = $('picker'), ed = $('ed'), edSt = $('edSt'), edSend = $('edSend');
const cv = $('cv'), g = cv.getContext('2d');
let emptyEl = $('empty');

function load(k){ try { return localStorage.getItem(k) || ''; } catch(e){ return ''; } }
function save(k,v){ try { localStorage.setItem(k, v); } catch(e){} }
// Brouillons : gardés le temps de l'onglet, pour survivre à un rechargement accidentel
function sload(k){ try { return sessionStorage.getItem(k) || ''; } catch(e){ return ''; } }
function ssave(k,v){ try { sessionStorage.setItem(k, v); } catch(e){} }

function newTok(){
  const a = new Uint8Array(8);
  try { crypto.getRandomValues(a); } catch(e){ for (let i = 0; i < 8; i++) a[i] = Math.random() * 256 | 0; }
  return Array.from(a, b => b.toString(16).padStart(2,'0')).join('');
}

// La session (jeton + pseudo) n'est valable que pour un démarrage donné de l'ESP32
let S = {};
try { S = JSON.parse(load('dc_s') || '{}') || {}; } catch(e){ S = {}; }
function saveS(){ save('dc_s', JSON.stringify(S)); }

let tok = '', nick = '', myUid = 0, boot = null, msgRev = 0;
let busy = false, again = false, joining = false, firstLoad = true;
let forceScroll = false, lastMsgId = 0, lastKey = 0, wasTyping = false;
let noteUntil = 0, users = [], edOpen = false, reopenEd = false;
let soundOn = load('dc_snd') !== '0';
const els = new Map();

function post(url, params){
  const body = Object.keys(params).map(k => k + '=' + encodeURIComponent(params[k])).join('&');
  return fetch(url, { method:'POST', headers:{ 'Content-Type':'application/x-www-form-urlencoded' }, body });
}

function colorOf(s){
  let h = 0;
  for (const c of s) h = (h * 31 + c.codePointAt(0)) >>> 0;
  return 'hsl(' + (h % 360) + ',70%,65%)';
}

function hhmm(t){
  const d = new Date(t);
  return String(d.getHours()).padStart(2,'0') + ':' + String(d.getMinutes()).padStart(2,'0');
}

function nearBottom(){ return document.documentElement.scrollHeight - window.scrollY - window.innerHeight < 60; }
function scrollBottom(){ window.scrollTo(0, document.documentElement.scrollHeight); }

// La barre d'envoi est fixée en bas : on réserve sa hauteur sous la conversation
const dock = $('dock');
function padDock(){ document.body.style.paddingBottom = dock.offsetHeight + 'px'; }

// Clavier virtuel (iPhone surtout) : la zone réellement visible est plus petite que la page.
// La barre d'envoi et la fenêtre du pseudo se calent donc sur le haut et le bas de cette zone visible.
function fitVisual(){
  const v = window.visualViewport;
  let top = 0, up = 0;
  if (v && v.scale < 1.02) {
    top = Math.round(v.offsetTop);
    up = Math.round(document.documentElement.clientHeight - v.offsetTop - v.height);
    if (top < 3) top = 0;
    if (up < 3) up = 0;
  }
  dock.style.bottom = up + 'px';
  modal.style.top = top + 'px';
  modal.style.bottom = up + 'px';
}

// Éditeur de dessin : le carré prend la place disponible, sans jamais repousser la palette
// hors de l'écran (portrait : palette dessous ; paysage : palette à droite)
const landQ = window.matchMedia ? matchMedia('(orientation:landscape) and (max-height:520px)') : null;
function fitEditor(){
  if (!edOpen) return;
  const cs = getComputedStyle(ed), side = $('edSide');
  const w = ed.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
  const h = ed.clientHeight - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom) - $('edTop').offsetHeight - 10;
  const land = !!(landQ && landQ.matches);
  let s;
  if (land) { side.style.width = ''; s = Math.min(h, w / 2); }
  else s = Math.min(w, h - side.offsetHeight - 10);
  s = Math.max(120, Math.floor(s));
  cv.style.width = cv.style.height = s + 'px';
  if (!land) side.style.width = s + 'px';
}

function refit(){ padDock(); fitVisual(); fitEditor(); }
window.addEventListener('resize', refit);
window.addEventListener('orientationchange', () => setTimeout(refit, 300));
if (window.visualViewport) {
  visualViewport.addEventListener('resize', fitVisual);
  visualViewport.addEventListener('scroll', fitVisual);
}

// La fenêtre de connexion Android recharge la page si on tire vers le bas tout en haut.
// On garde donc la page décalée d'1 pixel : elle ne se croit jamais « tout en haut ».
let liftT = null;
function lift(){ if (window.scrollY < 1) window.scrollTo(0, 1); }
window.addEventListener('scroll', () => { clearTimeout(liftT); liftT = setTimeout(lift, 150); }, { passive: true });

function paintPixels(ctx, getC){
  const img = ctx.createImageData(DW, DH);
  for (let i = 0; i < DW * DH; i++) {
    const rgb = PAL_RGB[getC(i)] || PAL_RGB[0];
    img.data[i*4] = rgb[0]; img.data[i*4+1] = rgb[1]; img.data[i*4+2] = rgb[2]; img.data[i*4+3] = 255;
  }
  ctx.putImageData(img, 0, 0);
}

// ---------- Son / vibration ----------
let actx = null;
function unlockAudio(){
  try {
    if (!actx) actx = new (window.AudioContext || window.webkitAudioContext)();
    if (actx.state !== 'running') actx.resume();
  } catch(e){}
}
document.addEventListener('pointerdown', unlockAudio, { passive:true });
document.addEventListener('keydown', unlockAudio);

function ping(){
  if (!soundOn) return;
  try { if (navigator.vibrate) navigator.vibrate(40); } catch(e){}
  if (!actx || actx.state !== 'running') return;
  const t = actx.currentTime;
  [[880,0],[1320,0.09]].forEach(([f,d]) => {
    const o = actx.createOscillator(), gn = actx.createGain();
    o.type = 'sine'; o.frequency.value = f;
    gn.gain.setValueAtTime(0.0001, t + d);
    gn.gain.exponentialRampToValueAtTime(0.08, t + d + 0.01);
    gn.gain.exponentialRampToValueAtTime(0.0001, t + d + 0.12);
    o.connect(gn); gn.connect(actx.destination);
    o.start(t + d); o.stop(t + d + 0.14);
  });
}

function renderSnd(){ snd.textContent = soundOn ? '🔔' : '🔕'; }
snd.addEventListener('click', () => { soundOn = !soundOn; save('dc_snd', soundOn ? '1' : '0'); renderSnd(); unlockAudio(); });

// ---------- Chargement des dessins (un à la fois pour ménager l'ESP32) ----------
const dq = [];
let dBusy = false;
function queueDraw(o){ dq.push(o); runQ(); }
async function runQ(){
  if (dBusy) return;
  const o = dq.shift();
  if (!o) return;
  dBusy = true;
  try {
    const r = await fetch('/d?b=' + boot + '&id=' + o.id);
    if (r.ok) {
      const t = await r.text();
      if (t.length === DW * DH && o.cv) {
        o.hex = t;
        paintPixels(o.cv.getContext('2d'), i => parseInt(t[i], 16) || 0);
      }
    } else if (r.status === 404) {
      markExpired(o);
    }
  } catch(e) {
    o.tries = (o.tries || 0) + 1;
    if (o.tries < 3) setTimeout(() => queueDraw(o), 3000);
  }
  dBusy = false;
  runQ();
}

function markExpired(o){
  if (!o.cv || o.hex) return;
  const d = document.createElement('div');
  d.className = 'exp';
  d.textContent = '(dessin expiré)';
  o.cv.replaceWith(d);
  o.cv = null;
}

// ---------- Messages ----------
function makeMsg(m, srvNow){
  const el = document.createElement('div');
  el.className = 'm' + (m.uid === myUid ? ' mine' : '');
  const h = document.createElement('div'); h.className = 'h';
  const ns = document.createElement('span'); ns.className = 'n';
  ns.textContent = m.nick; ns.style.color = colorOf(m.nick);
  h.appendChild(ns);
  h.appendChild(document.createTextNode(' · ' + hhmm(Date.now() - (srvNow - m.ms))));
  el.appendChild(h);

  const o = { id: m.id, uid: m.uid, kind: m.kind, el, rx: null, rc: '', cv: null, hex: null };

  if (m.kind === 'T') {
    const b = document.createElement('div'); b.className = 't'; b.textContent = m.text;
    el.appendChild(b);
  } else if (m.kind === 'D') {
    const c = document.createElement('canvas');
    c.className = 'dw'; c.width = DW; c.height = DH;
    el.appendChild(c);
    o.cv = c;
    queueDraw(o);
  } else {
    const d = document.createElement('div'); d.className = 'exp'; d.textContent = '(dessin expiré)';
    el.appendChild(d);
  }

  const rx = document.createElement('div'); rx.className = 'rx';
  el.appendChild(rx);
  o.rx = rx;

  el.addEventListener('click', e => {
    if (e.target.closest('.chip') || e.target.closest('#picker')) return;
    e.stopPropagation();
    togglePicker(o);
  });
  return o;
}

function renderReacs(o){
  const counts = [0,0,0,0,0,0];
  let mine = -1;
  if (o.rc) o.rc.split(',').forEach(p => {
    const [u, e] = p.split(':').map(Number);
    if (e >= 0 && e < EMO.length) { counts[e]++; if (u === myUid) mine = e; }
  });
  o.rx.innerHTML = '';
  counts.forEach((c, e) => {
    if (!c) return;
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'chip' + (mine === e ? ' on' : '');
    b.textContent = EMO[e] + ' ' + c;
    b.addEventListener('click', ev => { ev.stopPropagation(); react(o.id, e); });
    o.rx.appendChild(b);
  });
}

function upsert(m, srvNow){
  let o = els.get(m.id), isNew = false;
  if (!o) {
    if (m.id <= lastMsgId) return false;
    if (emptyEl) { emptyEl.remove(); emptyEl = null; }
    o = makeMsg(m, srvNow);
    els.set(m.id, o);
    list.appendChild(o.el);
    lastMsgId = m.id;
    isNew = true;
    while (els.size > MAX_DOM) {
      const k = els.keys().next().value;
      els.get(k).el.remove(); els.delete(k);
    }
  }
  o.rc = m.rc;
  renderReacs(o);
  return isNew;
}

function remarkMine(){
  els.forEach(o => { o.el.classList.toggle('mine', o.uid === myUid); renderReacs(o); });
}

// Sélecteur : emoji + "reprendre ce dessin"
EMO.forEach((em, e) => {
  const b = document.createElement('button');
  b.type = 'button'; b.textContent = em;
  b.addEventListener('click', ev => {
    ev.stopPropagation();
    react(+picker.dataset.id, e);
    closePicker();
  });
  picker.appendChild(b);
});
const pickCopy = document.createElement('button');
pickCopy.type = 'button'; pickCopy.textContent = '✏️'; pickCopy.title = 'Reprendre ce dessin';
pickCopy.addEventListener('click', ev => {
  ev.stopPropagation();
  const o = els.get(+picker.dataset.id);
  closePicker();
  if (o && o.hex) openEditor(o.hex);
});
picker.appendChild(pickCopy);

function togglePicker(o){
  if (picker.classList.contains('show') && +picker.dataset.id === o.id) { closePicker(); return; }
  picker.dataset.id = o.id;
  pickCopy.style.display = o.hex ? '' : 'none';
  o.el.appendChild(picker);
  picker.classList.add('show');
}
function closePicker(){ picker.classList.remove('show'); }

async function react(id, e){
  if (!myUid) { askNick(); return; }
  try {
    const r = await post('/react', { tok, id, e });
    if (r.status === 401) { myUid = 0; join(nick); return; }
  } catch(err){}
  poll();
}

// ---------- Présence / statut ----------
function note(t){ st.textContent = t; noteUntil = Date.now() + 1500; }

function renderUsers(){
  pres.classList.remove('off');
  pres.textContent = '● ' + users.length;
  who.innerHTML = '';
  if (!users.length) who.textContent = 'Personne pour l’instant';
  users.forEach(u => {
    const d = document.createElement('div');
    d.textContent = u.nick + (u.uid === myUid ? ' (toi)' : '');
    d.style.color = colorOf(u.nick);
    who.appendChild(d);
  });
  if (Date.now() < noteUntil) return;
  const a = users.filter(u => u.act && u.uid !== myUid);
  st.textContent = a.length === 0 ? '' :
                   a.length <= 2 ? a.map(u => u.nick + (u.act === 2 ? ' dessine…' : ' écrit…')).join(' · ') :
                   a.length + ' personnes écrivent ou dessinent…';
}

pres.addEventListener('click', e => { e.stopPropagation(); who.classList.toggle('show'); });
document.addEventListener('click', () => { who.classList.remove('show'); closePicker(); });

function isTyping(){ return !!txt.value.trim() && Date.now() - lastKey < 3000; }
function activity(){ return !myUid ? 0 : edOpen ? 2 : isTyping() ? 1 : 0; }

txt.addEventListener('input', () => {
  lastKey = Date.now();
  ssave('dc_txt', boot + '|' + txt.value);
  if (!wasTyping && txt.value.trim() && myUid) { wasTyping = true; poll(); }
});

// ---------- Éditeur de dessin ----------
const pix = new Uint8Array(DW * DH);
const undo = [];
const SIZES = [[1,'fin'],[2,'moyen'],[3,'épais']];
let cur = 1, sizeIdx = 1, drawing = false, lastX = -1, lastY = -1;

function setPx(i, c){
  pix[i] = c;
  g.fillStyle = PAL[c];
  g.fillRect(i % DW, (i / DW) | 0, 1, 1);
}
function redraw(){ paintPixels(g, i => pix[i]); }

function stamp(x, y){
  const s = SIZES[sizeIdx][0];
  const a = -Math.floor((s - 1) / 2);
  for (let dy = a; dy < a + s; dy++) {
    for (let dx = a; dx < a + s; dx++) {
      const px = x + dx, py = y + dy;
      if (px < 0 || py < 0 || px >= DW || py >= DH) continue;
      const i = py * DW + px;
      if (pix[i] !== cur) setPx(i, cur);
    }
  }
}

function lineTo(x0, y0, x1, y1){
  const dx = Math.abs(x1 - x0), dy = Math.abs(y1 - y0);
  const sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  let err = dx - dy;
  for (;;) {
    stamp(x0, y0);
    if (x0 === x1 && y0 === y1) break;
    const e2 = 2 * err;
    if (e2 > -dy) { err -= dy; x0 += sx; }
    if (e2 < dx)  { err += dx; y0 += sy; }
  }
}

function cellAt(cx, cy){
  const r = cv.getBoundingClientRect();
  const x = Math.floor((cx - r.left) / r.width * DW);
  const y = Math.floor((cy - r.top) / r.height * DH);
  return [Math.max(0, Math.min(DW - 1, x)), Math.max(0, Math.min(DH - 1, y))];
}

function saveDraft(){
  ssave('dc_draft', boot + '|' + (pix.some(v => v) ? Array.from(pix, v => HEXS[v]).join('') : ''));
}

function pushUndo(){
  undo.push(pix.slice());
  if (undo.length > 20) undo.shift();
}

function dStart(cx, cy){
  drawing = true;
  pushUndo();
  const [x, y] = cellAt(cx, cy);
  stamp(x, y); lastX = x; lastY = y;
  edSt.textContent = '';
}
function dMove(cx, cy){
  if (!drawing) return;
  const [x, y] = cellAt(cx, cy);
  if (x === lastX && y === lastY) return;
  lineTo(lastX, lastY, x, y);
  lastX = x; lastY = y;
}
function dEnd(){
  if (drawing) saveDraft();
  drawing = false;
}

// Un seul doigt dessine : un second doigt posé en même temps (paume, pouce) est ignoré
if (window.PointerEvent) {
  let pid = null;
  cv.addEventListener('pointerdown', e => {
    e.preventDefault();
    if (!e.isPrimary) return;
    pid = e.pointerId;
    try { cv.setPointerCapture(pid); } catch(err){}
    dStart(e.clientX, e.clientY);
  });
  cv.addEventListener('pointermove', e => { if (e.pointerId === pid) dMove(e.clientX, e.clientY); });
  ['pointerup','pointercancel'].forEach(t => cv.addEventListener(t, e => {
    if (e.pointerId === pid) { pid = null; dEnd(); }
  }));
} else {
  // iOS 12 et plus ancien : pas d'événements pointeur (ni touch-action), on prend les événements tactiles
  let tid = null;
  cv.addEventListener('touchstart', e => {
    e.preventDefault();
    if (e.touches.length > 1) return;
    const t = e.changedTouches[0];
    tid = t.identifier;
    dStart(t.clientX, t.clientY);
  }, { passive:false });
  cv.addEventListener('touchmove', e => {
    e.preventDefault();
    for (let i = 0; i < e.changedTouches.length; i++) {
      if (e.changedTouches[i].identifier === tid) dMove(e.changedTouches[i].clientX, e.changedTouches[i].clientY);
    }
  }, { passive:false });
  ['touchend','touchcancel'].forEach(n => cv.addEventListener(n, e => {
    for (let i = 0; i < e.changedTouches.length; i++) {
      if (e.changedTouches[i].identifier === tid) { tid = null; dEnd(); }
    }
  }));
}
// Rien ne doit faire défiler la page derrière l'éditeur (touch-action ne suffit pas sur les vieux iOS)
ed.addEventListener('touchmove', e => e.preventDefault(), { passive:false });

PAL.forEach((col, c) => {
  const b = document.createElement('button');
  b.type = 'button';
  b.style.background = col;
  if (c === cur) b.classList.add('sel');
  b.addEventListener('click', () => {
    cur = c;
    document.querySelectorAll('#pal button').forEach(x => x.classList.remove('sel'));
    b.classList.add('sel');
  });
  $('pal').appendChild(b);
});

$('tSize').addEventListener('click', () => {
  sizeIdx = (sizeIdx + 1) % SIZES.length;
  $('tSize').textContent = 'Trait : ' + SIZES[sizeIdx][1];
});
$('tUndo').addEventListener('click', () => { if (undo.length) { pix.set(undo.pop()); redraw(); saveDraft(); } });
$('tClear').addEventListener('click', () => { if (pix.some(v => v)) { pushUndo(); pix.fill(0); redraw(); saveDraft(); } });

function openEditor(hex){
  if (!myUid) { askNick(); return; }
  // Sur iPhone, toucher un bouton ne ferme pas le clavier : il resterait ouvert devant le dessin
  if (document.activeElement && document.activeElement.blur) document.activeElement.blur();
  if (hex) {
    pushUndo();
    for (let i = 0; i < DW * DH; i++) pix[i] = parseInt(hex[i], 16) || 0;
    redraw();
    saveDraft();
  }
  edSt.textContent = '';
  lift();
  ed.style.display = 'flex';
  edOpen = true;
  fitEditor();
  ssave('dc_ed', '1');
  poll();
}
function closeEditor(){
  ed.style.display = 'none';
  edOpen = false;
  ssave('dc_ed', '');
  poll();
}

// Dernier filet de sécurité : demande confirmation avant de quitter avec un dessin en cours
window.addEventListener('beforeunload', e => {
  if (edOpen && pix.some(v => v)) { e.preventDefault(); e.returnValue = ''; }
});

$('drawBtn').addEventListener('click', () => openEditor(null));
$('edClose').addEventListener('click', closeEditor);

edSend.addEventListener('click', async () => {
  if (!myUid) { askNick(); return; }
  if (!pix.some(v => v)) { edSt.textContent = 'Dessine quelque chose d’abord.'; return; }
  edSend.disabled = true;
  const p = Array.from(pix, v => HEXS[v]).join('');
  try {
    const r = await post('/draw', { tok, p });
    if (r.ok) {
      pix.fill(0); undo.length = 0; redraw(); saveDraft();
      closeEditor();
      forceScroll = true;
      poll();
    } else if (r.status === 429) {
      edSt.textContent = 'Attends un peu avant un autre dessin.';
    } else if (r.status === 401) {
      myUid = 0; join(nick);
      edSt.textContent = 'Reconnexion… renvoie ton dessin.';
    } else throw 0;
  } catch(e) {
    edSt.textContent = 'Échec de l’envoi, réessaie.';
  }
  edSend.disabled = false;
});

// ---------- Session / synchronisation ----------
function startSession(b, rebooted){
  boot = b;
  if (!rebooted && S.boot === b && /^[0-9a-f]{16}$/.test(S.tok || '')) {
    tok = S.tok; nick = S.nick || '';
    // Page rechargée pendant la même session : on restaure les brouillons
    const d = sload('dc_draft');
    if (d.startsWith(b + '|') && d.length === b.length + 1 + DW * DH) {
      for (let i = 0; i < DW * DH; i++) pix[i] = parseInt(d[b.length + 1 + i], 16) || 0;
      redraw();
    }
    const t = sload('dc_txt');
    if (t.startsWith(b + '|')) txt.value = t.slice(b.length + 1);
    reopenEd = sload('dc_ed') === '1';
  } else {
    S = { boot: b, tok: newTok(), nick: '' };
    saveS();
    tok = S.tok; nick = '';
  }
  myUid = 0;
  meBtn.textContent = nick || 'pseudo';
  if (nick) join(nick);
  else {
    nickErr.textContent = rebooted ? 'DROPCHAT a redémarré : tout a été effacé.' : '';
    askNick('');
  }
}

function resetAll(){
  document.body.appendChild(picker); closePicker();
  dq.length = 0;
  list.innerHTML = '';
  emptyEl = document.createElement('div');
  emptyEl.id = 'empty';
  emptyEl.innerHTML = "Aucun message pour l'instant.<br>Écris ou dessine quelque chose !";
  list.appendChild(emptyEl);
  els.clear();
  lastMsgId = 0; msgRev = 0; myUid = 0; firstLoad = true;
  pix.fill(0); undo.length = 0; redraw();
  ed.style.display = 'none'; edOpen = false;
}

function handle(body){
  const lines = body.split('\n');
  const H = lines[0].split('\t');
  if (H[0] !== 'H') return;

  if (boot === null) {
    startSession(H[1], false);
  } else if (H[1] !== boot) {
    // L'ESP32 a redémarré : la conversation et les pseudos n'existent plus
    resetAll();
    startSession(H[1], true);
    again = true;
    return;
  }

  const srvNow = +H[2], srvUid = +H[3];
  if (myUid && !srvUid && nick && !joining) { myUid = 0; join(nick); }

  const stick = nearBottom() || forceScroll;
  let fresh = 0;
  users = [];

  for (let i = 1; i < lines.length; i++) {
    const L = lines[i];
    if (!L) continue;
    const p = L.split('\t');
    if (p[0] === 'U' && p.length >= 4) {
      users.push({ uid: +p[1], act: +p[2], nick: p.slice(3).join('\t') });
    } else if (p[0] === 'M' && p.length >= 9) {
      const m = { id: +p[1], ms: +p[3], uid: +p[4], rc: p[5], kind: p[6], nick: p[7], text: p.slice(8).join('\t') };
      if (upsert(m, srvNow) && m.uid !== myUid) fresh++;
    }
  }

  msgRev = +H[4];

  if (stick) { scrollBottom(); forceScroll = false; }
  lift();
  if (fresh && !firstLoad) ping();
  firstLoad = false;
  renderUsers();
}

async function poll(){
  if (busy) { again = true; return; }
  busy = true;
  try {
    const a = activity();
    wasTyping = a === 1;
    const r = await fetch('/poll?tok=' + tok + '&s=' + msgRev + '&ty=' + a, { cache:'no-store' });
    if (!r.ok) throw 0;
    handle(await r.text());
  } catch(e) {
    pres.textContent = '○ hors ligne';
    pres.classList.add('off');
  }
  busy = false;
  if (again) { again = false; poll(); }
}

function tick(){
  if (!document.hidden) poll();
  setTimeout(tick, 2000);
}
document.addEventListener('visibilitychange', () => { if (!document.hidden) poll(); });

// ---------- Pseudo ----------
function askNick(prefill){
  nickIn.value = prefill !== undefined ? prefill : nick;
  modal.style.display = 'flex';
  fitVisual();
  // Sur iPhone, le clavier ne s'ouvre que si focus() est appelé pendant le geste de l'utilisateur
  // (un setTimeout ne compte plus) : on le fait donc tout de suite, puis on réessaie au cas où.
  nickIn.focus();
  setTimeout(() => nickIn.focus(), 50);
}
function closeNick(){
  modal.style.display = 'none';
  nickIn.blur();   // sinon le clavier peut rester ouvert sur iPhone alors que le champ a disparu
}

async function join(n){
  if (!n) { askNick(''); return false; }
  if (!tok) return false;
  joining = true;
  try {
    const r = await post('/join', { tok, n });
    const t = await r.text();
    if (r.ok) {
      myUid = +t; nick = n;
      S.nick = n; saveS();
      meBtn.textContent = n;
      nickErr.textContent = '';
      closeNick();
      remarkMine();
      joining = false;
      if (reopenEd) { reopenEd = false; openEditor(null); }
      poll();
      return true;
    }
    nickErr.textContent = r.status === 409 ? 'Ce pseudo est déjà pris.' :
                          r.status === 503 ? 'Trop de monde, réessaie dans un moment.' :
                          'Pseudo invalide.';
  } catch(e) {
    nickErr.textContent = 'Connexion impossible, réessaie.';
  }
  joining = false;
  askNick(n);
  return false;
}

$('nickForm').addEventListener('submit', e => {
  e.preventDefault();
  const v = nickIn.value.trim().slice(0, 16);
  if (!v) return;
  unlockAudio();
  join(v);
});
modal.addEventListener('click', e => { if (e.target === modal && myUid) closeNick(); });
meBtn.addEventListener('click', () => { nickErr.textContent = ''; askNick(); });

// ---------- Envoi texte ----------
$('bar').addEventListener('submit', async e => {
  e.preventDefault();
  const t = txt.value.trim();
  if (!t) return;
  if (!myUid) { askNick(); return; }
  txt.focus();   // pendant le geste : sur iPhone le clavier reste ouvert (après l'envoi réseau, focus() ne le rouvrirait plus)
  sendBtn.disabled = true;
  try {
    const r = await post('/send', { tok, m: t });
    if (r.status === 429) note('doucement…');
    else if (r.status === 401) { myUid = 0; await join(nick); note('reconnecté, renvoie ton message'); }
    else if (!r.ok) throw 0;
    else { txt.value = ''; ssave('dc_txt', ''); wasTyping = false; forceScroll = true; poll(); }
  } catch(err) {
    note('échec envoi');
  }
  sendBtn.disabled = false;
  txt.focus();
});

// ---------- Démarrage ----------
renderSnd();
refit();
redraw();
poll();   // le premier échange donne l'identifiant de démarrage, puis demande le pseudo
setTimeout(tick, 2000);
</script>
</body>
</html>
)rawliteral";

// ---------- Outils ----------

// Copie nettoyée : supprime tabulations/retours ligne, coupe sans casser un caractère UTF-8
void cleanCopy(const String& in, char* out, size_t maxBytes) {
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

int findUser(const String& t) {
  if (!validTok(t)) return -1;
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i].used && strcmp(users[i].tok, t.c_str()) == 0) return i;
  }
  return -1;
}

// Comparaison sans tenir compte des majuscules (ASCII)
bool sameNick(const char* a, const char* b) {
  while (*a && *b) {
    if (tolower((uint8_t)*a) != tolower((uint8_t)*b)) return false;
    a++; b++;
  }
  return *a == *b;
}

int freeSlot(uint32_t now) {
  int oldest = -1;
  for (int i = 0; i < MAX_USERS; i++) {
    if (!users[i].used) return i;
    if (oldest < 0 || users[i].seen < users[oldest].seen) oldest = i;
  }
  if (oldest >= 0 && now - users[oldest].seen > EVICT_MS) return oldest;
  return -1;
}

int drawSlotOf(uint32_t msgId) {
  if (msgId == 0) return -1;
  for (int i = 0; i < MAX_DRAW; i++) if (draws[i].msgId == msgId) return i;
  return -1;
}

bool isPost() {
  if (server.method() == HTTP_POST) return true;
  server.send(405, "text/plain", "POST uniquement");
  return false;
}

// Envoie le tampon de /poll quand il dépasse ~1 Ko (ou à la fin)
void pflush(bool end) {
  if (pbuf.length() >= 1024 || (end && pbuf.length())) {
    server.sendContent(pbuf);
    pbuf = "";
  }
}

// Crée un message et renvoie sa référence
Msg& newMsg(User& u, uint8_t kind, uint32_t now) {
  lastId++;
  Msg& m = msgs[(lastId - 1) % MAX_MSG];
  m.id   = lastId;
  m.rev  = ++msgRev;
  m.ms   = now;
  m.uid  = u.uid;
  m.kind = kind;
  m.nr   = 0;
  m.text[0] = 0;
  strcpy(m.nick, u.nick);
  return m;
}

// ---------- Routes ----------

void handleRedirect() {
  server.sendHeader("Location", portalUrl, true);
  server.sendHeader("Cache-Control", "no-store");
  server.send(302, "text/plain", "");
}

void handlePage() {
  // Requête destinée à un autre site (détection de portail) : on redirige vers notre adresse
  if (server.hostHeader().length() && server.hostHeader() != apHost) {
    handleRedirect();
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", PAGE);
}

// Requêtes inutiles (icône, proxy auto) : réponse vide immédiate, sans renvoyer la page
void handleNothing() {
  server.send(404, "text/plain", "");
}

// POST /join  tok, n  -> 200 uid | 409 pris | 503 plein
void handleJoin() {
  if (!isPost()) return;
  String tok = server.arg("tok");
  if (!validTok(tok)) { server.send(400, "text/plain", "jeton"); return; }

  char nick[NICK_MAX + 1];
  cleanCopy(server.arg("n"), nick, NICK_MAX);
  if (!nick[0]) { server.send(400, "text/plain", "pseudo vide"); return; }

  int me = findUser(tok);
  for (int i = 0; i < MAX_USERS; i++) {
    if (i != me && users[i].used && sameNick(users[i].nick, nick)) {
      server.send(409, "text/plain", "pris");
      return;
    }
  }

  uint32_t now = millis();
  if (me < 0) {
    me = freeSlot(now);
    if (me < 0) { server.send(503, "text/plain", "plein"); return; }
    User& u = users[me];
    memset(&u, 0, sizeof(User));
    u.used = true;
    strcpy(u.tok, tok.c_str());
    u.uid = ++nextUid;
    if (nextUid == 0) u.uid = nextUid = 1;
  }

  strcpy(users[me].nick, nick);
  users[me].seen = now;
  Serial.printf("pseudo #%u : %s\n", users[me].uid, nick);
  server.send(200, "text/plain", String(users[me].uid));
}

// POST /send  tok, m
void handleSend() {
  if (!isPost()) return;
  int me = findUser(server.arg("tok"));
  if (me < 0) { server.send(401, "text/plain", "inconnu"); return; }

  uint32_t now = millis();
  User& u = users[me];
  if (u.lastSend && now - u.lastSend < SEND_COOLDOWN_MS) {
    server.send(429, "text/plain", "trop vite");
    return;
  }

  char text[TEXT_MAX + 1];
  cleanCopy(server.arg("m"), text, TEXT_MAX);
  if (!text[0]) { server.send(400, "text/plain", "message vide"); return; }

  u.lastSend = now;
  u.seen = now;
  u.actUntil = 0;

  Msg& m = newMsg(u, 0, now);
  strcpy(m.text, text);

  Serial.printf("[%lu] %s : %s\n", (unsigned long)m.id, m.nick, m.text);
  server.send(200, "text/plain", "ok");
}

// POST /draw  tok, p = 4096 caractères hexa (un par pixel, couleur 0..15)
void handlePostDraw() {
  if (!isPost()) return;
  int me = findUser(server.arg("tok"));
  if (me < 0) { server.send(401, "text/plain", "inconnu"); return; }

  uint32_t now = millis();
  User& u = users[me];
  if (u.lastDraw && now - u.lastDraw < DRAW_COOLDOWN_MS) {
    server.send(429, "text/plain", "trop vite");
    return;
  }

  const String& p = server.arg("p");
  if (p.length() != DW * DH) { server.send(400, "text/plain", "taille"); return; }

  // Vérification complète avant d'écraser un ancien dessin
  bool any = false;
  for (size_t i = 0; i < DW * DH; i++) {
    int v = hexVal(p[i]);
    if (v < 0) { server.send(400, "text/plain", "invalide"); return; }
    if (v) any = true;
  }
  if (!any) { server.send(400, "text/plain", "vide"); return; }

  u.lastDraw = now;
  u.seen = now;
  u.actUntil = 0;

  Msg& m = newMsg(u, 1, now);

  Drawing& d = draws[drawCount % MAX_DRAW];
  drawCount++;
  d.msgId = m.id;
  for (size_t k = 0; k < DRAW_BYTES; k++) {
    d.px[k] = (hexVal(p[2 * k]) << 4) | hexVal(p[2 * k + 1]);
  }

  Serial.printf("[%lu] %s : (dessin)\n", (unsigned long)m.id, m.nick);
  server.send(200, "text/plain", "ok");
}

// GET /d?id=  -> 4096 caractères hexa, ou 404 si le dessin a expiré
void handleGetDraw() {
  uint32_t id = strtoul(server.arg("id").c_str(), nullptr, 10);
  int slot = drawSlotOf(id);
  if (slot < 0) { server.send(404, "text/plain", "expire"); return; }

  String out;
  out.reserve(DW * DH + 4);
  const uint8_t* px = draws[slot].px;
  for (size_t k = 0; k < DRAW_BYTES; k++) {
    out += HEXCH[px[k] >> 4];
    out += HEXCH[px[k] & 15];
  }
  // L'URL contient l'identifiant de démarrage : le navigateur peut garder le dessin en cache
  server.sendHeader("Cache-Control", "max-age=3600");
  server.send(200, "text/plain", out);
}

// POST /react  tok, id, e  -> ajoute, change ou retire la réaction de l'utilisateur
void handleReact() {
  if (!isPost()) return;
  int me = findUser(server.arg("tok"));
  if (me < 0) { server.send(401, "text/plain", "inconnu"); return; }

  uint32_t id = strtoul(server.arg("id").c_str(), nullptr, 10);
  int e = server.arg("e").toInt();
  if (e < 0 || e >= NUM_EMO || id == 0 || id > lastId) { server.send(400, "text/plain", "invalide"); return; }

  Msg& m = msgs[(id - 1) % MAX_MSG];
  if (m.id != id) { server.send(404, "text/plain", "trop ancien"); return; }

  uint16_t uid = users[me].uid;
  users[me].seen = millis();

  int k = -1;
  for (int i = 0; i < m.nr; i++) if (m.r[i].uid == uid) { k = i; break; }

  if (k >= 0 && m.r[k].e == e) {
    m.r[k] = m.r[m.nr - 1];        // même emoji : on retire
    m.nr--;
  } else if (k >= 0) {
    m.r[k].e = e;                  // autre emoji : on remplace
  } else if (m.nr < MAX_REAC) {
    m.r[m.nr].uid = uid;
    m.r[m.nr].e = e;
    m.nr++;
  } else {
    server.send(409, "text/plain", "plein");
    return;
  }

  m.rev = ++msgRev;
  server.send(200, "text/plain", "ok");
}

// GET /poll?tok=&s=&ty=
// H \t boot \t millis \t monUid \t msgRev
// U \t uid \t activité(0 rien, 1 écrit, 2 dessine) \t pseudo
// M \t id \t rev \t ms \t uid \t réactions \t type(T texte, D dessin, X dessin expiré) \t pseudo \t texte
void handlePoll() {
  uint32_t now = millis();
  int me = findUser(server.arg("tok"));
  if (me >= 0) {
    users[me].seen = now;
    int ty = server.arg("ty").toInt();
    if (ty == 1 || ty == 2) { users[me].act = ty; users[me].actUntil = now + ACT_MS; }
    else users[me].actUntil = 0;
  }

  uint32_t s = strtoul(server.arg("s").c_str(), nullptr, 10);
  if (s > msgRev) s = 0;

  uint32_t first = (lastId > MAX_MSG) ? lastId - MAX_MSG + 1 : 1;

  // Réponse envoyée par morceaux d'environ 1 Ko : pas de gros bloc mémoire même avec 150 messages
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", "");

  String& out = pbuf;
  out = "";

  out += "H\t";
  out += bootTag;            out += '\t';
  out += (unsigned long)now; out += '\t';
  out += (unsigned int)(me >= 0 ? users[me].uid : 0); out += '\t';
  out += (unsigned long)msgRev; out += '\n';

  for (int i = 0; i < MAX_USERS; i++) {
    const User& u = users[i];
    if (!u.used || now - u.seen > ONLINE_MS) continue;
    out += "U\t";
    out += (unsigned int)u.uid; out += '\t';
    out += ((int32_t)(u.actUntil - now) > 0) ? (char)('0' + u.act) : '0';
    out += '\t';
    out += u.nick; out += '\n';
  }
  pflush(false);

  for (uint32_t id = first; id <= lastId; id++) {
    const Msg& m = msgs[(id - 1) % MAX_MSG];
    if (m.rev <= s) continue;
    out += "M\t";
    out += (unsigned long)m.id;  out += '\t';
    out += (unsigned long)m.rev; out += '\t';
    out += (unsigned long)m.ms;  out += '\t';
    out += (unsigned int)m.uid;  out += '\t';
    for (int k = 0; k < m.nr; k++) {
      if (k) out += ',';
      out += (unsigned int)m.r[k].uid;
      out += ':';
      out += (char)('0' + m.r[k].e);
    }
    out += '\t';
    out += (m.kind == 0) ? 'T' : (drawSlotOf(m.id) >= 0 ? 'D' : 'X');
    out += '\t';
    out += m.nick; out += '\t';
    out += m.text; out += '\n';
    pflush(false);
  }

  pflush(true);
  server.sendContent("");          // fin de la réponse
}

// ---------- Setup / Loop ----------

void setup() {
  Serial.begin(115200);
  delay(200);

  // Identifiant aléatoire de ce démarrage : quand il change, les téléphones repartent de zéro
  snprintf(bootTag, sizeof(bootTag), "%08lx", (unsigned long)esp_random());
  memset(users, 0, sizeof(users));
  memset(msgs, 0, sizeof(msgs));
  memset(draws, 0, sizeof(draws));
  pbuf.reserve(1600);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, AP_MAX_CLIENTS);

  // Certaines cartes ESP32-C3 Super Mini ont une antenne faible :
  // si le réseau n'apparaît pas ou décroche, décommenter la ligne suivante.
  // WiFi.setTxPower(WIFI_POWER_8_5dBm);

  apHost = apIP.toString();
  portalUrl = "http://" + apHost + "/";

  // Option DHCP 114 (RFC 8910) : annonce directement l'adresse du portail aux téléphones
  // récents. Disponible seulement avec un cœur ESP32 récent (ESP-IDF 5.4.2 ou plus) :
  // sur un cœur plus ancien, ce bloc est simplement ignoré à la compilation.
#if defined(ESP_IDF_VERSION_VAL) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
  if (WiFi.AP.enableDhcpCaptivePortal()) Serial.println("Option DHCP 114 active");
#endif

  dns.start(53, "*", apIP);

  // Page principale
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

  // Requêtes à couper court
  server.on("/favicon.ico", handleNothing);
  server.on("/wpad.dat", handleNothing);

  // Chat
  server.on("/poll",  HTTP_GET, handlePoll);
  server.on("/d",     HTTP_GET, handleGetDraw);
  server.on("/join",  handleJoin);
  server.on("/send",  handleSend);
  server.on("/draw",  handlePostDraw);
  server.on("/react", handleReact);

  server.onNotFound(handleRedirect);
  server.begin();

  Serial.println();
  Serial.print("DROPCHAT v7 pret : ");
  Serial.println(portalUrl);
  Serial.printf("RAM libre : %u octets\n", (unsigned)ESP.getFreeHeap());
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
}
