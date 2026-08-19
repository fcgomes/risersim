import { switchTab } from './ui/TabPanel.js';
import { initThemeToggle } from './ui/ThemeToggle.js';
import { alertDialog } from './ui/ConfirmDialog.js';
import { escapeHtml, fetchJSON } from './utils/runManagerFormat.js';
import { Riser3DRenderer } from './renderers/Riser3DRenderer.js';
import { CameraViewController } from './renderers/CameraViewController.js';
import { ZoomWindowController } from './ui/ZoomWindowController.js';
import { bindCameraToolbar } from './ui/CameraToolbar.js';
import { ColorMapService } from './services/ColorMapService.js';
import { createSpreadsheetTable } from './ui/SpreadsheetTable.js';

/**
 * editor_app.js
 * The "JSON de interface" form editor (docs/roadmap.md, Eixo 3a) -- edits a project's
 * `source/interface.json` (soils/materials/currents/waves/lines/load_cases/analyses, all
 * cross-referenced by integer id) and saves it back via `PUT /api/projects/<id>/interface`, which
 * recompiles `input_simulation.json` server-side (`risersim_runner.py::build_config_from_interface()`).
 * Only projects created via the dashboard's "✏️ Em branco" tab (`source.interface_editable`) are
 * editable here -- an AML/XML-derived project's interface.json, when present, is a read-only
 * compiler input (see `risersim_projects.py::create_project()`'s docstring).
 *
 * Layout: the same 3-region skeleton as preprocessor.html/posprocessor.html (css/shared.css --
 * camera sidebar | 3D canvas | tabbed panel), mirrored left-right -- the tabbed panel (here, the
 * editable forms) is on the LEFT and the always-on 3D canvas is on the RIGHT. Requested explicitly
 * ("a cara do pós-processador, só que editável"): edit the model with a live view of the whole
 * thing beside you, not a separate preview tucked into one tab. The canvas shows the FULL model
 * (every line, via `POST /api/interface/preview` -> `build_config_from_interface()`, the same
 * compiler a real run uses) regardless of which left-hand tab is active -- editing a material or a
 * soil can change the geometry (weight affects the catenary solve) just as much as editing a line,
 * so the preview isn't scoped to the "Linhas" tab.
 *
 * `this.model` IS the live interface JSON object -- every render*() function below both displays
 * and binds inputs directly against it (no separate "form state" copy), so a save() just PUTs
 * `this.model` as-is. Table-shaped catalogs (soils/materials/currents/waves/load_cases/analyses,
 * plus a line's segments) all use `ui/SpreadsheetTable.js` -- a real spreadsheet grid (paste from
 * Excel/Sheets, per-column units, smooth with large row counts) instead of a plain HTML table --
 * see that module's docstring for why. Each catalog table is created ONCE (`this.tables.<name>`)
 * and updated in place from then on; two exceptions: `this.tables.segments`, rebuilt every time
 * the active line changes since it's bound to a different underlying array (that line's own
 * `segments`) each time -- see renderSegments() -- and `this.tables.materials`, which is 4 tables
 * (one per `finite_element_type` sub-tab) all sharing the SAME `this.model.materials` array via
 * `SpreadsheetTable.js`'s `filter` option, not 4 separate arrays -- see renderMaterials().
 */

const TABS = {
    site: 'panel-site', soils: 'panel-soils', materials: 'panel-materials',
    curves: 'panel-curves', currents: 'panel-currents', waves: 'panel-waves',
    buoys: 'panel-buoys', lines: 'panel-lines',
    loadcases: 'panel-loadcases', analyses: 'panel-analyses',
};

/** Sub-tabs of the "Materiais" tab -- one per `finite_element_type`, each showing only the fields
 * that type actually uses (see renderMaterials()'s docstring for why). `panelId` feeds
 * `switchTab()` (ui/TabPanel.js) directly -- same {key: panelId} shape as `TABS` above, just a
 * separate map so the two tab bars don't interfere (switchTab() only touches the keys it's given). */
const MATERIAL_SUBTABS = {
    beam: 'material-subtab-beam', truss: 'material-subtab-truss',
    scalar: 'material-subtab-scalar', winch: 'material-subtab-winch',
};

function nextId(items) {
    return items.reduce((max, x) => Math.max(max, x.id || 0), 0) + 1;
}

/**
 * Drag handle between the data panel and the 3D canvas -- same idea as `ui/PanelResizer.js`
 * (shared by preprocessor.html/posprocessor.html), but with the drag direction flipped: this page
 * puts the data panel on the LEFT of the resizer (those two put it on the right), so growing it
 * means dragging the handle right (`+dx`), not left (`-dx`).
 * @param {() => void} [onResize]
 */
function initReversedPanelResizer(onResize) {
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
        const newWidth = Math.min(window.innerWidth * 0.75, Math.max(320, startWidth + dx));
        dataPanel.style.width = `${newWidth}px`;
        if (onResize) onResize();
    });

    window.addEventListener('mouseup', () => {
        if (isDragging) {
            isDragging = false;
            resizer.classList.remove('dragging');
            document.body.style.cursor = '';
            if (onResize) onResize();
        }
    });
}

class EditorApp {
    constructor(projectId) {
        this.projectId = projectId;
        this.model = null;
        this.activeLineId = null;
        this.currentTheme = 'dark';
        // Persistent 3D preview state (always-on canvas, see the module docstring) -- `lastStep`/
        // `lastEnvBounds` are what the camera-toolbar's view buttons (ISO/XY/../zoom-fit) frame
        // against, kept current by both the instant placeholder (renderPreviewFrame()) and the
        // real debounced compile (updatePreviewNow()). `_hasFramedReal` gates auto-reframing to
        // "once per structural change" (see reframeCamera()'s docstring) so the camera doesn't
        // jump around while the user is mid-edit on an unrelated field.
        this.lastStep = null;
        this.lastEnvBounds = [null, null];
        this._hasFramedReal = false;
        this._previewDebounceTimer = null;

        // SpreadsheetTable.js handles per-tab, per-table -- `this.tables[name]` holds the live
        // handle (or null before first render). Every catalog is created ONCE and updated in
        // place from then on, except `segments`, rebuilt each time the active line changes (see
        // renderSegments()). `materialUnits` is the display unit currently picked per unit-bearing
        // Materiais column, independent of the model itself (which always stays in SI).
        this.tables = {
            soils: null,
            // One handle per Materiais sub-tab (Vigas/Cabos/Juntas/Guinchos), all sharing the SAME
            // `this.model.materials` array via SpreadsheetTable.js's `filter` option -- see
            // renderMaterials()'s docstring.
            materials: { beam: null, truss: null, scalar: null, winch: null },
            curves: null, currents: null, waves: null, buoys: null, loadcases: null, analyses: null, segments: null,
        };
        this.materialUnits = {
            diameter_external_m: 'm', diameter_internal_m: 'm',
            weight_dry_kNm: 'kN/m', weight_wet_kNm: 'kN/m',
            stiffness_axial_kN: 'kN', initial_tension_kN: 'kN',
        };

        this.initViewport();
        this.bindEvents();
        this.load();
    }

