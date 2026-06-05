// LumenSender — the SDK's public sending object.
//
// Encodes a caller-provided MediaStream with WebCodecs, fragments each frame
// with the Lumen wire header, and ships it over WebTransport datagrams, with
// built-in reconnect + error recovery. It does NOT capture media or touch the
// DOM — the caller (an app) owns getDisplayMedia/getUserMedia and the UI, and
// passes the stream in. Progress is reported through callbacks.

import { fragment } from './sender/fragmenter.js';
import { CHANNEL_VIDEO, CHANNEL_AUDIO, CHANNEL_CONTROL } from './wire/channel.js';
import { fetchSession, openTransport } from './sender/transport.js';
import { pickVideoConfig, AUDIO_CONFIG } from './sender/encoders.js';
import { nextBackoffMs } from './sender/reconnect.js';

const SSRC_VIDEO = 0x10000001;
const SSRC_AUDIO = 0x10000002;
const noop = () => {};

export class LumenSender {
    /**
     * @param {object} o
     * @param {MediaStream} o.stream          caller-owned capture stream
     * @param {object}      [o.session]       session descriptor; else fetched
     * @param {string}      [o.sessionUrl]    where to fetch the session
     * @param {object}      [o.video]         {bitrate, framerate} overrides
     * @param {object}      [o.test]          {frames, seconds} test budget
     * @param {object}      [o.hooks]         {onLog,onStatus,onStats,onError}
     */
    constructor({ stream, session = null, sessionUrl = '/api/session',
                  video = {}, test = {}, hooks = {} } = {}) {
        if (!stream) throw new Error('LumenSender: a MediaStream is required');
        this._stream = stream;
        this._session = session;
        this._sessionUrl = sessionUrl;
        this._videoOpts = video;
        this._test = test;
        this._cb = {
            onLog: hooks.onLog || noop,
            onStatus: hooks.onStatus || noop,
            onStats: hooks.onStats || noop,
            onError: hooks.onError || noop,
        };

        this._running = false;
        this._reconnecting = false;
        this._wt = null;
        this._writer = null;
        this._videoEnc = null;
        this._audioEnc = null;
        this._vConfig = null;
        this._forceKeyframe = false;
        this._videoFrameIndex = 0n;
        this._audioFrameIndex = 0n;
        this._sentVideo = 0;
        this._sentAudio = 0;
        this._encoderErrors = 0;
    }

    async start() {
        if (this._running) return;
        this._running = true;
        try {
            if (!this._session) {
                this._cb.onLog('fetching session …');
                this._session = await fetchSession(this._sessionUrl);
            }
            this._cb.onStatus('connecting');
            await this._connect();
            await this._setupEncoders();
            this._cb.onStatus('live');
            await this._pump();
            await this._finish('stopped');
        } catch (e) {
            this._cb.onError(e);
            this._cb.onStatus('error');
            this._running = false;
        }
    }

    async stop() {
        this._running = false;  // the pump loop observes this and unwinds
    }

    // ── transport ───────────────────────────────────────────────────
    async _connect() {
        this._wt = await openTransport(this._session);
        this._writer = this._wt.datagrams.writable.getWriter();
        this._cb.onLog('WebTransport open');
        // Background watch: a closed session triggers reconnect.
        this._wt.closed
            .then(() => this._onDisconnected('closed'))
            .catch(() => this._onDisconnected('closed (error)'));
    }

    _onDisconnected(why) {
        if (!this._running || this._reconnecting) return;
        this._cb.onLog('transport down: ' + why);
        this._reconnect();
    }

    async _reconnect() {
        if (this._reconnecting) return;
        this._reconnecting = true;
        this._writer = null;
        this._cb.onStatus('reconnecting');
        let attempt = 0;
        while (this._running) {
            const delay = nextBackoffMs(attempt++);
            await new Promise((r) => setTimeout(r, delay));
            if (!this._running) break;
            try {
                this._wt = await openTransport(this._session);
                this._writer = this._wt.datagrams.writable.getWriter();
                this._wt.closed
                    .then(() => this._onDisconnected('closed'))
                    .catch(() => this._onDisconnected('closed (error)'));
                this._forceKeyframe = true;  // resync the decoder after a gap
                this._cb.onLog('reconnected');
                this._cb.onStatus('live');
                break;
            } catch (e) {
                this._cb.onLog('reconnect attempt failed: ' + e.message);
            }
        }
        this._reconnecting = false;
    }

    _sendFrags(frags) {
        const writer = this._writer;
        if (!writer) return;  // dropping while reconnecting (datagrams are lossy)
        for (const f of frags) {
            try {
                writer.write(f);
            } catch (e) {
                this._onDisconnected('write failed: ' + e.message);
                return;
            }
        }
    }

