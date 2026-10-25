/*
  DROP CHESS — échecs local sur ESP32 Super Mini
  - Contre le bot (3 niveaux) : le bot tourne dans le téléphone, l'ESP32 n'est pas sollicité ;
    il prend un temps de réflexion, sélectionne sa pièce puis la fait glisser
  - À deux sur le Wi-Fi : l'ESP32 garde la partie en cours et relaie les coups ;
    les autres personnes connectées peuvent regarder
  - Règles complètes : roque, prise en passant, promotion au choix, échec et mat, pat,
    règle des 50 coups, triple répétition, matériel insuffisant (moteur vérifié par tests "perft")
  - Point d'accès Wi-Fi ouvert "DROP CHESS", DNS captif, page sur http://4.3.2.1
    (ou http://192.168.4.1 si USE_PUBLIC_IP vaut 0)
  Bibliothèques : WiFi.h, WebServer.h, DNSServer.h (incluses dans le cœur Arduino ESP32)
  Idées de portail captif reprises de CDFER/Captive-Portal-ESP32 (licence MIT) :
  https://github.com/CDFER/Captive-Portal-ESP32
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

const char* AP_SSID = "DROP CHESS";
const char* AP_PASS = "";          // vide = réseau ouvert
#define USE_PUBLIC_IP 1            // 1 = 4.3.2.1 (portail mieux détecté), 0 = 192.168.4.1

#define MAX_MOVES   600            // coups mémorisés pour la partie à deux
#define SEAT_STALE  180000UL       // une place inactive depuis 3 min se libère

WebServer server(80);
DNSServer dns;
#if USE_PUBLIC_IP
const IPAddress apIP(4, 3, 2, 1);
#else
const IPAddress apIP(192, 168, 4, 1);
#endif
String apHost;
String portalUrl;

// ---------- Partie à deux (en RAM) ----------
String   moves;                    // coups en notation UCI séparés par des espaces ("e2e4 e7e5 ...")
int      nMoves = 0;
uint32_t seq = 1;                  // augmente à chaque changement : les téléphones se mettent à jour
char     tokW[17] = "", tokB[17] = "";
uint32_t seenW = 0, seenB = 0;
char     result = '-';             // '-' en cours, 'w' les blancs gagnent par abandon, 'b' les noirs

const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no, viewport-fit=cover">
<title>DROP CHESS</title>
<link rel="icon" href="data:,">
<style>
:root { --bg:#f3efe6; --ink:#2b2622; --soft:#7a7065; --light:#ecdcbc; --dark:#b38b62; --sel:#f6d36b; --last:rgba(246,211,107,.55); --chk:#e05a4a; --accent:#2f5d50; }
* { box-sizing: border-box; margin: 0; padding: 0; }
/* Document 2 px plus haut que l'écran, gardé à 1 px : pas de rechargement par glissement */
html { background: var(--bg); overscroll-behavior: none; }
body { min-height: calc(100vh + 2px); background: var(--bg); color: var(--ink); font-family: Georgia, "Times New Roman", serif;
       overscroll-behavior: none; touch-action: none; -webkit-user-select: none; user-select: none; -webkit-touch-callout: none; }
.app { position: fixed; inset: 0; display: flex; flex-direction: column; align-items: center; gap: 10px;
       padding: calc(12px + env(safe-area-inset-top)) 12px calc(12px + env(safe-area-inset-bottom)); }
