import { DataLoaderService } from './services/DataLoaderService.js';
import { ColorMapService } from './services/ColorMapService.js';
import { Riser3DRenderer } from './renderers/Riser3DRenderer.js';
import { CameraViewController } from './renderers/CameraViewController.js';
import { ProfileChartsController } from './charts/ProfileChartsController.js';
import { TimeHistoryChartController } from './charts/TimeHistoryChartController.js';
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
        this.selectedPoint = null; // {type: 'node'|'element', id: number} -- picked from a table row, see selectPoint()
        this.tableSortKey = null; // 'id' | 'status' | 'tension' | null (natural/by-id order) -- see setSortKey()
        this.tableSortDir = 'asc';
        this.historyField = 'tension'; // field plotted for an ELEMENT's time-history -- independent from #scalar-field-select (3D coloring)

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
        this.historyChartsController = new TimeHistoryChartController();

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

    /** Switches between the side data-panel tabs (Visualization/Table). */
    setDataTab(tab) {
        switchTab({ viz: 'tab-viz', table: 'tab-table' }, tab);
    }

    bindEvents() {
        // Floating tab switcher at the top of the viewport
        const vtabs = [
            { id: 'vtab-3d-btn', view: '3d' },
            { id: 'vtab-tension-btn', view: 'tension' },
            { id: 'vtab-moment-btn', view: 'moment' },
            { id: 'vtab-vm-btn', view: 'vm' },
            { id: 'vtab-history-btn', view: 'history' }
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
        document.getElementById('tab-viz-btn').addEventListener('click', () => this.setDataTab('viz'));
        document.getElementById('tab-table-btn').addEventListener('click', () => this.setDataTab('table'));

        // Elements / Nodes toggle within the "Table" tab
        document.getElementById('table-view-elements-btn').addEventListener('click', () => this.setTableViewMode('elements'));
        document.getElementById('table-view-nodes-btn').addEventListener('click', () => this.setTableViewMode('nodes'));

        // Sortable column headers in the (now compact) Elements table
        document.getElementById('th-sort-id').addEventListener('click', () => this.setSortKey('id'));
        document.getElementById('th-sort-status').addEventListener('click', () => this.setSortKey('status'));
        document.getElementById('th-sort-tension').addEventListener('click', () => this.setSortKey('tension'));

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

        // Scalar field selector (3D coloring -- independent from the history chart's own field,
        // see historyFieldSelect below). Only auto-navigates away from a PROFILE chart tab (so the
        // user sees the field they just picked); doesn't touch 'history', which has its own field
        // and shouldn't be yanked away from just because the 3D coloring field changed.
        const scalarSelect = document.getElementById('scalar-field-select');
        if (scalarSelect) {
            scalarSelect.addEventListener('change', (e) => {
                const val = e.target.value;
                if (this.activeViewportView !== '3d' && this.activeViewportView !== 'history') {
                    if (val === 'tension') this.switchViewportView('tension');
                    else if (val === 'moment' || val === 'curvature' || val === 'mbr') this.switchViewportView('moment');
                    else if (val === 'vonmises') this.switchViewportView('vm');
                }
                this.render();
            });
        }

        // Colormap
        document.getElementById('colormap-select').addEventListener('change', () => this.render());

        // Envelope overlay toggle (min/max over the active mode's whole time series)
        const envelopeToggle = document.getElementById('envelope-toggle');
        if (envelopeToggle) envelopeToggle.addEventListener('change', () => this.render());

        // Time-history chart's own field selector (independent from #scalar-field-select above)
        const historyFieldSelect = document.getElementById('history-field-select');
        if (historyFieldSelect) {
            historyFieldSelect.addEventListener('change', (e) => {
                this.historyField = e.target.value;
                this.render();
            });
        }

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
        const historyChart = document.getElementById('history-chart');
        const colorbarLegend = document.getElementById('colorbar-legend');

        const btn3D = document.getElementById('vtab-3d-btn');
        const btnTension = document.getElementById('vtab-tension-btn');
        const btnMoment = document.getElementById('vtab-moment-btn');
        const btnVM = document.getElementById('vtab-vm-btn');
        const btnHistory = document.getElementById('vtab-history-btn');

        [btn3D, btnTension, btnMoment, btnVM, btnHistory].forEach(b => { if (b) b.className = 'btn-tab'; });

        if (canvas3D) canvas3D.style.display = view === '3d' ? 'block' : 'none';
        if (tensionChart) tensionChart.style.display = view === 'tension' ? 'block' : 'none';
        if (momentChart) momentChart.style.display = view === 'moment' ? 'block' : 'none';
        if (vmChart) vmChart.style.display = view === 'vm' ? 'block' : 'none';
        if (historyChart) historyChart.style.display = view === 'history' ? 'block' : 'none';
        if (colorbarLegend) colorbarLegend.style.display = view === '3d' ? 'block' : 'none';

        if (view === '3d' && btn3D) btn3D.className = 'btn-tab active';
        if (view === 'tension' && btnTension) btnTension.className = 'btn-tab active';
        if (view === 'moment' && btnMoment) btnMoment.className = 'btn-tab active';
        if (view === 'vm' && btnVM) btnVM.className = 'btn-tab active';
        if (view === 'history' && btnHistory) btnHistory.className = 'btn-tab active';

        if (view === '3d') {
            setTimeout(() => {
                this.renderer3D.onWindowResize();
            }, 50);
        } else {
            setTimeout(() => {
                window.dispatchEvent(new Event('resize'));
                if (this.chartsController) this.chartsController.resizeCharts();
                if (this.historyChartsController) this.historyChartsController.resize();
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
        this.renderer3D.renderStep(step, colormap, scalarRange, this.currentTheme, scalarField, ...this.getEnvBounds(), this.selectedPoint);
        this.updateTable(step);

        // Update the 2D profile charts, optionally overlaid with the min/max-over-time envelope
        // (see FEASimulation::getElementEnvelope) when the user has the toggle checked.
        const envelopeToggle = document.getElementById('envelope-toggle');
        let envelope = null;
        if (envelopeToggle && envelopeToggle.checked && this.simulation) {
            envelope = {
                tension: this.simulation.getElementEnvelope('tension'),
                moment: this.simulation.getElementEnvelope('moment'),
                vonmises: this.simulation.getElementEnvelope('vonmises')
            };
        }
        if (this.chartsController) {
            this.chartsController.updateCharts(step, this.currentTheme, envelope);
        }

        // Update the time-history chart for whatever point is currently selected in the table,
        // using its OWN field selector (this.historyField) -- independent from `scalarField`
        // above, which only drives the 3D coloring. The field bar itself only makes sense for an
        // element selection (a node's history is always X/Y/Z) and only while that chart is
        // visible, so it's shown/hidden here alongside every render, not just on tab switch.
        const historyFieldBar = document.getElementById('history-field-bar');
        const showFieldBar = this.activeViewportView === 'history' && this.selectedPoint && this.selectedPoint.type === 'element';
        if (historyFieldBar) historyFieldBar.style.display = showFieldBar ? 'flex' : 'none';
        // The field bar can wrap to its own line below #viewport-tabs on a narrow canvas (see
        // #viewport-toolbar in posprocessor.html) -- reserve extra top clearance on the chart
        // itself whenever the bar is visible at all, so a wrapped line never lands on the title.
        const historyChartDiv = document.getElementById('history-chart');
        if (historyChartDiv) historyChartDiv.classList.toggle('with-field-bar', showFieldBar);
        const historyFieldSelect = document.getElementById('history-field-select');
        if (historyFieldSelect) historyFieldSelect.value = this.historyField;

        if (this.historyChartsController) {
            this.historyChartsController.updateChart(this.simulation, this.selectedPoint, this.historyField, this.currentStepIdx, this.currentTheme);
        }
    }

    /**
     * Selects a node/element as the "point of interest" for the time-history chart, picked from a
     * row in the results table (see updateTable()) -- also drives the 3D highlight marker
     * (Riser3DRenderer::updateSelectionHighlight).
     * @param {'node'|'element'} type
     * @param {number} id
     */
    selectPoint(type, id) {
        if (this.selectedPoint && this.selectedPoint.type === type && this.selectedPoint.id === id) {
            this.selectedPoint = null; // clicking the same row again deselects it
        } else {
            this.selectedPoint = { type, id };
        }
        this.render();
    }

    /**
     * Sorts the (compact) Elements table by the given column -- clicking the same column again
     * flips direction, a different column resets to ascending. `null` key means the natural/by-id
     * order (the table's original behavior, also the riser's physical order along its length).
     * @param {'id'|'status'|'tension'} key
     */
    setSortKey(key) {
        if (this.tableSortKey === key) {
            this.tableSortDir = this.tableSortDir === 'asc' ? 'desc' : 'asc';
        } else {
            this.tableSortKey = key;
            this.tableSortDir = 'asc';
        }
        this.render();
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
        const detailCard = document.getElementById('element-detail-card');
        if (elTable) elTable.style.display = mode === 'elements' ? 'table' : 'none';
        if (nodeTable) nodeTable.style.display = mode === 'nodes' ? 'table' : 'none';
        if (detailCard) detailCard.style.display = mode === 'elements' ? '' : 'none';
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

        // Elements table -- compact (ID/Nó/Status/Tração only; the rest lives in the detail panel,
        // see updateElementDetailCard()). Seabed detected from this simulation's real depth (not a
        // fixed value), since the element index/range varies from model to model. There's no way
        // to detect buoyancy modules (Lazy Wave) from the exported data -- the exporter doesn't
        // include per-element diameter/module metadata (see simulation_exporter.cpp), so that
        // status was removed instead of "guessed".
        const seabedDepth = this.simulation ? this.simulation.seabedDepth : -100.0;
        const isSeabedElement = (elem) => {
            const ordinalIdx = elem.id - 1;
            const n2 = nodes[ordinalIdx + 1] || nodes[ordinalIdx];
            return !!(n2 && n2.z <= seabedDepth + 0.5);
        };

        // Sorted on a COPY so element/node lookups elsewhere (3D highlight, history chart) stay
        // keyed by `elem.id`, unaffected by display order -- ids are contiguous 1-indexed ordinals
        // (simulation_exporter.cpp), so `elem.id - 1` always finds the right node pair regardless
        // of where the row ends up in the sorted table.
        let orderedElements = elements.slice();
        if (this.tableSortKey) {
            const dir = this.tableSortDir === 'asc' ? 1 : -1;
            orderedElements.sort((a, b) => {
                let cmp = 0;
                if (this.tableSortKey === 'id') {
                    cmp = a.id - b.id;
                } else if (this.tableSortKey === 'tension') {
                    cmp = this.simulation.getElementScalar(a, 'tension') - this.simulation.getElementScalar(b, 'tension');
                } else if (this.tableSortKey === 'status') {
                    cmp = (isSeabedElement(a) ? 1 : 0) - (isSeabedElement(b) ? 1 : 0);
                }
                return cmp * dir;
            });
        }

        ['id', 'status', 'tension'].forEach(key => {
            const indicator = document.querySelector(`#th-sort-${key} .sort-indicator`);
            if (!indicator) return;
            indicator.innerText = this.tableSortKey === key ? (this.tableSortDir === 'asc' ? '▲' : '▼') : '⇅';
        });

        const elTbody = document.getElementById('elements-tbody');
        if (elTbody) {
            elTbody.innerHTML = '';
            orderedElements.forEach((elem) => {
                const tr = document.createElement('tr');
                const ordinalIdx = elem.id - 1;

                const statusBadge = isSeabedElement(elem)
                    ? `<span class="status-seabed">🏖️ Fundo do Mar (TDZ)</span>`
                    : `<span class="status-water">🌊 Suspenso</span>`;

                // Scientific notation matches the tension column's original formatting (typically
                // large values or a wide dynamic range).
                const tensionStr = elem.tensionEffectiveKn !== undefined ? elem.tensionEffectiveKn.toExponential(2) : "0.00e+0";

                tr.innerHTML = `
                    <td style="font-weight:bold;">Elemento ${elem.id}</td>
                    <td>Nó ${ordinalIdx + 1} ➔ Nó ${ordinalIdx + 2}</td>
                    <td>${statusBadge}</td>
                    <td class="tension-val">${tensionStr}</td>
                `;
                if (this.selectedPoint && this.selectedPoint.type === 'element' && this.selectedPoint.id === elem.id) {
                    tr.classList.add('selected-row');
                }
                tr.addEventListener('click', () => this.selectPoint('element', elem.id));
                elTbody.appendChild(tr);
            });
        }

        this.updateElementDetailCard(step);

        // Nodes table (coordinates)
        const nodeTbody = document.getElementById('nodes-tbody');
        if (nodeTbody) {
            nodeTbody.innerHTML = '';
            nodes.forEach((node, idx) => {
                const nodeId = node.id !== undefined ? node.id : idx + 1;
                const tr = document.createElement('tr');
                tr.innerHTML = `
                    <td style="font-weight:bold;">Nó ${nodeId}</td>
                    <td>${node.x.toFixed(2)}</td>
                    <td>${node.y.toFixed(2)}</td>
                    <td>${node.z.toFixed(2)}</td>
                `;
                if (this.selectedPoint && this.selectedPoint.type === 'node' && this.selectedPoint.id === nodeId) {
                    tr.classList.add('selected-row');
                }
                tr.addEventListener('click', () => this.selectPoint('node', nodeId));
                nodeTbody.appendChild(tr);
            });
        }
    }

    /**
     * Fills the "🔍 Detalhe do Elemento" panel above the Elements table with the fields the
     * compact table itself no longer shows (Momento/Curvatura/von Mises/MBR) -- see
     * updateTable(), Proposta C. Shows a placeholder when nothing (or a node) is selected.
     * @param {SimulationStep} step
     */
    updateElementDetailCard(step) {
        const emptyEl = document.getElementById('element-detail-empty');
        const statsEl = document.getElementById('element-detail-stats');
        const titleEl = document.getElementById('element-detail-title');
        if (!emptyEl || !statsEl || !titleEl) return;

        const elem = (this.selectedPoint && this.selectedPoint.type === 'element')
            ? step.elements[this.selectedPoint.id - 1]
            : null;

        if (!elem) {
            titleEl.innerText = '🔍 DETALHE DO ELEMENTO';
            emptyEl.style.display = '';
            statsEl.style.display = 'none';
            return;
        }

        titleEl.innerText = `🔍 DETALHE DO ELEMENTO ${elem.id}`;
        document.getElementById('detail-moment').innerText = elem.bendingMomentKnm !== undefined ? elem.bendingMomentKnm.toFixed(2) : '0.00';
        document.getElementById('detail-curvature').innerText = elem.curvature !== undefined ? elem.curvature.toExponential(3) : '0.000e+00';
        document.getElementById('detail-vonmises').innerText = elem.vonMisesMpa !== undefined ? elem.vonMisesMpa.toExponential(2) : '0.00e+0';
        document.getElementById('detail-mbr').innerText = elem.mbrSafetyFactor !== undefined ? elem.mbrSafetyFactor.toFixed(2) : '1.00';
        emptyEl.style.display = 'none';
        statsEl.style.display = '';
    }
}

// Bootstraps the application once the page loads
window.addEventListener('DOMContentLoaded', () => {
    window.riserApp = new RiserSimApp();
});
