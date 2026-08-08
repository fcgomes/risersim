import { initThemeToggle } from './ui/ThemeToggle.js';

/**
 * dashboard.js
 * Home page of the run manager: stat-card row, project grid, recent-runs grid, and the "Novo
 * Projeto" creation modal. Talks only to the REST API exposed by run_server.py (tools/run_server.py).
 */

const STATUS_LABELS = {
    pending: 'Na fila',
    running: 'Executando',
    converged: 'Convergiu',
    failed: 'Falhou',
};

function statusBadge(status) {
    const label = STATUS_LABELS[status] || status || 'desconhecido';
    return `<span class="badge badge-${status}">${label}</span>`;
}

function formatDate(iso) {
    if (!iso) return '—';
    try {
        return new Date(iso).toLocaleString('pt-BR');
    } catch (e) {
        return iso;
    }
}

async function fetchJSON(url, options) {
    const res = await fetch(url, options);
    if (!res.ok) {
        let detail = '';
        try { detail = (await res.json()).error || ''; } catch (e) { /* ignore */ }
        throw new Error(detail || `HTTP ${res.status}`);
    }
    return res.json();
}

class Dashboard {
    constructor() {
        this.projects = [];
        this.recentRuns = [];
        this.currentTheme = 'dark';
        this.sourceTab = 'example'; // 'example' | 'upload' -- qual aba do modal "Novo Projeto" está ativa
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
        document.getElementById('example-select').addEventListener('change', (e) => {
            const opt = e.target.selectedOptions[0];
            const nameInput = document.getElementById('project-name-input');
            if (opt && !nameInput.dataset.userEdited) nameInput.value = opt.dataset.name || '';
        });
        document.getElementById('project-name-input').addEventListener('input', (e) => {
            e.target.dataset.userEdited = '1';
        });
    }

    async refresh() {
        try {
            this.projects = await fetchJSON('/api/projects');
        } catch (err) {
            console.error('Falha ao carregar projetos:', err);
            this.projects = [];
        }
        this.renderStats();
        this.renderProjects();
        await this.loadRecentRuns();
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

    renderProjects() {
        const grid = document.getElementById('projects-grid');
        if (this.projects.length === 0) {
            grid.innerHTML = '<div class="empty-state">Nenhum projeto ainda. Clique em "Novo Projeto" para começar.</div>';
            return;
        }
        grid.innerHTML = this.projects.map(p => {
            const counts = p.status_counts || {};
            return `
                <div class="card">
                    <h3><a href="project.html?project=${encodeURIComponent(p.id)}">${escapeHtml(p.name)}</a></h3>
                    <div class="meta">ID: ${escapeHtml(p.id)} · Criado em ${formatDate(p.created_at)}</div>
                    <div>${p.run_count || 0} rodada(s)
                        ${counts.converged ? ` · ${counts.converged} convergida(s)` : ''}
                        ${counts.running ? ` · ${counts.running} em execução` : ''}
                        ${counts.failed ? ` · ${counts.failed} falhada(s)` : ''}
                    </div>
                    <div class="row">
                        <a class="link-btn" href="project.html?project=${encodeURIComponent(p.id)}">Ver projeto →</a>
                        <button class="btn small" data-run-project="${escapeHtml(p.id)}">▶ Nova Rodada</button>
                    </div>
                </div>
            `;
        }).join('');

        grid.querySelectorAll('[data-run-project]').forEach(btn => {
            btn.addEventListener('click', async (e) => {
                const projectId = e.target.getAttribute('data-run-project');
                e.target.disabled = true;
                e.target.innerText = 'Criando…';
                try {
                    await fetchJSON(`/api/projects/${encodeURIComponent(projectId)}/runs`, { method: 'POST' });
                    window.location.href = `project.html?project=${encodeURIComponent(projectId)}`;
                } catch (err) {
                    alert(`Falha ao criar rodada: ${err.message}`);
                    e.target.disabled = false;
                    e.target.innerText = '▶ Nova Rodada';
                }
            });
        });
    }

    async loadRecentRuns() {
        const grid = document.getElementById('runs-grid');
        if (this.projects.length === 0) {
            grid.innerHTML = '<div class="empty-state">Nenhuma rodada ainda.</div>';
            return;
        }
        // O resumo de /api/projects não traz a lista de rodadas -- busca o detalhe de cada
        // projeto (poucos projetos esperados neste Phase 1, sem paginação) e junta tudo numa
        // lista só, ordenada pela mais recente.
        const details = await Promise.all(this.projects.map(p =>
            fetchJSON(`/api/projects/${encodeURIComponent(p.id)}`).catch(() => null)
        ));

        const allRuns = [];
        details.forEach((detail, idx) => {
            if (!detail) return;
            const project = this.projects[idx];
            (detail.runs || []).forEach(run => allRuns.push({ ...run, projectName: project.name }));
        });
        allRuns.sort((a, b) => (b.created_at || '').localeCompare(a.created_at || ''));
        this.recentRuns = allRuns.slice(0, 9);

        if (this.recentRuns.length === 0) {
            grid.innerHTML = '<div class="empty-state">Nenhuma rodada ainda.</div>';
            return;
        }

        grid.innerHTML = this.recentRuns.map(r => `
            <div class="card">
                <h3><a href="project.html?project=${encodeURIComponent(r.project_id)}">${escapeHtml(r.projectName)}</a></h3>
                <div class="meta">${escapeHtml(r.id)} · Criada em ${formatDate(r.created_at)}</div>
                <div class="row">
                    ${statusBadge(r.status)}
                    <a class="link-btn" href="project.html?project=${encodeURIComponent(r.project_id)}">Detalhes →</a>
                </div>
            </div>
        `).join('');
    }

    /** Alterna entre as duas abas de origem do modal "Novo Projeto": exemplo pré-descoberto vs. upload manual. */
    setSourceTab(tab) {
        this.sourceTab = tab;
        document.getElementById('source-tab-example').style.display = tab === 'example' ? '' : 'none';
        document.getElementById('source-tab-upload').style.display = tab === 'upload' ? '' : 'none';
        document.getElementById('source-tab-example-btn').className = tab === 'example' ? 'btn-tab active' : 'btn-tab';
        document.getElementById('source-tab-upload-btn').className = tab === 'upload' ? 'btn-tab active' : 'btn-tab';
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
            const examples = await fetchJSON('/api/examples');
            if (examples.length === 0) {
                select.innerHTML = '<option value="">Nenhum exemplo XML+H5 disponível</option>';
                return;
            }
            select.innerHTML = examples.map(ex =>
                `<option value="${escapeHtml(ex.id)}" data-name="${escapeHtml(ex.name)}">${escapeHtml(ex.id)}</option>`
            ).join('');
            nameInput.value = examples[0].name;
        } catch (err) {
            select.innerHTML = '<option value="">Falha ao carregar exemplos</option>';
            errorEl.innerText = err.message;
        }
    }

