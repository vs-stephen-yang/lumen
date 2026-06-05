// Lumen web sender entry point: wires the DOM to runSender().

import { runSender } from './sender/sender.js';

const logEl = document.getElementById('log');
const fpsEl = document.getElementById('fps');
const statusEl = document.getElementById('status');

function log(msg, level = 'info') {
    const el = document.createElement('div');
    el.className = level;
    el.textContent = msg;
    logEl?.appendChild(el);
}
function setStatus(s) {
    if (statusEl) statusEl.textContent = s;
    document.title = s;  // the e2e harness reads the title/DOM for OK|ERROR
}
function setFps(s) {
    if (fpsEl) fpsEl.textContent = s;
}

const q = new URLSearchParams(location.search);
const frames = parseInt(q.get('frames') || '120', 10);
const seconds = parseFloat(q.get('seconds') || '6');

runSender({ frames, seconds, log, setStatus, setFps }).catch((e) => {
    log('uncaught: ' + e.message, 'err');
    setStatus('ERROR');
});
