import { initThemeToggle } from './ui/ThemeToggle.js';
import { confirmDialog, alertDialog } from './ui/ConfirmDialog.js';
import { statusPill, formatDate, formatRelativeDate, escapeHtml, fetchJSON } from './utils/runManagerFormat.js';

/** Chip label per status, used on project cards' status-dots row (see Dashboard.renderProjects) --
 * separate from STATUS_LABELS (run-table wording) because these read as a short summary count
 * ("2 convergidas") rather than a standalone badge ("Convergiu"), and singular/plural varies with
 * the count. */
const STATUS_CHIP_LABEL = {
    pending: () => 'na fila',
    running: () => 'rodando',
    converged: (n) => (n === 1 ? 'convergida' : 'convergidas'),
    failed: (n) => (n === 1 ? 'falhou' : 'falharam'),
    aborted: (n) => (n === 1 ? 'abortada' : 'abortadas'),
};
// Display order for the chips -- successes first, then in-progress, then problems.
const STATUS_CHIP_ORDER = ['converged', 'running', 'pending', 'failed', 'aborted'];

/**
 * dashboard.js
 * Home page of the run manager: stat-card row, project grid, recent-runs grid, and the
 * "New Project" creation modal. Talks only to the REST API exposed by `run_server.py`.
 */

class Dashboard {
    constructor() {
        this.projects = [];
        this.recentRuns = [];
        this.currentTheme = 'dark';
        this.projectSort = 'name'; // 'name' | 'modified' | 'created' -- see renderProjects()
        this.sourceTab = 'example'; // 'example' | 'upload' -- which tab of the "New Project" modal is active
        this.bindEvents();
        this.refresh();
    }

    bindEvents() {
        // ThemeToggle.js assumes a 3D-viewer app (app.renderer3D.scene/app.render()) -- this
        // page has none, so a harmless no-op shim satisfies its interface instead of
        // re-implementing the same body-class/button-label toggle logic here.
        initThemeToggle({
            currentTheme: this.currentTheme,
            renderer3D: { scene: { background: { setHex() {} } } },
            render() {},
        });

        document.getElementById('new-project-btn').addEventListener('click', () => this.openNewProjectModal());
        document.getElementById('new-project-cancel-btn').addEventListener('click', () => this.closeNewProjectModal());
        document.getElementById('new-project-backdrop').addEventListener('click', (e) => {
            if (e.target.id === 'new-project-backdrop') this.closeNewProjectModal();
        });
        document.getElementById('new-project-confirm-btn').addEventListener('click', () => this.submitNewProject());
        document.getElementById('source-tab-example-btn').addEventListener('click', () => this.setSourceTab('example'));
        document.getElementById('source-tab-upload-btn').addEventListener('click', () => this.setSourceTab('upload'));
        document.getElementById('source-tab-blank-btn').addEventListener('click', () => this.setSourceTab('blank'));
        document.getElementById('example-select').addEventListener('change', (e) => {
            const opt = e.target.selectedOptions[0];
            const nameInput = document.getElementById('project-name-input');
            if (opt && !nameInput.dataset.userEdited) nameInput.value = opt.dataset.name || '';
            this.renderExampleLoadCaseSelect();
        });
        document.getElementById('project-name-input').addEventListener('input', (e) => {
            e.target.dataset.userEdited = '1';
        });

        document.querySelectorAll('#project-sort-switch button[data-sort]').forEach(btn => {
            btn.addEventListener('click', () => {
                this.projectSort = btn.getAttribute('data-sort');
                document.querySelectorAll('#project-sort-switch button').forEach(b => {
                    b.classList.toggle('active', b === btn);
                });
                this.renderProjects();
            });
        });
    }

    async refresh() {
        try {
            this.projects = await fetchJSON('/api/projects');
        } catch (err) {
            console.error('Falha ao carregar projetos:', err);
            this.projects = [];
        }
        // Fills in each project's `lastActivity` (most recent run, or the project's own
        // created_at if it has none yet) before the first render -- both renderProjects()'s
        // "Última atividade" row and the sort-by-modified option need it, and it isn't part of
        // the /api/projects summary (list_projects() doesn't track a project-level updated_at).
        await this.loadRecentRuns();
        this.renderStats();
        this.renderProjects();
        this.renderRecentRuns();
    }

