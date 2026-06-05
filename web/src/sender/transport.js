// WebTransport connection helpers for the sender.

export function hexToBytes(hex) {
    const out = new Uint8Array(hex.length / 2);
    for (let i = 0; i < out.length; ++i) {
        out[i] = parseInt(hex.substr(i * 2, 2), 16);
    }
    return out;
}

/** Fetch the receiver's session descriptor (transport URL + cert hash). */
export async function fetchSession(url = '/api/session') {
    const res = await fetch(url);
    if (!res.ok) throw new Error('session fetch status ' + res.status);
    return res.json();
}

/**
 * Open a WebTransport connection to the receiver, pinning its self-signed
 * cert via serverCertificateHashes. Resolves once the session is ready.
 */
export async function openTransport(session) {
    const wt = new WebTransport(session.transport.url, {
        serverCertificateHashes: [{
            algorithm: 'sha-256',
            value: hexToBytes(session.transport.cert_sha256),
        }],
    });
    await wt.ready;
    return wt;
}