    /** Sets up the always-on 3D canvas + its camera/zoom sidebar -- same pieces
     * preprocessor_app.js/app.js use, just wired to this page's own preview data instead of a
     * loaded input/results file. */
    initViewport() {
        // When embedded in project.html's "Editar" tab <iframe> (the normal case -- same check
        // as preprocessor_app.js::initUI()/app.js::initUI()), the parent page already shows a
        // header with the brand mark, title, and theme toggle; showing this page's own copy too
        // doubled it up on screen (a duplicate title bar with its own separate theme-toggle
        // button). Hidden via inline style (not a body class) so it can't collide with
        // ThemeToggle.js's `body.className = '...'` full overwrites.
        if (window.self !== window.top) {
            document.getElementById('header').style.display = 'none';
            document.getElementById('main-layout').style.height = '100vh';
        }

        const canvas = document.getElementById('three-canvas');
        this.renderer3D = new Riser3DRenderer(canvas);
        this.cameraController = new CameraViewController(this.renderer3D.camera, this.renderer3D.controls, this.renderer3D);
        this.zoomWindow = new ZoomWindowController(this.cameraController, this.renderer3D);
        bindCameraToolbar(this.cameraController, this.zoomWindow, () => this.lastStep, () => this.lastEnvBounds);
        initReversedPanelResizer(() => this.renderer3D.onWindowResize());
    }

    bindEvents() {
        // ThemeToggle.js wants `currentTheme`/`renderer3D`/`render()` on the SAME object it
        // mutates `currentTheme` on -- unlike dashboard.js/project.js (no 3D at all, a throwaway
        // shim was enough), this page has a real 3D scene whose colors need to follow the toggle,
        // so pass `this` itself (see render() below), same as preprocessor_app.js/app.js do.
        initThemeToggle(this);

        document.querySelectorAll('.btn-tab[data-tab]').forEach(btn => {
            btn.addEventListener('click', () => switchTab(TABS, btn.getAttribute('data-tab')));
        });
        document.querySelectorAll('.btn-tab[data-material-subtab]').forEach(btn => {
            btn.addEventListener('click', () => switchTab(MATERIAL_SUBTABS, btn.getAttribute('data-material-subtab')));
        });

        document.getElementById('save-draft-btn').addEventListener('click', () => this.save(false));
        document.getElementById('save-and-go-btn').addEventListener('click', () => this.save(true));

        document.getElementById('add-soil-btn').addEventListener('click', () => this.addSoil());
        document.getElementById('add-material-beam-btn').addEventListener('click', () => this.addMaterialOfType('beam'));
        document.getElementById('add-material-truss-btn').addEventListener('click', () => this.addMaterialOfType('truss'));
        document.getElementById('add-material-scalar-btn').addEventListener('click', () => this.addMaterialOfType('scalar'));
        document.getElementById('add-material-winch-btn').addEventListener('click', () => this.addMaterialOfType('winch'));
        document.getElementById('add-curve-btn').addEventListener('click', () => this.addCurve());
        document.getElementById('add-current-btn').addEventListener('click', () => this.addCurrent());
        document.getElementById('add-wave-btn').addEventListener('click', () => this.addWave());
        document.getElementById('add-buoy-btn').addEventListener('click', () => this.addBuoy());
        document.getElementById('add-loadcase-btn').addEventListener('click', () => this.addLoadCase());
        document.getElementById('add-analysis-btn').addEventListener('click', () => this.addAnalysis());

        document.getElementById('add-line-btn').addEventListener('click', () => this.addLine());
        document.getElementById('add-segment-btn').addEventListener('click', () => this.addSegment());
    }

    async load() {
        let project;
        try {
            project = await fetchJSON(`/api/projects/${encodeURIComponent(this.projectId)}`);
        } catch (err) {
            document.getElementById('editor-error').innerText = `Falha ao carregar projeto: ${err.message}`;
            return;
        }
        document.getElementById('editor-project-name').innerText = project.name;
        if (!(project.source || {}).interface_editable) {
            document.getElementById('editor-error').innerText =
                'Este projeto não foi criado como "em branco" -- a JSON de interface dele (se existir) não é editável aqui.';
            document.querySelectorAll('.editor-toolbar button').forEach(b => { b.disabled = true; });
            return;
        }
        try {
            this.model = await fetchJSON(`/api/projects/${encodeURIComponent(this.projectId)}/interface`);
        } catch (err) {
            document.getElementById('editor-error').innerText = `Falha ao carregar JSON de interface: ${err.message}`;
            return;
        }
        // Backfill for projects/interface JSONs created before the element-type fields existed
        // (curves/buoys catalogs, per-material type/type-specific fields) -- same defensive
        // pattern as the segment `id` backfill in renderSegments(), so an older project shows the
        // real defaults the compiler already assumes instead of blank/undefined cells.
        this.model.curves = this.model.curves || [];
        this.model.buoys = this.model.buoys || [];
        this.model.materials.forEach(m => {
            m.finite_element_type = m.finite_element_type ?? 'beam';
            m.initial_tension_kN = m.initial_tension_kN ?? 0.0;
            m.winch_payout_curve_id = m.winch_payout_curve_id ?? null;
            m.scalar_stiffness_translational_kNm = m.scalar_stiffness_translational_kNm ?? 0.0;
            m.scalar_stiffness_torsional_kNm_per_rad = m.scalar_stiffness_torsional_kNm_per_rad ?? 0.0;
            m.scalar_stiffness_bending_kNm_per_rad = m.scalar_stiffness_bending_kNm_per_rad ?? 0.0;
        });
        this.model.lines.forEach(l => l.segments.forEach(s => { s.buoy_id = s.buoy_id ?? null; }));

        this.activeLineId = (this.model.lines[0] || {}).id ?? null;
        this.renderAll();

        // Instant placeholder + the first real compile, framed once it lands (see
        // reframeCamera()'s docstring) -- same "context now, real mesh a moment later" sequence
        // as any other edit, just triggered by the initial load instead of a field change.
        this.renderPreviewFrame();
        this.schedulePreviewUpdate();

        // `?tab=<key>` (one of TABS' keys) opens straight to that tab -- e.g. a "criar linha"
        // shortcut elsewhere in the app could deep-link to `editor.html?project=<id>&tab=lines`.
        const initialTab = new URLSearchParams(window.location.search).get('tab');
        if (initialTab && TABS[initialTab]) switchTab(TABS, initialTab);
    }