    renderStats() {
        const totalProjects = this.projects.length;
        let totalRuns = 0, converged = 0, running = 0;
        for (const p of this.projects) {
            totalRuns += p.run_count || 0;
            const counts = p.status_counts || {};
            converged += counts.converged || 0;
            running += counts.running || 0;
        }
        document.getElementById('stat-projects').innerText = totalProjects;
        document.getElementById('stat-runs').innerText = totalRuns;
        document.getElementById('stat-converged').innerText = converged;
        document.getElementById('stat-running').innerText = running;
    }

    /** The filename (or example id) shown under a project's name on its card -- one input source
     * per project, so this alone identifies where it came from (same reasoning as
     * project.js::renderHeader's "Origem do modelo" panel, just condensed to one line here). */
    projectSourceLabel(p) {
        const src = p.source || {};
        if (src.xml_path) return src.xml_path.split('/').pop();
        if (src.example_id) return src.example_id;
        return '—';
    }

    sortedProjects() {
        const list = this.projects.slice();
        if (this.projectSort === 'modified') {
            list.sort((a, b) => (b.lastActivity || '').localeCompare(a.lastActivity || ''));
        } else if (this.projectSort === 'created') {
            list.sort((a, b) => (b.created_at || '').localeCompare(a.created_at || ''));
        } else {
            list.sort((a, b) => a.name.localeCompare(b.name));
        }
        return list;
    }

    renderProjects() {
        const grid = document.getElementById('projects-grid');
        document.getElementById('projects-count').innerText = this.projects.length ? String(this.projects.length) : '';
        if (this.projects.length === 0) {
            grid.innerHTML = '<div class="empty-state">Nenhum projeto ainda. Clique em "Novo Projeto" para começar.</div>';
            return;
        }
        grid.innerHTML = this.sortedProjects().map(p => {
            const counts = p.status_counts || {};
            const chips = STATUS_CHIP_ORDER
                .filter(status => counts[status])
                .map(status => `<span class="status-dot-chip badge-${status}"><span class="dot"></span>${counts[status]} ${STATUS_CHIP_LABEL[status](counts[status])}</span>`)
                .join('');
            return `
                <div class="project-card">
                    <div class="top-row">
                        <div>
                            <div class="name"><a href="project.html?project=${encodeURIComponent(p.id)}">${escapeHtml(p.name)}</a></div>
                            <div class="source mono">${escapeHtml(this.projectSourceLabel(p))}</div>
                        </div>
                        <div class="run-count-badge">${p.run_count || 0} simulaç${p.run_count === 1 ? 'ão' : 'ões'}</div>
                    </div>
                    ${chips ? `<div class="status-dots">${chips}</div>` : ''}
                    <div class="meta">
                        <div class="row"><span>Criado em</span><span class="mono">${formatRelativeDate(p.created_at)}</span></div>
                        <div class="row"><span>Última atividade</span><span class="mono">${formatRelativeDate(p.lastActivity)}</span></div>
                    </div>
                    <div class="card-actions">
                        <a class="link-btn" href="project.html?project=${encodeURIComponent(p.id)}">Ver projeto →</a>
                        <div class="card-actions-right">
                            <button class="btn small info" data-duplicate-project="${escapeHtml(p.id)}" data-tip="Duplicar projeto: cria uma cópia com os mesmos arquivos de origem e configuração atual, sem repetir o histórico de simulações">⧉</button>
                            <button class="btn small danger" data-delete-project="${escapeHtml(p.id)}" data-tip="Apagar projeto: remove o projeto e TODAS as suas simulações permanentemente">🗑</button>
                        </div>
                    </div>
                </div>
            `;
        }).join('');

        // Duplicates the project (new id, same source files + compiled input, no run history --
        // see risersim_projects.py::duplicate_project) straight from the card.
        grid.querySelectorAll('[data-duplicate-project]').forEach(btn => {
            btn.addEventListener('click', async (e) => {
                const projectId = e.target.getAttribute('data-duplicate-project');
                e.target.disabled = true;
                try {
                    const project = await fetchJSON(`/api/projects/${encodeURIComponent(projectId)}/duplicate`, { method: 'POST' });
                    window.location.href = `project.html?project=${encodeURIComponent(project.id)}`;
                } catch (err) {
                    await alertDialog({ title: 'Falha ao duplicar projeto', message: err.message });
                    e.target.disabled = false;
                }
            });
        });

        // Quick deletion straight from the card, without opening the project -- see
        // project.js::deleteProject for the full flow (same API, DELETE /api/projects/<id>);
        // refuses (409) if any run is pending/running (see risersim_projects.py::delete_project).
        grid.querySelectorAll('[data-delete-project]').forEach(btn => {
            btn.addEventListener('click', async (e) => {
                const projectId = e.target.getAttribute('data-delete-project');
                const ok = await confirmDialog({
                    title: 'Apagar projeto',
                    message: 'Apagar esse projeto e TODAS as suas simulações? Essa ação não pode ser desfeita.',
                    confirmLabel: 'Apagar tudo',
                });
                if (!ok) return;
                e.target.disabled = true;
                try {
                    const res = await fetch(`/api/projects/${encodeURIComponent(projectId)}`, { method: 'DELETE' });
                    if (!res.ok && res.status !== 204) {
                        let detail = '';
                        try { detail = (await res.json()).error || ''; } catch (err) { /* ignore */ }
                        throw new Error(detail || `HTTP ${res.status}`);
                    }
                    await this.refresh();
                } catch (err) {
                    await alertDialog({ title: 'Falha ao apagar projeto', message: err.message });
                    e.target.disabled = false;
                }
            });
        });
    }

