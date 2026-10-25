/*
  DROP-BOOK v3 — livre d'or local sur ESP32 Super Mini
  v3 : plus de numéros, bouton « coller », livre plein sans effacement automatique,
       mode gardien pour sélectionner et effacer des textes
  v2 : défilement de page web normal (on remonte librement), plus de rechargement
       intempestif dans la fenêtre de connexion Android, retour au même endroit si la
       page se recharge quand même, brouillon et éditeur restaurés
  - Point d'accès Wi-Fi ouvert "DROP-BOOK", DNS captif, page sur http://4.3.2.1
    (ou http://192.168.4.1 si USE_PUBLIC_IP vaut 0)
  - Chacun laisse un texte : titre, texte, signature (pseudo libre)
  - Seul l'auteur peut modifier ou effacer son texte (jeton secret gardé par son téléphone)
  - Chaque texte peut être copié.
  - Les textes sont enregistrés dans la flash (LittleFS) et survivent au redémarrage.
    Quand la mémoire est pleine (ou au-delà de MAX_ENTRIES), le livre indique qu'il est plein :
    rien n'est effacé automatiquement. Le gardien du livre (GUARD_USER / GUARD_PASS) peut
    sélectionner et effacer des textes à tout moment.
  Bibliothèques : WiFi.h, WebServer.h, DNSServer.h, LittleFS.h (incluses dans le cœur Arduino ESP32)

  Arduino IDE : Type de carte "ESP32C3 Dev Module", USB CDC On Boot "Enabled",
  Partition Scheme "No OTA (2MB APP/2MB SPIFFS)" conseillé (le schéma par défaut fonctionne aussi,
  avec moins de place pour les textes).

  Idées de portail captif reprises de CDFER/Captive-Portal-ESP32 (licence MIT) :
  https://github.com/CDFER/Captive-Portal-ESP32
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>

// ---------- Réglages ----------
const char* AP_SSID        = "DROP-BOOK";
const char* AP_PASS        = "";   // vide = réseau ouvert (sinon 8 caractères minimum)
const int   AP_CHANNEL     = 1;
const int   AP_MAX_CLIENTS = 8;    // 10 maximum sur ESP32

#define USE_PUBLIC_IP 1            // 1 = 4.3.2.1 (portail mieux détecté), 0 = 192.168.4.1

#define MAX_ENTRIES     400        // textes gardés au maximum
#define TITLE_MAX       120        // octets UTF-8
#define NICK_MAX        48         // octets UTF-8
#define TEXT_MAX        3000       // octets UTF-8 (la page limite à 1000 caractères)
#define FREE_MARGIN     20480      // place gardée libre dans la flash (octets) : en dessous, le livre est "plein"

// Gardien du livre : peut sélectionner et effacer n'importe quel texte
const char* GUARD_USER = "marguerite";
const char* GUARD_PASS = "1226";
#define GUARD_MAX_FAILS 5          // après 5 erreurs, connexion bloquée...
#define GUARD_LOCK_MS   60000      // ...pendant 1 minute
#define POST_COOLDOWN   8000       // délai mini entre deux nouveaux textes d'un même appareil (ms)
#define SENDER_SLOTS    12

// ---------- Données ----------
uint32_t eid[MAX_ENTRIES];         // identifiants, du plus ancien au plus récent
uint32_t erev[MAX_ENTRIES];        // version (augmente à chaque modification)
uint32_t eown[MAX_ENTRIES];        // empreinte du jeton de l'auteur
int      ecount = 0;
uint32_t lastId = 0;
char     gen[9] = "00000000";

struct Sender { uint32_t ip; uint32_t t; };
Sender senders[SENDER_SLOTS];

// Tampons réutilisés (évite de gros tableaux sur la pile)
char bufNick[NICK_MAX + 1];
char bufTitle[TITLE_MAX + 1];
char bufText[TEXT_MAX + 1];

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
<title>DROP-BOOK</title>
<link rel="icon" href="data:,">
<style>
/* Livre : papier légèrement gris, encre presque noire, un seul accent (bleu d'encre) */
:root{
  --paper:#f5f5f2; --ink:#1e1f22; --soft:#6c6e72; --rule:#d6d6d0; --accent:#2c4360;
  --serif:"Iowan Old Style","Palatino Linotype",Palatino,"Book Antiqua",Georgia,"Noto Serif","Droid Serif",serif;
}
*{box-sizing:border-box;margin:0;padding:0}
/* Page web normale : c'est le document qui défile (indispensable dans la fenêtre de connexion Android) */
html{overflow-anchor:none;overscroll-behavior-y:none;background:var(--paper)}
body{min-height:100vh;background:var(--paper);color:var(--ink);font-family:var(--serif);overscroll-behavior-y:none}
button{font:inherit;color:inherit;background:none;border:none;cursor:pointer}

.mast{position:sticky;top:0;z-index:5;background:var(--paper);display:flex;align-items:baseline;justify-content:space-between;gap:12px;padding:12px max(20px,env(safe-area-inset-right)) 10px max(20px,env(safe-area-inset-left));border-bottom:1px solid var(--rule)}
.mast h1{font-size:13px;font-weight:normal;letter-spacing:.32em;text-transform:uppercase}
#count{font-size:12px;font-style:italic;color:var(--soft)}

