import { DataLoaderService } from './services/DataLoaderService.js';
import { ColorMapService } from './services/ColorMapService.js';
import { Riser3DRenderer } from './renderers/Riser3DRenderer.js';
import { CameraViewController } from './renderers/CameraViewController.js';
import { ProfileChartsController } from './charts/ProfileChartsController.js';
import { initPanelResizer } from './ui/PanelResizer.js';
import { ZoomWindowController } from './ui/ZoomWindowController.js';
import { bindCameraToolbar } from './ui/CameraToolbar.js';
import { switchTab } from './ui/TabPanel.js';
import { initThemeToggle } from './ui/ThemeToggle.js';

/**
 * app.js
 * Main controller for the riserSim 3D web application.
 */
class RiserSimApp {
    constructor() {
        this.simulation = null;
        this.currentStepIdx = 0;
        this.isPlaying = false;
        this.animationTimer = null;
        this.currentTheme = 'dark';
        this.activeTab = 'table'; // 'table' or 'charts'
        this.tableViewMode = 'elements'; // 'elements' or 'nodes'

        this.initUI();
    }

    async initUI() {
        // When embedded in project.html's <iframe> (the normal case -- see
        // preprocessor_app.js::initUI for the same check), the parent page already shows a
        // header with the brand mark, title, and theme toggle; showing this page's own copy too
        // doubled it up on screen. Hidden via inline style (not a body class) so it can't
        // collide with ThemeToggle.js's `body.className = '...'` full overwrites.
        if (window.self !== window.top) {
            document.getElementById('header').style.display = 'none';
            document.getElementById('main-layout').style.height = '100vh';
        }

        const canvas = document.getElementById('three-canvas');
        this.renderer3D = new Riser3DRenderer(canvas);
        this.cameraController = new CameraViewController(this.renderer3D.camera, this.renderer3D.controls, this.renderer3D);
        this.zoomWindow = new ZoomWindowController(this.cameraController, this.renderer3D);
        this.chartsController = new ProfileChartsController();

        this.activeViewportView = '3d'; // '3d', 'tension', 'moment', 'vm'
        this.bindEvents();
        initPanelResizer(() => this.renderer3D && this.renderer3D.onWindowResize());
        const { url, fallbackUrl } = this.resolveResultsUrl();
        await this.loadSimulationData(url, fallbackUrl);
    }

    /**
     * Resolves which results file to load on startup, in priority order:
     *   1. `?file=<url>` -- explicit override, any URL/relative path.
     *   2. `?project=<id>&run=<run-id>[&format=h5]` -- run-scoped results served by the run
     *      manager's API (run_server.py), e.g. after triggering a run from project.html.
     *   3. `../catenary_results.json` -- the historical hardcoded path, still populated by the
     *      manual `run_from_aml.py` CLI workflow (it copies results next to tools/ on completion)
     *      so opening posprocessor.html by hand keeps working unchanged.
     * @returns {{url: string, fallbackUrl: string}}
     */
    resolveResultsUrl() {
        const params = new URLSearchParams(window.location.search);

        const explicitFile = params.get('file');
        if (explicitFile) {
            return { url: explicitFile, fallbackUrl: '../catenary_results.json' };
        }

        const project = params.get('project');
        const run = params.get('run');
        if (project && run) {
            const base = `/api/projects/${encodeURIComponent(project)}/runs/${encodeURIComponent(run)}/results/`;
            const format = params.get('format') === 'h5' ? 'catenary_results.h5' : 'catenary_results.json';
            return { url: base + format, fallbackUrl: base + 'catenary_results.json' };
        }

        return { url: '../catenary_results.json', fallbackUrl: '../catenary_results.json' };
    }

    /** Switches between the side data-panel tabs (Controls/Visualization/Table/Load). */
    setDataTab(tab) {
        switchTab({ controls: 'tab-controls', viz: 'tab-viz', table: 'tab-table', load: 'tab-load' }, tab);
    }