    /** Fetches every project's detail (few projects expected in this phase, no pagination) to
     * get at their run lists -- /api/projects's summary only has counts, not the runs themselves.
     * Fills `this.recentRuns` (most recent 9, across all projects) and each project's
     * `lastActivity` (most recent run's created_at, falling back to the project's own
     * created_at when it has no runs yet) -- used by both renderProjects() and renderRecentRuns(). */
    async loadRecentRuns() {
        if (this.projects.length === 0) {
            this.recentRuns = [];
            return;
        }
        const details = await Promise.all(this.projects.map(p =>
            fetchJSON(`/api/projects/${encodeURIComponent(p.id)}`).catch(() => null)
        ));

        const allRuns = [];
        details.forEach((detail, idx) => {
            const project = this.projects[idx];
            const runs = (detail && detail.runs) || [];
            runs.forEach(run => allRuns.push({ ...run, projectName: project.name }));
            const mostRecent = runs.reduce((max, r) => (r.created_at || '') > max ? r.created_at : max, '');
            project.lastActivity = mostRecent || project.created_at;
        });
        allRuns.sort((a, b) => (b.created_at || '').localeCompare(a.created_at || ''));
        this.recentRuns = allRuns.slice(0, 9);
    }

    renderRecentRuns() {
        const container = document.getElementById('runs-list-container');
        if (this.recentRuns.length === 0) {
            container.innerHTML = '<div class="empty-state">Nenhuma simulação ainda.</div>';
            return;
        }
        const rows = this.recentRuns.map(r => `
            <tr>
                <td class="run-id-cell mono"><a class="link-btn" href="project.html?project=${encodeURIComponent(r.project_id)}&view=manager">${escapeHtml(r.id)}</a></td>
                <td class="project-name-cell">${escapeHtml(r.projectName)}</td>
                <td>${statusPill(r.status)}</td>
                <td class="mono">${formatDate(r.created_at)}</td>
            </tr>
        `).join('');
        container.innerHTML = `
            <div class="runs-list">
                <table class="run-table">
                    <thead>
                        <tr><th>Simulação</th><th>Projeto</th><th>Status</th><th>Criada</th></tr>
                    </thead>
                    <tbody>${rows}</tbody>
                </table>
            </div>
        `;
    }

