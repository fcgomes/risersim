/**
 * PanelResizer.js
 * Drag handle between the 3D canvas and the side data panel (#resizer-h/#data-panel) -- shared
 * between preprocessor.html and posprocessor.html, which have the same 3-region layout (camera
 * sidebar | canvas | data panel).
 */

/**
 * @param {() => void} [onResize] Called on every drag frame and on release -- typically
 * `renderer3D.onWindowResize()`, to keep the 3D canvas in sync with the new width.
 */
export function initPanelResizer(onResize) {
    const resizer = document.getElementById('resizer-h');
    const dataPanel = document.getElementById('data-panel');
    if (!resizer || !dataPanel) return;

    let isDragging = false, startX = 0, startWidth = 0;

    resizer.addEventListener('mousedown', (e) => {
        isDragging = true;
        startX = e.clientX;
        startWidth = dataPanel.getBoundingClientRect().width;
        resizer.classList.add('dragging');
        document.body.style.cursor = 'col-resize';
        e.preventDefault();
    });

    window.addEventListener('mousemove', (e) => {
        if (!isDragging) return;
        const dx = e.clientX - startX;
        const newWidth = Math.min(window.innerWidth * 0.75, Math.max(320, startWidth - dx));
        dataPanel.style.width = `${newWidth}px`;
        if (onResize) onResize();
    });

    window.addEventListener('mouseup', () => {
        if (isDragging) {
            isDragging = false;
            resizer.classList.remove('dragging');
            document.body.style.cursor = '';
            if (onResize) onResize();
        }
    });
}
