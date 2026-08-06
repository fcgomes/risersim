/**
 * ThemeToggle.js
 * Liga o botão #theme-toggle-btn à alternância claro/escuro -- idêntico entre
 * preprocessor.html/posprocessor.html: troca a classe do <body>, o texto do botão, a cor de
 * fundo da cena Three.js, e força um re-render.
 */

/**
 * @param {{currentTheme: string, renderer3D: object, render: () => void}} app Instância da app
 * (PreprocessorApp/RiserSimApp) -- só usa `currentTheme`/`renderer3D`/`render()`, que as duas já têm.
 */
export function initThemeToggle(app) {
    document.getElementById('theme-toggle-btn').addEventListener('click', () => {
        app.currentTheme = app.currentTheme === 'dark' ? 'light' : 'dark';
        document.body.className = app.currentTheme === 'dark' ? 'dark-mode' : 'light-mode';
        document.getElementById('theme-toggle-btn').innerText = app.currentTheme === 'dark' ? '☀️ Modo Claro' : '🌙 Modo Escuro';
        app.renderer3D.scene.background.setHex(app.currentTheme === 'dark' ? 0x1e1e2e : 0xffffff);
        app.render();
    });
}