    /** Switches between the "New Project" modal's three source tabs: pre-discovered example,
     * manual upload, or blank (docs/roadmap.md, Eixo 3a). */
    setSourceTab(tab) {
        this.sourceTab = tab;
        document.getElementById('source-tab-example').style.display = tab === 'example' ? '' : 'none';
        document.getElementById('source-tab-upload').style.display = tab === 'upload' ? '' : 'none';
        document.getElementById('source-tab-blank').style.display = tab === 'blank' ? '' : 'none';
        document.getElementById('source-tab-example-btn').className = tab === 'example' ? 'btn-tab active' : 'btn-tab';
        document.getElementById('source-tab-upload-btn').className = tab === 'upload' ? 'btn-tab active' : 'btn-tab';
        document.getElementById('source-tab-blank-btn').className = tab === 'blank' ? 'btn-tab active' : 'btn-tab';
        document.getElementById('new-project-error').innerText = '';
    }

    async openNewProjectModal() {
        const backdrop = document.getElementById('new-project-backdrop');
        const select = document.getElementById('example-select');
        const nameInput = document.getElementById('project-name-input');
        const errorEl = document.getElementById('new-project-error');
        errorEl.innerText = '';
        nameInput.value = '';
        delete nameInput.dataset.userEdited;
        ['upload-xml-input', 'upload-h5-input', 'upload-aml-input'].forEach(id => { document.getElementById(id).value = ''; });
        this.setSourceTab('example');
        select.innerHTML = '<option>Carregando…</option>';
        backdrop.classList.add('open');

        try {
            // Both real XML+H5 exports AND plain .aml files with no export (see
            // risersim_runner.py::discover_aml_only_examples()) -- an entry's `xml_path` tells
            // them apart (null for an .aml-only one), same shape either way.
            this.examples = await fetchJSON('/api/examples');
            if (this.examples.length === 0) {
                select.innerHTML = '<option value="">Nenhum exemplo disponível</option>';
                return;
            }
            select.innerHTML = this.examples.map(ex =>
                `<option value="${escapeHtml(ex.id)}" data-name="${escapeHtml(ex.name)}">${escapeHtml(ex.id)}${ex.xml_path ? '' : ' · .aml puro (sem XML+H5)'}</option>`
            ).join('');
            nameInput.value = this.examples[0].name;
            this.renderExampleLoadCaseSelect();
        } catch (err) {
            select.innerHTML = '<option value="">Falha ao carregar exemplos</option>';
            errorEl.innerText = err.message;
        }
    }

    /** Shows a load-case picker under the example <select> only when the CURRENTLY SELECTED
     * example is `.aml`-only (`xml_path === null`) and defines more than one %LOAD_CASE -- picks
     * which case becomes the new project's DEFAULT/project-level input (a real XML+H5 export has
     * no such choice, it's already resolved to one case; other cases remain reachable afterward
     * via the per-run selector already in project.js/preprocessor_app.js). */
    renderExampleLoadCaseSelect() {
        const row = document.getElementById('example-load-case-row');
        const select = document.getElementById('example-load-case-select');
        const exampleId = document.getElementById('example-select').value;
        const example = (this.examples || []).find(e => e.id === exampleId);
        const loadCases = (example && !example.xml_path) ? (example.load_cases || []) : [];
        if (loadCases.length <= 1) {
            row.style.display = 'none';
            select.innerHTML = '';
            return;
        }
        select.innerHTML = loadCases.map(lc =>
            `<option value="${lc.id}">${escapeHtml(lc.name)} (ID ${lc.id})</option>`
        ).join('');
        row.style.display = '';
    }

    closeNewProjectModal() {
        document.getElementById('new-project-backdrop').classList.remove('open');
    }

