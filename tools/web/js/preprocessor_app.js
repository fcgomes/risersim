import { InputLoaderService } from './services/InputLoaderService.js';
import { ColorMapService } from './services/ColorMapService.js';
import { Riser3DRenderer } from './renderers/Riser3DRenderer.js';
import { CameraViewController } from './renderers/CameraViewController.js';
import { initPanelResizer } from './ui/PanelResizer.js';
import { ZoomWindowController } from './ui/ZoomWindowController.js';
import { bindCameraToolbar } from './ui/CameraToolbar.js';
import { switchTab } from './ui/TabPanel.js';
import { initThemeToggle } from './ui/ThemeToggle.js';
import { confirmDialog, alertDialog } from './ui/ConfirmDialog.js';
import { statusPill, formatDate, escapeHtml, fetchJSON, modelSourceHtml, createRun, fetchLoadCases, DuplicateRunError } from './utils/runManagerFormat.js';

/**
 * preprocessor_app.js
 * Main controller for the web preprocessor: inspects/validates the INPUT JSON consumed by
 * risersim::ModelBuilder, BEFORE the simulation runs -- not the results (that's
 * posprocessor.html/app.js).
 */
class PreprocessorApp {
    constructor() {
        this.input = null; // result of InputLoaderService.parse()
        this.currentTheme = 'dark';
        this.tableViewMode = 'grouped'; // 'grouped' | 'byElement'
        this.projectId = null; // set by applyProjectMode() when opened with ?project=<id>
        this.runsPollTimer = null;
        this.initUI();
    }

    async initUI() {
        // When embedded in project.html's <iframe> (the normal case -- see app.js::initUI for
        // the same check), the parent page already shows a header with the brand mark, title,
        // and theme toggle; showing this page's own copy too doubled it up on screen. Hidden via
        // inline style (not a body class) so it can't collide with ThemeToggle.js's
        // `body.className = '...'` full overwrites.
        if (window.self !== window.top) {
            document.getElementById('header').style.display = 'none';
            document.getElementById('main-layout').style.height = '100vh';
        }

        const canvas = document.getElementById('three-canvas');
        this.renderer3D = new Riser3DRenderer(canvas);
        this.cameraController = new CameraViewController(this.renderer3D.camera, this.renderer3D.controls, this.renderer3D);
        this.zoomWindow = new ZoomWindowController(this.cameraController, this.renderer3D);

        this.bindEvents();
        this.applyProjectMode();
        initPanelResizer(() => this.renderer3D && this.renderer3D.onWindowResize());
        try {
            await this.loadInput(await this.resolveInputUrl());
        } catch (err) {
            console.error("Erro ao resolver a URL de entrada: ", err);
        }
    }

    /** When opened with `?project=<id>` (embedded in project.html's Pre-processing tab, or
     * standalone with the same query string): hides the manual upload/URL controls (meaningless
     * once a project already supplies the model) in favor of an "Origem" panel showing where the
     * data came from, and reveals the "Simulações" tab. Without `?project=`, the page is
     * untouched -- same manual-load behavior as always, no Simulações tab. */
    applyProjectMode() {
        this.projectId = new URLSearchParams(window.location.search).get('project');
        if (!this.projectId) return;

        document.getElementById('load-manual-controls').style.display = 'none';
        document.getElementById('origin-panel').style.display = '';
        document.getElementById('tab-load-btn').innerText = '📦 Origem';
        document.getElementById('tab-runs-btn').style.display = '';

        document.getElementById('runs-tab-new-run-btn').addEventListener('click', () => this.createRunFromTab());
        document.getElementById('runs-tab-manager-link').addEventListener('click', (e) => {
            e.preventDefault();
            this.switchParentView('manager');
        });

        this.loadCases = [];
        fetchLoadCases(this.projectId).then((data) => {
            this.loadCases = data.load_cases || [];
            this.renderLoadCaseSelect();
        });

        this.loadProjectMeta();
    }

    /** Same idea as project.js::renderLoadCaseSelect() -- populates/toggles the "new run"
     * load-case <select> in the Simulações tab, shown only when the project's .aml defines more
     * than one %LOAD_CASE. */
    renderLoadCaseSelect() {
        const select = document.getElementById('runs-tab-load-case-select');
        if (this.loadCases.length <= 1) {
            select.style.display = 'none';
            select.innerHTML = '';
            return;
        }
        select.innerHTML = this.loadCases.map(lc =>
            `<option value="${lc.id}">${escapeHtml(lc.name)} (ID ${lc.id})</option>`
        ).join('');
        select.style.display = '';
    }

