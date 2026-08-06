import { InputLoaderService } from './services/InputLoaderService.js';
import { ColorMapService } from './services/ColorMapService.js';
import { Riser3DRenderer } from './renderers/Riser3DRenderer.js';
import { CameraViewController } from './renderers/CameraViewController.js';

/**
 * preprocessor_app.js
 * Controlador principal do pré-processador web: inspeciona/valida o JSON de ENTRADA consumido
 * por main_test.cpp, ANTES de rodar a simulação -- não os resultados (isso é viewer_3d.html/app.js).
 */
class PreprocessorApp {
    constructor() {
        this.input = null; // resultado de InputLoaderService.parse()
        this.currentTheme = 'dark';
        this.tableViewMode = 'grouped'; // 'grouped' | 'byElement'
        this.initUI();
    }

    async initUI() {
        const canvas = document.getElementById('three-canvas');
        this.renderer3D = new Riser3DRenderer(canvas);
        this.cameraController = new CameraViewController(this.renderer3D.camera, this.renderer3D.controls, this.renderer3D);

        this.bindEvents();
        this.initResizer();
        this.initZoomWindow();
        await this.loadInput('../input_simulation.json');
    }

    /** Divisor arrastável entre o canvas 3D e o painel de dados (largura, não altura -- monitores widescreen aproveitam melhor lado a lado do que empilhado). */
    initResizer() {
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
            if (this.renderer3D) this.renderer3D.onWindowResize();
        });

        window.addEventListener('mouseup', () => {
            if (isDragging) {
                isDragging = false;
                resizer.classList.remove('dragging');
                document.body.style.cursor = '';
                if (this.renderer3D) this.renderer3D.onWindowResize();
            }
        });
    }

    /**
     * "Zoom por janela" (tipo CAD): ativa via botão, o próximo arraste do mouse sobre o canvas
     * desenha um retângulo de seleção; ao soltar, converte o retângulo para NDC e pede ao
     * CameraViewController para reenquadrar nele (ver zoomToWindow()), e desativa o modo.
     */
    initZoomWindow() {
        this.zoomWindowActive = false;
        const canvas = document.getElementById('three-canvas');
        const container = document.getElementById('canvas-container');
        const rectEl = document.getElementById('zoom-rect');
        let dragging = false, startX = 0, startY = 0;

        canvas.addEventListener('mousedown', (e) => {
            if (!this.zoomWindowActive) return;
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
            this.setZoomWindowActive(false);
        });

        window.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && this.zoomWindowActive) this.setZoomWindowActive(false);
        });
    }

    setZoomWindowActive(active) {
        this.zoomWindowActive = active;
        const canvas = document.getElementById('three-canvas');
        const btn = document.getElementById('zoom-window-btn');
        if (canvas) canvas.style.cursor = active ? 'crosshair' : '';
        if (btn) btn.classList.toggle('active', active);
        if (this.renderer3D && this.renderer3D.controls) this.renderer3D.controls.enabled = !active;
    }

    bindEvents() {
        document.getElementById('view-iso-btn').addEventListener('click', () => this.cameraController.setView('ISO', this.getSyntheticStep(), ...this.getEnvBounds()));
        document.getElementById('view-xy-btn').addEventListener('click', () => this.cameraController.setView('XY', this.getSyntheticStep(), ...this.getEnvBounds()));
        document.getElementById('view-xz-btn').addEventListener('click', () => this.cameraController.setView('XZ', this.getSyntheticStep(), ...this.getEnvBounds()));
        document.getElementById('view-yz-btn').addEventListener('click', () => this.cameraController.setView('YZ', this.getSyntheticStep(), ...this.getEnvBounds()));

        document.getElementById('zoom-fit-btn').addEventListener('click', () => this.cameraController.fitToModel(this.getSyntheticStep(), ...this.getEnvBounds()));
        document.getElementById('zoom-in-btn').addEventListener('click', () => this.cameraController.zoomIn());
        document.getElementById('zoom-out-btn').addEventListener('click', () => this.cameraController.zoomOut());
        document.getElementById('zoom-window-btn').addEventListener('click', () => this.setZoomWindowActive(!this.zoomWindowActive));

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

        document.getElementById('theme-toggle-btn').addEventListener('click', () => {
            this.currentTheme = this.currentTheme === 'dark' ? 'light' : 'dark';
            document.body.className = this.currentTheme === 'dark' ? 'dark-mode' : 'light-mode';
            document.getElementById('theme-toggle-btn').innerText = this.currentTheme === 'dark' ? '☀️ Modo Claro' : '🌙 Modo Escuro';
            this.renderer3D.scene.background.setHex(this.currentTheme === 'dark' ? 0x1e1e2e : 0xffffff);
            this.render();
        });
    }

    async loadInput(fileOrUrl) {
        try {
            this.input = await InputLoaderService.load(fileOrUrl);

            // Reenquadra a câmera no modelo recém-carregado -- sem isso, a câmera fica parada no
            // chute inicial de Riser3DRenderer (calibrado para ~130m), e um modelo de escala bem
            // diferente aparece como uma fatia minúscula/distorcida (mesmo bug já visto e
            // corrigido no viewer de resultados nesta sessão).
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

    /** Sintetiza um objeto no formato {nodes, elements} que Riser3DRenderer/CameraViewController já entendem, colorindo pelo diâmetro externo (D_outer) em vez de um resultado resolvido (que não existe para dados de entrada). */
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
     * Espelha a lógica de posicionamento de solo/superfície de main_test.cpp:104-336 --
     * seabed.enabled=false empurra o fundo para -1e6 (sem contato possível) e aproxima a
     * superfície pelo nó mais alto; caso contrário o fundo fica no Z mínimo real dos nós.
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

    /** Alterna entre as 4 abas do painel de dados: Carregar / Geometria / Condições Ambientais / Análise. */
    setBottomTab(tab) {
        const tabs = { load: 'tab-load', geometry: 'tab-geometry', environmental: 'tab-environmental', analysis: 'tab-analysis' };
        Object.entries(tabs).forEach(([key, panelId]) => {
            document.getElementById(panelId).classList.toggle('active', key === tab);
            document.getElementById(`tab-${key}-btn`).className = key === tab ? 'btn-tab active' : 'btn-tab';
        });
        if (tab === 'environmental' && typeof Plotly !== 'undefined') {
            // Plotly não desenha corretamente num container que estava com display:none --
            // força um resize assim que a aba fica visível.
            setTimeout(() => Plotly.Plots.resize('current-chart'), 50);
        }
    }

    /** Alterna entre a tabela de seções agrupadas, a tabela de elementos completa e a tabela de nós. */
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

    /** Marca nós com condição de contorno (prescritos/restringidos) com esferas coloridas -- Riser3DRenderer não faz isso sozinho (nodesGroup fica vazio em renderStep), então preenchido diretamente aqui, depois da chamada a renderStep (que limpa nodesGroup no início). */
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
            mesh.position.set(n.x, n.z, n.y); // mesma convenção de eixos do Riser3DRenderer (Y-three = Z-riser)
            group.add(mesh);
        };

        const span = this.computeSpan();
        const radius = Math.max(0.3, span * 0.01);
        this.input.restrainedIds.forEach(id => addMarker(id, 0xf59e0b, radius));
        this.input.prescribedIds.forEach(id => addMarker(id, 0xef4444, radius * 1.15));
    }

    /** Pa -> MPa, 1 casa decimal (E/G ficam mais legíveis assim do que em Pa cru). */
    formatMPa(v) {
        if (v === undefined || v === null) return '—';
        const num = Number(v);
        return Number.isFinite(num) ? (num / 1.0e6).toFixed(1) : String(v);
    }

    /** Notação científica -- para grandezas como IY/IZ/J (m⁴) onde a forma decimal fixa vira uma sequência de zeros ilegível. */
    formatSci(v, digits = 3) {
        if (v === undefined || v === null) return '—';
        const num = Number(v);
        return Number.isFinite(num) ? num.toExponential(digits) : String(v);
    }

    /** N·m² -> kN·m² (EI vem em N·m² no JSON de entrada). */
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
            `;
            tbody.appendChild(tr);
        });
    }

    /**
     * Tabela elemento-por-elemento com TODAS as propriedades de seção lidas por main_test.cpp
     * (não um subconjunto) -- só os IDs dos nós, sem coordenadas (isso fica na tabela de Nós,
     * evitando repetir a mesma coordenada uma vez por elemento adjacente).
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
                <td>${fmt(sp.Ca)}</td>
            `;
            tbody.appendChild(tr);
        });
    }

    /** Tabela de nós: coordenadas + condição de contorno (prescrito/restringido/livre). */
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
        const depths = curr.depths_m || [], vels = curr.velocities_ms || [], angles = curr.angles_deg || [];
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
                line: { color: '#89b4fa' }
            };
            Plotly.newPlot('current-chart', [trace], {
                margin: { t: 10, r: 10, b: 40, l: 50 },
                xaxis: { title: 'Velocidade (m/s)' },
                yaxis: { title: 'Profundidade (m)' },
                paper_bgcolor: 'transparent', plot_bgcolor: 'transparent',
                font: { color: this.currentTheme === 'dark' ? '#cdd6f4' : '#0f172a' }
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
}

window.addEventListener('DOMContentLoaded', () => {
    window.preprocessorApp = new PreprocessorApp();
});
