import { initThemeToggle } from './ui/ThemeToggle.js';

/**
 * project.js
 * Project detail page: source model info + run history table, "Nova Rodada" action, and a
 * per-run live log tail (polls GET .../stream with an increasing byte offset -- see
 * run_server.py's api_stream_run -- while the run hasn't finished).
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
    try { return new Date(iso).toLocaleString('pt-BR'); } catch (e) { return iso; }
}

function escapeHtml(str) {
    return String(str ?? '').replace(/[&<>"']/g, (c) => ({
        '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
    }[c]));
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

class ProjectPage {
    constructor(projectId) {
        this.projectId = projectId;
        this.project = null;
        this.openLogs = new Map(); // runId -> { offset, timer }
        this.refreshTimer = null;
        this.bindEvents();
        this.load();
    }

    bindEvents() {
        initThemeToggle({
            currentTheme: 'dark',
            renderer3D: { scene: { background: { setHex() {} } } },
            render() {},
        });
        document.getElementById('refresh-btn').addEventListener('click', () => this.load());
        document.getElementById('new-run-btn').addEventListener('click', () => this.createRun());
    }

    async load() {
        try {
            this.project = await fetchJSON(`/api/projects/${encodeURIComponent(this.projectId)}`);
        } catch (err) {
            document.getElementById('project-name').innerText = 'Projeto não encontrado';
            document.getElementById('project-meta').innerText = err.message;
            document.getElementById('runs-container').innerHTML = '';
            return;
        }
        this.renderHeader();
        this.renderRuns();
        this.scheduleAutoRefresh();
    }

    renderHeader() {
        document.getElementById('project-name').innerText = this.project.name;
        document.getElementById('project-meta').innerText =
            `ID: ${this.project.id} · Criado em ${formatDate(this.project.created_at)}`;
        document.getElementById('view-input-link').href =
            `preprocessor.html?project=${encodeURIComponent(this.projectId)}`;

        // xml_path/h5_path/aml_path agora são caminhos internos (projects/<id>/source/model.*,
        // ver ProjectStore._store_source_files) -- o projeto é autocontido mesmo se
        // trunk/exemplos/ deixar de estar montado.
        const src = this.project.source || {};
        const lines = [];
        if (src.example_id) lines.push(`<div class="kv-line"><span class="k">Exemplo:</span>${escapeHtml(src.example_id)}</div>`);
        if (src.kind === 'upload') lines.push(`<div class="kv-line"><span class="k">Origem:</span>📤 Upload manual</div>`);
        if (src.xml_path) lines.push(`<div class="kv-line"><span class="k">XML:</span>${escapeHtml(src.xml_path)}</div>`);
        if (src.h5_path) lines.push(`<div class="kv-line"><span class="k">H5:</span>${escapeHtml(src.h5_path)}</div>`);
        if (src.aml_path) lines.push(`<div class="kv-line"><span class="k">AML:</span>${escapeHtml(src.aml_path)}</div>`);
        document.getElementById('project-source').innerHTML = lines.join('') || '<div class="kv-line">—</div>';
    }

    renderRuns() {
        const container = document.getElementById('runs-container');
        const runs = this.project.runs || [];
        if (runs.length === 0) {
            container.innerHTML = '<div class="empty-state">Nenhuma rodada ainda. Clique em "Nova Rodada" para disparar a primeira.</div>';
            return;
        }

        // Badge discreta "= run-X" quando o model_hash bate com uma rodada anterior do mesmo
        // projeto -- só uma pista visual (ver docs/roadmap.md Eixo 3b), não uma reestruturação da
        // lista. `runs` já vem mais recente primeiro (list_runs), então percorre de trás pra
        // frente pra achar a rodada mais ANTIGA de cada hash (a "original" que a badge referencia).
        const firstRunForHash = new Map();
        for (let i = runs.length - 1; i >= 0; i--) {
            const h = runs[i].model_hash;
            if (h && !firstRunForHash.has(h)) firstRunForHash.set(h, runs[i].id);
        }

        const rows = runs.map(run => this.renderRunRow(run, firstRunForHash)).join('');
        container.innerHTML = `
            <table class="run-table">
                <thead>
                    <tr>
                        <th>Rodada</th>
                        <th>Status</th>
                        <th>Criada</th>
                        <th>Início</th>
                        <th>Fim</th>
                        <th>Ações</th>
                    </tr>
                </thead>
                <tbody>${rows}</tbody>
            </table>
        `;

        container.querySelectorAll('[data-toggle-log]').forEach(btn => {
            btn.addEventListener('click', () => this.toggleLog(btn.getAttribute('data-toggle-log')));
        });
    }

    renderRunRow(run, firstRunForHash) {
        const resultsLink = run.status === 'converged'
            ? `<a class="link-btn" href="posprocessor.html?project=${encodeURIComponent(this.projectId)}&run=${encodeURIComponent(run.id)}" target="_blank">Ver resultados</a>`
            : '';
        // "Entrada usada" (snapshot congelado dessa rodada) fica sempre disponível, diferente de
        // "Ver resultados" (só existe pra rodadas convergidas) -- input_simulation.json é
        // copiado em create_run() antes da rodada sequer começar a rodar.
        const inputLink = `<a class="link-btn" href="preprocessor.html?project=${encodeURIComponent(this.projectId)}&run=${encodeURIComponent(run.id)}" target="_blank">🔍 Entrada usada</a>`;

        const dupOf = run.model_hash ? firstRunForHash.get(run.model_hash) : null;
        const dupBadge = (dupOf && dupOf !== run.id)
            ? ` <span class="badge-dup" title="mesmo model_hash da rodada ${escapeHtml(dupOf)}">= ${escapeHtml(dupOf)}</span>`
            : '';

        return `
            <tr>
                <td>${escapeHtml(run.id)}${dupBadge}</td>
                <td id="status-${escapeHtml(run.id)}">${statusBadge(run.status)}</td>
                <td>${formatDate(run.created_at)}</td>
                <td>${formatDate(run.started_at)}</td>
                <td>${formatDate(run.finished_at)}</td>
                <td>
                    <button class="btn small secondary" data-toggle-log="${escapeHtml(run.id)}">📜 Log</button>
                    ${resultsLink}
                    ${inputLink}
                </td>
            </tr>
            <tr class="log-row" id="log-row-${escapeHtml(run.id)}" style="display:none;">
                <td colspan="6"><pre class="log-box" id="log-box-${escapeHtml(run.id)}"></pre></td>
            </tr>
        `;
    }

    toggleLog(runId) {
        const row = document.getElementById(`log-row-${runId}`);
        const isOpen = row.style.display !== 'none';
        if (isOpen) {
            row.style.display = 'none';
            this.stopLogPolling(runId);
        } else {
            row.style.display = '';
            this.startLogPolling(runId);
        }
    }

    startLogPolling(runId) {
        if (this.openLogs.has(runId)) return;
        const state = { offset: 0 };
        this.openLogs.set(runId, state);
        const poll = async () => {
            if (!this.openLogs.has(runId)) return;
            try {
                const data = await fetchJSON(`/api/projects/${encodeURIComponent(this.projectId)}/runs/${encodeURIComponent(runId)}/stream?offset=${state.offset}`);
                state.offset = data.next_offset;
                if (data.content) {
                    const box = document.getElementById(`log-box-${runId}`);
                    if (box) {
                        box.textContent += data.content;
                        box.scrollTop = box.scrollHeight;
                    }
                }
                const statusCell = document.getElementById(`status-${runId}`);
                if (statusCell) statusCell.innerHTML = statusBadge(data.status);
                if (!data.done && this.openLogs.has(runId)) {
                    state.timer = setTimeout(poll, 1000);
                } else {
                    this.openLogs.delete(runId);
                }
            } catch (err) {
                console.error(`Falha ao ler log da rodada ${runId}:`, err);
                if (this.openLogs.has(runId)) state.timer = setTimeout(poll, 2000);
            }
        };
        poll();
    }

    stopLogPolling(runId) {
        const state = this.openLogs.get(runId);
        if (state && state.timer) clearTimeout(state.timer);
        this.openLogs.delete(runId);
    }

    scheduleAutoRefresh() {
        if (this.refreshTimer) clearTimeout(this.refreshTimer);
        const runs = this.project.runs || [];
        const hasPending = runs.some(r => r.status === 'pending' || r.status === 'running');
        if (hasPending) {
            this.refreshTimer = setTimeout(() => this.load(), 2000);
        }
    }

    async createRun(force = false) {
        const btn = document.getElementById('new-run-btn');
        btn.disabled = true;
        btn.innerText = 'Criando…';
        try {
            const res = await fetch(`/api/projects/${encodeURIComponent(this.projectId)}/runs`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ force }),
            });
            if (res.status === 409) {
                // Rodada duplicada (mesmo model_hash de uma rodada já terminada) -- pergunta antes
                // de gastar a fila serial rodando de novo à toa (ver run_server.py::api_create_run).
                const body = await res.json().catch(() => ({}));
                const msg = `Já existe uma rodada terminada (${body.run_id || '?'}, status: ${body.status || '?'}) ` +
                            `com o mesmo modelo. Rodar mesmo assim?`;
                if (window.confirm(msg)) {
                    await this.createRun(true);
                    return;
                }
                return;
            }
            if (!res.ok) {
                let detail = '';
                try { detail = (await res.json()).error || ''; } catch (e) { /* ignore */ }
                throw new Error(detail || `HTTP ${res.status}`);
            }
            await this.load();
        } catch (err) {
            alert(`Falha ao criar rodada: ${err.message}`);
        } finally {
            btn.disabled = false;
            btn.innerText = '▶ Nova Rodada';
        }
    }
}

window.addEventListener('DOMContentLoaded', () => {
    const params = new URLSearchParams(window.location.search);
    const projectId = params.get('project');
    if (!projectId) {
        document.getElementById('project-name').innerText = 'Nenhum projeto informado';
        document.getElementById('project-meta').innerText = 'Use ?project=<id> na URL, ou volte ao painel.';
        document.getElementById('runs-container').innerHTML = '';
        return;
    }
    window.projectPage = new ProjectPage(projectId);
});
