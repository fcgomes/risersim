import { HDF5LoaderService } from './services/HDF5LoaderService.js';
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
            this.render();
        });
    }

    async loadSimulationData(fileOrUrl) {
        try {
            this.simulation = await HDF5LoaderService.load(fileOrUrl);
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
        if (!tbody) return;
        tbody.innerHTML = '';

        step.elements.forEach(elem => {
            const tr = document.createElement('tr');
            tr.innerHTML = `
                <td>${elem.id}</td>
                <td class="tension-val">${elem.tensionEffectiveKn.toFixed(2)} kN</td>
                <td>${elem.bendingMomentKnm.toFixed(2)} kN.m</td>
                <td style="font-family:monospace;">${elem.curvature.toFixed(5)}</td>
                <td style="font-weight:bold; color:#e11d48;">${elem.vonMisesMpa.toFixed(2)} MPa</td>
                <td style="font-weight:bold; color:#2563eb;">${elem.mbrSafetyFactor.toFixed(2)}</td>
            `;
            tbody.appendChild(tr);
        });
    }
}

// Inicializa a aplicação ao carregar a página
window.addEventListener('DOMContentLoaded', () => {
    window.riserApp = new RiserSimApp();
});