    /** Dispatches to one of the three creation flows depending on the modal's active tab (see setSourceTab). */
    async submitNewProject() {
        if (this.sourceTab === 'upload') return this.submitUploadProject();
        if (this.sourceTab === 'blank') return this.submitBlankProject();
        return this.submitExampleProject();
    }

    async submitExampleProject() {
        const select = document.getElementById('example-select');
        const nameInput = document.getElementById('project-name-input');
        const errorEl = document.getElementById('new-project-error');
        const confirmBtn = document.getElementById('new-project-confirm-btn');
        const exampleId = select.value;
        const name = nameInput.value.trim();

        if (!exampleId) { errorEl.innerText = 'Selecione um exemplo.'; return; }
        if (!name) { errorEl.innerText = 'Informe um nome para o projeto.'; return; }

        const loadCaseSelect = document.getElementById('example-load-case-select');
        const loadCaseRow = document.getElementById('example-load-case-row');
        const load_case_id = (loadCaseRow.style.display !== 'none' && loadCaseSelect.value)
            ? parseInt(loadCaseSelect.value, 10) : null;

        confirmBtn.disabled = true;
        confirmBtn.innerText = 'Criando…';
        errorEl.innerText = '';
        try {
            const project = await fetchJSON('/api/projects', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ example_id: exampleId, name, load_case_id }),
            });
            window.location.href = `project.html?project=${encodeURIComponent(project.id)}`;
        } catch (err) {
            errorEl.innerText = err.message;
            confirmBtn.disabled = false;
            confirmBtn.innerText = 'Criar Projeto';
        }
    }

    /** The "📤 Import file" tab: submits via FormData (multipart) to the upload endpoint, instead
     * of the JSON body used by the example flow -- see run_server.py::api_upload_project. */
    async submitUploadProject() {
        const nameInput = document.getElementById('project-name-input');
        const errorEl = document.getElementById('new-project-error');
        const confirmBtn = document.getElementById('new-project-confirm-btn');
        const xmlInput = document.getElementById('upload-xml-input');
        const h5Input = document.getElementById('upload-h5-input');
        const amlInput = document.getElementById('upload-aml-input');
        const name = nameInput.value.trim();

        if (!name) { errorEl.innerText = 'Informe um nome para o projeto.'; return; }
        const hasXmlH5 = xmlInput.files[0] && h5Input.files[0];
        const hasAmlOnly = amlInput.files[0] && !xmlInput.files[0] && !h5Input.files[0];
        if (!hasXmlH5 && !hasAmlOnly) {
            errorEl.innerText = 'Selecione XML+H5 juntos, ou só o arquivo AML/PML (sem XML+H5).';
            return;
        }

        const formData = new FormData();
        formData.append('name', name);
        if (xmlInput.files[0]) formData.append('xml_file', xmlInput.files[0]);
        if (h5Input.files[0]) formData.append('h5_file', h5Input.files[0]);
        if (amlInput.files[0]) formData.append('aml_file', amlInput.files[0]);

        confirmBtn.disabled = true;
        confirmBtn.innerText = 'Criando…';
        errorEl.innerText = '';
        try {
            // No explicit Content-Type header on purpose -- the browser sets
            // multipart/form-data with the correct boundary on its own from the FormData.
            const project = await fetchJSON('/api/projects/upload', { method: 'POST', body: formData });
            window.location.href = `project.html?project=${encodeURIComponent(project.id)}`;
        } catch (err) {
            errorEl.innerText = err.message;
            confirmBtn.disabled = false;
            confirmBtn.innerText = 'Criar Projeto';
        }
    }

    /** The "✏️ Em branco" tab: creates a project from a minimal-but-valid default "JSON de
     * interface" (docs/roadmap.md, Eixo 3a) -- one soil/material/line/analysis, no vessel motion
     * -- then navigates straight into the editor (not project.html) so the user starts adjusting
     * values immediately, same "land on the thing you came to do" pattern as the example/upload
     * flows landing on project.html. Reuses `POST /api/projects/blank`
     * (`risersim_projects.py::create_blank_project()`), which validates/compiles server-side --
     * any structural issue with the default itself would surface here as a clear error, not a
     * silent broken project. */
    async submitBlankProject() {
        const nameInput = document.getElementById('project-name-input');
        const errorEl = document.getElementById('new-project-error');
        const confirmBtn = document.getElementById('new-project-confirm-btn');
        const name = nameInput.value.trim();
        if (!name) { errorEl.innerText = 'Informe um nome para o projeto.'; return; }

        confirmBtn.disabled = true;
        confirmBtn.innerText = 'Criando…';
        errorEl.innerText = '';
        try {
            const project = await fetchJSON('/api/projects/blank', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ name, interface: buildDefaultInterface() }),
            });
            window.location.href = `editor.html?project=${encodeURIComponent(project.id)}`;
        } catch (err) {
            errorEl.innerText = err.message;
            confirmBtn.disabled = false;
            confirmBtn.innerText = 'Criar Projeto';
        }
    }
}