    renderAll() {
        this.renderSite();
        this.renderSoils();
        this.renderMaterials();
        this.renderCurves();
        this.renderCurrents();
        this.renderWaves();
        this.renderBuoys();
        this.renderLines();
        this.renderLoadCases();
        this.renderAnalyses();
    }

    // ---- Site (single object, not a table) ----
    renderSite() {
        const g = this.model.global;
        const fields = [
            ['water_depth_m', 'Lâmina d\'água (m)'],
            ['gravity_ms2', 'Gravidade (m/s²)'],
            ['water_specific_weight_kNm3', 'Peso esp. da água (kN/m³)'],
            ['steel_specific_weight_kNm3', 'Peso esp. do aço (kN/m³)'],
        ];
        const container = document.getElementById('site-fields');
        container.innerHTML = fields.map(([key, label]) => `
            <div>
                <label for="site-${key}">${escapeHtml(label)}</label>
                <input type="number" step="any" id="site-${key}" value="${escapeHtml(g[key] ?? '')}">
            </div>
        `).join('');
        fields.forEach(([key]) => {
            document.getElementById(`site-${key}`).addEventListener('input', (e) => {
                g[key] = e.target.value === '' ? null : parseFloat(e.target.value);
                // water_depth_m directly changes the water-surface Z used by the bounding box/
                // env planes -- reflect it instantly, same as a line's top position would.
                this.renderPreviewFrame();
                this.schedulePreviewUpdate();
            });
        });
    }

    // ---- Soils ----
    renderSoils() {
        if (this.tables.soils) return;
        this.tables.soils = createSpreadsheetTable(
            '#soils-table', this.model.soils,
            [
                { key: 'name', label: 'Nome', type: 'text' },
                { key: 'model', label: 'Modelo', type: 'select', options: () => [{ value: 'uncoupled', label: 'uncoupled' }, { value: 'coupled', label: 'coupled' }] },
                { key: 'friction_axial', label: 'Atrito axial', type: 'number' },
                { key: 'friction_lateral', label: 'Atrito lateral', type: 'number' },
                { key: 'deflection_axial_m', label: 'Defl. axial (m)', type: 'number' },
                { key: 'deflection_lateral_m', label: 'Defl. lateral (m)', type: 'number' },
                { key: 'spring_stiffness_kNm', label: 'Rigidez mola (kN/m)', type: 'number' },
            ],
            { onDelete: () => this.onSoilsChanged(), onChange: () => this.onSoilsChanged() },
        );
    }
    /** A soil's name/id is shown in the Segmentos table's `soil_id` dropdown -- refresh it so a
     * rename or delete shows up there too, not just here. */
    onSoilsChanged() {
        if (this.tables.segments) this.tables.segments.refresh();
        this.schedulePreviewUpdate();
    }
    addSoil() {
        this.tables.soils.addRow({
            id: nextId(this.model.soils), name: `Solo_${this.model.soils.length + 1}`, model: 'uncoupled',
            friction_axial: 0.5, friction_lateral: 0.5, deflection_axial_m: 0.03, deflection_lateral_m: 0.2,
            spring_stiffness_kNm: 800.0,
        });
        this.onSoilsChanged();
    }

