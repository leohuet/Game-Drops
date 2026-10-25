
const char* page = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no, viewport-fit=cover">
  <title>PING BONG TETRIS</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }

    html, body {
      width: 100%;
      height: 100%;
      overflow: hidden;
      background: #080808;
      color: #fff;
      font-family: monospace;
      touch-action: manipulation;
    }

    body {
      display: flex;
      align-items: center;
      justify-content: center;
    }

    .app {
      width: 100vw;
      height: 100vh;
      display: grid;
      grid-template-rows: auto 1fr auto;
      gap: 10px;
      padding: 10px;
    }

    .topbar {
      display: flex;
      justify-content: space-between;
      align-items: center;
      color: #bbb;
      font-size: 14px;
      gap: 10px;
      user-select: none;
    }

    .hint {
      color: #777;
      font-size: 11px;
      text-align: right;
    }

    .main {
      min-height: 0;
      display: grid;
      grid-template-columns: 1fr;
      gap: 10px;
      align-items: center;
      justify-items: center;
    }

    .boardWrap {
      width: 100%;
      display: flex;
      align-items: center;
      justify-content: center;
      min-height: 0;
    }

    canvas {
      display: block;
      image-rendering: pixelated;
      border: 2px solid #fff;
      background: #111;
      width: min(62vw, 46vh, 340px);
      height: min(calc(62vw * 2), 92vh - 180px, 680px);
      max-height: 70vh;
    }

    .side {
      width: min(92vw, 340px);
      display: flex;
      justify-content: space-between;
      align-items: flex-start;
      color: #aaa;
      font-size: 13px;
      gap: 14px;
    }

    .panel {
      flex: 1;
      border: 1px solid #444;
      padding: 8px;
      min-height: 78px;
      background: #0d0d0d;
    }

    .panelTitle {
      color: #777;
      font-size: 11px;
      margin-bottom: 6px;
    }

    .controls {
      display: grid;
      grid-template-columns: repeat(3, 78px);
      grid-template-rows: repeat(2, 62px);
      gap: 8px;
      justify-content: center;
      align-content: center;
      user-select: none;
      -webkit-user-select: none;
    }

    button {
      border: 2px solid #fff;
      background: #000;
      color: #fff;
      font-family: monospace;
      font-size: 18px;
      border-radius: 12px;
      min-height: 56px;
    }

    button:active {
      background: #222;
    }

    .left  { grid-column: 1; grid-row: 2; }
    .rot   { grid-column: 2; grid-row: 1; }
    .right { grid-column: 3; grid-row: 2; }
    .down  { grid-column: 2; grid-row: 2; }
    .drop  { grid-column: 1 / span 3; grid-row: 3; }

    .bottomBar {
      display: flex;
      justify-content: center;
      gap: 8px;
    }

    .small {
      font-size: 14px;
      padding: 10px 14px;
      min-height: 44px;
    }

    .overlay {
      position: fixed;
      inset: 0;
      display: flex;
      align-items: center;
      justify-content: center;
      pointer-events: none;
    }

    .msg {
      background: rgba(0,0,0,0.78);
      border: 1px solid #fff;
      padding: 12px 16px;
      text-align: center;
      color: #fff;
      font-size: 14px;
      line-height: 1.5;
      max-width: 84vw;
    }

    @media (orientation: landscape) {
      .main {
        grid-template-columns: auto auto;
        justify-content: center;
        gap: 16px;
      }

      .boardWrap {
        width: auto;
      }

      .sideColumn {
        display: flex;
        flex-direction: column;
        justify-content: center;
        gap: 12px;
        width: 270px;
      }

      .side {
        width: 100%;
        flex-direction: column;
      }
    }
  </style>