/** The starting point for a "projeto em branco" (docs/roadmap.md, Eixo 3a) -- one of everything
 * needed for `build_config_from_interface()` to compile a valid (if physically arbitrary) model
 * right away: a generic riser-like material, a soft/generic soil, a single 500m line with a
 * modest catenary angle (topo fixo no espaço, sem corpo flutuante -- v1 não inclui movimento de
 * topo/RAO), one load case with no current/wave (both optional -- the compiler falls back to
 * zero/defaults), and one analysis covering it. The editor is where the user actually shapes
 * this into their real model -- these values only need to be schema-valid, not meaningful. */
function buildDefaultInterface() {
    return {
        schema_version: 1,
        title: 'Novo modelo',
        global: { water_depth_m: 100.0, gravity_ms2: 9.81, water_specific_weight_kNm3: 10.0553, steel_specific_weight_kNm3: 77.0 },
        soils: [{
            id: 1, name: 'Solo_1', model: 'uncoupled', friction_axial: 0.5, friction_lateral: 0.5,
            deflection_axial_m: 0.03, deflection_lateral_m: 0.2, spring_stiffness_kNm: 800.0,
        }],
        materials: [{
            id: 1, name: 'Material_1', finite_element_type: 'beam',
            diameter_external_m: 0.25, diameter_internal_m: 0.20,
            weight_dry_kNm: 1.0, weight_wet_kNm: 0.4, morison_inertia_Cm: 2.0, morison_drag_Cd: 1.0,
            stiffness_axial_kN: 360000.0, initial_tension_kN: 0.0,
            stiffness_bending_kNm2: 21.7, stiffness_torsional_kNm2: 3200.0,
            rayleigh_period_first_s: 0.0, rayleigh_period_second_s: 0.0,
            rayleigh_damping_first: 0.0, rayleigh_damping_second: 0.0,
            scalar_stiffness_translational_kNm: 0.0, scalar_stiffness_torsional_kNm_per_rad: 0.0,
            scalar_stiffness_bending_kNm_per_rad: 0.0, winch_payout_curve_id: null,
        }],
        curves: [],
        currents: [],
        waves: [],
        buoys: [],
        lines: [{
            id: 1, name: 'Linha_1', top_position_m: [0.0, 0.0, 100.0],
            catenary_angle_deg: 25.0, azimuth_deg: 0.0,
            segments: [{ id: 1, length_m: 150.0, material_id: 1, soil_id: 1, element_length_m: 3.0, buoy_id: null }],
        }],
        load_cases: [{ id: 1, name: 'Caso_1', current_id: null, wave_id: null }],
        analyses: [{
            id: 1, name: 'Analise_1',
            static: { steps: 20, max_iterations: 300, tolerance: 0.01 },
            dynamic: { enabled: true, duration_s: 20.0, dt_s: 0.05, max_iterations: 20, tolerance: 1.0e-4 },
            load_case_ids: [1],
        }],
    };
}

window.addEventListener('DOMContentLoaded', () => {
    window.dashboard = new Dashboard();
});