    // ---- Materials ----
    /**
     * One spreadsheet grid PER `finite_element_type` (Vigas/Cabos/Juntas/Guinchos sub-tabs,
     * `MATERIAL_SUBTABS`), each showing only the fields that type actually reads (see
     * `docs/roadmap.md`, Eixo 3a) -- e.g. a scalar/flexjoint has no diameter/weight/EA, a truss has
     * no bending/torsion. All 4 tables share the SAME underlying `this.model.materials` array (the
     * "Tipo" field decides which sub-tab a material shows up in -- there's no separate per-type
     * catalog, matching real ANFLEX where `%MATERIAL.FINITE_ELEMENT` is a field ON the material,
     * not its own entity); `SpreadsheetTable.js`'s `filter` option hides non-matching rows per
     * table WITHOUT slicing the array, so add/delete/id-generation all keep operating on the real
     * catalog no matter which sub-tab they're clicked from.
     *
     * Rayleigh damping (T1/T2/ξ1/ξ2) is the one exception to "each sub-tab only shows its own
     * fields" -- it's not actually per-element-type at all, it's a single GLOBAL value the
     * compiler derives from whichever material happens to be the FIRST segment of the FIRST line
     * (`risersim_runner.py`'s `build_config_from_interface()`), regardless of that material's
     * type. Since any type could end up being that material, all 4 sub-tabs show these columns
     * (see the panel's hint text in editor.html for the explanation shown to the user).
     */
    renderMaterials() {
        if (this.tables.materials.beam) return;
        const unitCol = (type, key, label, group, decimals) => ({
            key, label, type: 'unit', group, decimals,
            getUnit: () => this.materialUnits[key],
            setUnit: (u) => { this.materialUnits[key] = u; this.tables.materials[type].refresh(); },
        });
        const rayleighCols = [
            { key: 'rayleigh_period_first_s', label: 'Rayleigh T1 (s)', type: 'number', decimals: 2 },
            { key: 'rayleigh_period_second_s', label: 'Rayleigh T2 (s)', type: 'number', decimals: 2 },
            { key: 'rayleigh_damping_first', label: 'Rayleigh ξ1', type: 'number', decimals: 4 },
            { key: 'rayleigh_damping_second', label: 'Rayleigh ξ2', type: 'number', decimals: 4 },
        ];
        const nameCol = { key: 'name', label: 'Nome', type: 'text' };

        this.tables.materials.beam = createSpreadsheetTable(
            '#materials-table-beam', this.model.materials,
            [
                nameCol,
                unitCol('beam', 'diameter_external_m', 'D. externo', 'length', 4),
                unitCol('beam', 'diameter_internal_m', 'D. interno', 'length', 4),
                unitCol('beam', 'weight_dry_kNm', 'Peso seco', 'forcePerLength', 4),
                unitCol('beam', 'weight_wet_kNm', 'Peso molhado', 'forcePerLength', 4),
                { key: 'morison_inertia_Cm', label: 'Cm', type: 'number', decimals: 2 },
                { key: 'morison_drag_Cd', label: 'Cd', type: 'number', decimals: 2 },
                unitCol('beam', 'stiffness_axial_kN', 'EA', 'force', 1),
                { key: 'stiffness_bending_kNm2', label: 'EI (kN·m²)', type: 'number', decimals: 1 },
                { key: 'stiffness_torsional_kNm2', label: 'GJ (kN·m²)', type: 'number', decimals: 1 },
                ...rayleighCols,
            ],
            { filter: m => (m.finite_element_type ?? 'beam') === 'beam', onDelete: () => this.onMaterialsChanged(), onChange: () => this.onMaterialsChanged() },
        );
        this.tables.materials.truss = createSpreadsheetTable(
            '#materials-table-truss', this.model.materials,
            [
                nameCol,
                unitCol('truss', 'diameter_external_m', 'D. externo', 'length', 4),
                unitCol('truss', 'diameter_internal_m', 'D. interno', 'length', 4),
                unitCol('truss', 'weight_dry_kNm', 'Peso seco', 'forcePerLength', 4),
                unitCol('truss', 'weight_wet_kNm', 'Peso molhado', 'forcePerLength', 4),
                unitCol('truss', 'stiffness_axial_kN', 'EA', 'force', 1),
                unitCol('truss', 'initial_tension_kN', 'Tração inicial', 'force', 1),
                ...rayleighCols,
            ],
            { filter: m => m.finite_element_type === 'truss', onDelete: () => this.onMaterialsChanged(), onChange: () => this.onMaterialsChanged() },
        );
        this.tables.materials.scalar = createSpreadsheetTable(
            '#materials-table-scalar', this.model.materials,
            [
                nameCol,
                { key: 'scalar_stiffness_translational_kNm', label: 'Rigidez translacional (kN/m)', type: 'number', decimals: 1 },
                { key: 'scalar_stiffness_torsional_kNm_per_rad', label: 'Rigidez torcional (kN·m/rad)', type: 'number', decimals: 1 },
                { key: 'scalar_stiffness_bending_kNm_per_rad', label: 'Rigidez à flexão (kN·m/rad)', type: 'number', decimals: 1 },
                ...rayleighCols,
            ],
            { filter: m => m.finite_element_type === 'scalar', onDelete: () => this.onMaterialsChanged(), onChange: () => this.onMaterialsChanged() },
        );
        this.tables.materials.winch = createSpreadsheetTable(
            '#materials-table-winch', this.model.materials,
            [
                nameCol,
                unitCol('winch', 'diameter_external_m', 'D. externo', 'length', 4),
                unitCol('winch', 'diameter_internal_m', 'D. interno', 'length', 4),
                unitCol('winch', 'weight_dry_kNm', 'Peso seco', 'forcePerLength', 4),
                unitCol('winch', 'weight_wet_kNm', 'Peso molhado', 'forcePerLength', 4),
                unitCol('winch', 'stiffness_axial_kN', 'EA', 'force', 1),
                unitCol('winch', 'initial_tension_kN', 'Tração inicial', 'force', 1),
                {
                    key: 'winch_payout_curve_id', label: 'Curva de recolhimento', type: 'select', nullable: true,
                    options: () => this.model.curves.map(c => ({ value: c.id, label: `${c.name} (${c.id})` })),
                },
                ...rayleighCols,
            ],
            { filter: m => m.finite_element_type === 'winch', onDelete: () => this.onMaterialsChanged(), onChange: () => this.onMaterialsChanged() },
        );
    }
    /** A material's name/diameter feeds the Segmentos table's `material_id` dropdown AND the
     * catenary solve (weight) -- refresh the segments labels and recompile the preview. */
    onMaterialsChanged() {
        if (this.tables.segments) this.tables.segments.refresh();
        this.schedulePreviewUpdate();
    }
    /** @param {'beam'|'truss'|'scalar'|'winch'} type */
    addMaterialOfType(type) {
        const n = this.model.materials.filter(m => (m.finite_element_type ?? 'beam') === type).length + 1;
        const NAMES = { beam: 'Viga', truss: 'Cabo', scalar: 'Junta', winch: 'Guincho' };
        const base = {
            id: nextId(this.model.materials), name: `${NAMES[type]}_${n}`, finite_element_type: type,
            diameter_external_m: 0.25, diameter_internal_m: 0.20, weight_dry_kNm: 1.0, weight_wet_kNm: 0.4,
            morison_inertia_Cm: 2.0, morison_drag_Cd: 1.0, stiffness_axial_kN: 360000.0,
            stiffness_bending_kNm2: 21.7, stiffness_torsional_kNm2: 3200.0,
            initial_tension_kN: 0.0,
            rayleigh_period_first_s: 0.0, rayleigh_period_second_s: 0.0,
            rayleigh_damping_first: 0.0, rayleigh_damping_second: 0.0,
            // A freshly-created flexjoint defaults to non-zero stiffness -- 0.0 would be a free
            // hinge (no resistance at all), which is never what "add a joint" means in practice.
            scalar_stiffness_translational_kNm: type === 'scalar' ? 50000.0 : 0.0,
            scalar_stiffness_torsional_kNm_per_rad: type === 'scalar' ? 500.0 : 0.0,
            scalar_stiffness_bending_kNm_per_rad: type === 'scalar' ? 500.0 : 0.0,
            winch_payout_curve_id: null,
        };
        this.tables.materials[type].addRow(base);
        this.onMaterialsChanged();
    }

