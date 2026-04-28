import { defineConfig } from 'vite';

export default defineConfig({
    base: '/sender/',
    build: {
        outDir: 'dist',
        target: 'es2022',
        emptyOutDir: true,
    },
    server: {
        port: 5173,
    },
});