    bindEvents() {
        // Floating tab switcher at the top of the viewport
        const vtabs = [
            { id: 'vtab-3d-btn', view: '3d' },
            { id: 'vtab-tension-btn', view: 'tension' },
            { id: 'vtab-moment-btn', view: 'moment' },
            { id: 'vtab-vm-btn', view: 'vm' }
        ];

        vtabs.forEach(item => {
            const btn = document.getElementById(item.id);
            if (btn) {
                btn.addEventListener('click', () => {
                    this.switchViewportView(item.view);
                });
            }
        });

        // Slider
        const slider = document.getElementById('step-slider');
        slider.addEventListener('input', (e) => {
            this.currentStepIdx = parseInt(e.target.value);
            this.render();
        });

        // Animation
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

        // Camera/zoom shortcuts (ISO/XY/XZ/YZ, fit/+/−/window)
        bindCameraToolbar(this.cameraController, this.zoomWindow, () => this.getCurrentStep(), () => this.getEnvBounds());

        // Side data-panel tabs
        document.getElementById('tab-controls-btn').addEventListener('click', () => this.setDataTab('controls'));
        document.getElementById('tab-viz-btn').addEventListener('click', () => this.setDataTab('viz'));
        document.getElementById('tab-table-btn').addEventListener('click', () => this.setDataTab('table'));
        document.getElementById('tab-load-btn').addEventListener('click', () => this.setDataTab('load'));

        // Elements / Nodes toggle within the "Table" tab
        document.getElementById('table-view-elements-btn').addEventListener('click', () => this.setTableViewMode('elements'));
        document.getElementById('table-view-nodes-btn').addEventListener('click', () => this.setTableViewMode('nodes'));

        // Analysis mode selector (Static / Dynamic)
        const modeSelect = document.getElementById('analysis-mode-select');
        if (modeSelect) {
            modeSelect.addEventListener('change', (e) => {
                if (!this.simulation) return;
                this.pause();
                this.simulation.mode = e.target.value;
                const slider = document.getElementById('step-slider');
                slider.max = Math.max(0, this.simulation.totalSteps - 1);
                slider.value = 0;
                this.currentStepIdx = 0;
                this.render();
            });
        }

        // Scalar field selector
        const scalarSelect = document.getElementById('scalar-field-select');
        if (scalarSelect) {
            scalarSelect.addEventListener('change', (e) => {
                const val = e.target.value;
                if (this.activeViewportView !== '3d') {
                    if (val === 'tension') this.switchViewportView('tension');
                    else if (val === 'moment' || val === 'curvature' || val === 'mbr') this.switchViewportView('moment');
                    else if (val === 'vonmises') this.switchViewportView('vm');
                }
                this.render();
            });
        }

        // Colormap
        document.getElementById('colormap-select').addEventListener('change', () => this.render());

        // File Upload
        document.getElementById('json-input').addEventListener('change', (e) => {
            const file = e.target.files[0];
            if (file) this.loadSimulationData(file);
        });

        // Light/dark theme
        initThemeToggle(this);
    }