    // ---- Curves (winch payout, referenced by a material's winch_payout_curve_id) ----
    renderCurves() {
        if (this.tables.curves) return;
        this.tables.curves = createSpreadsheetTable(
            '#curves-table', this.model.curves,
            [
                { key: 'name', label: 'Nome', type: 'text' },
                { key: 'time_s', label: 'Tempo (s)', type: 'list' },
                { key: 'fraction', label: 'Fração (0-1)', type: 'list' },
            ],
            { onDelete: () => this.onCurvesChanged(), onChange: () => this.onCurvesChanged() },
        );
    }
    /** A curve's name/id is shown in the Materiais/Guinchos table's `winch_payout_curve_id`
     * dropdown -- the only Materiais sub-tab that references curves. */
    onCurvesChanged() {
        if (this.tables.materials.winch) this.tables.materials.winch.refresh();
        this.schedulePreviewUpdate();
    }
    addCurve() {
        this.tables.curves.addRow({
            id: nextId(this.model.curves), name: `Curva_${this.model.curves.length + 1}`,
            time_s: [0.0, 3600.0], fraction: [1.0, 1.0],
        });
        this.onCurvesChanged();
    }

    // ---- Currents ----
    renderCurrents() {
        if (this.tables.currents) return;
        this.tables.currents = createSpreadsheetTable(
            '#currents-table', this.model.currents,
            [
                { key: 'name', label: 'Nome', type: 'text' },
                { key: 'depth_below_surface_m', label: 'Profundidades (m, 0=superfície)', type: 'list' },
                { key: 'velocities_ms', label: 'Velocidades (m/s)', type: 'list' },
                { key: 'angles_deg', label: 'Ângulos (°)', type: 'list' },
            ],
            { onDelete: () => this.onCurrentsChanged(), onChange: () => this.onCurrentsChanged() },
        );
    }
    /** A current's name/id is shown in the Casos de Carga table's `current_id` dropdown. */
    onCurrentsChanged() {
        if (this.tables.loadcases) this.tables.loadcases.refresh();
        this.schedulePreviewUpdate();
    }
    addCurrent() {
        this.tables.currents.addRow({
            id: nextId(this.model.currents), name: `Corrente_${this.model.currents.length + 1}`,
            depth_below_surface_m: [0.0, 100.0], velocities_ms: [1.0, 0.3], angles_deg: [90.0, 90.0],
        });
        this.onCurrentsChanged();
    }

    // ---- Waves ----
    renderWaves() {
        if (this.tables.waves) return;
        this.tables.waves = createSpreadsheetTable(
            '#waves-table', this.model.waves,
            [
                { key: 'name', label: 'Nome', type: 'text' },
                { key: 'type', label: 'Tipo', type: 'select', options: () => [{ value: 'REGULAR', label: 'Regular' }, { value: 'JONSWAP', label: 'JONSWAP' }] },
                { key: 'period_s', label: 'Período (s)', type: 'number' },
                { key: 'height_m', label: 'Altura (m)', type: 'number' },
                { key: 'angle_deg', label: 'Ângulo (°)', type: 'number' },
                { key: 'gamma', label: 'Gamma', type: 'number' },
            ],
            { onDelete: () => this.onWavesChanged(), onChange: () => this.onWavesChanged() },
        );
    }
    /** A wave's name/id is shown in the Casos de Carga table's `wave_id` dropdown. */
    onWavesChanged() {
        if (this.tables.loadcases) this.tables.loadcases.refresh();
        this.schedulePreviewUpdate();
    }
    addWave() {
        this.tables.waves.addRow({
            id: nextId(this.model.waves), name: `Onda_${this.model.waves.length + 1}`, type: 'REGULAR',
            period_s: 10.0, height_m: 2.0, angle_deg: 0.0, gamma: 3.3,
            alpha: 0.0, wini_rad_s: 0.2, wfin_rad_s: 3.0, nwave: 100,
        });
        this.onWavesChanged();
    }

    // ---- Buoys (referenced by a segment's buoy_id) ----
    renderBuoys() {
        if (this.tables.buoys) return;
        this.tables.buoys = createSpreadsheetTable(
            '#buoys-table', this.model.buoys,
            [
                { key: 'name', label: 'Nome', type: 'text' },
                { key: 'weight_kN', label: 'Peso (kN)', type: 'number', decimals: 2 },
                { key: 'volume_m3', label: 'Volume (m³)', type: 'number', decimals: 3 },
                { key: 'z_area_m2', label: 'Área-Z (m²)', type: 'number', decimals: 3 },
                { key: 'Ca', label: 'Ca (X, Y, Z)', type: 'list' },
                { key: 'moment_inertia', label: 'Momento de inércia (X, Y, Z)', type: 'list' },
            ],
            { onDelete: () => this.onBuoysChanged(), onChange: () => this.onBuoysChanged() },
        );
    }
    /** A buoy's name/id is shown in the Segmentos table's `buoy_id` dropdown. */
    onBuoysChanged() {
        if (this.tables.segments) this.tables.segments.refresh();
        this.schedulePreviewUpdate();
    }
    addBuoy() {
        this.tables.buoys.addRow({
            id: nextId(this.model.buoys), name: `Boia_${this.model.buoys.length + 1}`,
            weight_kN: 5.0, volume_m3: 2.0, z_area_m2: 1.5,
            Ca: [0.0, 0.0, 0.5], moment_inertia: [0.0, 0.0, 0.0],
        });
        this.onBuoysChanged();
    }

    // ---- Lines (+ segments) ----
    getActiveLine() {
        return this.model.lines.find(l => l.id === this.activeLineId) || null;
    }