</head>
<body>
  <div class="app">
    <div class="topbar">
      <div>
        <span id="score">score 0</span>
        &nbsp;&nbsp;
        <span id="lines">lignes 0</span>
      </div>
      <div class="hint">tactile + clavier</div>
    </div>

    <div class="main">
      <div class="boardWrap">
        <canvas id="game" width="240" height="480"></canvas>
      </div>

      <div class="sideColumn">
        <div class="side">
          <div class="panel">
            <div class="panelTitle">ÉTAT</div>
            <div id="stateText">appuie sur une touche</div>
          </div>
          <div class="panel">
            <div class="panelTitle">SUIVANT</div>
            <div id="nextText">?</div>
          </div>
        </div>

        <div class="controls">
          <button class="rot" id="rot">↻</button>
          <button class="left" id="left">←</button>
          <button class="down" id="down">↓</button>
          <button class="right" id="right">→</button>
          <button class="drop" id="drop">DROP</button>
        </div>
      </div>
    </div>

    <div class="bottomBar">
      <button class="small" id="restart">restart</button>
      <button class="small" id="pause">pause</button>
    </div>
  </div>

  <div class="overlay" id="overlay">
    <div class="msg" id="msg">appuie sur une direction ou rotation pour commencer</div>
  </div>

  <script>
    const canvas = document.getElementById('game');
    const ctx = canvas.getContext('2d');

    const scoreEl = document.getElementById('score');
    const linesEl = document.getElementById('lines');
    const stateText = document.getElementById('stateText');
    const nextText = document.getElementById('nextText');
    const pauseBtn = document.getElementById('pause');
    const overlay = document.getElementById('overlay');
    const msg = document.getElementById('msg');

    const COLS = 10;
    const ROWS = 20;
    const BLOCK = 24;

    const colors = {
      I: '#00e5ff',
      O: '#ffd400',
      T: '#b46cff',
      S: '#59ff59',
      Z: '#ff5c5c',
      J: '#4f7cff',
      L: '#ffae42'
    };

    const tetrominos = {
      I: [
        [0,0,0,0],
        [1,1,1,1],
        [0,0,0,0],
        [0,0,0,0]
      ],
      O: [
        [1,1],
        [1,1]
      ],
      T: [
        [0,1,0],
        [1,1,1],
        [0,0,0]
      ],
      S: [
        [0,1,1],
        [1,1,0],
        [0,0,0]
      ],
      Z: [
        [1,1,0],
        [0,1,1],
        [0,0,0]
      ],
      J: [
        [1,0,0],
        [1,1,1],
        [0,0,0]
      ],
      L: [
        [0,0,1],
        [1,1,1],
        [0,0,0]
      ]
    };

    let board = [];
    let bag = [];
    let current = null;
    let nextPiece = null;
    let score = 0;
    let lines = 0;
    let started = false;
    let paused = false;
    let gameOver = false;

    const FALL_MS = 520;
    const SOFT_DROP_MS = 45;
    let lastTime = 0;
    let acc = 0;
    let softDropping = false;

    function show(text) {
      msg.textContent = text;
      overlay.style.display = 'flex';
      stateText.textContent = text;
    }

    function hide() {
      overlay.style.display = 'none';
    }

    function updateHud() {
      scoreEl.textContent = 'score ' + score;
      linesEl.textContent = 'lignes ' + lines;
      nextText.textContent = nextPiece ? nextPiece.name : '?';
      if (!gameOver && !paused && started) stateText.textContent = 'en cours';
      if (!started && !gameOver && !paused) stateText.textContent = 'prêt';
      if (paused) stateText.textContent = 'pause';
      if (gameOver) stateText.textContent = 'game over';
    }

    function makeEmptyBoard() {
      board = [];
      for (let r = 0; r < ROWS; r++) {
        board[r] = [];
        for (let c = 0; c < COLS; c++) board[r][c] = 0;
      }
    }

    function randBag() {
      const arr = ['I', 'J', 'L', 'O', 'S', 'T', 'Z'];
      for (let i = arr.length - 1; i > 0; i--) {
        const j = (Math.random() * (i + 1)) | 0;
        const tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
      }
      return arr;
    }

    function makePiece(name) {
      const matrix = tetrominos[name].map(row => row.slice());
      return {
        name,
        matrix,
        row: name === 'I' ? -1 : 0,
        col: Math.floor(COLS / 2) - Math.ceil(matrix[0].length / 2)
      };
    }

    function pullPiece() {
      if (bag.length === 0) bag = randBag();
      return makePiece(bag.pop());
    }

    function rotate(matrix) {
      const N = matrix.length;
      const out = [];
      for (let r = 0; r < N; r++) {
        out[r] = [];
        for (let c = 0; c < N; c++) {
          out[r][c] = matrix[N - c - 1][r];
        }
      }
      return out;
    }

    function isValid(matrix, row, col) {
      for (let r = 0; r < matrix.length; r++) {
        for (let c = 0; c < matrix[r].length; c++) {
          if (!matrix[r][c]) continue;

          const nr = row + r;
          const nc = col + c;

          if (nc < 0 || nc >= COLS || nr >= ROWS) return false;
          if (nr >= 0 && board[nr][nc]) return false;
        }
      }
      return true;
    }

    function mergeCurrent() {
      const { matrix, row, col, name } = current;
      for (let r = 0; r < matrix.length; r++) {
        for (let c = 0; c < matrix[r].length; c++) {
          if (!matrix[r][c]) continue;

          const br = row + r;
          const bc = col + c;

          if (br < 0) {
            triggerGameOver();
            return;
          }

          board[br][bc] = name;
        }
      }
      clearLines();
      spawnNext();
    }

    function clearLines() {
      let cleared = 0;

      for (let r = ROWS - 1; r >= 0; ) {
        let full = true;
        for (let c = 0; c < COLS; c++) {
          if (!board[r][c]) {
            full = false;
            break;
          }
        }

        if (full) {
          board.splice(r, 1);
          board.unshift(new Array(COLS).fill(0));
          cleared++;
        } else {
          r--;
        }
      }

      if (cleared > 0) {
        lines += cleared;
        score += [0, 100, 300, 500, 800][cleared] || cleared * 200;
      }
    }

    function spawnNext() {
      current = nextPiece || pullPiece();
      nextPiece = pullPiece();

      if (!isValid(current.matrix, current.row, current.col)) {
        triggerGameOver();
      }
      updateHud();
    }

    function triggerGameOver() {
      gameOver = true;
      started = false;
      show('game over');
      updateHud();
    }

    function startIfNeeded() {
      if (gameOver) return;
      if (!started) {
        started = true;
        hide();
        updateHud();
      }
    }

    function tryMove(dx, dy) {
      if (!current || paused || gameOver) return false;
      startIfNeeded();

      const newRow = current.row + dy;
      const newCol = current.col + dx;

      if (isValid(current.matrix, newRow, newCol)) {
        current.row = newRow;
        current.col = newCol;
        return true;
      }

      if (dy === 1) {
        mergeCurrent();
      }
      return false;
    }

    function tryRotate() {
      if (!current || paused || gameOver) return;
      startIfNeeded();

      const rotated = rotate(current.matrix);
      const kicks = [0, -1, 1, -2, 2];

      for (const kick of kicks) {
        if (isValid(rotated, current.row, current.col + kick)) {
          current.matrix = rotated;
          current.col += kick;
          return;
        }
      }
    }

    function hardDrop() {
      if (!current || paused || gameOver) return;
      startIfNeeded();

      while (isValid(current.matrix, current.row + 1, current.col)) {
        current.row++;
      }
      mergeCurrent();
    }

    function resetGame() {
      makeEmptyBoard();
      bag = [];
      score = 0;
      lines = 0;
      started = false;
      paused = false;
      gameOver = false;
      acc = 0;
      softDropping = false;
      pauseBtn.textContent = 'pause';

      current = pullPiece();
      nextPiece = pullPiece();

      show('appuie sur une direction ou rotation pour commencer');
      updateHud();
      draw();
    }

    function drawCell(x, y, color) {
      ctx.fillStyle = color;
      ctx.fillRect(x * BLOCK, y * BLOCK, BLOCK - 1, BLOCK - 1);
    }

    function drawBoard() {
      ctx.fillStyle = '#111';
      ctx.fillRect(0, 0, canvas.width, canvas.height);

      for (let r = 0; r < ROWS; r++) {
        for (let c = 0; c < COLS; c++) {
          if (board[r][c]) {
            drawCell(c, r, colors[board[r][c]]);
          } else {
            ctx.strokeStyle = '#1c1c1c';
            ctx.strokeRect(c * BLOCK, r * BLOCK, BLOCK, BLOCK);
          }
        }
      }
    }

    function drawPiece(piece, alpha = 1) {
      if (!piece) return;
      ctx.globalAlpha = alpha;
      for (let r = 0; r < piece.matrix.length; r++) {
        for (let c = 0; c < piece.matrix[r].length; c++) {
          if (!piece.matrix[r][c]) continue;
          const y = piece.row + r;
          if (y < 0) continue;
          drawCell(piece.col + c, y, colors[piece.name]);
        }
      }
      ctx.globalAlpha = 1;
    }

    function drawGhost() {
      if (!current) return;
      let ghostRow = current.row;
      while (isValid(current.matrix, ghostRow + 1, current.col)) {
        ghostRow++;
      }
      drawPiece({ ...current, row: ghostRow }, 0.22);
    }

    function draw() {
      drawBoard();
      drawGhost();
      drawPiece(current, 1);
    }

    function tick(dt) {
      if (!started || paused || gameOver) return;
      acc += dt;
      const step = softDropping ? SOFT_DROP_MS : FALL_MS;

      while (acc >= step) {
        acc -= step;
        if (gameOver) break;
        if (!tryMove(0, 1)) break;
      }
    }

    function loop(ts) {
      if (!lastTime) lastTime = ts;
      const dt = ts - lastTime;
      lastTime = ts;

      tick(dt);
      draw();
      requestAnimationFrame(loop);
    }

    function tapAction(fn) {
      return function(ev) {
        ev.preventDefault();
        fn();
      };
    }

    document.getElementById('left').addEventListener('touchstart', tapAction(() => tryMove(-1, 0)), { passive: false });
    document.getElementById('right').addEventListener('touchstart', tapAction(() => tryMove(1, 0)), { passive: false });
    document.getElementById('rot').addEventListener('touchstart', tapAction(() => tryRotate()), { passive: false });
    document.getElementById('drop').addEventListener('touchstart', tapAction(() => hardDrop()), { passive: false });

    const downBtn = document.getElementById('down');
    downBtn.addEventListener('touchstart', function(ev) {
      ev.preventDefault();
      startIfNeeded();
      softDropping = true;
    }, { passive: false });
    downBtn.addEventListener('touchend', function(ev) {
      ev.preventDefault();
      softDropping = false;
    }, { passive: false });
    downBtn.addEventListener('touchcancel', function(ev) {
      ev.preventDefault();
      softDropping = false;
    }, { passive: false });

    document.getElementById('left').addEventListener('click', () => tryMove(-1, 0));
    document.getElementById('right').addEventListener('click', () => tryMove(1, 0));
    document.getElementById('rot').addEventListener('click', () => tryRotate());
    document.getElementById('drop').addEventListener('click', () => hardDrop());

    downBtn.addEventListener('mousedown', () => { startIfNeeded(); softDropping = true; });
    downBtn.addEventListener('mouseup', () => { softDropping = false; });
    downBtn.addEventListener('mouseleave', () => { softDropping = false; });

    document.getElementById('restart').addEventListener('click', resetGame);

    pauseBtn.addEventListener('click', () => {
      if (gameOver) return;
      paused = !paused;
      pauseBtn.textContent = paused ? 'resume' : 'pause';

      if (paused) show('pause');
      else {
        if (started) hide();
        else show('appuie sur une direction ou rotation pour commencer');
      }
      updateHud();
    });

    document.addEventListener('keydown', (e) => {
      if (e.key === 'ArrowLeft') tryMove(-1, 0);
      else if (e.key === 'ArrowRight') tryMove(1, 0);
      else if (e.key === 'ArrowUp') tryRotate();
      else if (e.key === 'ArrowDown') {
        startIfNeeded();
        softDropping = true;
      }
      else if (e.key === ' ') hardDrop();
      else if (e.key.toLowerCase() === 'p') {
        pauseBtn.click();
      }
    });

    document.addEventListener('keyup', (e) => {
      if (e.key === 'ArrowDown') {
        softDropping = false;
      }
    });

    resetGame();
    requestAnimationFrame(loop);
  </script>
</body>
</html>
)rawliteral";
