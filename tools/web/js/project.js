import { initThemeToggle } from './ui/ThemeToggle.js';
import { confirmDialog, alertDialog } from './ui/ConfirmDialog.js';
import { statusPill, formatDate, escapeHtml, fetchJSON, modelSourceHtml, createRun as apiCreateRun, fetchLoadCases, DuplicateRunError } from './utils/runManagerFormat.js';

/**
 * project.js
 * Project detail page: source model info + run history table, "New Run" action, a
 * per-run live log tail (polls `GET .../stream` with an increasing byte offset -- see
 * `run_server.py::api_stream_run` -- while the run hasn't finished yet), and the workspace tabs
 * (Manager/Pre-processing/Post-processing, see `switchView()`).
 */

class ProjectPage {
    constructor(projectId) {
        this.projectId = projectId;
        this.project = null;
        // runId -> { offset, timer, text }. `text` accumulates the whole log (not just the
        // offset) because scheduleAutoRefresh() re-renders the whole table (innerHTML) every 2s
        // while there's a pending/running run, which would destroy the open log's <pre> --
        // keeping the text here lets renderRunRow() repopulate the box already with the
        // accumulated content.
        this.openLogs = new Map();
        this.refreshTimer = null;
        // State of the Manager/Pre/Post tabs (see switchView) -- which one is currently visible
        // and which run each iframe has loaded (so as not to needlessly reload when the same
        // tab/run is reopened, which would lose the viewer's camera/zoom position).
        this.currentView = 'manager';
        this.currentRun = undefined;
        // Load cases available for this project's .aml (see GET .../load-cases), fetched once
        // (not on every scheduleAutoRefresh() poll -- they never change while the page is open,
        // unlike run status). Empty until the first load() resolves; renderLoadCaseSelect() only
        // shows the selector once populated with >1 case.
        this.loadCases = [];
        this.bindEvents();
        this.load().then(() => this.applyViewFromUrl());
    }