    /**
     * Resolves which input file to load on startup, in priority order (mirrors
     * app.js::resolveResultsUrl()):
     *   1. `?file=<url>` -- explicit override, any URL/relative path.
     *   2. `?project=<id>&run=<run-id>` -- the exact config snapshot that run used, under its own
     *      case-identifiable name (`ProjectStore.create_run()`'s `input_filename`, e.g.
     *      "Exemplo_01a_Far_input.json") -- written synchronously when the run is CREATED (unlike
     *      the results file, no need to wait on `status`), served by the run manager's generic
     *      per-run results route (run_server.py::api_run_results). Fetches the run's metadata
     *      first to read the real filename, then builds the URL from it.
     *   3. `?project=<id>` (no run) -- the project's CURRENT input_simulation.json ("what the
     *      next run would use"), served by GET /api/projects/<id>/input.
     *   4. `../input_simulation.json` -- the historical hardcoded path, unchanged (manual use
     *      without the run manager keeps working).
     * @returns {Promise<string>}
     */
    async resolveInputUrl() {
        const params = new URLSearchParams(window.location.search);

        const explicitFile = params.get('file');
        if (explicitFile) return explicitFile;

        const project = params.get('project');
        const run = params.get('run');
        if (project && run) {
            const runInfoRes = await fetch(`/api/projects/${encodeURIComponent(project)}/runs/${encodeURIComponent(run)}`);
            if (!runInfoRes.ok) throw new Error(`Não foi possível obter informações da rodada (HTTP ${runInfoRes.status})`);
            const runInfo = await runInfoRes.json();
            if (!runInfo.input_filename) throw new Error('Esta rodada não tem um nome de arquivo de entrada registrado.');
            const base = `/api/projects/${encodeURIComponent(project)}/runs/${encodeURIComponent(run)}/results/`;
            return base + encodeURIComponent(runInfo.input_filename);
        }
        if (project) {
            return `/api/projects/${encodeURIComponent(project)}/input`;
        }

        return '../input_simulation.json';
    }

    bindEvents() {
        bindCameraToolbar(this.cameraController, this.zoomWindow, () => this.getSyntheticStep(), () => this.getEnvBounds());

        document.getElementById('json-input').addEventListener('change', (e) => {
            const file = e.target.files[0];
            if (file) this.loadInput(file);
        });

        document.getElementById('url-load-btn').addEventListener('click', () => {
            const url = document.getElementById('url-input').value.trim();
            if (url) this.loadInput(url);
        });

        document.getElementById('view-grouped-btn').addEventListener('click', () => this.setTableViewMode('grouped'));
        document.getElementById('view-byelement-btn').addEventListener('click', () => this.setTableViewMode('byElement'));
        document.getElementById('view-nodes-btn').addEventListener('click', () => this.setTableViewMode('nodes'));

        document.getElementById('tab-load-btn').addEventListener('click', () => this.setBottomTab('load'));
        document.getElementById('tab-geometry-btn').addEventListener('click', () => this.setBottomTab('geometry'));
        document.getElementById('tab-environmental-btn').addEventListener('click', () => this.setBottomTab('environmental'));
        document.getElementById('tab-analysis-btn').addEventListener('click', () => this.setBottomTab('analysis'));
        document.getElementById('tab-runs-btn').addEventListener('click', () => this.setBottomTab('runs'));

        initThemeToggle(this);
    }

    async loadInput(fileOrUrl) {
        try {
            this.input = await InputLoaderService.load(fileOrUrl);

            // Reframes the camera on the newly-loaded model -- without this, the camera stays at
            // Riser3DRenderer's initial guess (calibrated for ~130m), and a model at a very
            // different scale shows up as a tiny/distorted sliver (the same bug already fixed in
            // the results viewer earlier this session).
            this.cameraController.setView('ISO', this.getSyntheticStep(), ...this.getEnvBounds());

            this.render();
            if (this.input.valid) this.setBottomTab('geometry');
            console.log("✅ JSON de entrada carregado.", this.input);
        } catch (err) {
            console.error("Erro ao carregar JSON de entrada: ", err);
            this.input = {
                valid: false,
                warnings: [{ level: 'error', message: `Falha ao carregar/parsear o arquivo: ${err.message}` }]
            };
            this.render();
        }
    }