    switchViewportView(view) {
        this.activeViewportView = view;

        const scalarSelect = document.getElementById('scalar-field-select');
        if (scalarSelect) {
            if (view === 'tension') scalarSelect.value = 'tension';
            else if (view === 'moment') scalarSelect.value = 'moment';
            else if (view === 'vm') scalarSelect.value = 'vonmises';
        }

        const canvas3D = document.getElementById('three-canvas');
        const tensionChart = document.getElementById('tension-chart');
        const momentChart = document.getElementById('moment-curv-chart');
        const vmChart = document.getElementById('vonmises-chart');
        const colorbarLegend = document.getElementById('colorbar-legend');

        const btn3D = document.getElementById('vtab-3d-btn');
        const btnTension = document.getElementById('vtab-tension-btn');
        const btnMoment = document.getElementById('vtab-moment-btn');
        const btnVM = document.getElementById('vtab-vm-btn');

        [btn3D, btnTension, btnMoment, btnVM].forEach(b => { if (b) b.className = 'btn-tab'; });

        if (canvas3D) canvas3D.style.display = view === '3d' ? 'block' : 'none';
        if (tensionChart) tensionChart.style.display = view === 'tension' ? 'block' : 'none';
        if (momentChart) momentChart.style.display = view === 'moment' ? 'block' : 'none';
        if (vmChart) vmChart.style.display = view === 'vm' ? 'block' : 'none';
        if (colorbarLegend) colorbarLegend.style.display = view === '3d' ? 'block' : 'none';

        if (view === '3d' && btn3D) btn3D.className = 'btn-tab active';
        if (view === 'tension' && btnTension) btnTension.className = 'btn-tab active';
        if (view === 'moment' && btnMoment) btnMoment.className = 'btn-tab active';
        if (view === 'vm' && btnVM) btnVM.className = 'btn-tab active';

        if (view === '3d') {
            setTimeout(() => {
                this.renderer3D.onWindowResize();
            }, 50);
        } else {
            setTimeout(() => {
                window.dispatchEvent(new Event('resize'));
                if (this.chartsController) this.chartsController.resizeCharts();
            }, 50);
        }

        this.render();
    }

    async loadSimulationData(fileOrUrl, fallbackUrl) {
        try {
            this.simulation = await DataLoaderService.load(fileOrUrl, fallbackUrl);

            const modeSelect = document.getElementById('analysis-mode-select');
            if (modeSelect) {
                this.simulation.mode = 'static';
                modeSelect.value = 'static';
            }

            const slider = document.getElementById('step-slider');
            slider.max = Math.max(0, this.simulation.totalSteps - 1);
            slider.value = Math.max(0, this.simulation.totalSteps - 1);
            this.currentStepIdx = Math.max(0, this.simulation.totalSteps - 1);

            // Frames the camera on the newly-loaded model -- without this, the camera stays at
            // Riser3DRenderer's initial guess (calibrated for Exemplo_01a's scale, ~130m), which
            // makes any model at a very different scale (e.g. a riser in 1800m of water) show up
            // as a tiny, distorted sliver -- visually indistinguishable from "the line is
            // bent/collapsed", even with correct coordinates.
            this.cameraController.setView('ISO', this.getCurrentStep(), ...this.getEnvBounds());

            this.render();
            console.log("✅ Simulação OO carregada com sucesso!", this.simulation);
        } catch (err) {
            console.error("Erro ao carregar simulação OO: ", err);
        }
    }

    getCurrentStep() {
        return this.simulation ? this.simulation.getStep(this.currentStepIdx) : null;
    }

