// Reconnect backoff policy (pure — unit-tested in reconnect.test.js).

/**
 * Exponential backoff with a cap. attempt is 0-based (first retry = attempt 0).
 * @returns {number} delay in ms before the given retry attempt.
 */
export function nextBackoffMs(attempt, { baseMs = 500, maxMs = 8000 } = {}) {
    if (attempt < 0) attempt = 0;
    const ms = baseMs * 2 ** attempt;
    return Math.min(ms, maxMs);
}
