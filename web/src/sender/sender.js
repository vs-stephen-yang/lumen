// Sender orchestration: capture (getUserMedia) → WebCodecs encode → fragment →
// WebTransport datagrams, then a control-channel "done:" message the receiver
// keys its FINAL_STATS line off. Productionized from the original test page.

import { fragment } from './fragmenter.js';
import { CHANNEL_VIDEO, CHANNEL_AUDIO, CHANNEL_CONTROL } from '../wire/channel.js';
import { fetchSession, openTransport } from './transport.js';
import { pickVideoConfig, AUDIO_CONFIG } from './encoders.js';

const SSRC_VIDEO = 0x10000001;
const SSRC_AUDIO = 0x10000002;

const VIDEO = { width: 320, height: 240, framerate: 30, bitrate: 200_000 };

/**
 * Run one capture→send session. Resolves when the target frame/time budget is
 * reached and the done message has been flushed.
 *
 * @param {object}   opts
 * @param {number}   opts.frames    stop after this many video frames
 * @param {number}   opts.seconds   …or this many seconds, whichever first
 * @param {function} opts.log       progress line sink
 * @param {function} opts.setStatus status string sink ('OK' | 'ERROR' | …)
 * @param {function} opts.setFps    fps/heartbeat string sink
 */
export async function runSender({ frames = 120, seconds = 6,
                                  log = () => {}, setStatus = () => {},
                                  setFps = () => {} } = {}) {
    if (typeof WebTransport === 'undefined') {
        log('WebTransport API not available'); setStatus('ERROR'); return;
    }
    if (typeof VideoEncoder === 'undefined' ||
        typeof AudioEncoder === 'undefined') {
        log('WebCodecs not available'); setStatus('ERROR'); return;
    }

    log('Fetching /api/session …');
    let session;
    try {
        session = await fetchSession();
    } catch (e) {
        log('Session fetch failed: ' + e.message); setStatus('ERROR'); return;
    }
    log('transport.url = ' + session.transport.url);

    let wt;
    try {
        wt = await openTransport(session);
    } catch (e) {
        log('WebTransport.ready rejected: ' + e.message);
        setStatus('ERROR'); return;
    }
    log('WebTransport open');
    const writer = wt.datagrams.writable.getWriter();

    const vConfig = await pickVideoConfig(VIDEO, log);
    if (!vConfig) { log('No supported video codec'); setStatus('ERROR'); return; }
    log('video codec = ' + vConfig.codec);

    let stream;
    try {
        stream = await navigator.mediaDevices.getUserMedia({
            video: { width: VIDEO.width, height: VIDEO.height,
                     frameRate: VIDEO.framerate },
            audio: { sampleRate: 48000, channelCount: 2 },
        });
    } catch (e) {
        log('getUserMedia failed: ' + e.message); setStatus('ERROR'); return;
    }
    const [vTrack] = stream.getVideoTracks();
    const [aTrack] = stream.getAudioTracks();

    let videoFrameIndex = 0n;
    let audioFrameIndex = 0n;
    let sentVideo = 0;
    let sentAudio = 0;

    const videoEnc = new VideoEncoder({
        output: (chunk) => {
            const buf = new Uint8Array(chunk.byteLength);
            chunk.copyTo(buf);
            const frags = fragment({
                channel: CHANNEL_VIDEO, ssrc: SSRC_VIDEO,
                frameIndex: videoFrameIndex,
                timestampUs: BigInt(Math.round(chunk.timestamp || 0)),
                isKeyframe: chunk.type === 'key', payload: buf,
            });
            for (const f of frags) writer.write(f);
            videoFrameIndex++; sentVideo++;
        },
        error: (e) => log('VideoEncoder error: ' + e.message),
    });
    videoEnc.configure(vConfig);

    const audioEnc = new AudioEncoder({
        output: (chunk) => {
            const buf = new Uint8Array(chunk.byteLength);
            chunk.copyTo(buf);
            const frags = fragment({
                channel: CHANNEL_AUDIO, ssrc: SSRC_AUDIO,
                frameIndex: audioFrameIndex,
                timestampUs: BigInt(Math.round(chunk.timestamp || 0)),
                isKeyframe: false, payload: buf,
            });
            for (const f of frags) writer.write(f);
            audioFrameIndex++; sentAudio++;
        },
        error: (e) => log('AudioEncoder error: ' + e.message),
    });
    audioEnc.configure(AUDIO_CONFIG);

    const vReader =
        new MediaStreamTrackProcessor({ track: vTrack }).readable.getReader();
    const aReader =
        new MediaStreamTrackProcessor({ track: aTrack }).readable.getReader();

    const start = performance.now();
    let stopped = false;
    const heartbeat = setInterval(() => {
        const dt = (performance.now() - start) / 1000;
        setFps(`sent: video=${sentVideo} ` +
               `(${(sentVideo / Math.max(0.001, dt)).toFixed(1)} fps), ` +
               `audio=${sentAudio} over ${dt.toFixed(2)}s`);
    }, 250);

    // Audio pump (runs alongside the video pump that drives the lifecycle).
    (async () => {
        while (!stopped) {
            const { value, done } = await aReader.read();
            if (done) break;
            if (audioEnc.state === 'configured') audioEnc.encode(value);
            value.close();
        }
    })();

    while (!stopped && sentVideo < frames &&
           (performance.now() - start) / 1000 < seconds) {
        const { value, done } = await vReader.read();
        if (done) break;
        if (videoEnc.state === 'configured') videoEnc.encode(value);
        value.close();
    }
    stopped = true;

    try { await videoEnc.flush(); } catch (_) { /* ignore */ }
    try { await audioEnc.flush(); } catch (_) { /* ignore */ }
    clearInterval(heartbeat);

    const dt = (performance.now() - start) / 1000;
    log(`sent ${sentVideo} video, ${sentAudio} audio in ${dt.toFixed(2)}s ` +
        `(${(sentVideo / dt).toFixed(2)} fps)`);

    const doneMsg =
        `done:video=${sentVideo},audio=${sentAudio},dt_ms=${Math.round(dt * 1000)}`;
    const ctrl = fragment({
        channel: CHANNEL_CONTROL, ssrc: 0, frameIndex: 0n, timestampUs: 0n,
        isKeyframe: false, payload: new TextEncoder().encode(doneMsg),
    });
    for (const f of ctrl) await writer.write(f);
    await writer.close();

    setStatus('OK');
    // Keep the page alive briefly so the receiver flushes before teardown.
    await new Promise((r) => setTimeout(r, 1500));
}
