import { DataLoaderService } from './services/DataLoaderService.js';
import { ColorMapService } from './services/ColorMapService.js';
import { Riser3DRenderer } from './renderers/Riser3DRenderer.js';
import { CameraViewController } from './renderers/CameraViewController.js';

/**
 * app.js
 * Controlador principal da aplicação Web riserSim 3D.
 */
class RiserSimApp {
    constructor() {
        this.simulation = null;
        this.currentStepIdx = 0;
        this.isPlaying = false;
        this.animationTimer = null;
        this.currentTheme = 'dark';

        this.initUI();
    }

    async initUI() {
        const canvas = document.getElementById('three-canvas');
        this.renderer3D = new Riser3DRenderer(canvas);
        this.cameraController = new CameraViewController(this.renderer3D.camera, this.renderer3D.controls);

        this.bindEvents();
        await this.loadSimulationData('../catenary_results.h5');
    }

    bindEvents() {
        // Slider
        const slider = document.getElementById('step-slider');
        slider.addEventListener('input', (e) => {
            this.currentStepIdx = parseInt(e.target.value);
            this.render();
        });

        // Animação
        document.getElementById('play-btn').addEventListener('click', () => this.play());
        document.getElementById('pause-btn').addEventListener('click', () => this.pause());
        document.getElementById('prev-btn').addEventListener('click', () => {
            this.currentStepIdx = Math.max(0, this.currentStepIdx - 1);
            slider.value = this.currentStepIdx;
            this.render();
        });
        document.getElementById('next-btn').addEventListener('click', () => {
            if (!this.simulation) return;
            this.currentStepIdx = Math.min(this.simulation.totalSteps - 1, this.currentStepIdx + 1);
            slider.value = this.currentStepIdx;
            this.render();
        });

        // Atalhos de Câmera (ISO, XY, XZ, YZ)
        document.getElementById('view-iso-btn').addEventListener('click', () => this.cameraController.setView('ISO', this.getCurrentStep()));
        document.getElementById('view-xy-btn').addEventListener('click', () => this.cameraController.setView('XY', this.getCurrentStep()));
        document.getElementById('view-xz-btn').addEventListener('click', () => this.cameraController.setView('XZ', this.getCurrentStep()));
        document.getElementById('view-yz-btn').addEventListener('click', () => this.cameraController.setView('YZ', this.getCurrentStep()));

        // Mapa de cores
        document.getElementById('colormap-select').addEventListener('change', () => this.render());

        // File Upload
        document.getElementById('json-input').addEventListener('change', (e) => {
            const file = e.target.files[0];
            if (file) this.loadSimulationData(file);
        });

        // Tema Claro/Escuro
        document.getElementById('theme-toggle-btn').addEventListener('click', () => {
            this.currentTheme = this.currentTheme === 'dark' ? 'light' : 'dark';
            document.body.className = this.currentTheme === 'dark' ? 'dark-mode' : 'light-mode';
            document.getElementById('theme-toggle-btn').innerText = this.currentTheme === 'dark' ? '☀️ Modo Claro' : '🌙 Modo Escuro';
            this.renderer3D.scene.background.setHex(this.currentTheme === 'dark' ? 0x1e1e2e : 0xffffff);
            this.render();
        });
    }

    async loadSimulationData(fileOrUrl) {
        try {
            this.simulation = await DataLoaderService.load(fileOrUrl);
            const slider = document.getElementById('step-slider');
            slider.max = this.simulation.totalSteps - 1;
            slider.value = this.simulation.totalSteps - 1;
            this.currentStepIdx = this.simulation.totalSteps - 1;

            this.render();
            console.log("✅ Simulação OO carregada com sucesso!", this.simulation);
        } catch (err) {
            console.error("Erro ao carregar simulação OO: ", err);
        }
    }

    getCurrentStep() {
        return this.simulation ? this.simulation.getStep(this.currentStepIdx) : null;
    }