    /** Synthesizes a {nodes, elements} object that Riser3DRenderer/CameraViewController already understand, coloring by outer diameter (D_outer) instead of a solved result (which doesn't exist for input data). */
    getSyntheticStep() {
        if (!this.input || !this.input.valid) return null;
        const elements = this.input.elements.map(e => ({
            id: e.id,
            tensionEffectiveKn: e.sectionProperties.D_outer ?? 0,
            bendingMomentKnm: 0, curvature: 0, vonMisesMpa: 0, mbrSafetyFactor: 1.0
        }));
        return { stepIndex: 0, loadFactor: 0, nodes: this.input.nodes, elements };
    }

    /**
     * Mirrors the seabed/surface positioning logic from ModelBuilder::load_from_json()
     * (risersim/src/model_builder.cpp) -- seabed.enabled=false pushes the bottom to -1e6 (no
     * contact possible) and approximates the surface with the highest node; otherwise the bottom
     * sits at the nodes' real minimum Z.
     * @returns {[number|null, number|null]}
     */
    getEnvBounds() {
        if (!this.input || !this.input.valid || this.input.nodes.length === 0) return [null, null];
        const zs = this.input.nodes.map(n => n.z);
        const minZ = Math.min(...zs), maxZ = Math.max(...zs);
        const seabed = this.input.seabed || {};
        const enabled = seabed.enabled !== false;
        if (!enabled) return [-1.0e6, maxZ];
        const waterDepthMagnitude = Math.abs(seabed.depth_m ?? -100.0);
        return [minZ, minZ + waterDepthMagnitude];
    }

    render() {
        this.renderModelStatus();
        this.renderWarnings();

        if (!this.input || !this.input.valid) {
            this.clear3D();
            this.renderSectionTable([]);
            this.renderElementTable([]);
            this.renderNodeTable([]);
            this.renderBoundaryConditions();
            this.renderEnvironmental();
            this.renderAnalysisOptions();
            return;
        }

        const step = this.getSyntheticStep();
        const diameters = this.input.elements.map(e => e.sectionProperties.D_outer).filter(v => v !== undefined && v !== null);
        let minD = diameters.length ? Math.min(...diameters) : 0;
        let maxD = diameters.length ? Math.max(...diameters) : 1;
        if (maxD <= minD) maxD = minD + 1.0;

        this.renderer3D.renderStep(step, 'Viridis', { min: minD, max: maxD }, this.currentTheme, 'tension', ...this.getEnvBounds());
        this.highlightBoundaryNodes();
        this.updateColorbar(minD, maxD);

        this.renderSectionTable(this.input.elements);
        this.renderElementTable(this.input.elements);
        this.renderNodeTable(this.input.nodes);
        this.renderBoundaryConditions();
        this.renderEnvironmental();
        this.renderAnalysisOptions();
    }

    /** Switches between the data-panel tabs: Load(/Origem) / Geometry / Environmental Conditions /
     * Analysis / Runs (the last one only shown with ?project=<id>, see applyProjectMode()). */
    setBottomTab(tab) {
        switchTab({ load: 'tab-load', geometry: 'tab-geometry', environmental: 'tab-environmental', analysis: 'tab-analysis', runs: 'tab-runs' }, tab);
        if (tab === 'environmental' && typeof Plotly !== 'undefined') {
            // Plotly doesn't draw correctly in a container that had display:none -- force a
            // resize as soon as the tab becomes visible.
            setTimeout(() => Plotly.Plots.resize('current-chart'), 50);
        }
    }

    /** Switches between the grouped-sections table, the full element table, and the node table. */
    setTableViewMode(mode) {
        this.tableViewMode = mode;
        document.getElementById('section-table').style.display = mode === 'grouped' ? 'table' : 'none';
        document.getElementById('element-table').style.display = mode === 'byElement' ? 'table' : 'none';
        document.getElementById('node-table').style.display = mode === 'nodes' ? 'table' : 'none';
        document.getElementById('view-grouped-btn').className = mode === 'grouped' ? 'btn-tab active' : 'btn-tab';
        document.getElementById('view-byelement-btn').className = mode === 'byElement' ? 'btn-tab active' : 'btn-tab';
        document.getElementById('view-nodes-btn').className = mode === 'nodes' ? 'btn-tab active' : 'btn-tab';
    }

    clear3D() {
        this.renderer3D.renderStep({ nodes: [], elements: [] }, 'Viridis', { min: 0, max: 1 }, this.currentTheme, 'tension', null, null);
    }