    /** @returns {[number|null, number|null]} [seabedDepth, waterSurfaceZ] of the loaded simulation, for the 3D planes/framing. */
    getEnvBounds() {
        return this.simulation ? [this.simulation.seabedDepth, this.simulation.waterSurfaceZ] : [null, null];
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
        const scalarFieldEl = document.getElementById('scalar-field-select');
        const scalarField = scalarFieldEl ? scalarFieldEl.value : 'tension';

        const globalRange = this.simulation ? this.simulation.getScalarRange(scalarField) : { min: 0, max: 100 };
        const stepValues = step.elements.map(e => this.simulation ? this.simulation.getElementScalar(e, scalarField) : 0);

        let minVal = Math.min(...stepValues);
        let maxVal = Math.max(...stepValues);

        if (isNaN(minVal) || isNaN(maxVal) || maxVal <= minVal) {
            minVal = globalRange.min;
            maxVal = globalRange.max;
        }

        const scalarRange = { min: minVal, max: maxVal };

        // Update metric cards
        const isDynamic = this.simulation ? this.simulation.mode === 'dynamic' : true;
        const totalSteps = this.simulation ? this.simulation.totalSteps - 1 : 0;
        const topNode = step.nodes && step.nodes.length > 0 ? step.nodes[0] : null;

        if (isDynamic) {
            const timeVal = (step.stepIndex * 0.05).toFixed(2);
            const waveZ = topNode ? topNode.z.toFixed(2) : '-100.00';
            document.getElementById('step-label').innerText = `${step.stepIndex}/${totalSteps} (${timeVal}s)`;
            document.getElementById('load-factor').innerText = `t = ${timeVal}s (Onda Z: ${waveZ}m)`;
        } else {
            const loadPct = (step.loadFactor * 100).toFixed(0);
            const topX = topNode ? topNode.x.toFixed(2) : '0.00';
            document.getElementById('step-label').innerText = `${step.stepIndex}/${totalSteps} (${loadPct}%)`;
            document.getElementById('load-factor').innerText = `${loadPct}% (Offset X: ${topX}m)`;
        }

        document.getElementById('top-tension').innerText = `${step.getTopTension().toFixed(2)} kN`;
        document.getElementById('max-depth').innerText = `${step.getMaxDepth().toFixed(2)} m`;

        // Update floating legend
        this.updateColorbar(colormap, minVal, maxVal, scalarField);

        // Render the 3D scene
        this.renderer3D.renderStep(step, colormap, scalarRange, this.currentTheme, scalarField, ...this.getEnvBounds());
        this.updateTable(step);

        // Update the 2D profile charts
        if (this.chartsController) {
            this.chartsController.updateCharts(step, this.currentTheme);
        }
    }

    updateColorbar(colormap, minVal, maxVal, scalarField = 'tension') {
        const bar = document.getElementById('colorbar-bar');
        // Generates the gradient from the same control points used to color the 3D tubes
        // (ColorMapService), instead of a second, independent hardcoded palette that could
        // silently drift from the color actually rendered.
        const gradientStr = ColorMapService.getCssGradient(colormap);

        if (bar) bar.style.background = gradientStr;

        const titleEl = document.getElementById('cbar-unit-title');
        let titleText = 'Tração (kN)';
        let formatter = (v) => v.toFixed(1);

        switch (scalarField) {
            case 'moment':
                titleText = 'Momento (kN·m)';
                formatter = (v) => v.toFixed(2);
                break;
            case 'curvature':
                titleText = 'Curvatura (1/m)';
                formatter = (v) => v.toExponential(2);
                break;
            case 'vonmises':
                titleText = 'von Mises (MPa)';
                formatter = (v) => v.toFixed(1);
                break;
            case 'mbr':
                titleText = 'Fator MBR (SF)';
                formatter = (v) => v.toFixed(2);
                break;
            case 'tension':
            default:
                titleText = 'Tração (kN)';
                formatter = (v) => v.toFixed(1);
                break;
        }

        if (titleEl) titleEl.innerText = titleText;

        const maxEl = document.getElementById('cbar-max');
        const mid2El = document.getElementById('cbar-mid2');
        const mid1El = document.getElementById('cbar-mid1');
        const minEl = document.getElementById('cbar-min');

        if (maxEl) maxEl.innerText = formatter(maxVal);
        if (mid2El) mid2El.innerText = formatter(minVal + 0.75 * (maxVal - minVal));
        if (mid1El) mid1El.innerText = formatter(minVal + 0.25 * (maxVal - minVal));
        if (minEl) minEl.innerText = formatter(minVal);
    }

    /** Switches between the elements table and the nodes table in the "Table" tab. */
    setTableViewMode(mode) {
        this.tableViewMode = mode;
        const elTable = document.getElementById('elements-table');
        const nodeTable = document.getElementById('nodes-table');
        const elBtn = document.getElementById('table-view-elements-btn');
        const nodeBtn = document.getElementById('table-view-nodes-btn');
        if (elTable) elTable.style.display = mode === 'elements' ? 'table' : 'none';
        if (nodeTable) nodeTable.style.display = mode === 'nodes' ? 'table' : 'none';
        if (elBtn) elBtn.className = mode === 'elements' ? 'btn-tab active' : 'btn-tab';
        if (nodeBtn) nodeBtn.className = mode === 'nodes' ? 'btn-tab active' : 'btn-tab';
    }