h1 { font-size: 15px; letter-spacing: .35em; font-weight: normal; }
.view { display: none; width: 100%; max-width: 520px; flex-direction: column; align-items: center; gap: 10px; flex: 1; min-height: 0; }
.view.on { display: flex; }
.btn { font-family: inherit; font-size: 16px; color: var(--ink); background: #fff; border: 1.5px solid var(--ink); border-radius: 10px; padding: 12px 16px; width: 100%; box-shadow: 0 2px 0 var(--ink); }
.btn:active { transform: translateY(2px); box-shadow: none; }
.btn.main { background: var(--accent); color: #fff; border-color: var(--accent); box-shadow: 0 2px 0 #1c3a31; }
.btn.sm { width: auto; font-size: 14px; padding: 8px 12px; }
.row { display: flex; gap: 8px; width: 100%; }
.row .btn { flex: 1; }
.card { width: 100%; background: #fff; border: 1.5px solid var(--ink); border-radius: 12px; padding: 14px; display: flex; flex-direction: column; gap: 10px; }
.card h2 { font-size: 18px; font-weight: normal; }
.card p { font-size: 14px; color: var(--soft); line-height: 1.4; }
.seg { display: flex; border: 1.5px solid var(--ink); border-radius: 10px; overflow: hidden; }
.seg button { flex: 1; font-family: inherit; font-size: 14px; padding: 9px 4px; background: #fff; border: none; color: var(--ink); }
.seg button + button { border-left: 1.5px solid var(--ink); }
.seg button.on { background: var(--ink); color: #fff; }
.label { font-size: 12px; color: var(--soft); letter-spacing: .1em; text-transform: uppercase; }

.bar { width: 100%; display: flex; justify-content: space-between; align-items: center; font-size: 14px; min-height: 22px; }
.who { display: flex; align-items: center; gap: 6px; }
.dot { width: 12px; height: 12px; border-radius: 50%; border: 1.5px solid var(--ink); }
.dot.w { background: #fff; } .dot.b { background: var(--ink); }
.cap { font-size: 16px; letter-spacing: -2px; color: var(--soft); min-height: 20px; }
#status { font-size: 15px; text-align: center; min-height: 22px; }
#status.alert { color: var(--chk); }

.boardBox { flex: 1; min-height: 0; width: 100%; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 6px; }
#board { position: relative; display: grid; grid-template-columns: repeat(8, 1fr); grid-template-rows: repeat(8, 1fr); border: 2px solid var(--ink); border-radius: 4px; overflow: hidden; touch-action: none; }
.sq { position: relative; min-width: 0; min-height: 0; overflow: hidden; display: flex; align-items: center; justify-content: center; line-height: 1; }
.sq.l { background: var(--light); } .sq.d { background: var(--dark); }
.sq.last { box-shadow: inset 0 0 0 100px var(--last); }
.sq.sel { box-shadow: inset 0 0 0 100px var(--sel); }
.sq.chk { box-shadow: inset 0 0 0 100px rgba(224,90,74,.75); }
.sq .pc { position: relative; z-index: 1; }
.pc.w { color: #fff; -webkit-text-stroke: 1.3px var(--ink); text-shadow: 0 1px 0 rgba(0,0,0,.25); }
.pc.b { color: var(--ink); text-shadow: 0 1px 0 rgba(255,255,255,.25); }
.sq .hint { position: absolute; width: 28%; height: 28%; border-radius: 50%; background: rgba(43,38,34,.28); }
.sq .ring { position: absolute; inset: 6%; border-radius: 50%; border: 4px solid rgba(43,38,34,.28); }
.coord { position: absolute; font-size: 9px; font-family: monospace; opacity: .6; }
.coord.f { right: 2px; bottom: 1px; } .coord.r { left: 2px; top: 1px; }
#moves { width: 100%; font-size: 13px; color: var(--soft); white-space: nowrap; overflow-x: auto; min-height: 18px; scrollbar-width: none; }

#promo { position: fixed; inset: 0; background: rgba(43,38,34,.45); display: none; align-items: center; justify-content: center; z-index: 10; }
#promo .box { background: #fff; border: 1.5px solid var(--ink); border-radius: 12px; padding: 14px; display: flex; gap: 8px; }
#promo button { width: 58px; height: 58px; font-size: 40px; background: var(--light); border: 1.5px solid var(--ink); border-radius: 8px; line-height: 1; }
</style>
</head>
<body>
<div class="app">
  <h1>DROP CHESS</h1>

  <!-- Menu -->
  <div class="view on" id="vMenu">
    <div class="card">
      <h2>Contre le bot</h2>
      <span class="label">Niveau</span>
      <div class="seg" id="segLvl"><button data-v="1">Facile</button><button data-v="2" class="on">Moyen</button><button data-v="3">Fort</button></div>
      <span class="label">Je joue</span>
      <div class="seg" id="segCol"><button data-v="w" class="on">Blancs</button><button data-v="b">Noirs</button><button data-v="r">Au hasard</button></div>
      <button class="btn main" id="bBot" type="button">Jouer</button>
      <button class="btn" id="bResume" type="button" style="display:none">Reprendre ma partie</button>
    </div>
    <div class="card">
      <h2>À deux sur le Wi-Fi</h2>
      <p id="mpInfo">Recherche…</p>
      <button class="btn main" id="bMulti" type="button">Créer une partie</button>
    </div>
  </div>

  <!-- Partie -->
  <div class="view" id="vGame">
    <div class="boardBox">
      <div class="bar" id="topBar"><span class="who"><span class="dot" id="topDot"></span><span id="topName"></span></span><span class="cap" id="topCap"></span></div>
      <div id="board"></div>
      <div class="bar" id="botBar"><span class="who"><span class="dot" id="botDot"></span><span id="botName"></span></span><span class="cap" id="botCap"></span></div>
    </div>
    <div id="status"></div>
    <div id="moves"></div>
    <div class="row">
      <button class="btn sm" id="bUndo" type="button">Annuler</button>
      <button class="btn sm" id="bResign" type="button">Abandonner</button>
      <button class="btn sm" id="bNew" type="button">Nouvelle</button>
      <button class="btn sm" id="bMenu" type="button">Menu</button>
    </div>
  </div>
</div>

<div id="promo"><div class="box" id="promoBox"></div></div>

<script>
// ================= Moteur d'échecs (règles complètes) =================
// Plateau : 64 cases, index = rangée * 8 + colonne, rangée 0 = 8e rangée (côté noir)
// Pièces : 'PNBRQK' blanches, 'pnbrqk' noires, '' vide
const FILES = 'abcdefgh';
function sqName(i){ return FILES[i & 7] + (8 - (i >> 3)); }
function sqIdx(s){ return (8 - +s[1]) * 8 + FILES.indexOf(s[0]); }
function isW(p){ return p !== '' && p === p.toUpperCase(); }
function colorOf(p){ return p === '' ? '' : isW(p) ? 'w' : 'b'; }

function startPos(){
  const b = [];
  'rnbqkbnr'.split('').forEach(p => b.push(p));
  for (let i = 0; i < 8; i++) b.push('p');
  for (let i = 0; i < 32; i++) b.push('');
  for (let i = 0; i < 8; i++) b.push('P');
  'RNBQKBNR'.split('').forEach(p => b.push(p));
  return { b, turn: 'w', cK: true, cQ: true, ck: true, cq: true, ep: -1, half: 0, full: 1 };
}

const KN = [[-2,-1],[-2,1],[-1,-2],[-1,2],[1,-2],[1,2],[2,-1],[2,1]];
const KG = [[-1,-1],[-1,0],[-1,1],[0,-1],[0,1],[1,-1],[1,0],[1,1]];
const DIAG = [[-1,-1],[-1,1],[1,-1],[1,1]], ORTH = [[-1,0],[1,0],[0,-1],[0,1]];

// La case sq est-elle attaquée par la couleur 'by' ?
function attacked(S, sq, by){
  const b = S.b, r = sq >> 3, f = sq & 7;
  const P = by === 'w' ? 'P' : 'p', N = by === 'w' ? 'N' : 'n', B = by === 'w' ? 'B' : 'b',
        R = by === 'w' ? 'R' : 'r', Q = by === 'w' ? 'Q' : 'q', K = by === 'w' ? 'K' : 'k';
  const pr = by === 'w' ? r + 1 : r - 1;            // un pion blanc attaque vers le haut (rangée - 1)
  if (pr >= 0 && pr < 8) for (const df of [-1, 1]) { const ff = f + df; if (ff >= 0 && ff < 8 && b[pr * 8 + ff] === P) return true; }
  for (const [dr, df] of KN) { const rr = r + dr, ff = f + df; if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8 && b[rr * 8 + ff] === N) return true; }
  for (const [dr, df] of KG) { const rr = r + dr, ff = f + df; if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8 && b[rr * 8 + ff] === K) return true; }
  for (const [dr, df] of DIAG) {
    let rr = r + dr, ff = f + df;
    while (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) { const p = b[rr * 8 + ff]; if (p) { if (p === B || p === Q) return true; break; } rr += dr; ff += df; }
  }
  for (const [dr, df] of ORTH) {
    let rr = r + dr, ff = f + df;
    while (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) { const p = b[rr * 8 + ff]; if (p) { if (p === R || p === Q) return true; break; } rr += dr; ff += df; }
  }
  return false;
}
function kingSq(S, c){ const k = c === 'w' ? 'K' : 'k'; return S.b.indexOf(k); }
function inCheck(S, c){ return attacked(S, kingSq(S, c), c === 'w' ? 'b' : 'w'); }

// Coups pseudo-légaux : { f, t, p (promotion), cap, ep, castle }
function pseudo(S){
  const b = S.b, me = S.turn, out = [];
  const add = (f, t, extra) => out.push(Object.assign({ f, t, cap: b[t] }, extra || {}));
  for (let i = 0; i < 64; i++) {
    const p = b[i];
    if (!p || colorOf(p) !== me) continue;
    const r = i >> 3, f = i & 7, P = p.toUpperCase();
    if (P === 'P') {
      const dir = me === 'w' ? -1 : 1, start = me === 'w' ? 6 : 1, last = me === 'w' ? 0 : 7;
      const r1 = r + dir;
      if (r1 >= 0 && r1 < 8 && !b[r1 * 8 + f]) {
        if (r1 === last) for (const q of 'qrbn') add(i, r1 * 8 + f, { p: q });
        else {
          add(i, r1 * 8 + f);
          const r2 = r + 2 * dir;
          if (r === start && !b[r2 * 8 + f]) add(i, r2 * 8 + f);
        }
      }
      for (const df of [-1, 1]) {
        const ff = f + df;
        if (ff < 0 || ff > 7 || r1 < 0 || r1 > 7) continue;
        const t = r1 * 8 + ff;
        if (b[t] && colorOf(b[t]) !== me) {
          if (r1 === last) for (const q of 'qrbn') add(i, t, { p: q }); else add(i, t);
        } else if (t === S.ep) add(i, t, { ep: true, cap: me === 'w' ? 'p' : 'P' });
      }
    } else if (P === 'N' || P === 'K') {
      for (const [dr, df] of (P === 'N' ? KN : KG)) {
        const rr = r + dr, ff = f + df;
        if (rr < 0 || rr > 7 || ff < 0 || ff > 7) continue;
        const t = rr * 8 + ff;
        if (!b[t] || colorOf(b[t]) !== me) add(i, t);
      }
      if (P === 'K') {
        const opp = me === 'w' ? 'b' : 'w', home = me === 'w' ? 60 : 4;
        if (i === home && !attacked(S, home, opp)) {
          const kS = me === 'w' ? S.cK : S.ck, kQ = me === 'w' ? S.cQ : S.cq;
          if (kS && !b[home + 1] && !b[home + 2] && !attacked(S, home + 1, opp) && !attacked(S, home + 2, opp)) add(i, home + 2, { castle: 'k' });
          if (kQ && !b[home - 1] && !b[home - 2] && !b[home - 3] && !attacked(S, home - 1, opp) && !attacked(S, home - 2, opp)) add(i, home - 2, { castle: 'q' });
        }
      }
    } else {
      const dirs = P === 'B' ? DIAG : P === 'R' ? ORTH : DIAG.concat(ORTH);
      for (const [dr, df] of dirs) {
        let rr = r + dr, ff = f + df;
        while (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) {
          const t = rr * 8 + ff;
          if (!b[t]) add(i, t);
          else { if (colorOf(b[t]) !== me) add(i, t); break; }
          rr += dr; ff += df;
        }
      }
    }
  }
  return out;
}

function make(S, m){
  const b = S.b.slice(), me = S.turn, p = b[m.f];
  const N = { b, turn: me === 'w' ? 'b' : 'w', cK: S.cK, cQ: S.cQ, ck: S.ck, cq: S.cq, ep: -1,
              half: (p.toUpperCase() === 'P' || m.cap) ? 0 : S.half + 1, full: S.full + (me === 'b' ? 1 : 0) };
  b[m.t] = m.p ? (me === 'w' ? m.p.toUpperCase() : m.p) : p;
  b[m.f] = '';
  if (m.ep) b[m.t + (me === 'w' ? 8 : -8)] = '';
  if (m.castle === 'k') { b[m.t - 1] = b[m.t + 1]; b[m.t + 1] = ''; }
  if (m.castle === 'q') { b[m.t + 1] = b[m.t - 2]; b[m.t - 2] = ''; }
  if (p.toUpperCase() === 'P' && Math.abs(m.t - m.f) === 16) N.ep = (m.t + m.f) / 2;
  if (p === 'K') { N.cK = N.cQ = false; } if (p === 'k') { N.ck = N.cq = false; }
  if (m.f === 63 || m.t === 63) N.cK = false; if (m.f === 56 || m.t === 56) N.cQ = false;
  if (m.f === 7 || m.t === 7) N.ck = false;   if (m.f === 0 || m.t === 0) N.cq = false;
  return N;
}

function legal(S){ return pseudo(S).filter(m => !inCheck(make(S, m), S.turn)); }

function uci(m){ return sqName(m.f) + sqName(m.t) + (m.p || ''); }
function fromUci(S, u){
  const f = sqIdx(u.slice(0, 2)), t = sqIdx(u.slice(2, 4)), p = u[4] || undefined;
  return legal(S).find(m => m.f === f && m.t === t && (m.p || undefined) === p) || null;
}
function posKey(S){ return S.b.map(p => p || '.').join('') + S.turn + (S.cK ? 1 : 0) + (S.cQ ? 1 : 0) + (S.ck ? 1 : 0) + (S.cq ? 1 : 0) + S.ep; }

// Notation lisible : "Cf3", "exd5", "O-O", "e8=D+"
const FR = { N: 'C', B: 'F', R: 'T', Q: 'D', K: 'R' };
function san(S, m, all){
  if (m.castle) return m.castle === 'k' ? 'O-O' : 'O-O-O';
  const P = S.b[m.f].toUpperCase();
  let s = '';
  if (P === 'P') { if (m.cap) s = FILES[m.f & 7] + 'x'; }
  else {
    s = FR[P];
    const twins = all.filter(o => o.t === m.t && o.f !== m.f && S.b[o.f] === S.b[m.f]);
    if (twins.length) s += twins.some(o => (o.f & 7) === (m.f & 7)) ? (8 - (m.f >> 3)) : FILES[m.f & 7];
    if (m.cap) s += 'x';
  }
  s += sqName(m.t);
  if (m.p) s += '=' + FR[m.p.toUpperCase()];
  const N = make(S, m);
  if (inCheck(N, N.turn)) s += legal(N).length ? '+' : '#';
  return s;
}

// Fin de partie : null, ou { kind, winner }
function outcome(S, history){
  const moves = legal(S);
  if (!moves.length) return inCheck(S, S.turn) ? { kind: 'mat', winner: S.turn === 'w' ? 'b' : 'w' } : { kind: 'pat' };
  if (S.half >= 100) return { kind: '50' };
  const pcs = S.b.filter(p => p && p.toUpperCase() !== 'K');
  if (!pcs.length || (pcs.length === 1 && 'NBnb'.includes(pcs[0]))) return { kind: 'mat-insuf' };
  if (history) { const k = posKey(S); if (history.filter(x => x === k).length >= 3) return { kind: 'rep' }; }
  return null;
}

// ================= Bot : alpha-bêta + quiescence, tables de position =================
const VAL = { P: 100, N: 320, B: 330, R: 500, Q: 900, K: 0 };
const PST = {
  P: [0,0,0,0,0,0,0,0, 50,50,50,50,50,50,50,50, 10,10,20,30,30,20,10,10, 5,5,10,25,25,10,5,5, 0,0,0,20,20,0,0,0, 5,-5,-10,0,0,-10,-5,5, 5,10,10,-20,-20,10,10,5, 0,0,0,0,0,0,0,0],
  N: [-50,-40,-30,-30,-30,-30,-40,-50, -40,-20,0,0,0,0,-20,-40, -30,0,10,15,15,10,0,-30, -30,5,15,20,20,15,5,-30, -30,0,15,20,20,15,0,-30, -30,5,10,15,15,10,5,-30, -40,-20,0,5,5,0,-20,-40, -50,-40,-30,-30,-30,-30,-40,-50],
  B: [-20,-10,-10,-10,-10,-10,-10,-20, -10,0,0,0,0,0,0,-10, -10,0,5,10,10,5,0,-10, -10,5,5,10,10,5,5,-10, -10,0,10,10,10,10,0,-10, -10,10,10,10,10,10,10,-10, -10,5,0,0,0,0,5,-10, -20,-10,-10,-10,-10,-10,-10,-20],
  R: [0,0,0,0,0,0,0,0, 5,10,10,10,10,10,10,5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, 0,0,0,5,5,0,0,0],
  Q: [-20,-10,-10,-5,-5,-10,-10,-20, -10,0,0,0,0,0,0,-10, -10,0,5,5,5,5,0,-10, -5,0,5,5,5,5,0,-5, 0,0,5,5,5,5,0,-5, -10,5,5,5,5,5,0,-10, -10,0,5,0,0,0,0,-10, -20,-10,-10,-5,-5,-10,-10,-20],
  K: [-30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -20,-30,-30,-40,-40,-30,-30,-20, -10,-20,-20,-20,-20,-20,-20,-10, 20,20,0,0,0,0,20,20, 20,30,10,0,0,10,30,20],
  KE:[-50,-40,-30,-20,-20,-30,-40,-50, -30,-20,-10,0,0,-10,-20,-30, -30,-10,20,30,30,20,-10,-30, -30,-10,30,40,40,30,-10,-30, -30,-10,30,40,40,30,-10,-30, -30,-10,20,30,30,20,-10,-30, -30,-30,0,0,0,0,-30,-30, -50,-30,-30,-30,-30,-30,-30,-50]
};
// Évaluation du point de vue des blancs
function evaluate(S){
  let s = 0, mat = 0;
  for (const p of S.b) if (p && p.toUpperCase() !== 'K' && p.toUpperCase() !== 'P') mat += VAL[p.toUpperCase()];
  const endgame = mat <= 2600;
  for (let i = 0; i < 64; i++) {
    const p = S.b[i];
    if (!p) continue;
    const P = p.toUpperCase();
    const tbl = P === 'K' && endgame ? PST.KE : PST[P];
    if (isW(p)) s += VAL[P] + tbl[i];
    else s -= VAL[P] + tbl[(7 - (i >> 3)) * 8 + (i & 7)];
  }
  return s;
}
function order(moves){
  return moves.sort((a, b) => ((b.cap ? VAL[b.cap.toUpperCase()] * 10 : 0) + (b.p === 'q' ? 800 : 0)) -
                              ((a.cap ? VAL[a.cap.toUpperCase()] * 10 : 0) + (a.p === 'q' ? 800 : 0)));
}
let nodes = 0, deadline = 0, aborted = false;
function quiesce(S, alpha, beta, depth){
  nodes++;
  const stand = (S.turn === 'w' ? 1 : -1) * evaluate(S);
  if (stand >= beta) return beta;
  if (alpha < stand) alpha = stand;
  if (depth <= 0) return alpha;
  const caps = order(pseudo(S).filter(m => m.cap));
  for (const m of caps) {
    const N = make(S, m);
    if (inCheck(N, S.turn)) continue;
    const sc = -quiesce(N, -beta, -alpha, depth - 1);
    if (sc >= beta) return beta;
    if (sc > alpha) alpha = sc;
  }
  return alpha;
}
function negamax(S, depth, alpha, beta, ply){
  if ((++nodes & 1023) === 0 && performance.now() > deadline) aborted = true;
  if (aborted) return 0;
  if (depth <= 0) return quiesce(S, alpha, beta, 4);
  const moves = order(pseudo(S));
  let any = false;
  for (const m of moves) {
    const N = make(S, m);
    if (inCheck(N, S.turn)) continue;
    any = true;
    const sc = -negamax(N, depth - 1, -beta, -alpha, ply + 1);
    if (aborted) return 0;
    if (sc >= beta) return beta;
    if (sc > alpha) alpha = sc;
  }
  if (!any) return inCheck(S, S.turn) ? -100000 + ply : 0;
  return alpha;
}
// level : 1 facile, 2 moyen, 3 fort
function botMove(S, level){
  const moves = legal(S);
  if (!moves.length) return null;
  const maxDepth = level === 1 ? 1 : level === 2 ? 2 : 5;
  const budget = level === 1 ? 200 : level === 2 ? 400 : 1800;
  // Part d'imprécision : le facile se trompe souvent, le moyen parfois, le fort jamais
  const noise = level === 1 ? 170 : level === 2 ? 80 : 0;
  deadline = performance.now() + budget; aborted = false; nodes = 0;
  let best = moves[0], bestScore = -Infinity;
  let ordered = order(moves.slice());
  for (let d = 1; d <= maxDepth; d++) {
    let localBest = null, localScore = -Infinity, alpha = -Infinity;
    for (const m of ordered) {
      const N = make(S, m);
      let sc = -negamax(N, d - 1, -Infinity, -alpha, 1);
      if (aborted) break;
      if (noise) sc += (Math.random() - 0.5) * noise * 2;
      if (sc > localScore) { localScore = sc; localBest = m; }
      if (sc > alpha) alpha = sc;
    }
    if (aborted && localBest === null) break;
    if (localBest) { best = localBest; bestScore = localScore; }
    if (aborted) break;
    ordered = [best].concat(ordered.filter(m => m !== best));
  }
  return best;
}


// ================= Interface =================
const $ = id => document.getElementById(id);
const GLYPH = { k: '♚', q: '♛', r: '♜', b: '♝', n: '♞', p: '♟' };
const T = '︎';   // force l'affichage "texte" (évite le pion emoji sur certains téléphones)

// Anti-rechargement
function lift(){ if (window.scrollY < 1) window.scrollTo(0, 1); }
document.addEventListener('touchmove', e => e.preventDefault(), { passive: false });
window.addEventListener('scroll', lift, { passive: true });
window.addEventListener('load', () => setTimeout(lift, 50));

function load(k, d){ try { const v = localStorage.getItem(k); return v === null ? d : v; } catch(e){ return d; } }
function save(k, v){ try { localStorage.setItem(k, v); } catch(e){} }
function sload(k){ try { return sessionStorage.getItem(k) || ''; } catch(e){ return ''; } }
function ssave(k, v){ try { sessionStorage.setItem(k, v); } catch(e){} }

function newTok(){
  const a = new Uint8Array(8);
  try { crypto.getRandomValues(a); } catch(e){ for (let i = 0; i < 8; i++) a[i] = Math.random() * 256 | 0; }
  return Array.from(a, b => b.toString(16).padStart(2, '0')).join('');
}
let tok = load('chess_tok', '');
if (!/^[0-9a-f]{16}$/.test(tok)) { tok = newTok(); save('chess_tok', tok); }

function post(url, params){
  const body = Object.keys(params).map(k => k + '=' + encodeURIComponent(params[k])).join('&');
  return fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
}

// Sons très discrets
let ac = null;
function unlock(){ try { if (!ac) ac = new (window.AudioContext || window.webkitAudioContext)(); if (ac.state !== 'running') ac.resume(); } catch(e){} }
function tone(f, d, vol, when){
  if (!ac || ac.state !== 'running') return;
  const t = ac.currentTime + (when || 0), o = ac.createOscillator(), g = ac.createGain();
  o.type = 'triangle'; o.frequency.value = f;
  g.gain.setValueAtTime(0.0001, t); g.gain.exponentialRampToValueAtTime(vol, t + 0.005); g.gain.exponentialRampToValueAtTime(0.0001, t + d);
  o.connect(g); g.connect(ac.destination); o.start(t); o.stop(t + d + 0.02);
}
function sndMove(cap, chk){
  if (cap) { tone(220, 0.08, 0.05); tone(165, 0.1, 0.04, 0.03); } else tone(330, 0.06, 0.035);
  if (chk) tone(880, 0.12, 0.025, 0.08);
}
function sndEnd(){ [523, 440, 392].forEach((f, i) => tone(f, 0.18, 0.03, i * 0.15)); }

// ---------- État ----------
let mode = 'menu';                 // menu | bot | multi
let S = startPos(), hist = [], keys = [], sans = [];
let me = 'w', level = 2, sel = -1, legalNow = [], over = null, thinking = false;
let flipped = false;
// multi
let mp = { seq: 0, w: false, b: false, mine: '-', ageW: 0, ageB: 0, res: '-', moves: '' };

function setView(v){
  $('vMenu').classList.toggle('on', v === 'menu');
  $('vGame').classList.toggle('on', v !== 'menu');
  mode = v;
  if (v !== 'menu') requestAnimationFrame(sizeBoard);
}

// Rejoue une liste de coups UCI depuis le départ
function replay(list){
  S = startPos(); hist = []; keys = [posKey(S)]; sans = [];
  for (const u of list) {
    const m = fromUci(S, u);
    if (!m) break;
    sans.push(san(S, m, legal(S)));
    hist.push(u);
    S = make(S, m);
    keys.push(posKey(S));
  }
}

// ---------- Plateau ----------
const board = $('board');
let cells = [];
function buildBoard(){
  board.innerHTML = ''; cells = [];
  for (let v = 0; v < 64; v++) {
    const d = document.createElement('div');
    d.addEventListener('pointerdown', e => { e.preventDefault(); unlock(); tap(d._sq); });
    board.appendChild(d); cells.push(d);
  }
}
function sizeBoard(){
  const box = document.querySelector('.boardBox');
  const s = Math.floor(Math.min(box.clientWidth, box.clientHeight - 64) / 8) * 8;
  $('topBar').style.width = $('botBar').style.width = Math.max(160, s) + 'px';
  board.style.width = board.style.height = Math.max(160, s) + 'px';
  board.style.fontSize = Math.max(18, s / 8 * 0.78) + 'px';
}
window.addEventListener('resize', sizeBoard);

function render(){
  const lastU = hist[hist.length - 1];
  const lf = lastU ? sqIdx(lastU.slice(0, 2)) : -1, lt = lastU ? sqIdx(lastU.slice(2, 4)) : -1;
  const ksq = inCheck(S, S.turn) ? kingSq(S, S.turn) : -1;
  const targets = sel >= 0 && !thinking ? legalNow.filter(m => m.f === sel) : [];
  for (let v = 0; v < 64; v++) {
    const sq = flipped ? 63 - v : v, d = cells[v];
    d._sq = sq;
    const r = sq >> 3, f = sq & 7;
    let cls = 'sq ' + ((r + f) % 2 ? 'd' : 'l');
    if (sq === lf || sq === lt) cls += ' last';
    if (sq === sel) cls += ' sel';
    if (sq === ksq) cls += ' chk';
    d.className = cls;
    let h = '';
    const p = S.b[sq];
    if (p) h += '<span class="pc ' + (isW(p) ? 'w' : 'b') + '">' + GLYPH[p.toLowerCase()] + T + '</span>';
    const t = targets.find(m => m.t === sq);
    if (t) h += t.cap ? '<span class="ring"></span>' : '<span class="hint"></span>';
    if ((v & 7) === 7) h += '<span class="coord r">' + (8 - r) + '</span>';
    if (v >= 56) h += '<span class="coord f">' + FILES[f] + '</span>';
    d.innerHTML = h;
  }
  // Pièces prises
  const count = { w: {}, b: {} };
  for (const p of S.b) if (p) { const c = isW(p) ? 'w' : 'b'; count[c][p.toLowerCase()] = (count[c][p.toLowerCase()] || 0) + 1; }
  const full = { q: 1, r: 2, b: 2, n: 2, p: 8 };
  const lost = c => Object.keys(full).map(k => GLYPH[k].repeat(Math.max(0, full[k] - (count[c][k] || 0)))).join('');
  const top = flipped ? 'w' : 'b', bot = flipped ? 'b' : 'w';
  $('topDot').className = 'dot ' + top; $('botDot').className = 'dot ' + bot;
  $('topCap').textContent = lost(bot); $('botCap').textContent = lost(top);
  $('topName').textContent = playerName(top); $('botName').textContent = playerName(bot);
  // Liste des coups
  let txt = '';
  for (let i = 0; i < sans.length; i += 2) txt += (i / 2 + 1) + '. ' + sans[i] + (sans[i + 1] ? ' ' + sans[i + 1] : '') + '   ';
  $('moves').textContent = txt;
  $('moves').scrollLeft = 1e6;
  renderStatus();
  $('bUndo').style.display = mode === 'bot' ? '' : 'none';
}

function playerName(c){
  const col = c === 'w' ? 'Blancs' : 'Noirs';
  if (mode === 'bot') return c === me ? col + ' (toi)' : col + ' (bot)';
  if (mp.mine === c) return col + ' (toi)';
  const here = c === 'w' ? mp.w : mp.b, age = c === 'w' ? mp.ageW : mp.ageB;
  if (!here) return col + ' (place libre)';
  return col + (age > 20 ? ' (déconnecté ?)' : '');
}

function renderStatus(){
  const st = $('status');
  st.classList.remove('alert');
  if (mode === 'multi' && mp.res !== '-') { st.textContent = (mp.res === 'w' ? 'Les noirs abandonnent : victoire des blancs' : 'Les blancs abandonnent : victoire des noirs'); return; }
  if (over) {
    st.textContent = over.kind === 'mat' ? 'Échec et mat : victoire des ' + (over.winner === 'w' ? 'blancs' : 'noirs')
      : over.kind === 'pat' ? 'Pat : partie nulle'
      : over.kind === '50' ? 'Règle des 50 coups : partie nulle'
      : over.kind === 'rep' ? 'Triple répétition : partie nulle'
      : over.kind === 'resign' ? (over.winner === me ? 'Le bot abandonne' : 'Tu as abandonné')
      : 'Matériel insuffisant : partie nulle';
    return;
  }
  if (mode === 'multi') {
    if (!mp.w || !mp.b) { st.textContent = mp.mine === '-' ? 'En attente des joueurs…' : 'En attente d’un adversaire…'; return; }
    if (mp.mine === '-') { st.textContent = 'Tu regardes la partie · trait aux ' + (S.turn === 'w' ? 'blancs' : 'noirs'); return; }
  }
  if (thinking) { st.textContent = 'Le bot réfléchit…'; return; }
  const mine = mode === 'bot' ? S.turn === me : S.turn === mp.mine;
  st.textContent = (mine ? 'À toi de jouer' : 'Au tour de l’adversaire') + (inCheck(S, S.turn) ? ' · échec !' : '');
  if (inCheck(S, S.turn)) st.classList.add('alert');
}

// ---------- Jouer ----------
function canPlay(){
  if (over || thinking) return false;
  if (mode === 'bot') return S.turn === me;
  if (mode === 'multi') return mp.res === '-' && mp.w && mp.b && mp.mine === S.turn;
  return false;
}

function tap(sq){
  if (!canPlay()) return;
  if (sel >= 0) {
    const cands = legalNow.filter(m => m.f === sel && m.t === sq);
    if (cands.length) {
      if (cands.length > 1) { askPromo(cands); return; }
      play(cands[0]); return;
    }
  }
  const p = S.b[sq];
  sel = (p && colorOf(p) === S.turn && sq !== sel) ? sq : -1;
  render();
}

function askPromo(cands){
  const box = $('promoBox'); box.innerHTML = '';
  const c = S.turn;
  ['q', 'r', 'b', 'n'].forEach(k => {
    const b = document.createElement('button');
    b.type = 'button';
    b.innerHTML = '<span class="pc ' + c + '">' + GLYPH[k] + T + '</span>';
    b.addEventListener('click', () => { $('promo').style.display = 'none'; play(cands.find(m => m.p === k)); });
    box.appendChild(b);
  });
  $('promo').style.display = 'flex';
}
$('promo').addEventListener('click', e => { if (e.target === $('promo')) $('promo').style.display = 'none'; });

function applyMove(m){
  const all = legal(S);
  sans.push(san(S, m, all));
  hist.push(uci(m));
  const cap = !!m.cap;
  S = make(S, m);
  keys.push(posKey(S));
  sel = -1;
  legalNow = legal(S);
  over = outcome(S, keys);
  sndMove(cap, inCheck(S, S.turn));
  if (over) sndEnd();
}

async function play(m){
  if (mode === 'bot') {
    applyMove(m);
    saveBot();
    render();
    if (!over) botTurn();
    return;
  }
  if (mode === 'multi') {
    const u = uci(m), before = mp.seq;
    applyMove(m);                  // affichage immédiat
    render();
    try {
      const r = await post('/move', { t: tok, s: before, m: u });
      if (!r.ok) throw r.status;
      mp.seq = before + 1;
    } catch(e) {
      $('status').textContent = 'Coup refusé, mise à jour…';
    }
    poll(true);
  }
}

// ---------- Bot ----------
let botGen = 0;                    // annule un coup du bot en cours si on change de partie
function botTurn(){
  if (over || S.turn === me) return;
  const gen = ++botGen;
  thinking = true; render();
  const t0 = performance.now();
  setTimeout(() => {
    if (gen !== botGen || mode !== 'bot') return;
    const m = botMove(S, level);
    // Temps de réflexion "humain" : 0,8 à 2,5 s environ selon le niveau
    const think = (level === 1 ? 700 : level === 2 ? 900 : 1100) + Math.random() * 1400;
    setTimeout(() => {
      if (gen !== botGen || mode !== 'bot') return;
      if (!m) { thinking = false; render(); return; }
      sel = m.f; render();                     // il "prend" sa pièce…
      setTimeout(() => {
        if (gen !== botGen || mode !== 'bot') return;
        animateMove(m, () => {                 // …puis la fait glisser
          if (gen !== botGen || mode !== 'bot') return;
          thinking = false;
          applyMove(m);
          saveBot();
          render();
        });
      }, 450 + Math.random() * 250);
    }, Math.max(0, think - (performance.now() - t0)));
  }, 60);
}
function cancelBot(){ botGen++; thinking = false; }

// Fait glisser une pièce d'une case à l'autre (animation)
function animateMove(m, done){
  const a = cells[flipped ? 63 - m.f : m.f], b = cells[flipped ? 63 - m.t : m.t];
  const pc = a.querySelector('.pc');
  if (!pc) { done(); return; }
  const br = board.getBoundingClientRect(), ar = a.getBoundingClientRect(), tr = b.getBoundingClientRect();
  const g = pc.cloneNode(true);
  g.style.cssText = 'position:absolute;left:' + (ar.left - br.left) + 'px;top:' + (ar.top - br.top) + 'px;width:' + ar.width + 'px;height:' + ar.height +
                    'px;display:flex;align-items:center;justify-content:center;z-index:5;pointer-events:none;transition:transform .34s ease-in-out';
  board.appendChild(g);
  pc.style.visibility = 'hidden';
  requestAnimationFrame(() => requestAnimationFrame(() => {
    g.style.transform = 'translate(' + (tr.left - ar.left) + 'px,' + (tr.top - ar.top) + 'px)';
  }));
  setTimeout(() => { g.remove(); done(); }, 360);
}
function saveBot(){ ssave('chess_bot', JSON.stringify({ me, level, hist })); }
function startBot(resume){
  mode = 'bot';
  if (!resume) {
    const c = segVal('segCol');
    me = c === 'r' ? (Math.random() < 0.5 ? 'w' : 'b') : c;
    level = +segVal('segLvl');
    replay([]);
  }
  flipped = me === 'b';
  legalNow = legal(S); over = outcome(S, keys); sel = -1;
  setView('bot'); saveBot(); render();
  botTurn();
}
$('bBot').addEventListener('click', () => { unlock(); startBot(false); });
$('bResume').addEventListener('click', () => {
  try {
    const d = JSON.parse(sload('chess_bot'));
    me = d.me; level = d.level; replay(d.hist);
    startBot(true);
  } catch(e){}
});
$('bUndo').addEventListener('click', () => {
  if (mode !== 'bot' || thinking) return;
  let h = hist.slice();
  h.pop();
  if (h.length % 2 !== (me === 'w' ? 0 : 1)) h.pop();     // revient à mon tour
  replay(h);
  legalNow = legal(S); over = outcome(S, keys); sel = -1;
  saveBot(); render();
  botTurn();
});

function segVal(id){ return document.querySelector('#' + id + ' button.on').dataset.v; }
document.querySelectorAll('.seg').forEach(seg => seg.addEventListener('click', e => {
  const b = e.target.closest('button'); if (!b) return;
  seg.querySelectorAll('button').forEach(x => x.classList.toggle('on', x === b));
}));

// ---------- Multijoueur ----------
async function poll(force){
  try {
    const r = await fetch('/s?t=' + tok, { cache: 'no-store' });
    if (!r.ok) return;
    const lines = (await r.text()).split('\n');
    const h = lines[0].split('\t');
    const n = { seq: +h[0], w: h[1] === '1', b: h[2] === '1', mine: h[3], ageW: +h[4], ageB: +h[5], res: h[6], moves: lines[1] || '' };
    const changed = force || n.seq !== mp.seq || n.moves !== mp.moves;
    const before = mp.moves;
    mp = n;
    renderLobby();
    if (mode === 'multi' && changed) {
      const list = mp.moves ? mp.moves.split(' ') : [];
      if (list.join(' ') !== hist.join(' ')) {
        replay(list);
        legalNow = legal(S); over = outcome(S, keys);
        if (list.length > before.split(' ').filter(Boolean).length && list.length) sndMove(false, inCheck(S, S.turn));
        if (over) sndEnd();
        sel = -1;
      }
      flipped = mp.mine === 'b';
      render();
    } else if (mode === 'multi') render();
  } catch(e) {}
}

function renderLobby(){
  const info = $('mpInfo'), b = $('bMulti');
  b.style.display = '';
  if (mp.mine !== '-') {
    info.textContent = mp.w && mp.b ? 'Ta partie est en cours.' : 'Tu attends un adversaire.';
    b.textContent = 'Retourner à ma partie';
  } else if (!mp.w && !mp.b) {
    info.textContent = 'Personne ne joue. Crée une partie, l’adversaire n’aura qu’à la rejoindre.';
    b.textContent = 'Créer une partie';
  } else if (!mp.w || !mp.b) {
    info.textContent = 'Un joueur attend un adversaire !';
    b.textContent = 'Rejoindre la partie';
  } else {
    info.textContent = 'Une partie est en cours (' + Math.ceil((mp.moves ? mp.moves.split(' ').length : 0) / 2) + ' coups).';
    b.textContent = 'Regarder la partie';
  }
}

$('bMulti').addEventListener('click', async () => {
  unlock();
  if (mp.mine === '-' && (!mp.w || !mp.b)) {
    const c = !mp.w ? 'w' : 'b';
    try { await post('/seat', { t: tok, c }); } catch(e){}
  }
  mode = 'multi';
  hist = ['__']; // force le rejeu
  setView('multi');
  await poll(true);
});

$('bResign').addEventListener('click', async () => {
  if (over) return;
  if (mode === 'bot') { cancelBot(); sel = -1; over = { kind: 'resign', winner: me === 'w' ? 'b' : 'w' }; sndEnd(); saveBot(); render(); return; }
  if (mode === 'multi' && mp.mine !== '-') { try { await post('/resign', { t: tok }); } catch(e){} poll(true); }
});
$('bNew').addEventListener('click', async () => {
  if (mode === 'bot') { cancelBot(); replay([]); legalNow = legal(S); over = null; sel = -1; saveBot(); render(); botTurn(); return; }
  if (mode === 'multi' && mp.mine !== '-') { try { await post('/reset', { t: tok }); } catch(e){} poll(true); }
});
$('bMenu').addEventListener('click', async () => {
  if (mode === 'multi' && mp.mine !== '-' && (!mp.w || !mp.b || mp.res !== '-' || over)) {
    try { await post('/leave', { t: tok }); } catch(e){}      // libère la place si la partie n'a pas vraiment lieu
  }
  cancelBot();
  setView('menu');
  $('bResume').style.display = sload('chess_bot') ? '' : 'none';
  poll(true);
});

// ---------- Démarrage ----------
buildBoard();
$('bResume').style.display = sload('chess_bot') ? '' : 'none';
poll(true);
setInterval(() => { if (!document.hidden) poll(false); }, 1200);
document.addEventListener('visibilitychange', () => { if (!document.hidden) poll(true); });
</script>
</body>
</html>
)rawliteral";

// ---------- Outils ----------

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
bool validTok(const String& t) {
  if (t.length() != 16) return false;
  for (size_t i = 0; i < 16; i++) if (hexVal(t[i]) < 0) return false;
  return true;
}
bool isPost() {
  if (server.method() == HTTP_POST) return true;
  server.send(405, "text/plain", "POST uniquement");
  return false;
}
// Coup au format UCI : e2e4 ou e7e8q
bool validUci(const String& m) {
  if (m.length() != 4 && m.length() != 5) return false;
  if (m[0] < 'a' || m[0] > 'h' || m[2] < 'a' || m[2] > 'h') return false;
  if (m[1] < '1' || m[1] > '8' || m[3] < '1' || m[3] > '8') return false;
  if (m.length() == 5 && m[4] != 'q' && m[4] != 'r' && m[4] != 'b' && m[4] != 'n') return false;
  return true;
}
// Places restées inactives trop longtemps : libérées
void freeStale() {
  uint32_t now = millis();
  if (tokW[0] && now - seenW > SEAT_STALE) { tokW[0] = 0; seq++; }
  if (tokB[0] && now - seenB > SEAT_STALE) { tokB[0] = 0; seq++; }
}
char seatOf(const String& t) {
  if (!validTok(t)) return '-';
  if (tokW[0] && t == tokW) return 'w';
  if (tokB[0] && t == tokB) return 'b';
  return '-';
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

// ---------- Routes : partie à deux ----------

// GET /s?t=  ->  "seq \t blanc pris \t noir pris \t ma couleur \t inactivité blancs (s) \t inactivité noirs (s) \t résultat \n coups"
void handleState() {
  freeStale();
  String t = server.arg("t");
  char mine = seatOf(t);
  uint32_t now = millis();
  if (mine == 'w') seenW = now;
  if (mine == 'b') seenB = now;
  String out;
  out.reserve(64 + moves.length());
  out += (unsigned long)seq; out += '\t';
  out += tokW[0] ? '1' : '0'; out += '\t';
  out += tokB[0] ? '1' : '0'; out += '\t';
  out += mine; out += '\t';
  out += tokW[0] ? (unsigned long)((now - seenW) / 1000) : 0UL; out += '\t';
  out += tokB[0] ? (unsigned long)((now - seenB) / 1000) : 0UL; out += '\t';
  out += result; out += '\n';
  out += moves;
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", out);
}

// POST /seat  t, c (w|b)
void handleSeat() {
  if (!isPost()) return;
  freeStale();
  String t = server.arg("t"), c = server.arg("c");
  if (!validTok(t) || (c != "w" && c != "b")) { server.send(400, "text/plain", "invalide"); return; }
  if (seatOf(t) != '-') { server.send(200, "text/plain", "deja assis"); return; }
  char* slot = (c == "w") ? tokW : tokB;
  if (slot[0]) { server.send(409, "text/plain", "prise"); return; }
  strcpy(slot, t.c_str());
  if (c == "w") seenW = millis(); else seenB = millis();
  // Nouvelle table : on repart d'une partie vierge si personne n'était assis
  if ((c == "w" && !tokB[0]) || (c == "b" && !tokW[0])) { moves = ""; nMoves = 0; result = '-'; }
  seq++;
  Serial.printf("place %s prise\n", c.c_str());
  server.send(200, "text/plain", "ok");
}

// POST /move  t, s (seq attendu), m (coup UCI)
void handleMove() {
  if (!isPost()) return;
  String t = server.arg("t"), m = server.arg("m");
  char mine = seatOf(t);
  if (mine == '-') { server.send(403, "text/plain", "pas joueur"); return; }
  if (!tokW[0] || !tokB[0] || result != '-') { server.send(409, "text/plain", "pas en cours"); return; }
  char turn = (nMoves % 2 == 0) ? 'w' : 'b';
  if (mine != turn) { server.send(409, "text/plain", "pas ton tour"); return; }
  if ((uint32_t)server.arg("s").toInt() != seq) { server.send(409, "text/plain", "desynchronise"); return; }
  if (!validUci(m) || nMoves >= MAX_MOVES) { server.send(400, "text/plain", "invalide"); return; }
  if (moves.length()) moves += ' ';
  moves += m;
  nMoves++;
  seq++;
  if (mine == 'w') seenW = millis(); else seenB = millis();
  server.send(200, "text/plain", "ok");
}

// POST /resign  t
void handleResign() {
  if (!isPost()) return;
  char mine = seatOf(server.arg("t"));
  if (mine == '-' || result != '-') { server.send(409, "text/plain", "impossible"); return; }
  result = (mine == 'w') ? 'b' : 'w';
  seq++;
  server.send(200, "text/plain", "ok");
}

// POST /reset  t  (nouvelle partie, mêmes joueurs)
void handleReset() {
  if (!isPost()) return;
  if (seatOf(server.arg("t")) == '-') { server.send(403, "text/plain", "pas joueur"); return; }
  moves = ""; nMoves = 0; result = '-';
  seq++;
  server.send(200, "text/plain", "ok");
}

// POST /leave  t
void handleLeave() {
  if (!isPost()) return;
  char mine = seatOf(server.arg("t"));
  if (mine == 'w') tokW[0] = 0;
  if (mine == 'b') tokB[0] = 0;
  if (!tokW[0] && !tokB[0]) { moves = ""; nMoves = 0; result = '-'; }
  seq++;
  server.send(200, "text/plain", "ok");
}

// ---------- Setup / Loop ----------

void setup() {
  Serial.begin(115200);
  delay(200);
  moves.reserve(MAX_MOVES * 6);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS);

  // Certaines cartes ESP32-C3 Super Mini ont une antenne faible :
  // si le réseau n'apparaît pas ou décroche, décommenter la ligne suivante.
  // WiFi.setTxPower(WIFI_POWER_8_5dBm);

  apHost = apIP.toString();
  portalUrl = "http://" + apHost + "/";

#if defined(ESP_IDF_VERSION_VAL) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
  WiFi.AP.enableDhcpCaptivePortal();
#endif

  dns.start(53, "*", apIP);

  server.on("/", handlePage);
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

  server.on("/s",      HTTP_GET, handleState);
  server.on("/seat",   handleSeat);
  server.on("/move",   handleMove);
  server.on("/resign", handleResign);
  server.on("/reset",  handleReset);
  server.on("/leave",  handleLeave);

  server.onNotFound(handleRedirect);
  server.begin();

  Serial.print("DROP CHESS pret : ");
  Serial.println(portalUrl);
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
}