#book{padding-bottom:22vh}
.entry{padding:10vh max(26px,env(safe-area-inset-right)) 0 max(26px,env(safe-area-inset-left))}
.entry::after{content:"*  *  *";display:block;text-align:center;color:var(--soft);letter-spacing:.3em;margin-top:9vh;font-size:.9rem}
.inner{width:100%;max-width:33em;margin:0 auto}
.entry h2{font-size:1.5rem;line-height:1.25;font-weight:bold;text-wrap:balance;overflow-wrap:anywhere}
.entry .rule{width:2.4em;height:1px;background:var(--ink);opacity:.5;margin:1.1em 0 1.2em}
.entry .txt{font-size:1.07rem;line-height:1.68;white-space:pre-wrap;overflow-wrap:anywhere;hyphens:auto;-webkit-user-select:text;user-select:text}
.entry .sig{margin-top:1.6em;font-style:italic;font-size:.92rem;color:var(--soft)}
.entry .date{font-size:.78rem;color:var(--soft);letter-spacing:.04em;margin-top:.25em}
.entry .acts{margin-top:1.5em;display:flex;flex-wrap:wrap;gap:4px 14px;font-size:.85rem;color:var(--accent)}
.entry .acts button{padding:6px 0;text-decoration:underline;text-underline-offset:3px;text-decoration-thickness:1px}
.entry .acts span{color:var(--soft);padding:6px 0}
.entry.sel .inner{box-shadow:-14px 0 0 -12px var(--accent)}
.entry.sel h2{color:var(--accent)}

/* Pied de page et mode gardien */
.foot{text-align:center;padding:0 24px 24vh;font-size:.8rem;color:var(--soft)}
.foot button{padding:8px 4px;text-decoration:underline;text-underline-offset:3px;text-decoration-thickness:1px}
#full{display:none;margin:18px max(20px,env(safe-area-inset-right)) 0 max(20px,env(safe-area-inset-left));padding:12px 16px;border:1px solid var(--ink);font-size:.92rem;line-height:1.5;text-align:center}
#login{position:fixed;inset:0;background:rgba(245,245,242,.97);display:none;align-items:center;justify-content:center;z-index:15;padding:24px}
#login form{width:100%;max-width:300px;display:flex;flex-direction:column;gap:16px}
#login h3{font-weight:normal;font-size:13px;letter-spacing:.24em;text-transform:uppercase;text-align:center}
#login input{background:transparent;border:none;border-bottom:1px solid var(--rule);font-family:var(--serif);font-size:1.05rem;padding:8px 0;color:var(--ink);border-radius:0;outline:none}
#login input:focus{border-bottom-color:var(--ink)}
#lErr{min-height:1.2em;font-size:.88rem;font-style:italic;color:#8a2f2f;text-align:center}
.lrow{display:flex;justify-content:space-between}
.lrow button{padding:8px 0;font-size:15px}
.lrow .go{color:var(--accent);font-weight:bold}
#gbar{position:fixed;left:0;right:0;bottom:0;z-index:7;background:var(--ink);color:var(--paper);display:none;align-items:center;justify-content:space-between;gap:10px;padding:12px max(18px,env(safe-area-inset-right)) calc(12px + env(safe-area-inset-bottom)) max(18px,env(safe-area-inset-left));font-size:.92rem}
#gbar button{color:var(--paper);padding:6px 0;text-decoration:underline;text-underline-offset:3px}
#gbar .gacts{display:flex;gap:16px}
body.guard #write{bottom:calc(74px + env(safe-area-inset-bottom))}
body.guard #toast{bottom:calc(136px + env(safe-area-inset-bottom))}
.entry.wait .inner{min-height:30vh;color:var(--soft);font-style:italic}

#empty{padding:30vh 24px;text-align:center;color:var(--soft);font-style:italic;line-height:1.7}

#write{position:fixed;right:calc(18px + env(safe-area-inset-right));bottom:calc(18px + env(safe-area-inset-bottom));z-index:6;background:var(--paper);border:1px solid var(--ink);border-radius:999px;padding:11px 20px;font-size:15px;letter-spacing:.04em;box-shadow:0 2px 10px rgba(30,31,34,.12)}

#sheet{position:fixed;inset:0;background:var(--paper);display:none;flex-direction:column;z-index:10;overscroll-behavior:none}
#sheet .bar{display:flex;justify-content:space-between;align-items:center;padding:12px max(20px,env(safe-area-inset-right)) 10px max(20px,env(safe-area-inset-left));border-bottom:1px solid var(--rule)}
#sheet .bar span{font-size:13px;letter-spacing:.2em;text-transform:uppercase}
#sheet .bar button{font-size:15px;padding:6px 0}
#sheet .bar .pub{color:var(--accent);font-weight:bold}
#sheet .bar .pub:disabled{opacity:.4}
#sheet form{flex:1;overflow-y:auto;padding:22px max(26px,env(safe-area-inset-right)) calc(24px + env(safe-area-inset-bottom)) max(26px,env(safe-area-inset-left));display:flex;flex-direction:column;gap:18px;max-width:38em;width:100%;margin:0 auto}
#sheet input,#sheet textarea{width:100%;background:transparent;border:none;border-bottom:1px solid var(--rule);color:var(--ink);font-family:var(--serif);padding:8px 0;border-radius:0;outline:none}
#sheet input:focus,#sheet textarea:focus{border-bottom-color:var(--ink)}
#fTitle{font-size:1.35rem;font-weight:bold}
#fText{font-size:1.05rem;line-height:1.6;min-height:42vh;resize:none}
#fNick{font-style:italic;font-size:1rem}
#sheet label{font-size:.75rem;letter-spacing:.14em;text-transform:uppercase;color:var(--soft)}
.field{display:flex;flex-direction:column;gap:4px}
#fCount{font-size:.75rem;color:var(--soft);text-align:right}
.lab{display:flex;justify-content:space-between;align-items:baseline}
#fPaste{font-size:.85rem;color:var(--accent);text-decoration:underline;text-underline-offset:3px;padding:4px 0}
#fErr{font-size:.9rem;color:#8a2f2f;font-style:italic;min-height:1.2em}

