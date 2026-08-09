import { ColorMapService } from '../services/ColorMapService.js';

/**
 * Riser3DRenderer.js
 * Wraps the Three.js scene, lighting, seabed/water mesh, riser tubes, and bounding box.
 */
export class Riser3DRenderer {
    /**
     * @param {HTMLCanvasElement} canvasElement
     */
    constructor(canvasElement) {
        this.canvas = canvasElement;
        this.scene = new THREE.Scene();
        this.riserGroup = new THREE.Group();
        this.nodesGroup = new THREE.Group();
        this.envGroup = new THREE.Group(); // seabed/water-surface planes
        this.boundingBoxWireframe = null;

        this.initEngine();
    }

    initEngine() {
        const width = this.canvas.clientWidth || window.innerWidth || 800;
        const height = this.canvas.clientHeight || window.innerHeight || 600;

        this.renderer = new THREE.WebGLRenderer({ canvas: this.canvas, antialias: true });
        this.renderer.setSize(width, height);
        this.renderer.setPixelRatio(window.devicePixelRatio);

        this.scene.background = new THREE.Color(0x0a0a10);

        // far=2000/initial position are just a reasonable guess for Exemplo_01a's typical scale
        // -- CameraViewController.setView() reframes and adjusts the far plane to the model's
        // real scale as soon as data loads (see app.js), so this is never actually what the user
        // sees for a model at a different scale.
        this.camera = new THREE.PerspectiveCamera(45, width / height, 0.1, 2000);
        this.camera.position.set(60, 50, 160);

        this.controls = new THREE.OrbitControls(this.camera, this.renderer.domElement);
        this.controls.enableDamping = true;
        this.controls.dampingFactor = 0.05;
        this.controls.target.set(60, -50, 0);

        // Lighting
        const ambientLight = new THREE.AmbientLight(0xffffff, 0.8);
        this.scene.add(ambientLight);

        const dirLight = new THREE.DirectionalLight(0xffffff, 1.0);
        dirLight.position.set(100, 200, 100);
        this.scene.add(dirLight);

        // Add the groups
        this.scene.add(this.riserGroup);
        this.scene.add(this.nodesGroup);
        this.scene.add(this.envGroup);

        // 60 FPS render loop
        const animate = () => {
            requestAnimationFrame(animate);
            this.controls.update();
            this.renderer.render(this.scene, this.camera);
        };
        animate();

        // Window resize handling
        window.addEventListener('resize', () => this.onWindowResize());
    }

    onWindowResize() {
        if (!this.canvas || !this.canvas.parentElement) return;
        const parent = this.canvas.parentElement;
        const width = parent.clientWidth || 800;
        const height = parent.clientHeight || 600;

        if (width <= 0 || height <= 0) return;

        this.camera.aspect = width / height;
        this.camera.updateProjectionMatrix();
        this.renderer.setSize(width, height, true);
    }