    /** Lines table -- same spreadsheet-grid pattern as every other catalog (Solos/Materiais/...),
     * with one addition: `selectable: 1` (radio-button style, at most one highlighted row at a
     * time) drives which line's segments show in the Segmentos table below (see onRowSelected).
     * Top position is stored as `top_position_m: [x, y, z]` (an array, not three separate object
     * properties), so those three columns use `type: 'custom'` (SpreadsheetTable.js) with
     * accessors into that array instead of Tabulator's normal dot-notation field resolution. */
    renderLines() {
        if (this.tables.lines) return;
        const posCol = (label, i) => ({
            key: `top_${['x', 'y', 'z'][i]}`, label, type: 'custom', decimals: 2,
            getValue: (line) => line.top_position_m[i],
            setValue: (line, v) => { line.top_position_m[i] = v ?? 0; },
        });
        this.tables.lines = createSpreadsheetTable(
            '#lines-table', this.model.lines,
            [
                { key: 'name', label: 'Nome', type: 'text' },
                posCol('Topo X (m)', 0),
                posCol('Topo Y (m)', 1),
                posCol('Topo Z (m)', 2),
                { key: 'catenary_angle_deg', label: 'Ângulo catenária (°, da vertical)', type: 'number', decimals: 2 },
                { key: 'azimuth_deg', label: 'Azimute (°)', type: 'number', decimals: 2 },
            ],
            {
                selectable: 1,
                // Guarded against re-firing for the line that's already active -- addLine()/
                // onLineDeleted() below set `activeLineId`/call renderSegments() themselves
                // (synchronously, not dependent on Tabulator's row-selection timing) and only use
                // selectRow() as a best-effort visual highlight, so this only needs to actually do
                // something for a genuine user click on a DIFFERENT row.
                onRowSelected: (line) => {
                    if (this.activeLineId === line.id) return;
                    this.activeLineId = line.id;
                    this.renderSegments();
                },
                onDelete: (line) => this.onLineDeleted(line),
                // Any field (name/position/angle/azimuth) can change the geometry shown in the 3D
                // canvas -- unlike the per-field granularity the old form had, a table's onChange
                // doesn't know which column changed, so just always recompute both (cheap either
                // way).
                onChange: () => { this.renderPreviewFrame(); this.schedulePreviewUpdate(); },
            },
        );
        // `this.activeLineId` is already correct at this point (set in load(), or by
        // addLine()/onLineDeleted() on later calls) -- render its segments right away rather than
        // waiting on Tabulator's own (possibly still-building) row-selection state. The visual
        // highlight catches up once the table finishes building.
        this.tables.lines.table.on('tableBuilt', () => {
            if (this.activeLineId != null) this.tables.lines.selectRow(this.activeLineId);
        });
        this.renderSegments();
    }

    /** After a line is deleted (via its own row's 🗑, same as any other catalog -- no separate
     * "select then delete" step anymore), picks a new active line if the deleted one was it, and
     * always reframes -- the model's bounding box can shrink regardless of which line went away. */
    onLineDeleted(line) {
        if (this.activeLineId === line.id) {
            this.activeLineId = (this.model.lines[0] || {}).id ?? null;
            this.renderSegments();
            if (this.activeLineId != null) this.tables.lines.selectRow(this.activeLineId);
        }
        this._hasFramedReal = false;
        this.renderPreviewFrame();
        this.reframeCamera();
        this.schedulePreviewUpdate();
    }

    /** Instant "context" frame -- sea/seabed planes + a bounding box around every line's top
     * point (all of them, not just the active one -- see the module docstring) -- shown BEFORE
     * any backend round-trip. Also toggles the "no lines yet" empty state and the diameter
     * legend. Never moves the camera itself (see reframeCamera()); uses the same seabed/water-
     * surface Z convention the compiled simulation JSON always ends up with
     * (`solve_catenary_geometry()`'s anchor at Z~0 in the ANFLEX-native frame, surface at
     * Z ~= water_depth_m) -- see `catenary_geometry.solve_catenary_geometry()`'s own docstring
     * for why. */
    renderPreviewFrame() {
        if (!this.model) return;
        const lines = this.model.lines || [];
        const hasLines = lines.length > 0;
        document.getElementById('preview-empty-state').style.display = hasLines ? 'none' : 'flex';
        document.getElementById('colorbar-legend').style.display = hasLines ? '' : 'none';
        if (!hasLines) {
            this.renderer3D.renderStep({ nodes: [], elements: [] }, 'Viridis', { min: 0, max: 1 }, this.currentTheme, 'tension', null, null);
            this.lastStep = null;
            this.lastEnvBounds = [null, null];
            return;
        }
        const seabedZ = 0.0;
        const waterSurfaceZ = seabedZ + Math.abs(this.model.global.water_depth_m ?? 100.0);
        const topNodes = lines.map(l => ({ x: l.top_position_m[0], y: l.top_position_m[1], z: l.top_position_m[2] }));
        const bounds = this.renderer3D.computeSceneBounds(topNodes, seabedZ, waterSurfaceZ);
        this.renderer3D.updateBoundingBox(bounds, this.currentTheme);
        this.renderer3D.updateEnvironmentPlanes(bounds, seabedZ, waterSurfaceZ, this.currentTheme);
        this.lastStep = { stepIndex: 0, loadFactor: 0, nodes: topNodes, elements: [] };
        this.lastEnvBounds = [seabedZ, waterSurfaceZ];
    }

    /** `ThemeToggle.js`'s redraw hook -- re-issues the instant frame (bounding box/env planes
     * pick up dark/light material colors) and, if a real compiled mesh is already on screen,
     * re-renders it under the new theme too (using the min/max diameter from the last real
     * compile, stashed by updatePreviewNow()). */
    render() {
        this.renderPreviewFrame();
        if (this.model && this.model.lines.length && this.lastStep && this.lastStep.elements.length) {
            const [seabedZ, waterSurfaceZ] = this.lastEnvBounds;
            this.renderer3D.renderStep(this.lastStep, 'Viridis', { min: this._lastMinD ?? 0, max: this._lastMaxD ?? 1 }, this.currentTheme, 'tension', seabedZ, waterSurfaceZ);
        }
    }

    /** Reframes the camera (ISO view fit to `lastStep`/`lastEnvBounds`) -- called explicitly, not
     * automatically on every render, so the camera doesn't jump while the user is mid-edit on an
     * unrelated field or orbiting to inspect something. Called on initial load, right after
     * add/deleteLine (new/removed geometry), and once more when the FIRST real compile after such
     * a change lands (`_hasFramedReal`, reset by add/deleteLine) -- the instant placeholder frame
     * is a rough guess (top points only), the real compiled mesh is the accurate one worth
     * settling the camera on. */
    reframeCamera() {
        if (!this.lastStep) return;
        this.cameraController.setView('ISO', this.lastStep, ...this.lastEnvBounds);
    }