    /** Marks boundary-condition nodes (prescribed/restrained) with colored spheres -- Riser3DRenderer doesn't do this on its own (nodesGroup stays empty in renderStep), so it's filled in directly here, after the renderStep call (which clears nodesGroup at the start). */
    highlightBoundaryNodes() {
        const group = this.renderer3D.nodesGroup;
        while (group.children.length > 0) {
            const obj = group.children[0];
            if (obj.geometry) obj.geometry.dispose();
            if (obj.material) obj.material.dispose();
            group.remove(obj);
        }
        if (!this.input || !this.input.valid) return;

        const nodeById = new Map(this.input.nodes.map(n => [n.id, n]));
        const addMarker = (nodeId, color, radius) => {
            const n = nodeById.get(nodeId);
            if (!n) return;
            const mesh = new THREE.Mesh(
                new THREE.SphereGeometry(radius, 12, 12),
                new THREE.MeshStandardMaterial({ color })
            );
            mesh.position.set(n.x, n.z, n.y); // same axis convention as Riser3DRenderer (Y-three = Z-riser)
            group.add(mesh);
        };

        const span = this.computeSpan();
        const radius = Math.max(0.3, span * 0.01);
        this.input.restrainedIds.forEach(id => addMarker(id, 0xf59e0b, radius));
        this.input.prescribedIds.forEach(id => addMarker(id, 0xef4444, radius * 1.15));
    }

    /** Pa -> MPa, 1 decimal place (E/G are more readable this way than in raw Pa). */
    formatMPa(v) {
        if (v === undefined || v === null) return '—';
        const num = Number(v);
        return Number.isFinite(num) ? (num / 1.0e6).toFixed(1) : String(v);
    }

    /** Scientific notation -- for quantities like IY/IZ/J (m⁴) where fixed decimal form turns into an unreadable string of zeros. */
    formatSci(v, digits = 3) {
        if (v === undefined || v === null) return '—';
        const num = Number(v);
        return Number.isFinite(num) ? num.toExponential(digits) : String(v);
    }

    /** N·m² -> kN·m² (EI comes in N·m² in the input JSON). */
    formatKNm2(v, digits = 0) {
        if (v === undefined || v === null) return '—';
        const num = Number(v);
        return Number.isFinite(num) ? (num / 1.0e3).toFixed(digits) : String(v);
    }

    computeSpan() {
        if (!this.input || !this.input.valid || this.input.nodes.length === 0) return 10.0;
        const xs = this.input.nodes.map(n => n.x), ys = this.input.nodes.map(n => n.y), zs = this.input.nodes.map(n => n.z);
        return Math.max(Math.max(...xs) - Math.min(...xs), Math.max(...ys) - Math.min(...ys), Math.max(...zs) - Math.min(...zs), 10.0);
    }

    updateColorbar(minD, maxD) {
        const bar = document.getElementById('colorbar-bar');
        if (bar) bar.style.background = ColorMapService.getCssGradient('Viridis');
        const maxEl = document.getElementById('cbar-max'), minEl = document.getElementById('cbar-min');
        const mid2El = document.getElementById('cbar-mid2'), mid1El = document.getElementById('cbar-mid1');
        if (maxEl) maxEl.innerText = maxD.toFixed(3);
        if (mid2El) mid2El.innerText = (minD + 0.75 * (maxD - minD)).toFixed(3);
        if (mid1El) mid1El.innerText = (minD + 0.25 * (maxD - minD)).toFixed(3);
        if (minEl) minEl.innerText = minD.toFixed(3);
    }

    renderModelStatus() {
        const el = document.getElementById('model-status');
        if (!el) return;
        if (!this.input) { el.innerHTML = ''; return; }
        if (!this.input.valid) {
            el.innerHTML = `<div class="stat-card"><div class="stat-title">Status</div><div class="stat-value" style="color:#ef4444;">Inválido</div></div>`;
            return;
        }
        const nodes = this.input.nodes, elements = this.input.elements;
        let totalLength = 0;
        const nodeById = new Map(nodes.map(n => [n.id, n]));
        elements.forEach(e => {
            const n1 = nodeById.get(e.node1Id), n2 = nodeById.get(e.node2Id);
            if (n1 && n2) totalLength += n1.distanceTo(n2);
        });
        const span = this.computeSpan();
        el.innerHTML = `
            <div class="stat-card"><div class="stat-title">Nós</div><div class="stat-value">${nodes.length}</div></div>
            <div class="stat-card"><div class="stat-title">Elementos</div><div class="stat-value">${elements.length}</div></div>
            <div class="stat-card"><div class="stat-title">Comprimento total</div><div class="stat-value">${totalLength.toFixed(1)} m</div></div>
            <div class="stat-card"><div class="stat-title">Span (X/Y/Z)</div><div class="stat-value">${span.toFixed(1)} m</div></div>
        `;
    }