#toast{position:fixed;left:50%;bottom:calc(76px + env(safe-area-inset-bottom));transform:translateX(-50%);z-index:20;background:var(--ink);color:var(--paper);font-size:14px;padding:8px 16px;border-radius:999px;opacity:0;pointer-events:none;transition:opacity .2s}
#toast.on{opacity:1}
</style>
</head>
<body>
<header class="mast">
  <h1>Drop · Book</h1>
  <span id="count">ouverture…</span>
</header>

<div id="full">Le livre est plein.<br>De nouveaux textes pourront être écrits quand de la place aura été libérée.</div>

<main id="book"><div id="empty">Le livre est encore vide.<br>Écris la première page.</div></main>

<footer class="foot"><button id="gOpen" type="button">Gardien du livre</button></footer>

<div id="login">
  <form id="lForm" autocomplete="off">
    <h3>Gardien du livre</h3>
    <input id="lUser" placeholder="pseudo" autocapitalize="off" autocorrect="off" spellcheck="false">
    <input id="lPass" placeholder="mot de passe" type="password" inputmode="numeric">
    <div id="lErr"></div>
    <div class="lrow">
      <button id="lCancel" type="button">Annuler</button>
      <button class="go" type="submit">Entrer</button>
    </div>
  </form>
</div>

<div id="gbar">
  <span id="gInfo">Aucun texte sélectionné</span>
  <span class="gacts" id="gActs"></span>
</div>

<button id="write" type="button">Écrire</button>

<div id="sheet">
  <div class="bar">
    <button id="sCancel" type="button">Annuler</button>
    <span id="sHead">Nouveau texte</span>
    <button id="sPub" class="pub" type="button">Publier</button>
  </div>
  <form id="form" autocomplete="off">
    <div class="field">
      <label for="fTitle">Titre</label>
      <input id="fTitle" maxlength="60">
    </div>
    <div class="field">
      <div class="lab"><label for="fText">Texte</label><button id="fPaste" type="button">coller</button></div>
      <textarea id="fText" maxlength="1000"></textarea>
      <div id="fCount">0 / 1000</div>
    </div>
    <div class="field">
      <label for="fNick">Signature</label>
      <input id="fNick" maxlength="24" placeholder="ton pseudo">
    </div>
    <div id="fErr"></div>
  </form>
</div>

<div id="toast"></div>

<script>
const $ = id => document.getElementById(id);
const book = $('book'), countEl = $('count'), sheet = $('sheet'), toast = $('toast');
const fTitle = $('fTitle'), fText = $('fText'), fNick = $('fNick'), fErr = $('fErr'), sPub = $('sPub');
const mast = document.querySelector('.mast');
let emptyEl = $('empty');

// ---------- Mémoire du téléphone ----------
function load(k){ try { return localStorage.getItem(k) || ''; } catch(e){ return ''; } }
function save(k,v){ try { localStorage.setItem(k, v); } catch(e){} }
function sload(k){ try { return sessionStorage.getItem(k) || ''; } catch(e){ return ''; } }
function ssave(k,v){ try { sessionStorage.setItem(k, v); } catch(e){} }
function cookie(k){ const m = document.cookie.match(new RegExp('(?:^|; )' + k + '=([0-9a-f]{16})')); return m ? m[1] : ''; }

// Jeton d'auteur : il prouve qu'un texte est le tien (gardé dans le téléphone)
function newTok(){
  const a = new Uint8Array(8);
  try { crypto.getRandomValues(a); } catch(e){ for (let i = 0; i < 8; i++) a[i] = Math.random() * 256 | 0; }
  return Array.from(a, b => b.toString(16).padStart(2,'0')).join('');
}
let tok = load('book_tok');
if (!/^[0-9a-f]{16}$/.test(tok)) tok = cookie('bt');
if (!/^[0-9a-f]{16}$/.test(tok)) tok = newTok();
save('book_tok', tok);
document.cookie = 'bt=' + tok + '; max-age=31536000; path=/; SameSite=Lax';

function post(url, params){
  const body = Object.keys(params).map(k => k + '=' + encodeURIComponent(params[k])).join('&');
  return fetch(url, { method:'POST', headers:{ 'Content-Type':'application/x-www-form-urlencoded' }, body });
}

let tmo = null;
function say(t){
  toast.textContent = t;
  toast.classList.add('on');
  clearTimeout(tmo);
  tmo = setTimeout(() => toast.classList.remove('on'), 1800);
}

const MONTHS = ['janvier','février','mars','avril','mai','juin','juillet','août','septembre','octobre','novembre','décembre'];
function dateFr(t){
  if (!t) return '';
  const d = new Date(t * 1000);
  return d.getDate() + ' ' + MONTHS[d.getMonth()] + ' ' + d.getFullYear();
}

