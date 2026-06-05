import { describe, it, expect } from 'vitest';
import { nextBackoffMs } from './reconnect.js';

describe('nextBackoffMs', () => {
    it('doubles per attempt from the base', () => {
        expect(nextBackoffMs(0, { baseMs: 500, maxMs: 8000 })).toBe(500);
        expect(nextBackoffMs(1, { baseMs: 500, maxMs: 8000 })).toBe(1000);
        expect(nextBackoffMs(2, { baseMs: 500, maxMs: 8000 })).toBe(2000);
        expect(nextBackoffMs(3, { baseMs: 500, maxMs: 8000 })).toBe(4000);
    });

    it('caps at maxMs', () => {
        expect(nextBackoffMs(4, { baseMs: 500, maxMs: 8000 })).toBe(8000);
        expect(nextBackoffMs(10, { baseMs: 500, maxMs: 8000 })).toBe(8000);
    });

    it('clamps negative attempts and uses defaults', () => {
        expect(nextBackoffMs(-3)).toBe(500);   // default base
        expect(nextBackoffMs(99)).toBe(8000);   // default cap
    });
});
