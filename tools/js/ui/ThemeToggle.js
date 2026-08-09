/**
 * ThemeToggle.js
 * Wires the #theme-toggle-btn button to the light/dark toggle -- identical between
 * preprocessor.html/posprocessor.html: swaps the <body> class, the button text, the Three.js
 * scene background color, and forces a re-render.
 */

/**
 * @param {{currentTheme: string, renderer3D: object, render: () => void}} app App instance
 * (PreprocessorApp/RiserSimApp) -- only uses `currentTheme`/`renderer3D`/`render()`, which both already have.
 */
export function initThemeToggle(app) {
    document.getElementById('theme-toggle-btn').addEventListener('click', () => {
        app.currentTheme = app.currentTheme === 'dark' ? 'light' : 'dark';
        document.body.className = app.currentTheme === 'dark' ? 'dark-mode' : 'light-mode';
        document.getElementById('theme-toggle-btn').innerText = app.currentTheme === 'dark' ? '☀️ Modo Claro' : '🌙 Modo Escuro';
        app.renderer3D.scene.background.setHex(app.currentTheme === 'dark' ? 0x0a0a10 : 0xf7f6fb);
        app.render();
    });
}