    closeNewProjectModal() {
        document.getElementById('new-project-backdrop').classList.remove('open');
    }

    /** Despacha pra um dos dois fluxos de criação conforme a aba ativa do modal (ver setSourceTab). */
    async submitNewProject() {
        if (this.sourceTab === 'upload') return this.submitUploadProject();
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

        confirmBtn.disabled = true;
        confirmBtn.innerText = 'Criando…';
        errorEl.innerText = '';
        try {
            const project = await fetchJSON('/api/projects', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ example_id: exampleId, name }),
            });
            window.location.href = `project.html?project=${encodeURIComponent(project.id)}`;
        } catch (err) {
            errorEl.innerText = err.message;
            confirmBtn.disabled = false;
            confirmBtn.innerText = 'Criar Projeto';
        }
    }

    /** Aba "📤 Importar arquivo": submete via FormData (multipart) pro endpoint de upload, em vez
     * do corpo JSON usado pelo fluxo por exemplo -- ver run_server.py::api_upload_project. */
    async submitUploadProject() {
        const nameInput = document.getElementById('project-name-input');
        const errorEl = document.getElementById('new-project-error');
        const confirmBtn = document.getElementById('new-project-confirm-btn');
        const xmlInput = document.getElementById('upload-xml-input');
        const h5Input = document.getElementById('upload-h5-input');
        const amlInput = document.getElementById('upload-aml-input');
        const name = nameInput.value.trim();

        if (!name) { errorEl.innerText = 'Informe um nome para o projeto.'; return; }
        if (!xmlInput.files[0]) { errorEl.innerText = 'Selecione o arquivo XML.'; return; }
        if (!h5Input.files[0]) { errorEl.innerText = 'Selecione o arquivo H5.'; return; }

        const formData = new FormData();
        formData.append('name', name);
        formData.append('xml_file', xmlInput.files[0]);
        formData.append('h5_file', h5Input.files[0]);
        if (amlInput.files[0]) formData.append('aml_file', amlInput.files[0]);

        confirmBtn.disabled = true;
        confirmBtn.innerText = 'Criando…';
        errorEl.innerText = '';
        try {
            // Sem header Content-Type explícito de propósito -- o navegador define
            // multipart/form-data com o boundary correto sozinho a partir do FormData.
            const project = await fetchJSON('/api/projects/upload', { method: 'POST', body: formData });
            window.location.href = `project.html?project=${encodeURIComponent(project.id)}`;
        } catch (err) {
            errorEl.innerText = err.message;
            confirmBtn.disabled = false;
            confirmBtn.innerText = 'Criar Projeto';
        }
    }
}

function escapeHtml(str) {
    return String(str ?? '').replace(/[&<>"']/g, (c) => ({
        '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
    }[c]));
}

window.addEventListener('DOMContentLoaded', () => {
    window.dashboard = new Dashboard();
});
