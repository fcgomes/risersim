/**
 * ThemeToggle.js
 * Wires the #theme-toggle-btn button to the light/dark toggle -- identical between
 * preprocessor.html/posprocessor.html: swaps the <body> class, the button text, the Three.js
 * scene background color, and forces a re-render.
 *
 * Also applies whatever theme is already stored on init, and stays live-synced afterwards with
 * every other same-origin RiserSim document (dashboard.html, project.html, and any pre/post
 * <iframe> project.html has loaded) via theme.js -- see that file for how. Without this, the
 * theme used to reset on every full-page navigation and go stale in an already-open iframe when
 * toggled from its parent (or vice versa).
 */
import { getStoredTheme, setStoredTheme, onStoredThemeChange } from '../utils/theme.js';

/** @param {{currentTheme: string, renderer3D: object, render: () => void}} app */
function applyTheme(app, theme) {
    app.currentTheme = theme;
    document.body.className = theme === 'dark' ? 'dark-mode' : 'light-mode';
    const btn = document.getElementById('theme-toggle-btn');
    if (btn) btn.innerText = theme === 'dark' ? '☀️ Modo Claro' : '🌙 Modo Escuro';
    app.renderer3D.scene.background.setHex(theme === 'dark' ? 0x0a0a10 : 0xf7f6fb);
    app.render();
}

/**
 * @param {{currentTheme: string, renderer3D: object, render: () => void}} app App instance
 * (PreprocessorApp/RiserSimApp) -- only uses `currentTheme`/`renderer3D`/`render()`, which both already have.
 */
export function initThemeToggle(app) {
    applyTheme(app, getStoredTheme());

    document.getElementById('theme-toggle-btn').addEventListener('click', () => {
        const theme = app.currentTheme === 'dark' ? 'light' : 'dark';
        applyTheme(app, theme);
        setStoredTheme(theme);
    });

    onStoredThemeChange((theme) => {
        if (theme !== app.currentTheme) applyTheme(app, theme);
    });
}