    renderWarnings() {
        const el = document.getElementById('warnings-panel');
        if (!el) return;
        const warnings = (this.input && this.input.warnings) || [];
        if (warnings.length === 0) {
            el.innerHTML = `<div style="opacity:0.7;">✅ Nenhum aviso.</div>`;
            return;
        }
        const icons = { error: '🛑', warning: '⚠️', info: 'ℹ️' };
        el.innerHTML = warnings.map(w =>
            `<div class="warning-item warning-${w.level}">${icons[w.level] || ''} ${w.message}</div>`
        ).join('');
    }

    renderSectionTable(elements) {
        const tbody = document.getElementById('section-tbody');
        if (!tbody) return;
        tbody.innerHTML = '';
        if (elements.length === 0) return;

        const groups = new Map();
        elements.forEach(e => {
            if (!groups.has(e.groupKey)) groups.set(e.groupKey, { sample: e, ids: [] });
            groups.get(e.groupKey).ids.push(e.id);
        });

        groups.forEach(({ sample, ids }) => {
            const sp = sample.sectionProperties;
            const fmt = (v, digits = 3) => (v === undefined || v === null) ? '—' : Number(v).toFixed(digits);
            const idRange = ids.length > 3
                ? `${ids[0]}..${ids[ids.length - 1]} (${ids.length} elementos)`
                : ids.join(', ');
            const tr = document.createElement('tr');
            tr.innerHTML = `
                <td style="font-weight:bold;">${idRange}</td>
                <td>${fmt(sp.D_outer)}</td>
                <td>${fmt(sp.D_inner)}</td>
                <td>${this.formatMPa(sp.E)}</td>
                <td>${this.formatMPa(sp.G)}</td>
                <td>${fmt(sp.A, 5)}</td>
                <td>${this.formatKNm2(sp.EI)}</td>
                <td>${fmt(sp.weight_wet_kNm, 4)}</td>
                <td>${fmt(sp.Ca, 2)}</td>
                <td>${fmt(sp.Cd, 2)}</td>
            `;
            tbody.appendChild(tr);
        });
    }

    /**
     * Element-by-element table with ALL section properties read by ModelBuilder (not a subset) --
     * only node IDs, no coordinates (that's in the Nodes table, avoiding repeating the same
     * coordinate once per adjacent element).
     */
    renderElementTable(elements) {
        const tbody = document.getElementById('element-tbody');
        if (!tbody) return;
        tbody.innerHTML = '';
        if (elements.length === 0 || !this.input || !this.input.valid) return;

        const nodeById = new Map(this.input.nodes.map(n => [n.id, n]));
        const fmt = (v) => {
            if (v === undefined || v === null) return '—';
            const num = Number(v);
            return Number.isFinite(num) ? num.toPrecision(6) : String(v);
        };

        elements.forEach(e => {
            const n1 = nodeById.get(e.node1Id), n2 = nodeById.get(e.node2Id);
            const len = (n1 && n2) ? n1.distanceTo(n2) : null;
            const sp = e.sectionProperties;
            const tr = document.createElement('tr');
            tr.innerHTML = `
                <td style="font-weight:bold;">${e.id}</td>
                <td>${sp.line_name ?? '—'}</td>
                <td>${e.node1Id}</td><td>${e.node2Id}</td>
                <td>${len === null ? '—' : len.toFixed(3)}</td>
                <td>${this.formatMPa(sp.E)}</td><td>${this.formatMPa(sp.G)}</td><td>${fmt(sp.A)}</td>
                <td>${fmt(sp.D_outer)}</td><td>${fmt(sp.D_inner)}</td>
                <td>${this.formatSci(sp.IY)}</td><td>${this.formatSci(sp.IZ)}</td><td>${this.formatSci(sp.J)}</td><td>${this.formatKNm2(sp.EI, 1)}</td>
                <td>${fmt(sp.weight_wet_kNm)}</td><td>${fmt(sp.rho)}</td><td>${fmt(sp.rho_fluid)}</td>
                <td>${fmt(sp.Ca)}</td><td>${fmt(sp.Cd)}</td>
            `;
            tbody.appendChild(tr);
        });
    }

