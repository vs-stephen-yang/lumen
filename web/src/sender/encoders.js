// WebCodecs encoder configuration for the sender.

// Tried in order; first supported wins. H.264 Annex-B is preferred because the
// native receiver decodes it via Media Foundation; VP8 is a software fallback.
const VIDEO_CANDIDATES = [
    { codec: 'avc1.42E01E', isAvc: true },   // H.264 baseline
    { codec: 'avc1.640028', isAvc: true },   // H.264 high
    { codec: 'vp8',         isAvc: false },
];

/**
 * Probe VideoEncoder.isConfigSupported and return the first usable config
 * (or null if none). `log` receives human-readable progress lines.
 */
export async function pickVideoConfig({ width, height, framerate, bitrate },
                                       log = () => {}) {
    for (const c of VIDEO_CANDIDATES) {
        const cfg = {
            codec: c.codec,
            width, height, framerate, bitrate,
            latencyMode: 'realtime',
            hardwareAcceleration: 'no-preference',
        };
        if (c.isAvc) cfg.avc = { format: 'annexb' };
        try {
            const r = await VideoEncoder.isConfigSupported(cfg);
            log(`  ${c.codec}: supported=${r && r.supported}`);
            if (r && r.supported) return cfg;
        } catch (e) {
            log(`  ${c.codec}: error ${e.message}`);
        }
    }
    return null;
}

export const AUDIO_CONFIG = {
    codec: 'opus',
    sampleRate: 48000,
    numberOfChannels: 2,
    bitrate: 64_000,
};
