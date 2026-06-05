// Screen-share example app — consumes the Lumen web sender SDK.
//
// Owns capture (getDisplayMedia) and UI; the SDK (LumenSender) owns
// encode → fragment → WebTransport + reconnect. Query params:
//   ?source=screen|camera  capture source (default screen)
//   ?auto=1                start without a click (test mode)
//   ?frames=N&seconds=M    test budget (sender stops + sends done)

import { LumenSender } from '../../src/index.js';

const logEl = document.getElementById('log');
const fpsEl = document.getElementById('fps');
const statusEl = document.getElementById('status');
const startBtn = document.getElementById('start');
const stopBtn = document.getElementById('stop');

const q = new URLSearchParams(location.search);
const source = q.get('source') || 'screen';
const auto = q.get('auto') === '1';
const frames = parseInt(q.get('frames') || '0', 10);
const seconds = parseFloat(q.get('seconds') || '0');

function log(msg, level = 'info') {
    const el = document.createElement('div');
    el.className = level;
    el.textContent = msg;
    logEl.appendChild(el);
}
function setStatus(s) { statusEl.textContent = s; document.title = s; }
function setFps(s) { fpsEl.textContent = s; }

let sender = null;

async function capture() {
    if (source === 'camera') {
        // Deterministic path for headless e2e (Chrome --use-fake-device).
        return navigator.mediaDevices.getUserMedia({
            video: { width: 320, height: 240, frameRate: 30 },
            audio: { sampleRate: 48000, channelCount: 2 },
        });
    }
    // Real screen share. Needs a user gesture unless Chrome is launched with
    // --auto-select-desktop-capture-source / --auto-accept-this-tab-capture.
    return navigator.mediaDevices.getDisplayMedia({
        video: { frameRate: 30 },
        audio: true,
    });
}

async function run() {
    startBtn.disabled = true;
    try {
        log(`capturing (${source}) …`);
        const stream = await capture();
        stopBtn.disabled = false;
        sender = new LumenSender({
            stream,
            test: { frames, seconds },
            hooks: {
                onLog: (m) => log(m),
                onStatus: setStatus,
                onStats: (s) => setFps(
                    `video=${s.video} (${s.fps.toFixed(1)} fps) ` +
                    `audio=${s.audio} over ${s.dt.toFixed(1)}s`),
                onError: (e) => log('error: ' + e.message, 'err'),
            },
        });
        await sender.start();
    } catch (e) {
        log('failed: ' + e.message, 'err');
        setStatus('ERROR');
    } finally {
        stopBtn.disabled = true;
        startBtn.disabled = false;
    }
}

startBtn.addEventListener('click', run);
stopBtn.addEventListener('click', () => sender && sender.stop());

// Camera source needs no gesture; auto-start. Screen source auto-starts only
// in test mode (relies on Chrome's auto-select flags); otherwise wait for the
// button click (user gesture).
if (source === 'camera' || auto) {
    run();
}
