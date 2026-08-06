/**
 * PanelResizer.js
 * Divisor arrastável entre o canvas 3D e o painel de dados lateral (#resizer-h/#data-panel) --
 * compartilhado entre preprocessor.html e posprocessor.html, que têm o mesmo layout de 3 regiões
 * (sidebar de câmera | canvas | painel de dados).
 */

/**
 * @param {() => void} [onResize] Chamado a cada frame de arraste e ao soltar -- tipicamente
 * `renderer3D.onWindowResize()`, pra manter o canvas 3D em sincronia com a nova largura.
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