    /**
     * Renders the riser's 3D state at a given step.
     * @param {SimulationStep} step
     * @param {string} colormap
     * @param {{min: number, max: number}} scalarRange
     * @param {'dark'|'light'} currentTheme
     * @param {string} scalarField - 'tension' | 'moment' | 'curvature' | 'vonmises' | 'mbr'
     * @param {number|null} seabedDepth - Seabed Z elevation (null/absent = plane not drawn)
     * @param {number|null} waterSurfaceZ - Water surface Z elevation (null/absent = plane not drawn)
     */
    renderStep(step, colormap = 'Jet', scalarRange = { min: 0, max: 10 }, currentTheme = 'dark', scalarField = 'tension', seabedDepth = null, waterSurfaceZ = null) {
        if (!step) return;

        // Fully clear previous geometry
        while (this.riserGroup.children.length > 0) {
            const obj = this.riserGroup.children[0];
            if (obj.geometry) obj.geometry.dispose();
            if (obj.material) obj.material.dispose();
            this.riserGroup.remove(obj);
        }
        while (this.nodesGroup.children.length > 0) {
            const obj = this.nodesGroup.children[0];
            if (obj.geometry) obj.geometry.dispose();
            if (obj.material) obj.material.dispose();
            this.nodesGroup.remove(obj);
        }

        const nodes = step.nodes;
        const elements = step.elements;

        const bounds = this.computeSceneBounds(nodes, seabedDepth, waterSurfaceZ);
        this.updateBoundingBox(bounds, currentTheme);
        this.updateEnvironmentPlanes(bounds, seabedDepth, waterSurfaceZ, currentTheme);

        // Tube radius proportional to the model's real size -- a fixed value (0.6, calibrated for
        // Exemplo_01a's scale, ~130m) turns into a sub-pixel-thick, nearly invisible line on a
        // much larger model (e.g. Exemplo_02a, ~1800m) -- even with the camera correctly framed,
        // the line itself becomes too hard to see/interpret (mapa_classes_anflex_estatica.md).
        // 0.0045 * 130 ≈ 0.585, preserving the original look for Exemplo_01a-scale models.
        let modelSpan = 130.0;
        if (nodes.length > 0) {
            const xs = nodes.map(n => n.x), ys = nodes.map(n => n.y), zs = nodes.map(n => n.z);
            modelSpan = Math.max(
                Math.max(...xs) - Math.min(...xs),
                Math.max(...ys) - Math.min(...ys),
                Math.max(...zs) - Math.min(...zs),
                10.0
            );
        }
        const outerRadius = Math.max(0.15, modelSpan * 0.0045);
        const rangeMin = (scalarRange && scalarRange.min !== undefined) ? scalarRange.min : 0.0;
        const rangeMax = (scalarRange && scalarRange.max !== undefined) ? scalarRange.max : 1.0;
        const rangeSpan = (rangeMax > rangeMin) ? (rangeMax - rangeMin) : 1.0;

        for (let i = 0; i < elements.length; ++i) {
            const elem = elements[i];
            const n1 = nodes[i];
            const n2 = nodes[i + 1];
            if (!n1 || !n2 || !elem) continue;

            const p1 = new THREE.Vector3(n1.x, n1.z, n1.y);
            const p2 = new THREE.Vector3(n2.x, n2.z, n2.y);

            const dir = new THREE.Vector3().subVectors(p2, p1);
            const len = dir.length();
            if (len <= 0.001) continue;

            let val = 0.0;
            switch (scalarField) {
                case 'moment':
                    val = elem.bendingMomentKnm !== undefined ? elem.bendingMomentKnm : (elem.bending_moment_kNm || 0);
                    break;
                case 'curvature':
                    val = elem.curvature !== undefined ? elem.curvature : 0;
                    break;
                case 'vonmises':
                    val = elem.vonMisesMpa !== undefined ? elem.vonMisesMpa : (elem.von_mises_MPa || 0);
                    break;
                case 'mbr':
                    val = elem.mbrSafetyFactor !== undefined ? elem.mbrSafetyFactor : (elem.mbr_safety_factor || 1.0);
                    break;
                case 'tension':
                default:
                    val = elem.tensionEffectiveKn !== undefined ? elem.tensionEffectiveKn : (elem.tension_effective_kN || 0);
                    break;
            }

            let normVal = (val - rangeMin) / rangeSpan;
            if (isNaN(normVal)) normVal = 0.5;

            const rgb = ColorMapService.getColor(colormap, normVal);

            const cylinderGeo = new THREE.CylinderGeometry(outerRadius, outerRadius, len, 16);
            const material = new THREE.MeshStandardMaterial({
                color: new THREE.Color(rgb.r, rgb.g, rgb.b),
                metalness: 0.3,
                roughness: 0.4
            });

            const mesh = new THREE.Mesh(cylinderGeo, material);
            mesh.position.copy(p1).add(p2).multiplyScalar(0.5);
            mesh.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), dir.clone().normalize());

