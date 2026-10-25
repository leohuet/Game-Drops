/*
  DROP BLOCKS — jeu de blocs façon console 8 bits, servi par un ESP32 Super Mini
  Mécaniques reprises de "Tetric A" (Christopher "Kerm Martian" Mitchell, TI-84+CE) d'après son readme :
    - commandes : gauche/droite, rotation horaire et antihoraire, descente douce, chute instantanée, pause
    - 5 modes : MARATHON, HIGH SPEED, TOUCHDOWN, SADISTIC, COLLAPSE
  Le code d'origine (assembleur compilé) n'a pas été réutilisé ; les détails manquants suivent les règles
  classiques des consoles 8 bits (vitesses par niveau, points 40/100/300/1200 x niveau, tirage "anti-répétition").
  - Point d'accès Wi-Fi ouvert "DROP BLOCKS", DNS captif, page sur http://4.3.2.1
    (ou http://192.168.4.1 si USE_PUBLIC_IP vaut 0)
  Musique : "Korobeiniki", chanson populaire russe du XIXe siècle (domaine public), arrangée en chiptune.
  Bibliothèques : WiFi.h, WebServer.h, DNSServer.h (incluses dans le cœur Arduino ESP32)
  Idées de portail captif reprises de CDFER/Captive-Portal-ESP32 (licence MIT) :
  https://github.com/CDFER/Captive-Portal-ESP32
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

const char* AP_SSID = "DROP BLOCKS";
const char* AP_PASS = "";
#define USE_PUBLIC_IP 1            // 1 = 4.3.2.1 (portail mieux détecté), 0 = 192.168.4.1

WebServer server(80);
DNSServer dns;
#if USE_PUBLIC_IP
const IPAddress apIP(4, 3, 2, 1);
#else
const IPAddress apIP(192, 168, 4, 1);
#endif
String apHost;
String portalUrl;

const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no, viewport-fit=cover">
<title>DROP BLOCKS</title>
<link rel="icon" href="data:,">
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
/* Document 2 px plus haut que l'écran, gardé à 1 px : pas de rechargement par glissement */
html { background: #c9c3b5; overscroll-behavior: none; }
body { min-height: calc(100vh + 2px); background: #c9c3b5; font-family: monospace; color: #2b2b3a;
       overscroll-behavior: none; touch-action: none; -webkit-user-select: none; user-select: none; -webkit-touch-callout: none; }
/* Une petite console portable grise */
.app { position: fixed; inset: 0; display: flex; flex-direction: column; align-items: center; gap: 6px;
       padding: calc(6px + env(safe-area-inset-top)) 4px calc(8px + env(safe-area-inset-bottom));
       background: linear-gradient(#d6d0c2, #bdb6a6); }
.screenBox { flex: 1; min-height: 0; width: 100%; display: flex; align-items: center; justify-content: center; }
.bezel { background: #4a4a5c; padding: 5px 5px 3px; border-radius: 6px 6px 20px 6px; box-shadow: inset 0 2px 0 rgba(255,255,255,.15), 0 3px 0 rgba(0,0,0,.25); }
.bezel .lbl { color: #a9a9c4; font-size: 8px; letter-spacing: 2px; text-align: center; margin-top: 2px; font-style: italic; }
canvas { display: block; width: 128px; height: 172px; background: #000; image-rendering: pixelated; touch-action: none; }
.pad { width: min(100%, 420px); padding: 0 6px; display: grid; grid-template-columns: auto 1fr auto; align-items: center; }
.dpad { position: relative; width: 114px; height: 114px; }
.dpad button { position: absolute; width: 38px; height: 38px; background: #2b2b3a; border: none; color: #57577a; font-size: 18px; }
.dpad .u { left: 38px; top: 0; border-radius: 6px 6px 0 0; } .dpad .d { left: 38px; top: 76px; border-radius: 0 0 6px 6px; }
.dpad .l { left: 0; top: 38px; border-radius: 6px 0 0 6px; } .dpad .r { left: 76px; top: 38px; border-radius: 0 6px 6px 0; }
.dpad .c { position: absolute; left: 38px; top: 38px; width: 38px; height: 38px; background: #2b2b3a; }
.dpad button.on, .dpad button:active { background: #1a1a24; }
.ab { justify-self: end; position: relative; width: 120px; height: 92px; transform: rotate(-22deg); }
.ab button { position: absolute; width: 52px; height: 52px; border-radius: 50%; background: #a3245a; border: none; color: #e8c8d8;
             font: bold 18px monospace; box-shadow: 0 4px 0 #5e1435; }
.ab button.on, .ab button:active { transform: translateY(3px); box-shadow: 0 1px 0 #5e1435; }
.ab .b { left: 0; top: 30px; } .ab .a { right: 0; top: 2px; }
.ab span { position: absolute; font-size: 11px; color: #4a4a5c; font-weight: bold; }
.ss { display: flex; flex-direction: column; align-items: center; gap: 10px; }
.ss div { display: flex; flex-direction: column; align-items: center; gap: 7px; font-size: 10px; letter-spacing: 1px; color: #4a4a5c; font-weight: bold; }
.ss button { width: 46px; height: 14px; border-radius: 8px; border: none; background: #6d6d82; transform: rotate(-22deg); box-shadow: 0 2px 0 #3a3a48; }
.ss button.on, .ss button:active { background: #4a4a5c; box-shadow: none; }
#bSnd { position: fixed; right: 12px; top: calc(10px + env(safe-area-inset-top)); background: none; border: none; font-size: 18px; opacity: .7; }
</style>
</head>
<body>
<div class="app">
  <div class="screenBox">
    <div class="bezel"><canvas id="cv"></canvas><div class="lbl">DOT MATRIX WITH STEREO SOUND</div></div>
  </div>
  <div class="pad">
    <div class="dpad">
      <div class="c"></div>
      <button class="u" id="kU" type="button">▲</button>
      <button class="l" id="kL" type="button">◀</button>
      <button class="r" id="kR" type="button">▶</button>
      <button class="d" id="kD" type="button">▼</button>
    </div>
    <div class="ss">
      <div><button id="kSel" type="button"></button>SELECT</div>
      <div><button id="kSt" type="button"></button>START</div>
    </div>
    <div class="ab">
      <button class="b" id="kB" type="button">B</button>
      <button class="a" id="kA" type="button">A</button>
    </div>
  </div>
</div>
<button id="bSnd" type="button" aria-label="Son">♪</button>

<script>
const $ = id => document.getElementById(id);
const cv = $('cv'), g = cv.getContext('2d');
const W = 128, H = 172;                         // écran "triché" : le puits occupe presque tout l'écran

// ---------- Anti-rechargement ----------
function lift(){ if (window.scrollY < 1) window.scrollTo(0, 1); }
document.addEventListener('touchmove', e => e.preventDefault(), { passive: false });
window.addEventListener('scroll', lift, { passive: true });
window.addEventListener('load', () => setTimeout(lift, 50));
window.addEventListener('beforeunload', e => { if (state === 'play') { e.preventDefault(); e.returnValue = ''; } });

function load(k, d){ try { const v = localStorage.getItem(k); return v === null ? d : v; } catch(e){ return d; } }
function save(k, v){ try { localStorage.setItem(k, v); } catch(e){} }

// ---------- Écran net (pixels carrés) ----------
function fit(){
  const box = document.querySelector('.screenBox');
  const avW = Math.min(box.clientWidth, 640) - 10, avH = box.clientHeight - 18;
  const k = Math.max(0.8, Math.min(avW / W, avH / H, 5));
  cv.style.width = Math.floor(W * k) + 'px'; cv.style.height = Math.floor(H * k) + 'px';
  const r = Math.max(1, Math.min(4, Math.round(k * (window.devicePixelRatio || 1))));
  cv.width = W * r; cv.height = H * r;
  g.setTransform(r, 0, 0, r, 0, 0); g.imageSmoothingEnabled = false;
}
window.addEventListener('resize', fit);

// ---------- Police pixel 3x5 ----------
const FONT = {
  A:"010101111101101",B:"110101110101110",C:"011100100100011",D:"110101101101110",E:"111100110100111",
  F:"111100110100100",G:"011100101101011",H:"101101111101101",I:"111010010010111",J:"001001001101010",
  K:"101101110101101",L:"100100100100111",M:"101111111101101",N:"110101101101101",O:"010101101101010",
  P:"110101110100100",Q:"010101101110011",R:"110101110101101",S:"011100010001110",T:"111010010010010",
  U:"101101101101111",V:"101101101101010",W:"101101111111101",X:"101101010101101",Y:"101101010010010",
  Z:"111001010100111","0":"111101101101111","1":"010110010010111","2":"110001010100111","3":"110001010001110",
  "4":"101101111001001","5":"111100110001110","6":"011100111101111","7":"111001010010010","8":"111101111101111",
  "9":"111101111001110","-":"000000111000000","!":"010010010000010",":":"000010000010000",".":"000000000000010",
  ">":"100010001010100","<":"001010100010001"," ":"000000000000000"
};
function text(s, x, y, c, sc, center){
  sc = sc || 1; s = String(s).toUpperCase();
  if (center) x = Math.round(x - (s.length * 4 * sc - sc) / 2);
  g.fillStyle = c;
  for (let k = 0; k < s.length; k++) {
    const f = FONT[s[k]] || FONT[' '];
    for (let i = 0; i < 15; i++) if (f[i] === '1') g.fillRect(x + k * 4 * sc + (i % 3) * sc, y + ((i / 3) | 0) * sc, sc, sc);
  }
}

// ---------- Pièces (états de rotation dans une boîte 4x4) ----------
const SHAPES = {
  I: [[[0,1],[1,1],[2,1],[3,1]], [[2,0],[2,1],[2,2],[2,3]], [[0,2],[1,2],[2,2],[3,2]], [[1,0],[1,1],[1,2],[1,3]]],
  O: [[[1,0],[2,0],[1,1],[2,1]], [[1,0],[2,0],[1,1],[2,1]], [[1,0],[2,0],[1,1],[2,1]], [[1,0],[2,0],[1,1],[2,1]]],
  T: [[[1,0],[0,1],[1,1],[2,1]], [[1,0],[1,1],[2,1],[1,2]], [[0,1],[1,1],[2,1],[1,2]], [[1,0],[0,1],[1,1],[1,2]]],
  S: [[[1,0],[2,0],[0,1],[1,1]], [[1,0],[1,1],[2,1],[2,2]], [[1,1],[2,1],[0,2],[1,2]], [[0,0],[0,1],[1,1],[1,2]]],
  Z: [[[0,0],[1,0],[1,1],[2,1]], [[2,0],[1,1],[2,1],[1,2]], [[0,1],[1,1],[1,2],[2,2]], [[1,0],[0,1],[1,1],[0,2]]],
  J: [[[0,0],[0,1],[1,1],[2,1]], [[1,0],[2,0],[1,1],[1,2]], [[0,1],[1,1],[2,1],[2,2]], [[1,0],[1,1],[0,2],[1,2]]],
  L: [[[2,0],[0,1],[1,1],[2,1]], [[1,0],[1,1],[1,2],[2,2]], [[0,1],[1,1],[2,1],[0,2]], [[0,0],[1,0],[1,1],[1,2]]]
};
const TYPES = ['T', 'J', 'Z', 'O', 'S', 'L', 'I'];
// Style de bloc façon NES : 0 = blanc bordé, 1 = couleur 1, 2 = couleur 2
const STYLE = { T: 0, O: 0, I: 0, J: 1, S: 1, Z: 2, L: 2 };
// Deux couleurs par niveau, comme sur la console
const PALS = [['#0058f8','#3cbcfc'],['#00a800','#b8f818'],['#d800cc','#f878f8'],['#0058f8','#58d854'],['#e40058','#58f898'],
              ['#58f898','#6888fc'],['#f83800','#7c7c7c'],['#6844fc','#a80020'],['#0058f8','#f83800'],['#f83800','#fca044']];
// Images par rangée selon le niveau (60 images/s)
const GRAV = [48,43,38,33,28,23,18,13,8,6,5,5,5,4,4,4,3,3,3,2,2,2,2,2,2,2,2,2,2,1];

const MODES = [
  { id: 'marathon', name: 'MARATHON',   short: 'MARA', info: 'TIENS LE PLUS LONGTEMPS' },
  { id: 'speed',    name: 'HIGH SPEED', short: 'SPEED', info: 'MARATHON SOUS PRESSION' },
  { id: 'touch',    name: 'TOUCHDOWN',  short: 'TOUCH', info: 'NETTOIE LA PILE' },
  { id: 'sadistic', name: 'SADISTIC',   short: 'SADIC', info: 'PAS DE BARRE POUR TOI' },
  { id: 'collapse', name: 'COLLAPSE',   short: 'COLL.', info: 'SELECT: TOUT FAIRE TOMBER' }
];

// ---------- Audio ----------
let ac = null, soundOn = load('blk_snd', '1') === '1';
function unlock(){ try { if (!ac) ac = new (window.AudioContext || window.webkitAudioContext)(); if (ac.state !== 'running') ac.resume(); } catch(e){} }
function tone(f, d, type, vol, when, bend){
  if (!soundOn || !ac || ac.state !== 'running') return;
  const t = ac.currentTime + (when || 0), o = ac.createOscillator(), v = ac.createGain();
  o.type = type || 'square'; o.frequency.setValueAtTime(f, t);
  if (bend && bend !== 1) o.frequency.exponentialRampToValueAtTime(f * bend, t + d);
  v.gain.setValueAtTime(0.0001, t); v.gain.exponentialRampToValueAtTime(vol || 0.03, t + 0.004); v.gain.exponentialRampToValueAtTime(0.0001, t + d);
  o.connect(v); v.connect(ac.destination); o.start(t); o.stop(t + d + 0.02);
}
const sfx = {
  move(){ tone(880, 0.025, 'square', 0.012); },
  rot(){ tone(1175, 0.035, 'square', 0.016); },
  lock(){ tone(160, 0.06, 'triangle', 0.05, 0, 0.7); },
  drop(){ tone(330, 0.07, 'square', 0.03, 0, 0.4); },
  line(n){ for (let i = 0; i < n; i++) tone(660 + i * 110, 0.07, 'square', 0.03, i * 0.06); },
  four(){ [523, 659, 784, 1047, 1319].forEach((f, i) => tone(f, 0.09, 'square', 0.035, i * 0.06)); },
  level(){ [784, 988, 1175].forEach((f, i) => tone(f, 0.08, 'square', 0.03, i * 0.08)); },
  over(){ [494, 440, 392, 330, 262].forEach((f, i) => tone(f, 0.2, 'triangle', 0.045, i * 0.2)); },
  menu(){ tone(988, 0.04, 'square', 0.02); },
  ok(){ tone(784, 0.06, 'square', 0.025); tone(1568, 0.08, 'square', 0.02, 0.06); },
  win(){ [523, 659, 784, 1047, 784, 1047].forEach((f, i) => tone(f, 0.12, 'square', 0.035, i * 0.11)); }
};
// Korobeiniki (domaine public) : mélodie + basse, très discret
const MEL = [76,2, 71,1, 72,1, 74,2, 72,1, 71,1, 69,2, 69,1, 72,1, 76,2, 74,1, 72,1, 71,3, 72,1, 74,2, 76,2, 72,2, 69,2, 69,4,
             0,1, 74,2, 77,1, 81,2, 79,1, 77,1, 76,3, 72,1, 76,2, 74,1, 72,1, 71,2, 71,1, 72,1, 74,2, 76,2, 72,2, 69,2, 69,4];
const BAS = [40,45,40,45,45,52,45,52,44,52,44,52,45,52,45,52, 38,45,38,45,36,43,36,43,44,52,44,52,45,52,45,52];
const midi = n => 440 * Math.pow(2, (n - 69) / 12);
let mi = 0, mNext = 0, bi = 0, bNext = 0;
setInterval(() => {
  if (!ac || ac.state !== 'running' || state !== 'play' || !soundOn) { mNext = bNext = 0; return; }
  const beat = Math.max(0.11, 0.19 - level * 0.004);
  const now = ac.currentTime;
  if (!mNext || mNext < now) mNext = now + 0.05;
  if (!bNext || bNext < now) bNext = now + 0.05;
  while (mNext < now + 0.25) {
    const n = MEL[mi], d = MEL[mi + 1];
    if (n) tone(midi(n), beat * d * 0.85, 'square', 0.008, mNext - now);
    mNext += beat * d; mi = (mi + 2) % MEL.length;
  }
  while (bNext < now + 0.25) {
    tone(midi(BAS[bi]), beat * 0.9, 'triangle', 0.02, bNext - now);
    bNext += beat * 2; bi = (bi + 1) % BAS.length;
  }
}, 40);
$('bSnd').textContent = soundOn ? '♪' : '×';
$('bSnd').addEventListener('click', () => { soundOn = !soundOn; save('blk_snd', soundOn ? '1' : '0'); $('bSnd').textContent = soundOn ? '♪' : '×'; unlock(); });

// ---------- État ----------
let state = 'title';      // title | play | clear | pause | over | win
let modeIdx = 0, startLevel = 0, menuRow = 0;
let board, cur, next, prevType = '', score, lines, level, frames, are, lockedFlash = 0;
let clearRows = [], clearT = 0, flash = 0, curtain = 0, softRows = 0, garbageTotal = 0;
let tops = {};
try { tops = JSON.parse(load('blk_tops', '{}')) || {}; } catch(e){ tops = {}; }
const mode = () => MODES[modeIdx].id;

function emptyBoard(){ return Array.from({ length: 20 }, () => new Array(10).fill(0)); }

// Tirage "anti-répétition" des consoles 8 bits : on retire une fois si la pièce est la même que la précédente
function randomType(){
  const pool = mode() === 'sadistic' ? TYPES.filter(t => t !== 'I') : TYPES;
  let t = pool[(Math.random() * pool.length) | 0];
  if (t === prevType) t = pool[(Math.random() * pool.length) | 0];
  prevType = t;
  return t;
}
function cells(p, rot, x, y){ return SHAPES[p.t][rot != null ? rot : p.r].map(([cx, cy]) => [cx + (x != null ? x : p.x), cy + (y != null ? y : p.y)]); }
function fits(p, rot, x, y){
  return cells(p, rot, x, y).every(([cx, cy]) => cx >= 0 && cx < 10 && cy < 20 && (cy < 0 || !board[cy][cx]));
}
function spawn(){
  cur = { t: next, r: 0, x: 3, y: 0 };
  next = randomType();
  frames = 0; softRows = 0;
  if (!fits(cur)) {
    if (mode() === 'collapse' && litCount()) { doCollapse(); if (fits(cur)) return; }
    gameOver();
  }
}

function newGame(){
  board = emptyBoard(); score = 0; lines = 0; prevType = '';
  const m = mode();
  level = m === 'speed' ? Math.max(10, startLevel) : m === 'touch' ? 0 : startLevel;
  if (m === 'touch') {
    // Une pile de 10 rangées de déchets, un trou par rangée
    for (let y = 10; y < 20; y++) {
      const hole = (Math.random() * 10) | 0;
      for (let x = 0; x < 10; x++) if (x !== hole && Math.random() < 0.92) board[y][x] = 8;
    }
    garbageTotal = countGarbage();
  }
  next = randomType(); spawn();
  are = 0; flash = 0; curtain = 0; clearRows = [];
  state = 'play';
  sfx.ok();
}
function countGarbage(){ let n = 0; for (const r of board) for (const c of r) if (c === 8) n++; return n; }
function litCount(){ return board.filter(r => r[0] === 9).length; }

function levelFor(){
  const m = mode();
  if (m === 'speed') return Math.max(10, startLevel) + Math.floor(lines / 5);
  if (m === 'touch') return Math.floor(lines / 10);
  return Math.max(startLevel, Math.floor(lines / 10));
}

function lockPiece(){
  for (const [x, y] of cells(cur)) {
    if (y < 0) { gameOver(); return; }
    board[y][x] = TYPES.indexOf(cur.t) + 1;
  }
  score += softRows;
  sfx.lock();
  const full = [];
  for (let y = 0; y < 20; y++) if (board[y][0] !== 9 && board[y].every(c => c)) full.push(y);
  if (full.length) {
    if (mode() === 'collapse') {
      // Les lignes complètes s'allument et restent : on les fait tomber toutes ensemble avec SELECT
      for (const y of full) board[y] = new Array(10).fill(9);
      sfx.line(1);
      if (litCount() >= 12) doCollapse();
      cur = null; are = 10;
    } else {
      clearRows = full; clearT = 0; state = 'clear'; cur = null;
      if (full.length === 4) { flash = 12; sfx.four(); } else sfx.line(full.length);
    }
  } else { cur = null; are = 10; }
}

function finishClear(){
  const n = clearRows.length;
  board = board.filter((r, y) => !clearRows.includes(y));
  while (board.length < 20) board.unshift(new Array(10).fill(0));
  score += [0, 40, 100, 300, 1200][n] * (level + 1);
  lines += n;
  clearRows = [];
  const nl = levelFor();
  if (nl > level) { level = nl; sfx.level(); }
  state = 'play'; are = 10;
  if (mode() === 'touch' && countGarbage() === 0) win();
}

function doCollapse(){
  const n = litCount();
  if (!n) return;
  board = board.filter(r => r[0] !== 9);
  while (board.length < 20) board.unshift(new Array(10).fill(0));
  score += 100 * n * n * (level + 1);
  lines += n;
  flash = n >= 4 ? 12 : 6;
  n >= 4 ? sfx.four() : sfx.line(n);
  const nl = levelFor();
  if (nl > level) { level = nl; sfx.level(); }
}

function saveTop(){
  const k = mode();
  if (score > (tops[k] || 0)) { tops[k] = score; save('blk_tops', JSON.stringify(tops)); return true; }
  return false;
}
let newTop = false;
function gameOver(){ state = 'over'; curtain = 0; newTop = saveTop(); sfx.over(); }
function win(){ state = 'win'; newTop = saveTop(); sfx.win(); }

// ---------- Commandes (avec répétition automatique) ----------
const keys = { L: false, R: false, D: false, U: false, A: false, B: false, Sel: false, St: false };
const das = { L: 0, R: 0 };
function press(k){
  unlock();
  keys[k] = true;
  if (state === 'title') return menuKey(k);
  if (k === 'St') { if (state === 'play') state = 'pause'; else if (state === 'pause') state = 'play'; else if (state === 'over' || state === 'win') state = 'title'; sfx.menu(); return; }
  if (state === 'pause' && k === 'Sel') { state = 'title'; return; }
  if ((state === 'over' || state === 'win') && (k === 'A')) { newGame(); return; }
  if (state !== 'play' || !cur) {
    if (state === 'play' && k === 'Sel' && mode() === 'collapse') doCollapse();
    return;
  }
  if (k === 'L' || k === 'R') { shift(k === 'L' ? -1 : 1); das[k] = 0; }
  if (k === 'A') rotate(1);
  if (k === 'B') rotate(-1);
  if (k === 'U') hardDrop();
  if (k === 'Sel' && mode() === 'collapse') doCollapse();
}
function release(k){ keys[k] = false; }

function shift(dx){ if (fits(cur, cur.r, cur.x + dx, cur.y)) { cur.x += dx; sfx.move(); } }
function rotate(dir){
  const r = (cur.r + dir + 4) % 4;
  const kicks = cur.t === 'I' ? [0, -1, 1, -2, 2] : [0, -1, 1];
  for (const k of kicks) if (fits(cur, r, cur.x + k, cur.y)) { cur.r = r; cur.x += k; sfx.rot(); return; }
}
function hardDrop(){
  let n = 0;
  while (fits(cur, cur.r, cur.x, cur.y + 1)) { cur.y++; n++; }
  score += n * 2;
  sfx.drop();
  lockPiece();
}

function menuKey(k){
  if (k === 'U') { menuRow = (menuRow + 5) % 6; sfx.menu(); }
  if (k === 'D') { menuRow = (menuRow + 1) % 6; sfx.menu(); }
  if (menuRow < 5 && (k === 'L' || k === 'R')) { modeIdx = menuRow; }
  if (menuRow === 5 && (k === 'L' || k === 'R')) { startLevel = (startLevel + (k === 'L' ? 9 : 1)) % 10; sfx.menu(); }
  if (k === 'A' || k === 'St') { if (menuRow < 5) modeIdx = menuRow; newGame(); }
}

// ---------- Boucle de jeu (60 images/s fixes) ----------
function tick(){
  if (flash > 0) flash--;
  if (state === 'clear') { if (++clearT >= 20) finishClear(); return; }
  if (state === 'over') { if (curtain < 20) curtain += 0.5; return; }
  if (state !== 'play') return;
  if (!cur) { if (--are <= 0) spawn(); return; }

  for (const k of ['L', 'R']) {
    if (keys[k]) { das[k]++; if (das[k] >= 16 && (das[k] - 16) % 6 === 0) shift(k === 'L' ? -1 : 1); }
  }
  frames++;
  const gv = keys.D ? 2 : GRAV[Math.min(level, 29)];
  if (frames >= gv) {
    frames = 0;
    if (fits(cur, cur.r, cur.x, cur.y + 1)) { cur.y++; if (keys.D) softRows++; }
    else lockPiece();
  }
}

// ---------- Dessin ----------
const BX = 6, BY = 6;     // position du puits (10 x 20 cases de 8 px)
function block(x, y, val){
  const pal = PALS[level % 10];
  if (val === 8) {                                   // déchets
    g.fillStyle = '#7c7c7c'; g.fillRect(x, y, 8, 8);
    g.fillStyle = '#bcbcbc'; g.fillRect(x, y, 7, 1); g.fillRect(x, y, 1, 7);
    g.fillStyle = '#3c3c3c'; g.fillRect(x + 7, y, 1, 8); g.fillRect(x, y + 7, 8, 1);
    return;
  }
  if (val === 9) {                                   // ligne allumée (COLLAPSE)
    const on = ((performance.now() / 120) | 0) % 2;
    g.fillStyle = on ? '#fcfcfc' : pal[1]; g.fillRect(x, y, 7, 7);
    return;
  }
  const t = TYPES[val - 1], st = STYLE[t];
  const c = st === 2 ? pal[1] : pal[0];
  g.fillStyle = c; g.fillRect(x, y, 7, 7);
  g.fillStyle = '#fcfcfc'; g.fillRect(x, y, 1, 1);
  if (st === 0) g.fillRect(x + 1, y + 1, 5, 5);
  else { g.fillRect(x + 1, y + 1, 2, 1); g.fillRect(x + 1, y + 2, 1, 1); }
}
function panel(x, y, w, h){
  g.fillStyle = '#000'; g.fillRect(x, y, w, h);
  g.fillStyle = '#bcbcbc'; g.fillRect(x, y, w, 1); g.fillRect(x, y + h - 1, w, 1); g.fillRect(x, y, 1, h); g.fillRect(x + w - 1, y, 1, h);
  g.fillStyle = '#7c7c7c'; g.fillRect(x + 2, y + 2, w - 4, 1); g.fillRect(x + 2, y + h - 3, w - 4, 1); g.fillRect(x + 2, y + 2, 1, h - 4); g.fillRect(x + w - 3, y + 2, 1, h - 4);
}
function bricks(){
  const pal = PALS[level % 10];
  for (let y = 0; y < H; y += 8) for (let x = 0; x < W + 16; x += 16) {
    const ox = (y / 8) % 2 ? 8 : 0;
    g.fillStyle = '#2c2c3c'; g.fillRect(x - ox, y, 16, 8);
    g.fillStyle = pal[0]; g.globalAlpha = 0.18; g.fillRect(x - ox + 1, y + 1, 14, 6); g.globalAlpha = 1;
  }
}
function pad6(n){ return String(n).padStart(6, '0'); }

function mini(x, y, val){                          // bloc réduit pour l'aperçu "suivant"
  const pal = PALS[level % 10], t = TYPES[val - 1], st = STYLE[t];
  g.fillStyle = st === 2 ? pal[1] : pal[0]; g.fillRect(x, y, 4, 4);
  g.fillStyle = '#fcfcfc'; g.fillRect(x, y, 1, 1); if (st === 0) g.fillRect(x + 1, y + 1, 2, 2);
}
function drawGame(now){
  bricks();
  if (flash > 0 && flash % 4 < 2) { g.fillStyle = '#fcfcfc'; g.fillRect(0, 0, W, H); }
  const RX = 92, RW = 34, CX = RX + RW / 2;
  panel(BX - 4, BY - 4, 88, 168);                   // puits
  // Colonne d'infos compacte
  panel(RX, 2, RW, 14);  text(MODES[modeIdx].short, CX, 6, '#fcfcfc', 1, true);
  panel(RX, 18, RW, 22); text('SCORE', CX, 21, '#bcbcbc', 1, true); text(pad6(score), CX, 31, '#fcfcfc', 1, true);
  panel(RX, 42, RW, 22); text('TOP', CX, 45, '#bcbcbc', 1, true); text(pad6(Math.max(tops[mode()] || 0, score)), CX, 55, '#fca044', 1, true);
  panel(RX, 66, RW, 30); text('NEXT', CX, 69, '#bcbcbc', 1, true);
  if (next) {
    const w = next === 'I' ? 4 : next === 'O' ? 2 : 3;
    const ox = Math.round(CX - w * 2) - (next === 'O' ? 4 : 0), oy = next === 'I' ? 78 : 80;
    for (const [cx, cy] of SHAPES[next][0]) mini(ox + cx * 4, oy + cy * 4, TYPES.indexOf(next) + 1);
  }
  panel(RX, 98, RW, 22);  text('LIGNES', CX, 101, '#bcbcbc', 1, true); text(String(lines).padStart(3, '0'), CX, 111, '#fcfcfc', 1, true);
  panel(RX, 122, RW, 22); text('NIV.', CX, 125, '#bcbcbc', 1, true); text(String(level).padStart(2, '0'), CX, 135, '#fcfcfc', 1, true);
  if (mode() === 'touch' || mode() === 'collapse') {
    panel(RX, 146, RW, 24);
    text(mode() === 'touch' ? 'RESTE' : 'ALLUM.', CX, 150, '#bcbcbc', 1, true);
    text(String(mode() === 'touch' ? countGarbage() : litCount()), CX, 160, '#fca044', 1, true);
  }
  g.fillStyle = '#000'; g.fillRect(BX, BY, 80, 160);
  for (let y = 0; y < 20; y++) for (let x = 0; x < 10; x++) {
    const v = board[y][x];
    if (!v) continue;
    if (state === 'clear' && clearRows.includes(y)) {
      const step = Math.floor(clearT / 4);           // effacement depuis le centre
      if (Math.abs(x - 4.5) < step + 0.5) continue;
    }
    block(BX + x * 8, BY + y * 8, v);
  }
  if (cur) for (const [x, y] of cells(cur)) if (y >= 0) block(BX + x * 8, BY + y * 8, TYPES.indexOf(cur.t) + 1);
  const mid = BX + 40;
  if (state === 'over') {
    for (let r = 0; r < Math.floor(curtain); r++) {
      const y = BY + r * 8;
      g.fillStyle = '#7c7c7c'; g.fillRect(BX, y, 80, 8);
      g.fillStyle = '#bcbcbc'; g.fillRect(BX, y, 80, 1);
      g.fillStyle = '#3c3c3c'; g.fillRect(BX, y + 7, 80, 1);
    }
    if (curtain >= 20) {
      panel(BX - 4, 52, 88, 64);
      text('GAME', mid, 60, '#f83800', 2, true); text('OVER', mid, 74, '#f83800', 2, true);
      if (newTop) text('NOUVEAU TOP!', mid, 92, '#fca044', 1, true);
      if (((now / 450) | 0) % 2) text('A: REJOUER', mid, 104, '#fcfcfc', 1, true);
    }
  }
  if (state === 'win') {
    panel(BX - 4, 52, 88, 64);
    text('TOUCH', mid, 60, '#58f898', 2, true); text('DOWN!', mid, 74, '#58f898', 2, true);
    if (newTop) text('NOUVEAU TOP!', mid, 92, '#fca044', 1, true);
    if (((now / 450) | 0) % 2) text('A: REJOUER', mid, 104, '#fcfcfc', 1, true);
  }
  if (state === 'pause') {
    g.fillStyle = '#000'; g.fillRect(BX, BY, 80, 160);
    text('PAUSE', mid, 64, '#fcfcfc', 2, true);
    text('START:', mid, 88, '#bcbcbc', 1, true); text('REPRENDRE', mid, 96, '#bcbcbc', 1, true);
    text('SELECT: MENU', mid, 110, '#bcbcbc', 1, true);
  }
}

// Écran titre : blocs qui tombent en fond, menu des modes
const rain = Array.from({ length: 8 }, (_, i) => ({ x: (i * 17) % 120, y: -((i * 37) % 172), t: TYPES[i % 7], v: 0.3 + (i % 4) * 0.15 }));
function drawTitle(now){
  g.fillStyle = '#000'; g.fillRect(0, 0, W, H);
  const lv = level; level = ((now / 3000) | 0) % 10;
  for (const r of rain) {
    r.y += r.v; if (r.y > H) { r.y = -32; r.x = (Math.random() * 14 | 0) * 8; }
    g.globalAlpha = 0.35;
    for (const [cx, cy] of SHAPES[r.t][0]) block(r.x + cx * 8, (r.y | 0) + cy * 8, TYPES.indexOf(r.t) + 1);
    g.globalAlpha = 1;
  }
  level = lv;
  panel(8, 4, 112, 42);
  text('DROP', 64, 9, '#f83800', 3, true);
  text('BLOCKS', 64, 27, '#fca044', 3, true);
  panel(2, 50, 124, 104);
  MODES.forEach((m, i) => {
    const y = 56 + i * 15, on = menuRow === i;
    if (on && ((now / 300) | 0) % 2) text('>', 5, y, '#fcfcfc');
    text(m.name, 10, y, on ? '#fcfcfc' : '#7c7c7c');
    text(pad6(tops[m.id] || 0), 98, y, on ? '#fca044' : '#5c5c5c');
    if (on) text(m.info, 10, y + 7, '#58d854');
  });
  const y = 56 + 5 * 15 + 3, on = menuRow === 5;
  if (on && ((now / 300) | 0) % 2) text('>', 5, y, '#fcfcfc');
  text('NIVEAU DEPART < ' + startLevel + ' >', 10, y, on ? '#fcfcfc' : '#7c7c7c');
  if (((now / 500) | 0) % 2) text('A OU START POUR JOUER', 64, 161, '#fcfcfc', 1, true);
}

let acc = 0, last = 0;
function loop(now){
  if (!last) last = now;
  acc += Math.min(100, now - last); last = now;
  while (acc >= 1000 / 60) { tick(); acc -= 1000 / 60; }
  if (state === 'title') drawTitle(now); else drawGame(now);
  requestAnimationFrame(loop);
}

// ---------- Boutons ----------
function bind(id, k){
  const el = $(id);
  el.addEventListener('pointerdown', e => { e.preventDefault(); el.classList.add('on'); try { el.setPointerCapture(e.pointerId); } catch(er){} press(k); });
  const up = () => { el.classList.remove('on'); release(k); };
  ['pointerup', 'pointercancel', 'lostpointercapture'].forEach(t => el.addEventListener(t, up));
}
bind('kL', 'L'); bind('kR', 'R'); bind('kD', 'D'); bind('kU', 'U'); bind('kA', 'A'); bind('kB', 'B'); bind('kSel', 'Sel'); bind('kSt', 'St');
const KEYMAP = { ArrowLeft: 'L', ArrowRight: 'R', ArrowDown: 'D', ArrowUp: 'U', x: 'A', z: 'B', ' ': 'U', Enter: 'St', Shift: 'Sel', c: 'Sel' };
document.addEventListener('keydown', e => { const k = KEYMAP[e.key]; if (k) { e.preventDefault(); if (!e.repeat) press(k); } });
document.addEventListener('keyup', e => { const k = KEYMAP[e.key]; if (k) release(k); });
document.addEventListener('visibilitychange', () => { if (document.hidden && state === 'play') state = 'pause'; });

board = emptyBoard(); level = 0; score = 0; lines = 0;
fit();
requestAnimationFrame(loop);
</script>
</body>
</html>
)rawliteral";

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

void setup() {
  Serial.begin(115200);
  delay(200);

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
  server.onNotFound(handleRedirect);
  server.begin();

  Serial.print("DROP BLOCKS pret : ");
  Serial.println(portalUrl);
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
}