    /** Node table: coordinates + boundary condition (prescribed/restrained/free). */
    renderNodeTable(nodes) {
        const tbody = document.getElementById('node-tbody');
        if (!tbody) return;
        tbody.innerHTML = '';
        if (nodes.length === 0 || !this.input || !this.input.valid) return;

        const prescribed = new Set(this.input.prescribedIds);
        const restrained = new Set(this.input.restrainedIds);

        nodes.forEach(n => {
            let bc = 'Livre';
            if (prescribed.has(n.id)) bc = '🔴 Prescrito';
            else if (restrained.has(n.id)) bc = '🟠 Restringido';
            const tr = document.createElement('tr');
            tr.innerHTML = `
                <td style="font-weight:bold;">${n.id}</td>
                <td>${n.x.toFixed(3)}</td><td>${n.y.toFixed(3)}</td><td>${n.z.toFixed(3)}</td>
                <td>${bc}</td>
            `;
            tbody.appendChild(tr);
        });
    }

    renderBoundaryConditions() {
        const el = document.getElementById('bc-panel');
        if (!el) return;
        if (!this.input || !this.input.valid) { el.innerHTML = ''; return; }
        const p = this.input.prescribedIds, r = this.input.restrainedIds;
        el.innerHTML = `
            <div class="stat-card"><div class="stat-title">🔴 Prescritos (nós)</div><div class="stat-value" style="font-size:12px;">${p.length ? p.join(', ') : '(nenhum)'}</div></div>
            <div class="stat-card"><div class="stat-title">🟠 Restringidos (nós)</div><div class="stat-value" style="font-size:12px;">${r.length ? r.join(', ') : '(nenhum)'}</div></div>
        `;
    }

    renderEnvironmental() {
        const el = document.getElementById('env-panel');
        const currEl = document.getElementById('current-tbody');
        if (!el) return;
        if (!this.input || !this.input.valid) { el.innerHTML = ''; if (currEl) currEl.innerHTML = ''; return; }

        const sb = this.input.seabed, wv = this.input.wave;
        el.innerHTML = `
            <div class="control-title" style="margin-top:8px;">🏖️ SOLO</div>
            <table class="kv-table">
                <tr><td>Habilitado</td><td>${sb.enabled === false ? 'Não' : 'Sim'}</td></tr>
                <tr><td>Profundidade (depth_m)</td><td>${sb.depth_m ?? '—'} m</td></tr>
                <tr><td>Rigidez vertical</td><td>${sb.stiffness_Nm ?? '—'} N/m</td></tr>
                <tr><td>Modelo de solo</td><td>${sb.soil_model ?? 'uncoupled (padrão)'}</td></tr>
                <tr><td>Atrito (isotrópico)</td><td>${sb.friction_coeff ?? '—'}</td></tr>
                <tr><td>Atrito axial / limite elástico</td><td>${sb.axial_friction ?? '—'} / ${sb.axial_elastic_deflection_limit ?? '—'} m</td></tr>
                <tr><td>Atrito lateral / limite elástico</td><td>${sb.lateral_friction ?? '—'} / ${sb.lateral_elastic_deflection_limit ?? '—'} m</td></tr>
            </table>
            <div class="control-title" style="margin-top:8px;">🌊 ONDA</div>
            <table class="kv-table">
                <tr><td>Tipo</td><td>${wv.type ?? '—'}</td></tr>
                <tr><td>Período</td><td>${wv.period_s ?? '—'} s</td></tr>
                <tr><td>Altura / Amplitude</td><td>${wv.height_m ?? '—'} / ${wv.amplitude_m ?? '—'} m</td></tr>
                <tr><td>Ângulo</td><td>${wv.angle_deg ?? '—'}°</td></tr>
                <tr><td>Gamma (JONSWAP)</td><td>${wv.gamma ?? '—'}</td></tr>
            </table>
        `;

        const curr = this.input.current;
        const depths = curr.depth_below_surface_m || [], vels = curr.velocities_ms || [], angles = curr.angles_deg || [];
        const n = Math.max(depths.length, vels.length, angles.length);
        if (currEl) {
            currEl.innerHTML = '';
            for (let i = 0; i < n; i++) {
                const tr = document.createElement('tr');
                tr.innerHTML = `<td>${depths[i] ?? '—'}</td><td>${vels[i] ?? '—'}</td><td>${angles[i] ?? '—'}</td>`;
                currEl.appendChild(tr);
            }
        }

        if (typeof Plotly !== 'undefined' && n > 0) {
            const trace = {
                x: vels, y: depths.map(d => -Math.abs(d)),
                mode: 'lines+markers', type: 'scatter', name: 'Velocidade',
                line: { color: '#8a7cf2' }
            };
            Plotly.newPlot('current-chart', [trace], {
                margin: { t: 10, r: 10, b: 40, l: 50 },
                xaxis: { title: 'Velocidade (m/s)' },
                yaxis: { title: 'Profundidade (m)' },
                paper_bgcolor: 'transparent', plot_bgcolor: 'transparent',
                font: { color: this.currentTheme === 'dark' ? '#f1f0f6' : '#17151f' }
            }, { responsive: true, displayModeBar: false });
        }
    }

