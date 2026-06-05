import { defineConfig } from 'vite';

// Example app (consumes the SDK in ../../src). Served by web_receiver at
// /sender/, so the base path and asset URLs are /sender/...
export default defineConfig({
    base: '/sender/',
    build: {
        outDir: 'dist',
        target: 'es2022',
        emptyOutDir: true,
    },
    server: {
        port: 5173,
        // Allow importing the SDK source from the repo's web/src (above root).
        fs: { allow: ['../..'] },
    },
});