    bindEvents() {
        initThemeToggle({
            currentTheme: 'dark',
            renderer3D: { scene: { background: { setHex() {} } } },
            render() {},
        });
        document.getElementById('refresh-btn').addEventListener('click', () => this.load());
        document.getElementById('new-run-btn').addEventListener('click', () => this.createRun());
        document.getElementById('delete-project-btn').addEventListener('click', () => this.deleteProject());

        document.getElementById('rename-project-btn').addEventListener('click', () => this.startRename());
        document.getElementById('rename-save-btn').addEventListener('click', () => this.submitRename());
        document.getElementById('rename-cancel-btn').addEventListener('click', () => this.cancelRename());
        document.getElementById('rename-project-input').addEventListener('keydown', (e) => {
            if (e.key === 'Enter') this.submitRename();
            if (e.key === 'Escape') this.cancelRename();
        });

        document.querySelectorAll('.btn-tab[data-view]').forEach(btn => {
            btn.addEventListener('click', () => this.switchView(btn.getAttribute('data-view')));
        });

        // Delegated (not rebound per element) because "View input"/"Input used"/"View
        // results" are recreated all the time in renderHeader()/renderActionsCell() -- a single
        // listener on the document covers any of them, present or future. Only intercepts a
        // plain click (in-page tab switch); Ctrl/Cmd/Shift/Alt-click and middle click
        // (button!==0) let the real <a href> open in a new browser tab, as usual.
        document.addEventListener('click', (e) => {
            const el = e.target.closest('[data-switch-view]');
            if (!el || e.defaultPrevented || e.button !== 0 || e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
            e.preventDefault();
            this.switchView(el.getAttribute('data-switch-view'), { run: el.getAttribute('data-run') || undefined });
        });

        window.addEventListener('popstate', () => this.applyViewFromUrl());
    }

    /** Reads `?view=&run=` from the current URL and switches tabs accordingly (without pushing
     * another history entry -- used on initial load and on the browser's Back/Forward button,
     * see popstate above). Without `?view=`, falls back to Pre-processing -- opening a project
     * jumps straight to the 3D model (what you actually came here to look at), not a bare run
     * list; links that want the Manager explicitly (e.g. the dashboard's "Recent runs" table)
     * already pass `&view=manager`. */
    applyViewFromUrl() {
        const params = new URLSearchParams(window.location.search);
        const view = params.get('view') || 'pre';
        const run = params.get('run') || undefined;
        this.switchView(view, { run, pushHistory: false });
    }

    /** Switches which tab is visible (Manager/Pre/Post). Pre/Post live in an <iframe> (a
     * separate HTML document -- see the plan/roadmap: preprocessor.html/posprocessor.html are
     * full-page apps with the same IDs as each other, merging them into this page's DOM would
     * cost a major rewrite of them). The iframe's `src` is only set the first time (or when the
     * requested run changes) -- reopening the same tab/run doesn't reload the viewer, preserving
     * camera/zoom.
     * @param {'manager'|'pre'|'post'} view
     * @param {{run?: string, pushHistory?: boolean}} opts `run`: run id (actually required for
     *   'post' -- without it, uses the most recent converged run; 'pre' without `run` shows the
     *   project's CURRENT input, outside any run). `pushHistory`: false when reacting to the URL
     *   (initial load/popstate), true on a genuine user tab switch (pushes history so the Back
     *   button works).
     */
    switchView(view, { run, pushHistory = true } = {}) {
        if (!['manager', 'pre', 'post'].includes(view)) view = 'manager';

        document.querySelectorAll('.workspace-view').forEach(el => { el.style.display = 'none'; });
        document.getElementById(`view-${view}`).style.display = '';
        document.querySelectorAll('.btn-tab[data-view]').forEach(btn => {
            btn.classList.toggle('active', btn.getAttribute('data-view') === view);
        });

        if (view === 'pre') {
            const url = run
                ? `preprocessor.html?project=${encodeURIComponent(this.projectId)}&run=${encodeURIComponent(run)}`
                : `preprocessor.html?project=${encodeURIComponent(this.projectId)}`;
            this._loadFrameIfNeeded('pre-frame', url);
        } else if (view === 'post') {
            const resolvedRun = run || this.mostRecentConvergedRunId();
            const frame = document.getElementById('post-frame');
            const emptyEl = document.getElementById('post-empty');
            if (resolvedRun) {
                const url = `posprocessor.html?project=${encodeURIComponent(this.projectId)}&run=${encodeURIComponent(resolvedRun)}`;
                this._loadFrameIfNeeded('post-frame', url);
                frame.style.display = '';
                emptyEl.style.display = 'none';
            } else {
                frame.style.display = 'none';
                emptyEl.style.display = '';
            }
            run = resolvedRun || undefined;
        }

        this.currentView = view;
        this.currentRun = run;

        if (pushHistory) {
            const params = new URLSearchParams(window.location.search);
            params.set('view', view);
            if (run) params.set('run', run); else params.delete('run');
            history.pushState({ view, run }, '', `${window.location.pathname}?${params.toString()}`);
        }
    }

    _loadFrameIfNeeded(frameId, url) {
        const frame = document.getElementById(frameId);
        if (frame.dataset.loadedUrl !== url) {
            frame.src = url;
            frame.dataset.loadedUrl = url;
        }
    }

    /** `this.project.runs` already comes most-recent-first (see list_runs) -- used as the Post
     * tab's default when it's opened without a specific run in mind (clicking the generic tab,
     * not a row's "View results" link). */
    mostRecentConvergedRunId() {
        const runs = (this.project && this.project.runs) || [];
        const converged = runs.find(r => r.status === 'converged');
        return converged ? converged.id : null;
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
        if (!this.loadCasesFetched) {
            this.loadCasesFetched = true;
            const data = await fetchLoadCases(this.projectId);
            this.loadCases = data.load_cases || [];
            this.renderLoadCaseSelect();
        }
        this.renderHeader();
        this.renderRuns();
        this.scheduleAutoRefresh();
    }

    /** Populates/toggles the "new run" load-case <select> -- only shown when the project's .aml
     * defines more than one %LOAD_CASE (backward-compat: a project with no .aml, or one with 0-1
     * cases, keeps the selector hidden and run creation behaves exactly as before this feature). */
    renderLoadCaseSelect() {
        const select = document.getElementById('load-case-select');
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

    renderHeader() {
        document.getElementById('project-name').innerText = this.project.name;
        document.getElementById('project-meta').innerText =
            `ID: ${this.project.id} · Criado em ${formatDate(this.project.created_at)}`;
        // A real href (ctrl/cmd-click or middle click still opens a new browser tab) +
        // data-switch-view (a plain click switches to the "Pre" tab within this page -- see the
        // delegated listener in bindEvents()/switchView()). Without `data-run`: shows the
        // project's CURRENT input (not a specific run's snapshot -- per a user decision, the
        // manager no longer exposes that per-row snapshot, only the current input here).
        const viewInputLink = document.getElementById('view-input-link');
        viewInputLink.href = `preprocessor.html?project=${encodeURIComponent(this.projectId)}`;
        viewInputLink.setAttribute('data-switch-view', 'pre');

        // xml_path/h5_path/aml_path are internal paths (projects/<id>/source/<real-name>, see
        // ProjectStore._store_source_files) -- the project is self-contained even if
        // trunk/exemplos/ stops being mounted. On screen, only shows the file's NAME (not the
        // internal "source/..." path -- irrelevant to the user) -- one case = one input file, so
        // the name alone already identifies where it came from; the real downloads are in the
        // links below. Shared with preprocessor_app.js's "Origem" tab -- see modelSourceHtml().
        const { linesHtml, downloadsHtml } = modelSourceHtml(this.projectId, this.project.source);
        document.getElementById('project-source').innerHTML = linesHtml;
        document.getElementById('project-downloads').innerHTML = downloadsHtml;
    }

    renderRuns() {
        const container = document.getElementById('runs-container');
        const runs = this.project.runs || [];
        if (runs.length === 0) {
            this.lastRunsKey = null;
            container.innerHTML = '<div class="empty-state">Nenhuma simulação ainda. Clique em "Nova Simulação" para disparar a primeira.</div>';
            return;
        }

        // The set/order of IDs only changes when a run is created (list_runs already comes
        // most-recent-first) -- in that case (rare, once per "New Run" click) it's worth
        // rebuilding the whole table. On scheduleAutoRefresh()'s other polls (every 2s, with the
        // same run set), only each row's content changes (status/timestamps) -- rebuilding
        // everything via innerHTML in that case is wasteful (needlessly redoes DOM/listeners) and
        // fragile (it was the cause of the "flickering" log bug before this round of changes).
        // That's why patchRuns() below only updates the cells that actually changed.
        const runsKey = runs.map(r => r.id).join(',');
        if (this.lastRunsKey === runsKey) {
            this.patchRuns(runs);
            return;
        }
        this.lastRunsKey = runsKey;
        this.lastRunsById = new Map(runs.map(r => [r.id, r]));

        // A subtle "= run-X" badge when the model_hash matches an earlier run in the same
        // project -- just a visual hint (see docs/roadmap.md, Axis 3b), not a restructuring of
        // the list. `runs` already comes most-recent-first (list_runs), so it walks backwards to
        // find the OLDEST run for each hash (the "original" the badge references). It doesn't
        // change for an existing run (model_hash is fixed at creation), so it only needs to be
        // recomputed here, in the full-rebuild path.
        const firstRunForHash = new Map();
        for (let i = runs.length - 1; i >= 0; i--) {
            const h = runs[i].model_hash;
            if (h && !firstRunForHash.has(h)) firstRunForHash.set(h, runs[i].id);
        }

        const rows = runs.map(run => this.renderRunRow(run, firstRunForHash)).join('');
        container.innerHTML = `
            <div class="runs-list">
                <table class="run-table">
                    <thead>
                        <tr>
                            <th>Simulação</th>
                            <th>Caso</th>
                            <th>Status</th>
                            <th>Criada</th>
                            <th>Início</th>
                            <th>Fim</th>
                            <th>Ações</th>
                        </tr>
                    </thead>
                    <tbody>${rows}</tbody>
                </table>
            </div>
        `;

        this.bindRowActions(container);

        // Keeps the scroll at the bottom for open logs that were just repopulated (see
        // renderRunRow() -- the text already comes filled in the HTML, it just needs to be
        // scrolled to the end).
        for (const runId of this.openLogs.keys()) {
            const box = document.getElementById(`log-box-${runId}`);
            if (box) box.scrollTop = box.scrollHeight;
        }
    }

    bindRowActions(scope) {
        scope.querySelectorAll('[data-toggle-log]').forEach(btn => {
            btn.addEventListener('click', () => this.toggleLog(btn.getAttribute('data-toggle-log')));
        });
        scope.querySelectorAll('[data-delete-run]').forEach(btn => {
            btn.addEventListener('click', () => this.deleteRun(btn.getAttribute('data-delete-run')));
        });
        scope.querySelectorAll('[data-abort-run]').forEach(btn => {
            btn.addEventListener('click', () => this.abortRun(btn.getAttribute('data-abort-run')));
        });
    }

    /** Updates only the rows whose status/timestamps actually changed since the last poll,
     * instead of rebuilding the whole table (see the comment on renderRuns()). Since Actions
     * depends on the status (the "View results" link only exists once converged), the Actions
     * cell is re-rendered along with it when the status changes -- which is why it needs to
     * rebind that cell's log button specifically (the rest of the table, including any open log,
     * isn't touched at all). */
    patchRuns(runs) {
        for (const run of runs) {
            const prev = this.lastRunsById.get(run.id);
            this.lastRunsById.set(run.id, run);
            if (prev && prev.status === run.status && prev.started_at === run.started_at && prev.finished_at === run.finished_at) {
                continue;
            }

            const statusCell = document.getElementById(`status-${run.id}`);
            if (statusCell) statusCell.innerHTML = statusPill(run.status);

            const startedCell = document.getElementById(`started-${run.id}`);
            if (startedCell) startedCell.innerText = formatDate(run.started_at);

            const finishedCell = document.getElementById(`finished-${run.id}`);
            if (finishedCell) finishedCell.innerText = formatDate(run.finished_at);

            const actionsCell = document.getElementById(`actions-${run.id}`);
            if (actionsCell) {
                actionsCell.innerHTML = this.renderActionsCell(run);
                this.bindRowActions(actionsCell);
            }
        }
    }

    renderActionsCell(run) {
        const resultsLink = run.status === 'converged'
            ? `<a class="link-btn" href="posprocessor.html?project=${encodeURIComponent(this.projectId)}&run=${encodeURIComponent(run.id)}" data-switch-view="post" data-run="${escapeHtml(run.id)}">Ver resultados</a>`
            : '';
        // Download the raw results HDF5 -- only exists for a converged run (same gate as
        // resultsLink above; a failed/aborted run never generates this file). Uses the run's own
        // case-identifiable filename (`run.results_filename`, e.g. "Exemplo_01a_Far_results.h5" --
        // known from the moment the run was created, see ProjectStore.create_run()), not a
        // generic name, so downloading several runs' results doesn't collide in the browser's
        // Downloads folder.
        const resultsDownload = run.status === 'converged'
            ? `<a class="link-btn" href="/api/projects/${encodeURIComponent(this.projectId)}/runs/${encodeURIComponent(run.id)}/results/${encodeURIComponent(run.results_filename)}?download=1">⬇ Resultados</a>`
            : '';
        const isActive = run.status === 'pending' || run.status === 'running';
        // Aborting only makes sense for a run still in progress; deleting is only allowed for a
        // run that's already finished (see risersim_projects.py::request_abort/delete_run -- the
        // two cases are mutually exclusive, they never appear together on the same row).
        const abortBtn = isActive
            ? `<button class="btn small danger" data-abort-run="${escapeHtml(run.id)}">⏹ Abortar</button>`
            : '';
        const deleteBtn = !isActive
            ? `<button class="btn small danger" data-delete-run="${escapeHtml(run.id)}">🗑</button>`
            : '';
        return `
            <button class="btn small secondary" data-toggle-log="${escapeHtml(run.id)}">📜 Log</button>
            ${resultsLink}
            ${resultsDownload}
            ${abortBtn}
            ${deleteBtn}
        `;
    }

    renderRunRow(run, firstRunForHash) {
        // If this log was open before the re-render (see scheduleAutoRefresh()), repopulates the
        // already-visible <pre> with the accumulated text instead of starting hidden/empty --
        // without this the log "flickers" and closes itself every 2s while the run is in
        // progress. Only relevant here (full rebuild) -- patchRuns() never touches a run's log
        // row.
        const logState = this.openLogs.get(run.id);
        const logDisplay = logState ? '' : 'none';
        const logText = logState ? escapeHtml(logState.text) : '';

        const dupOf = run.model_hash ? firstRunForHash.get(run.model_hash) : null;
        const dupBadge = (dupOf && dupOf !== run.id)
            ? ` <span class="badge-dup" title="mesmo model_hash da simulação ${escapeHtml(dupOf)}">= ${escapeHtml(dupOf)}</span>`
            : '';

        return `
            <tr>
                <td class="run-id-cell mono">${escapeHtml(run.id)}${dupBadge}</td>
                <td>${escapeHtml(run.load_case_name || '—')}</td>
                <td id="status-${escapeHtml(run.id)}">${statusPill(run.status)}</td>
                <td class="mono">${formatDate(run.created_at)}</td>
                <td id="started-${escapeHtml(run.id)}" class="mono">${formatDate(run.started_at)}</td>
                <td id="finished-${escapeHtml(run.id)}" class="mono">${formatDate(run.finished_at)}</td>
                <td id="actions-${escapeHtml(run.id)}">${this.renderActionsCell(run)}</td>
            </tr>
            <tr class="log-row" id="log-row-${escapeHtml(run.id)}" style="display:${logDisplay};">
                <td colspan="7"><pre class="log-box" id="log-box-${escapeHtml(run.id)}">${logText}</pre></td>
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
        const state = { offset: 0, text: '' };
        this.openLogs.set(runId, state);
        const poll = async () => {
            if (!this.openLogs.has(runId)) return;
            try {
                const data = await fetchJSON(`/api/projects/${encodeURIComponent(this.projectId)}/runs/${encodeURIComponent(runId)}/stream?offset=${state.offset}`);
                state.offset = data.next_offset;
                if (data.content) {
                    // Accumulates into `state.text` (not just the DOM) -- renderRunRow() uses
                    // this to repopulate the box if the table gets re-rendered by
                    // scheduleAutoRefresh() while the log is open (see the comment in the
                    // constructor).
                    state.text += data.content;
                    const box = document.getElementById(`log-box-${runId}`);
                    if (box) {
                        box.textContent += data.content;
                        box.scrollTop = box.scrollHeight;
                    }
                }
                const statusCell = document.getElementById(`status-${runId}`);
                if (statusCell) statusCell.innerHTML = statusPill(data.status);
                if (!data.done && this.openLogs.has(runId)) {
                    state.timer = setTimeout(poll, 1000);
                } else {
                    this.openLogs.delete(runId);
                }
            } catch (err) {
                console.error(`Falha ao ler log da simulação ${runId}:`, err);
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
        const select = document.getElementById('load-case-select');
        const load_case_id = (select && select.style.display !== 'none' && select.value)
            ? parseInt(select.value, 10) : null;
        try {
            await apiCreateRun(this.projectId, { force, load_case_id });
            await this.load();
        } catch (err) {
            if (err instanceof DuplicateRunError) {
                // Duplicate run (same model_hash as an already-finished run) -- asks before
                // needlessly spending the serial queue running it again (see
                // run_server.py::api_create_run).
                const ok = await confirmDialog({ title: 'Simulação duplicada', message: `${err.message} Rodar mesmo assim?`, confirmLabel: 'Rodar mesmo assim', danger: false });
                if (ok) await this.createRun(true);
                return;
            }
            await alertDialog({ title: 'Falha ao criar simulação', message: err.message });
        } finally {
            btn.disabled = false;
            btn.innerText = '▶ Nova Simulação';
        }
    }

    async abortRun(runId) {
        const ok = await confirmDialog({
            title: 'Abortar simulação',
            message: `Abortar a simulação ${runId}? O solver será interrompido no meio da execução -- não há como retomar de onde parou.`,
            confirmLabel: 'Abortar',
        });
        if (!ok) return;
        try {
            const res = await fetch(`/api/projects/${encodeURIComponent(this.projectId)}/runs/${encodeURIComponent(runId)}/abort`, { method: 'POST' });
            if (!res.ok) {
                let detail = '';
                try { detail = (await res.json()).error || ''; } catch (e) { /* ignore */ }
                throw new Error(detail || `HTTP ${res.status}`);
            }
            // The worker is what actually changes the status to "aborted" (see run_worker.py) --
            // it can take up to ~ABORT_POLL_INTERVAL_S to react, so this reload might still show
            // pending/running for a moment; scheduleAutoRefresh() picks up the real update
            // shortly after.
            await this.load();
        } catch (err) {
            await alertDialog({ title: 'Falha ao abortar simulação', message: err.message });
        }
    }

    async deleteRun(runId) {
        const ok = await confirmDialog({
            title: 'Apagar simulação',
            message: `Apagar a simulação ${runId}? Isso remove o log e os resultados dela permanentemente.`,
            confirmLabel: 'Apagar',
        });
        if (!ok) return;
        try {
            const res = await fetch(`/api/projects/${encodeURIComponent(this.projectId)}/runs/${encodeURIComponent(runId)}`, { method: 'DELETE' });
            if (!res.ok && res.status !== 204) {
                let detail = '';
                try { detail = (await res.json()).error || ''; } catch (e) { /* ignore */ }
                throw new Error(detail || `HTTP ${res.status}`);
            }
            this.stopLogPolling(runId);
            await this.load();
        } catch (err) {
            await alertDialog({ title: 'Falha ao apagar simulação', message: err.message });
        }
    }

    startRename() {
        document.getElementById('project-name-display').style.display = 'none';
        const editEl = document.getElementById('project-name-edit');
        editEl.style.display = '';
        const input = document.getElementById('rename-project-input');
        input.value = this.project.name;
        input.focus();
        input.select();
    }

    cancelRename() {
        document.getElementById('project-name-edit').style.display = 'none';
        document.getElementById('project-name-display').style.display = '';
    }

    async submitRename() {
        const input = document.getElementById('rename-project-input');
        const newName = input.value.trim();
        if (!newName) { input.focus(); return; }
        if (newName === this.project.name) { this.cancelRename(); return; }
        try {
            // PATCH returns the raw project.json (without the `runs` list, which only
            // api_get_project attaches) -- merges just the `name` into the already-loaded
            // `this.project` instead of replacing the whole object, so as not to lose `runs`
            // until the next full load().
            const updated = await fetchJSON(`/api/projects/${encodeURIComponent(this.projectId)}`, {
                method: 'PATCH',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ name: newName }),
            });
            this.project = { ...this.project, name: updated.name };
            this.cancelRename();
            this.renderHeader();
        } catch (err) {
            await alertDialog({ title: 'Falha ao renomear projeto', message: err.message });
        }
    }

    async deleteProject() {
        const ok = await confirmDialog({
            title: 'Apagar projeto',
            message: `Apagar o projeto "${this.project.name}" e TODAS as suas simulações? Essa ação não pode ser desfeita.`,
            confirmLabel: 'Apagar tudo',
        });
        if (!ok) return;
        const btn = document.getElementById('delete-project-btn');
        btn.disabled = true;
        try {
            const res = await fetch(`/api/projects/${encodeURIComponent(this.projectId)}`, { method: 'DELETE' });
            if (!res.ok && res.status !== 204) {
                let detail = '';
                try { detail = (await res.json()).error || ''; } catch (e) { /* ignore */ }
                throw new Error(detail || `HTTP ${res.status}`);
            }
            window.location.href = 'dashboard.html';
        } catch (err) {
            await alertDialog({ title: 'Falha ao apagar projeto', message: err.message });
            btn.disabled = false;
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