    renderAnalysisOptions() {
        const el = document.getElementById('analysis-panel');
        if (!el) return;
        if (!this.input || !this.input.valid) { el.innerHTML = ''; return; }
        const st = this.input.staticOptions, dy = this.input.dynamicOptions;
        el.innerHTML = `
            <div class="control-title" style="margin-top:8px;">📊 ESTÁTICA</div>
            <table class="kv-table">
                <tr><td>Passos</td><td>${st.steps ?? '—'}</td></tr>
                <tr><td>Máx. iterações</td><td>${st.max_iterations ?? '—'}</td></tr>
                <tr><td>Tolerância</td><td>${st.tolerance ?? '—'}</td></tr>
                <tr><td>Offset (near/far)</td><td>${st.vessel_offset?.near_m ?? 0} / ${st.vessel_offset?.far_m ?? 0} m</td></tr>
            </table>
            <div class="control-title" style="margin-top:8px;">🌊 DINÂMICA</div>
            <table class="kv-table">
                <tr><td>Habilitada</td><td>${dy.enabled === false ? 'Não' : 'Sim'}</td></tr>
                <tr><td>Duração / dt</td><td>${dy.duration_s ?? '—'} s / ${dy.dt_s ?? '—'} s</td></tr>
                <tr><td>Máx. iterações</td><td>${dy.max_iterations ?? '—'}</td></tr>
                <tr><td>Tolerância</td><td>${dy.tolerance ?? '—'}</td></tr>
                <tr><td>Rayleigh (α/β)</td><td>${dy.rayleigh_damping?.alpha ?? '—'} / ${dy.rayleigh_damping?.beta ?? '—'}</td></tr>
                <tr><td>Para no 1º não-convergido</td><td>${dy.stop_on_first_non_convergence ? 'Sim' : 'Não'}</td></tr>
            </table>
            <div class="warning-item warning-info" style="margin-top:8px;">
                ℹ️ Este JSON <b>não tem nenhum campo de função do tempo/rampa</b> (o ANFLEX real
                tem uma <code>cTimeFunction</code> com rampa em meio-cosseno para peso/empuxo/
                corrente -- ver <code>libs/anf_movements/ramp_function.cpp</code>). No risersim,
                a forma da rampa de carga estática é fixa no código
                (<code>static_analysis.cpp</code>, atualmente meio-cosseno também), não
                configurável por modelo via este JSON -- só o número de passos
                (<code>analysis_options.static.steps</code> acima) é.
            </div>
        `;
    }

    /** Single fetch feeding both the "Origem" panel and the "Simulações" tab -- the same
     * `GET /api/projects/<id>` project.js already uses, so opening a project doesn't cost two
     * separate requests for what's really one payload. Reschedules itself every ~3s while any run
     * is pending/running, same idea as project.js::scheduleAutoRefresh, but independent of which
     * of THIS page's own bottom-tabs happens to be visible right now (simpler than starting/
     * stopping polling on tab switch, and the Simulações tab button's own "(N)" count needs it
     * kept current even while a different tab -- e.g. Geometria -- is the one on screen). */
    async loadProjectMeta() {
        if (this.runsPollTimer) clearTimeout(this.runsPollTimer);
        let project;
        try {
            project = await fetchJSON(`/api/projects/${encodeURIComponent(this.projectId)}`);
        } catch (err) {
            document.getElementById('runs-tab-list').innerHTML = `<div class="empty-state">Falha ao carregar simulações: ${escapeHtml(err.message)}</div>`;
            return;
        }

        const { linesHtml, downloadsHtml } = modelSourceHtml(this.projectId, project.source);
        document.getElementById('origin-source').innerHTML = linesHtml;
        document.getElementById('origin-downloads').innerHTML = downloadsHtml;

        const runs = project.runs || [];
        document.getElementById('tab-runs-btn').innerText = `🕒 Simulações (${runs.length})`;
        document.getElementById('runs-tab-manager-link').href = `project.html?project=${encodeURIComponent(this.projectId)}&view=manager`;
        this.renderRunsTab(runs);

        const hasPending = runs.some(r => r.status === 'pending' || r.status === 'running');
        if (hasPending) this.runsPollTimer = setTimeout(() => this.loadProjectMeta(), 3000);
    }

