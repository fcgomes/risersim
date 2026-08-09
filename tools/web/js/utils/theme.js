/**
 * theme.js
 * Persists the light/dark choice across full-page navigation (dashboard.html <-> project.html)
 * and keeps it live-synced between project.html and its pre/post-processor <iframe>s. Both rely
 * on `localStorage`, which same-origin iframes share with their parent -- so a write here is
 * visible to every other RiserSim document/tab already open, via the `storage` event (fired in
 * every OTHER same-origin Window, never the one that wrote it).
 */

const STORAGE_KEY = 'risersim-theme';

/** @returns {'dark'|'light'} Falls back to 'dark' if unset or storage is unavailable (e.g. a
 * strict privacy mode blocking localStorage) -- matches every page's hardcoded default class. */
export function getStoredTheme() {
    try {
        return localStorage.getItem(STORAGE_KEY) === 'light' ? 'light' : 'dark';
    } catch {
        return 'dark';
    }
}

/** @param {'dark'|'light'} theme */
export function setStoredTheme(theme) {
    try {
        localStorage.setItem(STORAGE_KEY, theme === 'light' ? 'light' : 'dark');
    } catch {
        // Storage unavailable -- the choice just won't survive navigation/sync this time.
    }
}

/** @param {(theme: 'dark'|'light') => void} callback Fires when theme changes in ANOTHER
 * same-origin document (parent, or a sibling pre/post iframe) -- never for this document's own
 * writes, so callers don't need to guard against reacting to themselves. */
export function onStoredThemeChange(callback) {
    window.addEventListener('storage', (e) => {
        if (e.key === STORAGE_KEY && e.newValue) {
            callback(e.newValue === 'light' ? 'light' : 'dark');
        }
    });
}
