/**
 * ZoomWindowController.js
 * "Zoom por janela" (tipo CAD): ativa via botão, o próximo arraste do mouse sobre o canvas
 * desenha um retângulo de seleção; ao soltar, converte o retângulo para NDC e pede ao
 * CameraViewController para reenquadrar nele (zoomToWindow()), e desativa o modo.
 *
 * Compartilhado entre preprocessor.html e posprocessor.html -- assume os IDs fixos que as duas
 * páginas já usam (#three-canvas, #canvas-container, #zoom-rect, #zoom-window-btn).
 */
export class ZoomWindowController {
    /**
     * @param {import('../renderers/CameraViewController.js').CameraViewController} cameraController
     * @param {import('../renderers/Riser3DRenderer.js').Riser3DRenderer} renderer3D
     */
    constructor(cameraController, renderer3D) {
        this.cameraController = cameraController;
        this.renderer3D = renderer3D;
        this.active = false;
        this._bindEvents();
    }

    _bindEvents() {
        const canvas = document.getElementById('three-canvas');
        const container = document.getElementById('canvas-container');
        const rectEl = document.getElementById('zoom-rect');
        let dragging = false, startX = 0, startY = 0;

        canvas.addEventListener('mousedown', (e) => {
            if (!this.active) return;
            dragging = true;
            const rect = container.getBoundingClientRect();
            startX = e.clientX - rect.left;
            startY = e.clientY - rect.top;
            rectEl.style.left = `${startX}px`;
            rectEl.style.top = `${startY}px`;
            rectEl.style.width = '0px';
            rectEl.style.height = '0px';
            rectEl.style.display = 'block';
            e.preventDefault();
        });

        window.addEventListener('mousemove', (e) => {
            if (!dragging) return;
            const rect = container.getBoundingClientRect();
            const curX = e.clientX - rect.left, curY = e.clientY - rect.top;
            const x = Math.min(startX, curX), y = Math.min(startY, curY);
            rectEl.style.left = `${x}px`;
            rectEl.style.top = `${y}px`;
            rectEl.style.width = `${Math.abs(curX - startX)}px`;
            rectEl.style.height = `${Math.abs(curY - startY)}px`;
        });

        window.addEventListener('mouseup', (e) => {
            if (!dragging) return;
            dragging = false;
            rectEl.style.display = 'none';
            const rect = container.getBoundingClientRect();
            const endX = e.clientX - rect.left, endY = e.clientY - rect.top;
            const toNdc = (px, py) => ({ x: (px / rect.width) * 2 - 1, y: -((py / rect.height) * 2 - 1) });
            const p1 = toNdc(startX, startY), p2 = toNdc(endX, endY);
            this.cameraController.zoomToWindow({ x1: p1.x, y1: p1.y, x2: p2.x, y2: p2.y });
            this.setActive(false);
        });

        window.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && this.active) this.setActive(false);
        });
    }

    setActive(active) {
        this.active = active;
        const canvas = document.getElementById('three-canvas');
        const btn = document.getElementById('zoom-window-btn');
        if (canvas) canvas.style.cursor = active ? 'crosshair' : '';
        if (btn) btn.classList.toggle('active', active);
        if (this.renderer3D && this.renderer3D.controls) this.renderer3D.controls.enabled = !active;
    }

    toggle() {
        this.setActive(!this.active);
    }
}