    /** Compact, read-only run list (id/status/created + "Ver resultados" when converged) -- full
     * management (abort/delete/rename/downloads/log) stays in project.html's Gerenciador tab, one
     * click away via "Abrir no Gerenciador →", instead of duplicating that whole UI here. */
    renderRunsTab(runs) {
        const container = document.getElementById('runs-tab-list');
        if (runs.length === 0) {
            container.innerHTML = '<div class="empty-state">Nenhuma simulação ainda.</div>';
            return;
        }
        const rows = runs.map(r => `
            <tr>
                <td class="run-id-cell mono">${escapeHtml(r.id)}</td>
                <td>${escapeHtml(r.load_case_name || '—')}</td>
                <td>${statusPill(r.status)}</td>
                <td class="mono">${formatDate(r.created_at)}</td>
                <td>${r.status === 'converged' ? `<a class="link-btn" href="#" data-view-results="${escapeHtml(r.id)}">Ver resultados →</a>` : ''}</td>
            </tr>
        `).join('');
        container.innerHTML = `
            <div class="runs-list">
                <table class="run-table">
                    <thead><tr><th>Simulação</th><th>Caso</th><th>Status</th><th>Criada</th><th></th></tr></thead>
                    <tbody>${rows}</tbody>
                </table>
            </div>
        `;
        container.querySelectorAll('[data-view-results]').forEach(a => {
            a.addEventListener('click', (e) => {
                e.preventDefault();
                this.switchParentView('post', a.getAttribute('data-view-results'));
            });
        });
    }

    /** Switches the OUTER project.html page's tab (Manager/Pre/Post) from in here. This page is
     * normally embedded in project.html's <iframe> (same origin), so `window.parent.projectPage`
     * is reachable directly -- no postMessage needed, and it reuses the parent's own
     * switchView()/history handling instead of a second implementation. If opened standalone
     * (pasted URL, no parent project.html), falls back to a real navigation. */
    switchParentView(view, run) {
        if (window.parent !== window && window.parent.projectPage) {
            window.parent.projectPage.switchView(view, { run });
            return;
        }
        if (view === 'post' && run) {
            window.location.href = `posprocessor.html?project=${encodeURIComponent(this.projectId)}&run=${encodeURIComponent(run)}`;
        } else {
            window.location.href = `project.html?project=${encodeURIComponent(this.projectId)}&view=${encodeURIComponent(view)}`;
        }
    }

    /** "▶ Nova Simulação" in the Simulações tab -- same dedup-aware creation flow as
     * project.js::createRun (see createRun()/DuplicateRunError in runManagerFormat.js), just
     * triggered from inside the pre-processor instead of the Gerenciador tab. On success, also
     * nudges the parent's Manager view to reload (if embedded) so it doesn't stay stale about a
     * run it doesn't know was created. */
    async createRunFromTab(force = false) {
        const btn = document.getElementById('runs-tab-new-run-btn');
        btn.disabled = true;
        btn.innerText = 'Criando…';
        const select = document.getElementById('runs-tab-load-case-select');
        const load_case_id = (select && select.style.display !== 'none' && select.value)
            ? parseInt(select.value, 10) : null;
        try {
            await createRun(this.projectId, { force, load_case_id });
            await this.loadProjectMeta();
            if (window.parent !== window && window.parent.projectPage) window.parent.projectPage.load();
        } catch (err) {
            if (err instanceof DuplicateRunError) {
                const ok = await confirmDialog({ title: 'Simulação duplicada', message: `${err.message} Rodar mesmo assim?`, confirmLabel: 'Rodar mesmo assim', danger: false });
                if (ok) await this.createRunFromTab(true);
                return;
            }
            await alertDialog({ title: 'Falha ao criar simulação', message: err.message });
        } finally {
            btn.disabled = false;
            btn.innerText = '▶ Nova Simulação';
        }
    }
}

window.addEventListener('DOMContentLoaded', () => {
    window.preprocessorApp = new PreprocessorApp();
});