// ---------- Garder sa place pendant que la page change ----------
// Repère le premier texte visible, applique la modification, puis recale la vue dessus.
function topLine(){ return mast.offsetHeight; }
function anchor(){
  const lim = topLine();
  for (const el of book.children) {
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

// Mémorise le texte en cours de lecture (pour revenir au même endroit après un rechargement)
let posTimer = null;
window.addEventListener('scroll', () => {
  clearTimeout(posTimer);
  posTimer = setTimeout(() => {
    const a = anchor();
    if (a && a._o) ssave('book_pos', a._o.id + ':' + Math.round(a.getBoundingClientRect().top));
    else ssave('book_pos', '');
  }, 250);
}, { passive: true });

// ---------- Textes ----------
const pages = new Map();           // id -> { el, rev, mine, data, want }
let gen = null, busy = false, again = false, goTop = false, first = true, isFull = false;

// ---------- Mode gardien (sélection et suppression de textes) ----------
let guard = null;                  // { u, p } une fois connecté
const picked = new Set();
try { const g = JSON.parse(sload('book_guard') || 'null'); if (g && g.u && g.p) guard = g; } catch(e){}

function refreshActs(){
  pages.forEach(o => {
    if (!o.acts) return;
    o.el.classList.toggle('sel', picked.has(o.id));
    fillActs(o, o.acts);
  });
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
  $('gOpen').textContent = guard ? 'Quitter le mode gardien' : 'Gardien du livre';
  if (!guard) return;
  const n = picked.size;
  $('gInfo').textContent = n === 0 ? 'Touche « sélectionner » sous un texte' : n === 1 ? '1 texte sélectionné' : n + ' textes sélectionnés';
  if (!n) { acts.replaceChildren(btn('quitter', leaveGuard)); return; }
  acts.replaceChildren(
    btn('effacer', () => {
      $('gInfo').textContent = 'Effacer ' + (n === 1 ? 'ce texte' : 'ces ' + n + ' textes') + ' ?';
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
      say(n === 1 ? 'Texte effacé' : n + ' textes effacés');
      poll();
    } else if (r.status === 403) {
      say('Identifiants refusés'); leaveGuard();
    } else if (r.status === 429) {
      say('Trop d’essais, patiente une minute');
    } else say('Échec, réessaie.');
  } catch(e) { say('Connexion perdue, réessaie.'); }
  refreshActs();
}

function leaveGuard(){
  guard = null; picked.clear(); ssave('book_guard', '');
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
      guard = { u, p }; ssave('book_guard', JSON.stringify(guard));
      $('login').style.display = 'none';
      refreshActs();
      say('Mode gardien');
      return;
    }
    $('lErr').textContent = r.status === 429 ? 'Trop d’essais, patiente une minute.' : 'Pseudo ou mot de passe incorrect.';
  } catch(err) { $('lErr').textContent = 'Connexion perdue, réessaie.'; }
});

function makeEntry(id){
  const el = document.createElement('article');
  el.className = 'entry wait';
  el.innerHTML = '<div class="inner">…</div>';
  const o = { id, el, rev: 0, mine: false, data: null, want: 0 };
  el._o = o;
  if (io) io.observe(el);
  return o;
}

function render(o){
  const d = o.data;
  if (!d) return;
  const inner = document.createElement('div');
  inner.className = 'inner';

  const h = document.createElement('h2'); h.textContent = d.title;
  const r = document.createElement('div'); r.className = 'rule';
  const t = document.createElement('div'); t.className = 'txt'; t.textContent = d.text;
  const s = document.createElement('div'); s.className = 'sig'; s.textContent = '— ' + d.nick;
  const dt = document.createElement('div'); dt.className = 'date';
  dt.textContent = dateFr(d.created) + (d.edited && d.edited !== d.created ? ' · modifié' : '');
  const acts = document.createElement('div'); acts.className = 'acts';
  o.acts = acts;
  fillActs(o, acts);

  inner.append(h, r, t, s, dt, acts);
  keepView(() => {
    o.el.className = 'entry' + (picked.has(o.id) ? ' sel' : '');
    o.el.replaceChildren(inner);
  });
}

function btn(label, fn){
  const b = document.createElement('button');
  b.type = 'button'; b.textContent = label;
  b.addEventListener('click', fn);
  return b;
}

function fillActs(o, acts){
  acts.replaceChildren(btn('copier', () => copyEntry(o)));
  if (guard) {
    const on = picked.has(o.id);
    acts.append(btn(on ? '✓ sélectionné' : 'sélectionner', () => togglePick(o)));
  }
  if (!o.mine) return;
  acts.append(
    btn('modifier', () => openSheet(o)),
    btn('effacer', () => {
      const q = document.createElement('span'); q.textContent = 'Effacer ce texte ?';
      acts.replaceChildren(q,
        btn('oui, effacer', () => del(o)),
        btn('non', () => fillActs(o, acts)));
    })
  );
}

// ---------- Copier (marche aussi sans HTTPS) ----------
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
async function copyEntry(o){
  const d = o.data;
  const s = d.title + '\n\n' + d.text + '\n\n— ' + d.nick;
  let ok = false;
  if (navigator.clipboard && window.isSecureContext) {
    try { await navigator.clipboard.writeText(s); ok = true; } catch(e){}
  }
  if (!ok) ok = copyText(s);
  say(ok ? 'Texte copié' : 'Copie impossible : sélectionne le texte à la main');
}