    play() {
        if (this.isPlaying || !this.simulation) return;
        this.isPlaying = true;
        const slider = document.getElementById('step-slider');
        this.animationTimer = setInterval(() => {
            this.currentStepIdx = (this.currentStepIdx + 1) % this.simulation.totalSteps;
            slider.value = this.currentStepIdx;
            this.render();
        }, 250);
    }

    pause() {
        if (this.isPlaying) {
            clearInterval(this.animationTimer);
            this.isPlaying = false;
        }
    }

    render() {
        const step = this.getCurrentStep();
        if (!step) return;

        const colormap = document.getElementById('colormap-select').value;
        const tensionRange = this.simulation.getTensionRange();

        // Atualiza Cards
        const totalSteps = this.simulation.totalSteps - 1;
        const loadPct = (step.loadFactor * 100).toFixed(0);
        document.getElementById('step-label').innerText = `${step.stepIndex}/${totalSteps} (${loadPct}%)`;
        document.getElementById('load-factor').innerText = `${loadPct} %`;
        document.getElementById('top-tension').innerText = `${step.getTopTension().toFixed(2)} kN`;
        document.getElementById('max-depth').innerText = `${step.getMaxDepth().toFixed(2)} m`;

        // Renderiza Cena 3D
        this.renderer3D.renderStep(step, colormap, tensionRange, this.currentTheme);
        this.updateTable(step);
    }

    updateTable(step) {
        const tbody = document.getElementById('elements-tbody');
        const stepTitle = document.getElementById('table-step-title');

        if (stepTitle && this.simulation) {
            const totalSteps = this.simulation.totalSteps - 1;
            const loadPct = (step.loadFactor * 100).toFixed(0);
            stepTitle.innerText = `Passo ${step.stepIndex}/${totalSteps} (${loadPct}% da Carga / Offset)`;
        }

        if (!tbody) return;
        tbody.innerHTML = '';

        const nodes = step.nodes;
        const elements = step.elements;

        elements.forEach((elem, idx) => {
            const tr = document.createElement('tr');

            const n1 = nodes[idx];
            const n2 = nodes[idx + 1] || n1;

            const z1 = n1 ? n1.z.toFixed(2) : "0.00";
            const z2 = n2 ? n2.z.toFixed(2) : "0.00";

            let statusBadge = `<span class="status-water">🌊 Suspenso</span>`;
            if (idx >= 14 && idx <= 26) {
                statusBadge = `<span class="status-buoy">🎈 Flutuador (Lazy Wave)</span>`;
            } else if (n2 && n2.z <= -99.5) {
                statusBadge = `<span class="status-seabed">🏖️ Fundo do Mar (TDZ)</span>`;
            }

            const tensionStr = elem.tensionEffectiveKn !== undefined ? elem.tensionEffectiveKn.toFixed(1) : "0.0";
            const momentStr = elem.bendingMomentKnm !== undefined ? elem.bendingMomentKnm.toFixed(2) : "0.00";
            const curvStr = elem.curvature !== undefined ? elem.curvature.toExponential(3) : "0.000e+00";
            const vmStr = elem.vonMisesMpa !== undefined ? elem.vonMisesMpa.toFixed(1) : "0.0";
            const mbrStr = elem.mbrSafetyFactor !== undefined ? elem.mbrSafetyFactor.toFixed(2) : "1.00";

            tr.innerHTML = `
                <td style="font-weight:bold;">Elemento ${elem.id}</td>
                <td>Nó ${idx + 1} ➔ Nó ${idx + 2}</td>
                <td>${z1}m ➔ ${z2}m</td>
                <td>${statusBadge}</td>
                <td class="tension-val">${tensionStr} kN</td>
                <td>${momentStr} kN.m</td>
                <td style="font-family:monospace;">${curvStr}</td>
                <td style="font-weight:bold; color:#e11d48;">${vmStr} MPa</td>
                <td style="font-weight:bold; color:#2563eb;">${mbrStr}</td>
            `;
            tbody.appendChild(tr);
        });
    }
}

// Inicializa a aplicação ao carregar a página
window.addEventListener('DOMContentLoaded', () => {
    window.riserApp = new RiserSimApp();
});