    // ── encoders ────────────────────────────────────────────────────
    async _setupEncoders() {
        const vTrack = this._stream.getVideoTracks()[0];
        const aTrack = this._stream.getAudioTracks()[0] || null;
        this._vTrack = vTrack;
        this._aTrack = aTrack;

        const s = vTrack.getSettings();
        const vparams = {
            width: s.width || 1280,
            height: s.height || 720,
            framerate: this._videoOpts.framerate || s.frameRate || 30,
            bitrate: this._videoOpts.bitrate || 2_500_000,
        };
        this._vConfig = await pickVideoConfig(vparams, this._cb.onLog);
        if (!this._vConfig) throw new Error('no supported video codec');
        this._cb.onLog('video codec = ' + this._vConfig.codec +
                       ` ${vparams.width}x${vparams.height}`);

        this._createVideoEncoder();
        if (aTrack) this._createAudioEncoder();

        // The video track ending (user clicks the browser's "Stop sharing")
        // cleanly stops the session.
        vTrack.addEventListener('ended', () => { this.stop(); });
    }

    _createVideoEncoder() {
        this._videoEnc = new VideoEncoder({
            output: (chunk) => {
                const buf = new Uint8Array(chunk.byteLength);
                chunk.copyTo(buf);
                this._sendFrags(fragment({
                    channel: CHANNEL_VIDEO, ssrc: SSRC_VIDEO,
                    frameIndex: this._videoFrameIndex,
                    timestampUs: BigInt(Math.round(chunk.timestamp || 0)),
                    isKeyframe: chunk.type === 'key', payload: buf,
                }));
                this._videoFrameIndex++; this._sentVideo++;
            },
            error: (e) => this._onEncoderError('video', e),
        });
        this._videoEnc.configure(this._vConfig);
    }

    _createAudioEncoder() {
        this._audioEnc = new AudioEncoder({
            output: (chunk) => {
                const buf = new Uint8Array(chunk.byteLength);
                chunk.copyTo(buf);
                this._sendFrags(fragment({
                    channel: CHANNEL_AUDIO, ssrc: SSRC_AUDIO,
                    frameIndex: this._audioFrameIndex,
                    timestampUs: BigInt(Math.round(chunk.timestamp || 0)),
                    isKeyframe: false, payload: buf,
                }));
                this._audioFrameIndex++; this._sentAudio++;
            },
            error: (e) => this._onEncoderError('audio', e),
        });
        this._audioEnc.configure(AUDIO_CONFIG);
    }

    _onEncoderError(kind, e) {
        this._cb.onLog(`${kind} encoder error: ${e.message}`);
        if (++this._encoderErrors > 5) {
            this._cb.onError(new Error('too many encoder errors'));
            this.stop();
            return;
        }
        // Best-effort recreate.
        try {
            if (kind === 'video') this._createVideoEncoder();
            else this._createAudioEncoder();
        } catch (_) { /* surfaced via next error */ }
    }

    // ── pump ────────────────────────────────────────────────────────
    async _pump() {
        const vReader = new MediaStreamTrackProcessor({ track: this._vTrack })
            .readable.getReader();
        const aReader = this._aTrack
            ? new MediaStreamTrackProcessor({ track: this._aTrack })
                  .readable.getReader()
            : null;

        const start = performance.now();
        const frames = this._test.frames || 0;     // 0 ⇒ unbounded
        const seconds = this._test.seconds || 0;

        const tick = setInterval(() => {
            const dt = (performance.now() - start) / 1000;
            this._cb.onStats({
                video: this._sentVideo, audio: this._sentAudio,
                fps: this._sentVideo / Math.max(0.001, dt), dt,
            });
        }, 250);

        if (aReader) {
            (async () => {
                while (this._running) {
                    const { value, done } = await aReader.read();
                    if (done) break;
                    if (this._audioEnc && this._audioEnc.state === 'configured') {
                        this._audioEnc.encode(value);
                    }
                    value.close();
                }
            })();
        }

        while (this._running) {
            if (frames && this._sentVideo >= frames) break;
            if (seconds && (performance.now() - start) / 1000 >= seconds) break;
            const { value, done } = await vReader.read();
            if (done) break;
            if (this._videoEnc && this._videoEnc.state === 'configured') {
                const opts = this._forceKeyframe ? { keyFrame: true } : undefined;
                this._forceKeyframe = false;
                this._videoEnc.encode(value, opts);
            }
            value.close();
        }
        this._running = false;
        clearInterval(tick);
    }

    // ── teardown ──────────────────────────────────────────────────────
    async _finish(status) {
        const start = this._finishStart || performance.now();
        try { await this._videoEnc?.flush(); } catch (_) { /* ignore */ }
        try { await this._audioEnc?.flush(); } catch (_) { /* ignore */ }

        // Tell the receiver we're done (it keys FINAL_STATS off this).
        const doneMsg = `done:video=${this._sentVideo},` +
                        `audio=${this._sentAudio},dt_ms=${Math.round(start)}`;
        this._sendFrags(fragment({
            channel: CHANNEL_CONTROL, ssrc: 0, frameIndex: 0n, timestampUs: 0n,
            isKeyframe: false, payload: new TextEncoder().encode(doneMsg),
        }));
        try { await this._writer?.close(); } catch (_) { /* ignore */ }
        for (const t of this._stream.getTracks()) t.stop();
        this._cb.onStatus(status);
        this._cb.onLog(`sent ${this._sentVideo} video, ${this._sentAudio} audio`);
    }
}