// ---------- Chargement des textes ----------
// Ceux qui sont à l'écran passent en priorité, les autres se chargent ensuite un par un.
const queue = [];
let qBusy = false;
const io = ('IntersectionObserver' in window) ? new IntersectionObserver(es => {
  es.forEach(e => { if (e.isIntersecting) want(e.target._o, true); });
}, { rootMargin: '100% 0px' }) : null;

function want(o, urgent){
  if (!o || (o.data && o.want === o.rev)) return;
  const i = queue.indexOf(o);
  if (i >= 0) { if (!urgent) return; queue.splice(i, 1); }
  if (urgent) queue.unshift(o); else queue.push(o);
  runQ();
}

async function fetchEntry(o){
  const rev = o.rev;
  const r = await fetch('/e?g=' + gen + '&id=' + o.id + '&r=' + rev);
  if (!r.ok) throw r.status;
  const lines = (await r.text()).split('\n');
  const hd = lines[0].split('\t');
  o.data = { created: +hd[0], edited: +hd[1], nick: lines[1] || '', title: lines[2] || '', text: lines.slice(3).join('\n') };
  o.want = rev;
  if (pages.get(o.id) === o) render(o);
  if (o.rev !== rev) want(o, false);    // modifié pendant le chargement
}

async function runQ(){
  if (qBusy) return;
  const o = queue.shift();
  if (!o) return;
  qBusy = true;
  try { if (pages.get(o.id) === o) await fetchEntry(o); }
  catch(e) { if (e !== 404) setTimeout(() => want(o, false), 3000); }
  qBusy = false;
  runQ();
}

// ---------- Synchronisation avec le livre ----------
// Format : "H \t gen \t nombre \t max"  puis  "id \t version \t mien(0/1)" du plus ancien au plus récent
function sync(body){
  const lines = body.split('\n');
  const h = lines[0].split('\t');
  if (h[0] !== 'H') return;

  if (gen !== null && h[1] !== gen) { pages.forEach(o => o.el.remove()); pages.clear(); }
  gen = h[1];

  const seen = new Set();
  const fresh = [];
  for (let i = 1; i < lines.length; i++) {
    if (!lines[i]) continue;
    const p = lines[i].split('\t');
    const id = +p[0], rev = +p[1], mine = p[2] === '1';
    seen.add(id);
    let o = pages.get(id);
    if (!o) { o = makeEntry(id); o.rev = rev; o.mine = mine; pages.set(id, o); fresh.push(o); continue; }
    if (o.mine !== mine) { o.mine = mine; if (o.data) render(o); }
    if (o.rev !== rev) { o.rev = rev; if (o.data) want(o, true); }
  }

  keepView(() => {
    // Textes effacés, ou disparus quand la mémoire est pleine
    pages.forEach((o, id) => {
      if (seen.has(id)) return;
      if (io) io.unobserve(o.el);
      o.el.remove(); pages.delete(id);
    });
    // Nouveaux textes : le plus récent en haut
    if (fresh.length) {
      if (emptyEl) { emptyEl.remove(); emptyEl = null; }
      fresh.sort((a, b) => a.id - b.id);
      fresh.forEach(o => {
        let after = null;
        for (const x of pages.values()) {
          if (x.id > o.id && x.el.isConnected && (!after || x.id < after.id)) after = x;
        }
        if (after) book.insertBefore(o.el, after.el.nextSibling);
        else book.insertBefore(o.el, book.firstChild);
      });
    }
  });

  // Tout le livre se charge petit à petit, du plus récent au plus ancien
  [...pages.values()].sort((a, b) => b.id - a.id).forEach(o => { if (!o.data) want(o, false); });

  if (!pages.size && !emptyEl) {
    emptyEl = document.createElement('div');
    emptyEl.id = 'empty';
    emptyEl.innerHTML = 'Le livre est encore vide.<br>Écris la première page.';
    book.appendChild(emptyEl);
  }

  if (goTop) { goTop = false; window.scrollTo(0, 0); }
  if (first) { first = false; restore(); }

  isFull = h[4] === '1';
  $('full').style.display = isFull ? 'block' : 'none';
  pages.forEach((o, id) => { if (!seen.has(id)) picked.delete(id); });
  for (const id of picked) if (!pages.has(id)) picked.delete(id);
  renderGuard();

  const n = +h[2];
  countEl.textContent = n === 0 ? 'aucun texte' : n === 1 ? '1 texte' : n + ' textes';
}