    updateTable(step) {
        const stepTitle = document.getElementById('table-step-title');

        if (stepTitle && this.simulation) {
            const totalSteps = this.simulation.totalSteps - 1;
            const loadPct = (step.loadFactor * 100).toFixed(0);
            stepTitle.innerText = `Passo ${step.stepIndex}/${totalSteps} (${loadPct}% da Carga / Offset)`;
        }

        const nodes = step.nodes;
        const elements = step.elements;

        // Elements table (tension, moment, curvature, von Mises, MBR, section status)
        const elTbody = document.getElementById('elements-tbody');
        if (elTbody) {
            elTbody.innerHTML = '';
            elements.forEach((elem, idx) => {
                const tr = document.createElement('tr');

                const n2 = nodes[idx + 1] || nodes[idx];

                // Seabed detected from this simulation's real depth (not a fixed value), since the
                // element index/range varies from model to model. There's no way to detect
                // buoyancy modules (Lazy Wave) from the exported data -- the exporter doesn't
                // include per-element diameter/module metadata (see simulation_exporter.cpp), so
                // that status was removed instead of "guessed".
                let statusBadge = `<span class="status-water">🌊 Suspenso</span>`;
                const seabedDepth = this.simulation ? this.simulation.seabedDepth : -100.0;
                if (n2 && n2.z <= seabedDepth + 0.5) {
                    statusBadge = `<span class="status-seabed">🏖️ Fundo do Mar (TDZ)</span>`;
                }

                // Effective tension and von Mises in scientific notation (typically large values
                // or a wide dynamic range); the other quantities stay in fixed-point. Units only
                // appear in the column header, not repeated in every cell.
                const tensionStr = elem.tensionEffectiveKn !== undefined ? elem.tensionEffectiveKn.toExponential(2) : "0.00e+0";
                const momentStr = elem.bendingMomentKnm !== undefined ? elem.bendingMomentKnm.toFixed(2) : "0.00";
                const curvStr = elem.curvature !== undefined ? elem.curvature.toExponential(3) : "0.000e+00";
                const vmStr = elem.vonMisesMpa !== undefined ? elem.vonMisesMpa.toExponential(2) : "0.00e+0";
                const mbrStr = elem.mbrSafetyFactor !== undefined ? elem.mbrSafetyFactor.toFixed(2) : "1.00";

                tr.innerHTML = `
                    <td style="font-weight:bold;">Elemento ${elem.id}</td>
                    <td>Nó ${idx + 1} ➔ Nó ${idx + 2}</td>
                    <td>${statusBadge}</td>
                    <td class="tension-val">${tensionStr}</td>
                    <td>${momentStr}</td>
                    <td style="font-family:monospace;">${curvStr}</td>
                    <td style="font-weight:bold; color:#e11d48;">${vmStr}</td>
                    <td style="font-weight:bold; color:#7c6ce0;">${mbrStr}</td>
                `;
                elTbody.appendChild(tr);
            });
        }

        // Nodes table (coordinates)
        const nodeTbody = document.getElementById('nodes-tbody');
        if (nodeTbody) {
            nodeTbody.innerHTML = '';
            nodes.forEach((node, idx) => {
                const tr = document.createElement('tr');
                tr.innerHTML = `
                    <td style="font-weight:bold;">Nó ${node.id !== undefined ? node.id : idx + 1}</td>
                    <td>${node.x.toFixed(2)}</td>
                    <td>${node.y.toFixed(2)}</td>
                    <td>${node.z.toFixed(2)}</td>
                `;
                nodeTbody.appendChild(tr);
            });
        }
    }
}

// Bootstraps the application once the page loads
window.addEventListener('DOMContentLoaded', () => {
    window.riserApp = new RiserSimApp();
});