    /** Debounces the REAL preview (the actual solved mesh, via `POST /api/interface/preview` --
     * `risersim_runner.py::build_config_from_interface()`, the same compiler a real run uses) so
     * a round-trip isn't fired on every keystroke -- 600ms of no further edits before it runs. */
    schedulePreviewUpdate() {
        if (this._previewDebounceTimer) clearTimeout(this._previewDebounceTimer);
        this._previewDebounceTimer = setTimeout(() => this.updatePreviewNow(), 600);
    }

    async updatePreviewNow() {
        const errorEl = document.getElementById('preview-error');
        if (!this.model.lines.length) { errorEl.style.display = 'none'; return; }
        try {
            const res = await fetchJSON('/api/interface/preview', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ interface: this.model }),
            });
            errorEl.style.display = 'none';
            const nodes = res.nodes || [];
            // No solved tension yet (this is a pre-run preview) -- colors by outer diameter
            // instead, same substitute trick preprocessor_app.js::getSyntheticStep() already
            // uses to reuse Riser3DRenderer (built for solved *results*) on unsolved input, and
            // the same "Diâmetro externo (m)" legend caption already in editor.html.
            // `node1_id`/`node2_id` carried through from the backend's own element objects --
            // Riser3DRenderer.js needs them to draw each element between its REAL nodes, not
            // whichever nodes happen to sit at the same array position (breaks for 2+ lines, see
            // its own comment). `node_id`/`element_type` are new here -- a `buoy` element only has
            // `node_id` (1 node, no `node1_id`/`node2_id`), and Riser3DRenderer.js needs
            // `element_type` to draw it as a marker instead of a cylinder between two nodes.
            const PROPS_KEY_BY_TYPE = { truss: 'truss_properties', winch: 'winch_properties', scalar: 'scalar_properties', beam: 'section_properties' };
            const elements = (res.elements || []).map(e => {
                const propsKey = PROPS_KEY_BY_TYPE[e.element_type] || 'section_properties';
                const props = e[propsKey];
                return {
                    id: e.id, element_type: e.element_type,
                    node1_id: e.node1_id, node2_id: e.node2_id, node_id: e.node_id,
                    // scalar/buoy have no real D_outer -- kept as 0 so the tube still renders
                    // (scalar) or is simply unused (buoy, drawn as a marker instead), but excluded
                    // below from the diameter legend's min/max so a fake 0 doesn't drag it down.
                    tensionEffectiveKn: (props && props.D_outer) ?? 0,
                    bendingMomentKnm: 0, curvature: 0, vonMisesMpa: 0, mbrSafetyFactor: 1.0,
                };
            });
            const diameters = elements
                .filter(e => e.element_type !== 'scalar' && e.element_type !== 'buoy')
                .map(e => e.tensionEffectiveKn).filter(v => v !== undefined && v !== null);
            let minD = diameters.length ? Math.min(...diameters) : 0;
            let maxD = diameters.length ? Math.max(...diameters) : 1;
            if (maxD <= minD) maxD = minD + 1.0;
            const step = { stepIndex: 0, loadFactor: 0, nodes, elements };
            const seabedZ = nodes.length ? Math.min(...nodes.map(n => n.z)) : 0.0;
            const waterSurfaceZ = seabedZ + Math.abs(this.model.global.water_depth_m ?? 100.0);
            this.renderer3D.renderStep(step, 'Viridis', { min: minD, max: maxD }, this.currentTheme, 'tension', seabedZ, waterSurfaceZ);
            this.updateColorbar(minD, maxD);
            this.lastStep = step;
            this.lastEnvBounds = [seabedZ, waterSurfaceZ];
            this._lastMinD = minD;
            this._lastMaxD = maxD;
            if (!this._hasFramedReal) {
                this.reframeCamera();
                this._hasFramedReal = true;
            }
        } catch (err) {
            // Expected/frequent while the form is mid-edit (e.g. a segment pointing at a
            // material_id not created yet) -- shown inline, not an alertDialog, so it doesn't
            // interrupt typing.
            errorEl.innerText = `Pré-visualização: ${err.message}`;
            errorEl.style.display = '';
        }
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

    /** Unlike the other catalog tables (created once, updated in place), the segments table is
     * bound to the ACTIVE LINE's own `segments` array -- a different array reference every time a
     * different row is selected in the Lines table -- so it's destroyed and rebuilt from scratch
     * on every call, rather than trying to repoint an existing Tabulator instance at a new backing
     * array. */
    renderSegments() {
        if (this.tables.segments) { this.tables.segments.destroy(); this.tables.segments = null; }
        const line = this.getActiveLine();
        const container = document.getElementById('segments-table');
        if (!line) { container.innerHTML = '<div class="empty-state-small">Selecione uma linha na tabela acima.</div>'; return; }
        // Segments coming from an AML-derived interface JSON (aml_reader.py::to_interface_json())
        // never carry an `id` (only the line itself does, not its segments) -- Tabulator's row
        // index defaults to that field, so it needs a stable numeric id per row -- backfill any
        // missing one here, once, before rendering (harmless extra field for the compiler either
        // way -- it never reads segment ids).
        let nextSegId = line.segments.reduce((max, s) => Math.max(max, s.id || 0), 0) + 1;
        line.segments.forEach(s => { if (s.id == null) s.id = nextSegId++; });
        this.tables.segments = createSpreadsheetTable(
            container, line.segments,
            [
                { key: 'length_m', label: 'Comprimento (m)', type: 'number' },
                { key: 'material_id', label: 'Material', type: 'select', options: () => this.model.materials.map(m => ({ value: m.id, label: `${m.name} (${m.id})` })) },
                { key: 'soil_id', label: 'Solo', type: 'select', options: () => this.model.soils.map(s => ({ value: s.id, label: `${s.name} (${s.id})` })) },
                { key: 'element_length_m', label: 'Malha (m/elemento)', type: 'number' },
                {
                    key: 'buoy_id', label: 'Boia', type: 'select', nullable: true,
                    options: () => this.model.buoys.map(b => ({ value: b.id, label: `${b.name} (${b.id})` })),
                },
            ],
            { onDelete: () => this.schedulePreviewUpdate(), onChange: () => this.schedulePreviewUpdate() },
        );
    }
    addLine() {
        const line = {
            id: nextId(this.model.lines), name: `Linha_${this.model.lines.length + 1}`,
            top_position_m: [0.0, 0.0, this.model.global.water_depth_m || 100.0],
            catenary_angle_deg: 10.0, azimuth_deg: 0.0,
            segments: [{
                id: 1, length_m: 500.0,
                material_id: (this.model.materials[0] || {}).id ?? null,
                soil_id: (this.model.soils[0] || {}).id ?? null,
                element_length_m: 5.0, buoy_id: null,
            }],
        };
        this.tables.lines.addRow(line);
        // Set directly rather than relying on selectRow()'s rowSelected event -- Tabulator's
        // addData() is promise-based internally, so the new row isn't guaranteed selectable the
        // instant addRow() returns. selectRow() is still called, best-effort, for the visual
        // highlight once the row actually exists.
        this.activeLineId = line.id;
        this.renderSegments();
        this.tables.lines.selectRow(line.id);
        this._hasFramedReal = false;
        this.renderPreviewFrame();
        this.reframeCamera();
        this.schedulePreviewUpdate();
    }
    addSegment() {
        const line = this.getActiveLine();
        if (!line || !this.tables.segments) return;
        const nextSegId = line.segments.reduce((max, s) => Math.max(max, s.id || 0), 0) + 1;
        this.tables.segments.addRow({
            id: nextSegId, length_m: 100.0,
            material_id: (this.model.materials[0] || {}).id ?? null,
            soil_id: (this.model.soils[0] || {}).id ?? null,
            element_length_m: 5.0, buoy_id: null,
        });
        this.schedulePreviewUpdate();
    }

    // ---- Load cases ----
    renderLoadCases() {
        if (this.tables.loadcases) return;
        this.tables.loadcases = createSpreadsheetTable(
            '#loadcases-table', this.model.load_cases,
            [
                { key: 'name', label: 'Nome', type: 'text' },
                { key: 'current_id', label: 'Corrente', type: 'select', options: () => this.model.currents.map(c => ({ value: c.id, label: `${c.name} (${c.id})` })) },
                { key: 'wave_id', label: 'Onda', type: 'select', options: () => this.model.waves.map(w => ({ value: w.id, label: `${w.name} (${w.id})` })) },
            ],
            { onDelete: () => this.schedulePreviewUpdate(), onChange: () => this.schedulePreviewUpdate() },
        );
    }
    addLoadCase() {
        this.tables.loadcases.addRow({
            id: nextId(this.model.load_cases), name: `Caso_${this.model.load_cases.length + 1}`,
            current_id: (this.model.currents[0] || {}).id ?? null,
            wave_id: (this.model.waves[0] || {}).id ?? null,
        });
        this.schedulePreviewUpdate();
    }

    // ---- Analyses ----
    renderAnalyses() {
        if (this.tables.analyses) return;
        this.tables.analyses = createSpreadsheetTable(
            '#analyses-table', this.model.analyses,
            [
                { key: 'name', label: 'Nome', type: 'text' },
                { key: 'static.steps', label: 'Passos estáticos', type: 'number', decimals: 0 },
                { key: 'static.max_iterations', label: 'Máx. iterações (estático)', type: 'number', decimals: 0 },
                { key: 'static.tolerance', label: 'Tolerância (estático)', type: 'number', decimals: 4 },
                { key: 'dynamic.enabled', label: 'Dinâmica?', type: 'checkbox' },
                { key: 'dynamic.duration_s', label: 'Duração dinâmica (s)', type: 'number' },
                { key: 'dynamic.dt_s', label: 'Passo de tempo (s)', type: 'number' },
                { key: 'load_case_ids', label: 'Casos de carga (IDs, vírgula)', type: 'list' },
            ],
            { onDelete: () => this.schedulePreviewUpdate(), onChange: () => this.schedulePreviewUpdate() },
        );
    }
    addAnalysis() {
        this.tables.analyses.addRow({
            id: nextId(this.model.analyses), name: `Analise_${this.model.analyses.length + 1}`,
            static: { steps: 20, max_iterations: 300, tolerance: 0.01 },
            dynamic: { enabled: true, duration_s: 20.0, dt_s: 0.05, max_iterations: 20, tolerance: 1.0e-4 },
            load_case_ids: this.model.load_cases.map(lc => lc.id),
        });
        this.schedulePreviewUpdate();
    }

    // ---- Save ----
    async save(goToProject) {
        const draftBtn = document.getElementById('save-draft-btn');
        const goBtn = document.getElementById('save-and-go-btn');
        draftBtn.disabled = true;
        goBtn.disabled = true;
        document.getElementById('editor-error').innerText = '';
        try {
            const res = await fetchJSON(`/api/projects/${encodeURIComponent(this.projectId)}/interface`, {
                method: 'PUT',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ interface: this.model }),
            });
            this.renderWarnings(res.warnings || []);
            if (goToProject) {
                window.location.href = `project.html?project=${encodeURIComponent(this.projectId)}`;
                return;
            }
            if ((res.warnings || []).length === 0) {
                await alertDialog({ title: 'Rascunho salvo', message: 'JSON de interface salva e compilada com sucesso, sem avisos.' });
            }
        } catch (err) {
            document.getElementById('editor-error').innerText = `Falha ao salvar: ${err.message}`;
        } finally {
            draftBtn.disabled = false;
            goBtn.disabled = false;
        }
    }

    renderWarnings(warnings) {
        const el = document.getElementById('editor-warnings');
        if (!warnings.length) {
            el.style.display = 'none';
            el.innerHTML = '';
            return;
        }
        el.style.display = '';
        el.innerHTML = `<div class="control-title">⚠️ AVISOS DO COMPILADOR</div><ul>${warnings.map(w => `<li>${escapeHtml(w)}</li>`).join('')}</ul>`;
    }
}

window.addEventListener('DOMContentLoaded', () => {
    const params = new URLSearchParams(window.location.search);
    const projectId = params.get('project');
    if (!projectId) {
        document.getElementById('editor-error').innerText = 'Use ?project=<id> na URL.';
        return;
    }
    window.editorApp = new EditorApp(projectId);
});