// Après un rechargement : retour au texte qu'on lisait, et à l'éditeur s'il était ouvert
function restore(){
  const pos = sload('book_pos').split(':');
  const o = pages.get(+pos[0]);
  if (o) {
    window.scrollTo(0, o.el.getBoundingClientRect().top + window.scrollY - (+pos[1] || topLine()));
    want(o, true);
  }
  const ed = sload('book_ed');
  if (ed === 'new') openSheet(null);
  else if (ed) {
    const e = pages.get(+ed);
    if (e && e.mine) {
      if (e.data) openSheet(e);
      else fetchEntry(e).then(() => openSheet(e)).catch(() => {});
    }
  }
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

// ---------- Écrire / modifier ----------
let editing = null;

function updCount(){ $('fCount').textContent = fText.value.length + ' / 1000'; }

function draft(){
  try { return JSON.parse(sload('book_draft') || '{}') || {}; } catch(e){ return {}; }
}
function saveDraft(){
  ssave('book_draft', JSON.stringify({ id: editing ? editing.id : 0, t: fTitle.value, x: fText.value, n: fNick.value }));
}

function openSheet(o){
  editing = o || null;
  fErr.textContent = '';
  const d = draft();
  const id = o ? o.id : 0;
  const useDraft = d.id === id && (d.t || d.x);
  if (o && o.data) {
    $('sHead').textContent = 'Modifier';
    sPub.textContent = 'Enregistrer';
    fTitle.value = useDraft ? d.t : o.data.title;
    fText.value  = useDraft ? d.x : o.data.text;
    fNick.value  = useDraft && d.n ? d.n : o.data.nick;
  } else {
    $('sHead').textContent = 'Nouveau texte';
    sPub.textContent = 'Publier';
    fTitle.value = useDraft ? d.t : '';
    fText.value  = useDraft ? d.x : '';
    fNick.value  = useDraft && d.n ? d.n : load('book_nick');
  }
  updCount();
  ssave('book_ed', o ? String(o.id) : 'new');
  sheet.style.display = 'flex';
}

function closeSheet(clear){
  sheet.style.display = 'none';
  editing = null;
  ssave('book_ed', '');
  if (clear) ssave('book_draft', '');
}

$('write').addEventListener('click', () => {
  if (isFull) { say('Le livre est plein'); window.scrollTo(0, 0); return; }
  openSheet(null);
});

// Coller : direct quand le navigateur l'autorise, sinon on guide vers l'appui long
$('fPaste').addEventListener('click', async () => {
  let txt = null;
  if (navigator.clipboard && navigator.clipboard.readText && window.isSecureContext) {
    try { txt = await navigator.clipboard.readText(); } catch(e){ txt = null; }
  }
  if (txt) {
    const s = fText.selectionStart ?? fText.value.length, e2 = fText.selectionEnd ?? s;
    fText.value = (fText.value.slice(0, s) + txt + fText.value.slice(e2)).slice(0, 1000);
    updCount(); saveDraft();
    return;
  }
  fText.focus();
  say('Appui long dans le texte, puis « Coller »');
});
$('sCancel').addEventListener('click', () => closeSheet(true));
fText.addEventListener('input', () => { updCount(); saveDraft(); });
fTitle.addEventListener('input', saveDraft);
fNick.addEventListener('input', saveDraft);
$('form').addEventListener('submit', e => e.preventDefault());

sPub.addEventListener('click', async () => {
  const ti = fTitle.value.trim(), tx = fText.value.trim(), n = fNick.value.trim();
  if (!ti) { fErr.textContent = 'Il manque un titre.'; fTitle.focus(); return; }
  if (!tx) { fErr.textContent = 'Il manque le texte.'; fText.focus(); return; }
  if (!n)  { fErr.textContent = 'Signe ton texte avec un pseudo.'; fNick.focus(); return; }
  save('book_nick', n);
  sPub.disabled = true;
  fErr.textContent = '';
  const t = Math.floor(Date.now() / 1000);
  try {
    const r = editing
      ? await post('/edit', { tok, id: editing.id, ti, tx, n, t })
      : await post('/new', { tok, ti, tx, n, t });
    if (r.ok) {
      const wasEdit = !!editing;
      if (!wasEdit) goTop = true;
      closeSheet(true);
      say(wasEdit ? 'Texte modifié' : 'Texte publié');
      poll();
    } else {
      fErr.textContent = r.status === 429 ? 'Patiente quelques secondes avant de publier à nouveau.' :
                         r.status === 403 ? 'Seul l’auteur peut modifier ce texte.' :
                         r.status === 404 ? 'Ce texte n’existe plus.' :
                         r.status === 507 ? 'Le livre est plein : impossible d’ajouter un texte pour le moment.' :
                         'Échec de l’enregistrement, réessaie.';
    }
  } catch(e) {
    fErr.textContent = 'Connexion perdue, réessaie.';
  }
  sPub.disabled = false;
});

async function del(o){
  try {
    const r = await post('/del', { tok, id: o.id });
    if (r.ok) { say('Texte effacé'); poll(); return; }
    say(r.status === 403 ? 'Seul l’auteur peut effacer ce texte.' : 'Échec, réessaie.');
  } catch(e) { say('Connexion perdue, réessaie.'); }
  if (o.data) render(o);
}

// ---------- Démarrage ----------
if ('scrollRestoration' in history) history.scrollRestoration = 'manual';
document.addEventListener('visibilitychange', () => { if (!document.hidden) poll(); });
function tick(){ if (!document.hidden) poll(); setTimeout(tick, 15000); }
poll();
setTimeout(tick, 15000);
</script>
</body>
</html>
)rawliteral";

// ---------- Outils ----------