            this.riserGroup.add(mesh);
        }
    }

    /**
     * Computes the scene's bounding box (this step's nodes + seabed + surface), already with
     * display margins applied -- the single source used by both the bounding-box wireframe and
     * the environmental planes, so the planes are always exactly the box's size, never
     * independently larger/smaller.
     * @param {Node3D[]} nodes
     * @param {number|null} seabedDepth
     * @param {number|null} waterSurfaceZ
     * @returns {{minX:number,maxX:number,minY:number,maxY:number,minZ:number,maxZ:number}|null}
     */
    computeSceneBounds(nodes, seabedDepth, waterSurfaceZ) {
        if (!nodes || nodes.length === 0) return null;

        let minX = Infinity, maxX = -Infinity;
        let minY = Infinity, maxY = -Infinity;
        let minZ = Infinity, maxZ = -Infinity;

        nodes.forEach(n => {
            minX = Math.min(minX, n.x); maxX = Math.max(maxX, n.x);
            minY = Math.min(minY, n.z); maxY = Math.max(maxY, n.z);
            minZ = Math.min(minZ, n.y); maxZ = Math.max(maxZ, n.y);
        });

        // Extends the vertical range (Three.js Y = risersim Z/depth) to always cover the seabed
        // and the surface -- not just this step's node extent -- so the box (and the planes)
        // never "hide" part of the real water column. `1.0e5` discards the sentinel used when the
        // seabed is disabled via `environmental.seabed.enabled=false` (pushed to -1e6, see
        // ModelBuilder::load_from_json() in model_builder.cpp), which doesn't represent a real
        // seabed position.
        if (Number.isFinite(seabedDepth) && Math.abs(seabedDepth) < 1.0e5) {
            minY = Math.min(minY, seabedDepth);
            maxY = Math.max(maxY, seabedDepth);
        }
        if (Number.isFinite(waterSurfaceZ)) {
            minY = Math.min(minY, waterSurfaceZ);
            maxY = Math.max(maxY, waterSurfaceZ);
        }

        // Margins proportional to each axis's span (with a minimum floor), instead of a fixed
        // distance -- a fixed value (e.g. marginPerpendicular=35 always) looks fine on models with
        // a reasonable horizontal spread, but dominates the frame when one axis is naturally small
        // (e.g. a near-vertical riser with little lateral offset): the top view (XY) ended up
        // showing a huge, empty box around a tiny line.
        const spanX = maxX - minX, spanY = maxY - minY, spanZ = maxZ - minZ;
        const boxMargin = Math.max(2.0, Math.min(spanX, spanY) * 0.08);
        const marginPerpendicular = Math.max(5.0, spanZ * 0.25); // Larger distance on the perpendicular axis (Y)

        minX -= boxMargin; maxX += boxMargin;
        minY -= boxMargin; maxY += boxMargin;
        minZ -= marginPerpendicular; maxZ += marginPerpendicular;

        return { minX, maxX, minY, maxY, minZ, maxZ };
    }

    /**
     * @param {{minX,maxX,minY,maxY,minZ,maxZ}|null} bounds - see computeSceneBounds()
     * @param {'dark'|'light'} currentTheme
     */
    updateBoundingBox(bounds, currentTheme = 'dark') {
        if (this.boundingBoxWireframe) this.scene.remove(this.boundingBoxWireframe);
        if (!bounds) return;
        const { minX, maxX, minY, maxY, minZ, maxZ } = bounds;

        const boxWidth = maxX - minX;
        const boxHeight = maxY - minY;
        const boxDepth = maxZ - minZ;

        const boxGeo = new THREE.BoxGeometry(boxWidth, boxHeight, boxDepth);
        const edgesGeo = new THREE.EdgesGeometry(boxGeo);

        const wireColor = currentTheme === 'dark' ? 0x89b4fa : 0x475569;
        const wireframeMat = new THREE.LineBasicMaterial({ color: wireColor, linewidth: 2 });
        this.boundingBoxWireframe = new THREE.LineSegments(edgesGeo, wireframeMat);

        this.boundingBoxWireframe.position.set(minX + boxWidth / 2, minY + boxHeight / 2, minZ + boxDepth / 2);
        this.scene.add(this.boundingBoxWireframe);
    }

    /**
     * Draws the horizontal environmental reference planes: seabed and water surface. Both
     * semi-transparent, sharing the same X/Z (plan) footprint as the bounding box -- never larger
     * or smaller than it -- at each one's correct Z elevation.
     * @param {{minX,maxX,minY,maxY,minZ,maxZ}|null} bounds - see computeSceneBounds()
     * @param {number|null} seabedDepth
     * @param {number|null} waterSurfaceZ
     * @param {'dark'|'light'} currentTheme
     */
    updateEnvironmentPlanes(bounds, seabedDepth, waterSurfaceZ, currentTheme = 'dark') {
        while (this.envGroup.children.length > 0) {
            const obj = this.envGroup.children[0];
            if (obj.geometry) obj.geometry.dispose();
            if (obj.material) obj.material.dispose();
            this.envGroup.remove(obj);
        }
        if (!bounds) return;
        const { minX, maxX, minZ, maxZ } = bounds;

        const planeW = maxX - minX;
        const planeD = maxZ - minZ;
        const centerX = (minX + maxX) / 2;
        const centerPlan = (minZ + maxZ) / 2;

        const seabedColor = currentTheme === 'dark' ? 0x8a7355 : 0xb8a074;
        const waterColor = currentTheme === 'dark' ? 0x4a9fd8 : 0x7fc0ec;

        // Adds a horizontal plane (semi-transparent area) + a solid rectangular outline at the
        // edge, at the given Y elevation. The outline exists because a thin plane seen nearly
        // edge-on (e.g. the ISO view, with the camera well above the box's center looking down,
        // sees the top plane almost at a grazing angle) becomes practically invisible as a filled
        // area -- but the border, being a line (no thickness to hide), stays visible from any
        // angle, just like the bounding-box wireframe.
        const addPlane = (y, color, opacity, order) => {
            const mesh = new THREE.Mesh(
                new THREE.PlaneGeometry(planeW, planeD),
                new THREE.MeshStandardMaterial({
                    color, transparent: true, opacity, depthWrite: false,
                    side: THREE.DoubleSide, roughness: 0.5, metalness: 0.1
                })
            );
            mesh.rotation.x = -Math.PI / 2;
            mesh.position.set(centerX, y, centerPlan);
            mesh.renderOrder = order;
            this.envGroup.add(mesh);

            const hw = planeW / 2, hd = planeD / 2;
            const borderPts = [
                new THREE.Vector3(centerX - hw, y, centerPlan - hd),
                new THREE.Vector3(centerX + hw, y, centerPlan - hd),
                new THREE.Vector3(centerX + hw, y, centerPlan + hd),
                new THREE.Vector3(centerX - hw, y, centerPlan + hd),
            ];
            const border = new THREE.LineLoop(
                new THREE.BufferGeometry().setFromPoints(borderPts),
                new THREE.LineBasicMaterial({ color, linewidth: 2 })
            );
            border.renderOrder = order;
            this.envGroup.add(border);
        };

        // Discards the sentinel used when the seabed is disabled (environmental.seabed.enabled=
        // false pushes the bottom to -1e6, see ModelBuilder::load_from_json() in
        // model_builder.cpp) -- in that case there's no real seabed plane to draw.
        if (Number.isFinite(seabedDepth) && Math.abs(seabedDepth) < 1.0e5) {
            addPlane(seabedDepth, seabedColor, 0.4, 1);
        }
        if (Number.isFinite(waterSurfaceZ)) {
            addPlane(waterSurfaceZ, waterColor, 0.28, 2);
        }
    }
}