// Copie nettoyée. multi = garde les retours à la ligne (2 lignes vides maximum d'affilée).
// Coupe proprement sans casser un caractère UTF-8.
size_t cleanText(const String& in, char* out, size_t maxBytes, bool multi) {
  const char* s = in.c_str();
  size_t len = in.length(), i = 0, n = 0;
  int nl = 0;

  while (i < len && (uint8_t)s[i] <= ' ') i++;

  for (; i < len && n < maxBytes; i++) {
    char c = s[i];
    if (c == '\r') continue;
    if (c == '\n') {
      if (multi) {
        if (++nl > 3) continue;
        out[n++] = '\n';
        continue;
      }
      c = ' ';
    } else if (c == '\t') {
      c = ' ';
    } else if ((uint8_t)c < 32 || c == 127) {
      continue;
    }
    nl = 0;
    out[n++] = c;
  }

  if (i < len && ((uint8_t)s[i] & 0xC0) == 0x80) {
    while (n > 0 && ((uint8_t)out[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0) n--;
  }

  while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\n')) n--;
  out[n] = 0;
  return n;
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
// Un fichier par texte : /b/<id>.txt
//   ligne 1 : jeton de l'auteur
//   ligne 2 : création \t modification \t version
//   ligne 3 : signature
//   ligne 4 : titre
//   reste   : texte

String entryPath(uint32_t id) { return "/b/" + String(id) + ".txt"; }

size_t freeBytes() {
  size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
  return used < total ? total - used : 0;
}

bool bookFull() {
  return ecount >= MAX_ENTRIES || freeBytes() < FREE_MARGIN;
}

int findEntry(uint32_t id) {
  for (int i = 0; i < ecount; i++) if (eid[i] == id) return i;
  return -1;
}

void removeAt(int k) {
  LittleFS.remove(entryPath(eid[k]));
  for (int i = k; i < ecount - 1; i++) { eid[i] = eid[i + 1]; erev[i] = erev[i + 1]; eown[i] = eown[i + 1]; }
  ecount--;
}

bool writeEntry(uint32_t id, const char* tok, uint32_t created, uint32_t edited, uint32_t rev) {
  File f = LittleFS.open("/b/tmp", "w");
  if (!f) return false;
  f.print(tok);                     f.print('\n');
  f.print((unsigned long)created);  f.print('\t');
  f.print((unsigned long)edited);   f.print('\t');
  f.print((unsigned long)rev);      f.print('\n');
  f.print(bufNick);                 f.print('\n');
  f.print(bufTitle);                f.print('\n');
  size_t want = strlen(bufText);
  size_t done = f.print(bufText);
  f.close();
  if (done != want) { LittleFS.remove("/b/tmp"); return false; }
  String p = entryPath(id);
  LittleFS.remove(p);
  return LittleFS.rename("/b/tmp", p);
}

// Lit le jeton (ligne 1) et la date de création d'un texte
bool readHead(uint32_t id, String& tok, uint32_t& created) {
  File f = LittleFS.open(entryPath(id), "r");
  if (!f) return false;
  tok = f.readStringUntil('\n');
  String hd = f.readStringUntil('\n');
  f.close();
  created = strtoul(hd.c_str(), nullptr, 10);
  return true;
}

void loadGen() {
  File f = LittleFS.open("/gen", "r");
  if (f && f.size() == 8) { f.readBytes(gen, 8); gen[8] = 0; f.close(); return; }
  if (f) f.close();
  snprintf(gen, sizeof(gen), "%08lx", (unsigned long)esp_random());
  f = LittleFS.open("/gen", "w");
  if (f) { f.print(gen); f.close(); }
}

// Relit l'index des textes au démarrage
void scanEntries() {
  if (!LittleFS.exists("/b")) LittleFS.mkdir("/b");
  LittleFS.remove("/b/tmp");

  ecount = 0; lastId = 0;
  File dir = LittleFS.open("/b");
  if (!dir) return;

  File f = dir.openNextFile();
  while (f) {
    String name = f.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    unsigned long id = 0, created = 0, edited = 0, rev = 1;
    String tok;
    if (sscanf(name.c_str(), "%lu.txt", &id) == 1 && id > 0) {
      tok = f.readStringUntil('\n');
      String hd = f.readStringUntil('\n');
      sscanf(hd.c_str(), "%lu\t%lu\t%lu", &created, &edited, &rev);
    }
    f.close();

    if (id > 0) {
      if (id > lastId) lastId = id;
      uint32_t own = tokHash(tok.c_str());
      if (ecount < MAX_ENTRIES) {
        int k = ecount++;
        while (k > 0 && eid[k - 1] > id) { eid[k] = eid[k - 1]; erev[k] = erev[k - 1]; eown[k] = eown[k - 1]; k--; }
        eid[k] = id; erev[k] = rev; eown[k] = own;
      } else if (id > eid[0]) {
        LittleFS.remove(entryPath(eid[0]));
        int k = 0;
        while (k < ecount - 1 && eid[k + 1] < id) { eid[k] = eid[k + 1]; erev[k] = erev[k + 1]; eown[k] = eown[k + 1]; k++; }
        eid[k] = id; erev[k] = rev; eown[k] = own;
      } else {
        LittleFS.remove("/b/" + name);
      }
    }
    f = dir.openNextFile();
  }
  dir.close();
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

// ---------- Routes : livre ----------

// GET /list?tok=
void handleList() {
  String tok = server.arg("tok");
  uint32_t mine = validTok(tok) ? tokHash(tok.c_str()) : 0;

  String out;
  out.reserve(40 + ecount * 18);
  out += "H\t"; out += gen; out += '\t';
  out += ecount; out += '\t';
  out += MAX_ENTRIES; out += '\t';
  out += bookFull() ? '1' : '0'; out += '\n';
  for (int i = 0; i < ecount; i++) {
    out += (unsigned long)eid[i];  out += '\t';
    out += (unsigned long)erev[i]; out += '\t';
    out += (mine && eown[i] == mine) ? '1' : '0';
    out += '\n';
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", out);
}

// GET /e?id=  -> texte sans le jeton de l'auteur
void handleEntry() {
  uint32_t id = strtoul(server.arg("id").c_str(), nullptr, 10);
  if (findEntry(id) < 0) { server.send(404, "text/plain", "absent"); return; }
  File f = LittleFS.open(entryPath(id), "r");
  if (!f) { server.send(404, "text/plain", "absent"); return; }
  f.readStringUntil('\n');         // on ne transmet jamais le jeton
  String out;
  out.reserve(f.available() + 4);
  while (f.available()) {
    char tmp[256];
    int n = f.read((uint8_t*)tmp, sizeof(tmp) - 1);
    if (n <= 0) break;
    tmp[n] = 0;
    out += tmp;
  }
  f.close();
  // L'adresse contient la version : le téléphone peut garder le texte en cache
  server.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
  server.send(200, "text/plain; charset=utf-8", out);
}

// Lit et vérifie titre / texte / signature envoyés par la page
bool readFields() {
  cleanText(server.arg("ti"), bufTitle, TITLE_MAX, false);
  cleanText(server.arg("tx"), bufText, TEXT_MAX, true);
  cleanText(server.arg("n"), bufNick, NICK_MAX, false);
  return bufTitle[0] && bufText[0] && bufNick[0];
}

// POST /new  tok, ti, tx, n, t
void handleNew() {
  if (!isPost()) return;
  String tok = server.arg("tok");
  if (!validTok(tok)) { server.send(400, "text/plain", "jeton"); return; }
  if (!readFields()) { server.send(400, "text/plain", "champs"); return; }
  if (!allowSender((uint32_t)server.client().remoteIP(), millis())) { server.send(429, "text/plain", "trop vite"); return; }

  // Livre plein : rien n'est effacé automatiquement, on refuse simplement le nouveau texte
  if (bookFull()) { server.send(507, "text/plain", "plein"); return; }

  uint32_t id = lastId + 1;
  uint32_t t = clientTime();
  if (!writeEntry(id, tok.c_str(), t, t, 1)) { server.send(500, "text/plain", "flash"); return; }

  lastId = id;
  eid[ecount] = id; erev[ecount] = 1; eown[ecount] = tokHash(tok.c_str());
  ecount++;

  Serial.printf("texte #%lu \"%s\" par %s - %d textes, %u octets libres\n",
                (unsigned long)id, bufTitle, bufNick, ecount, (unsigned)freeBytes());
  server.send(200, "text/plain", String(id));
}

// POST /edit  tok, id, ti, tx, n, t
void handleEdit() {
  if (!isPost()) return;
  String tok = server.arg("tok");
  uint32_t id = strtoul(server.arg("id").c_str(), nullptr, 10);
  int k = findEntry(id);
  if (k < 0) { server.send(404, "text/plain", "absent"); return; }

  String owner; uint32_t created = 0;
  if (!validTok(tok) || !readHead(id, owner, created) || owner != tok) { server.send(403, "text/plain", "pas l'auteur"); return; }
  if (!readFields()) { server.send(400, "text/plain", "champs"); return; }

  uint32_t rev = erev[k] + 1;
  uint32_t t = clientTime();
  if (!writeEntry(id, tok.c_str(), created, t ? t : created, rev)) { server.send(500, "text/plain", "flash"); return; }
  erev[k] = rev;

  Serial.printf("texte #%lu modifie\n", (unsigned long)id);
  server.send(200, "text/plain", "ok");
}

// POST /del  tok, id
void handleDel() {
  if (!isPost()) return;
  String tok = server.arg("tok");
  uint32_t id = strtoul(server.arg("id").c_str(), nullptr, 10);
  int k = findEntry(id);
  if (k < 0) { server.send(404, "text/plain", "absent"); return; }

  String owner; uint32_t created = 0;
  if (!validTok(tok) || !readHead(id, owner, created) || owner != tok) { server.send(403, "text/plain", "pas l'auteur"); return; }

  removeAt(k);
  Serial.printf("texte #%lu efface par son auteur\n", (unsigned long)id);
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
    int k = findEntry(id);
    if (k >= 0) { removeAt(k); n++; }
    start = comma + 1;
  }
  Serial.printf("gardien : %d texte(s) efface(s), %u octets libres\n", n, (unsigned)freeBytes());
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
  scanEntries();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, AP_MAX_CLIENTS);

  // Certaines cartes ESP32-C3 Super Mini ont une antenne faible :
  // si le réseau n'apparaît pas ou décroche, décommenter la ligne suivante.
  // WiFi.setTxPower(WIFI_POWER_8_5dBm);

  apHost = apIP.toString();
  portalUrl = "http://" + apHost + "/";

#if defined(ESP_IDF_VERSION_VAL) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
  if (WiFi.AP.enableDhcpCaptivePortal()) Serial.println("Option DHCP 114 active");
#endif

  dns.start(53, "*", apIP);

  server.on("/", handlePage);

  // Détection de portail captif
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

  // Livre
  server.on("/list", HTTP_GET, handleList);
  server.on("/e",    HTTP_GET, handleEntry);
  server.on("/new",  handleNew);
  server.on("/edit", handleEdit);
  server.on("/del",  handleDel);
  server.on("/mlogin", handleGuardLogin);
  server.on("/mdel",   handleGuardDelete);

  server.onNotFound(handleRedirect);
  server.begin();

  Serial.println();
  Serial.print("DROP-BOOK pret : ");
  Serial.println(portalUrl);
  Serial.printf("%d textes, flash : %u octets libres sur %u\n",
                ecount, (unsigned)freeBytes(), (unsigned)LittleFS.totalBytes());
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
}
