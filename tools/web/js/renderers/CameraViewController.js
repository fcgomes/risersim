/**
 * CameraViewController.js
 * OO controller for predefined CAD camera views (Isometric, XY, XZ, YZ).
 */
export class CameraViewController {
    /**
     * @param {THREE.Camera} camera
     * @param {OrbitControls} controls
     * @param {Riser3DRenderer|null} renderer - optional; when given, framing reuses
     *   renderer.computeSceneBounds() (the same box/margins the renderer actually draws)
     *   instead of recomputing the bounding box independently -- avoids the two margin
     *   calculations drifting apart over time.
     */
    constructor(camera, controls, renderer = null) {
        this.camera = camera;
        this.controls = controls;
        this.renderer = renderer;
    }

    /**
     * Sets the camera view based on the selected option.
     * @param {'ISO'|'XY'|'XZ'|'YZ'} viewType
     * @param {SimulationStep} currentStep
     * @param {number|null} seabedDepth - When given, ensures the framing covers the seabed even
     *   if this step's nodes don't reach it.
     * @param {number|null} waterSurfaceZ - Same, for the water surface.
     */
    setView(viewType, currentStep, seabedDepth = null, waterSurfaceZ = null) {
        if (!this.camera || !this.controls) return;

        const bounds = this._computeBounds(currentStep, seabedDepth, waterSurfaceZ);
        const target = new THREE.Vector3(bounds.centerX, bounds.centerY, bounds.centerZ);

        // Camera->target direction for each predefined view. XY carries a tiny residual Z
        // component (not exactly vertical) only so the cross product with world "up" doesn't
        // degenerate for a perfectly straight top-down view.
        let dir;
        let fillFraction = 0.7;
        if (viewType === 'XY') { dir = new THREE.Vector3(0, -1, -0.001); fillFraction = 0.55; }
        else if (viewType === 'XZ') { dir = new THREE.Vector3(0, 0, -1); fillFraction = 0.55; }
        else if (viewType === 'YZ') { dir = new THREE.Vector3(-1, 0, 0); fillFraction = 0.55; }
        else dir = new THREE.Vector3(-0.55, -0.55, -1.3); // ISO (default)
        dir.normalize();

        // Orthogonal views (looking straight along one axis) project the bounding box at its
        // EXACT cross-section (the true minimal rectangle) -- ISO, being a diagonal view,
        // projects a "shadow" of the whole box, which is always larger than any orthogonal
        // cross-section. With the same target fill fraction, this makes XY/XZ/YZ look visually
        // "tighter" around the model than ISO -- hence the extra margin (smaller fillFraction)
        // for orthogonal views only.
        this._frameBounds(target, bounds, dir, fillFraction);
    }

    /**
     * Reframes (zoom extents) the whole model WITHOUT changing the current view direction --
     * unlike setView(), which jumps to a predefined view. Useful after manually orbiting/zooming
     * and losing the model from view.
     * @param {SimulationStep} currentStep
     * @param {number|null} seabedDepth
     * @param {number|null} waterSurfaceZ
     */
    fitToModel(currentStep, seabedDepth = null, waterSurfaceZ = null) {
        if (!this.camera || !this.controls) return;
        const bounds = this._computeBounds(currentStep, seabedDepth, waterSurfaceZ);
        const target = new THREE.Vector3(bounds.centerX, bounds.centerY, bounds.centerZ);

        const dir = new THREE.Vector3();
        this.camera.getWorldDirection(dir); // keeps the current view angle

        this._frameBounds(target, bounds, dir);
    }

    /**
     * Bounding box (Three.js coordinates: Y-three = Z-riser/depth), already using the same
     * margins the renderer uses to draw the wireframe/environmental planes -- delegates to
     * renderer.computeSceneBounds() when available, so the camera always frames exactly what's
     * being drawn (not just the thin line of nodes). Without a renderer, falls back to a
     * marginless version (nodes + seabed/surface only) as a safe default.
     */
    _computeBounds(currentStep, seabedDepth, waterSurfaceZ) {
        const nodes = (currentStep && currentStep.nodes) || [];

        if (this.renderer && typeof this.renderer.computeSceneBounds === 'function') {
            const b = this.renderer.computeSceneBounds(nodes, seabedDepth, waterSurfaceZ);
            if (b) {
                return {
                    ...b,
                    centerX: (b.minX + b.maxX) / 2.0, centerY: (b.minY + b.maxY) / 2.0, centerZ: (b.minZ + b.maxZ) / 2.0,
                };
            }
        }

        let minX = 0, maxX = 120, minY = -100, maxY = 0, minZ = -35, maxZ = 35;
        if (nodes.length > 0) {
            minX = Math.min(...nodes.map(n => n.x));
            maxX = Math.max(...nodes.map(n => n.x));
            minY = Math.min(...nodes.map(n => n.z));
            maxY = Math.max(...nodes.map(n => n.z));
            minZ = Math.min(...nodes.map(n => n.y));
            maxZ = Math.max(...nodes.map(n => n.y));
        }
        if (Number.isFinite(seabedDepth) && Math.abs(seabedDepth) < 1.0e5) {
            minY = Math.min(minY, seabedDepth);
            maxY = Math.max(maxY, seabedDepth);
        }
        if (Number.isFinite(waterSurfaceZ)) {
            minY = Math.min(minY, waterSurfaceZ);
            maxY = Math.max(maxY, waterSurfaceZ);
        }
        return {
            minX, maxX, minY, maxY, minZ, maxZ,
            centerX: (minX + maxX) / 2.0, centerY: (minY + maxY) / 2.0, centerZ: (minZ + maxZ) / 2.0,
        };
    }

    /**
     * Positions the camera looking from `target` along `dir` (camera->target, already
     * normalized), at a distance computed so the `bounds` bounding box fits entirely in the
     * frustum -- projects the box's 8 corners onto the camera's right/up axes and solves for the
     * minimum distance that satisfies both the vertical FOV AND the horizontal FOV
     * (== vertical FOV * aspect), instead of a fixed multiplier -- a fixed factor (e.g.
     * `span * 0.9`) under-sizes the distance whenever the canvas is narrower than it is tall
     * (e.g. a wide data panel taking up screen space), leaving the model clipped at the edges
     * ("falls outside the frame").
     * @param {THREE.Vector3} target
     * @param {{minX,maxX,minY,maxY,minZ,maxZ}} bounds
     * @param {THREE.Vector3} dir - camera->target direction, normalized
     * @param {number} fillFraction - fraction of the frame the model should occupy (1.0 =
     *   touching the edge). Smaller = more margin around it.
     */
    _frameBounds(target, bounds, dir, fillFraction = 0.7) {
        const { minX, maxX, minY, maxY, minZ, maxZ } = bounds;

        let worldUp = new THREE.Vector3(0, 1, 0);
        if (Math.abs(dir.dot(worldUp)) > 0.999) worldUp = new THREE.Vector3(0, 0, 1);
        const right = new THREE.Vector3().crossVectors(dir, worldUp).normalize();
        const camUp = new THREE.Vector3().crossVectors(right, dir).normalize();

        const corners = [
            [minX, minY, minZ], [maxX, minY, minZ], [minX, maxY, minZ], [minX, minY, maxZ],
            [maxX, maxY, minZ], [maxX, minY, maxZ], [minX, maxY, maxZ], [maxX, maxY, maxZ],
        ];
        let halfW = 5.0, halfH = 5.0;
        const rel = new THREE.Vector3();
        corners.forEach(([x, y, z]) => {
            rel.set(x - target.x, y - target.y, z - target.z);
            halfW = Math.max(halfW, Math.abs(rel.dot(right)));
            halfH = Math.max(halfH, Math.abs(rel.dot(camUp)));
        });

        const fovRad = THREE.MathUtils.degToRad(this.camera.fov);
        const distV = halfH / Math.tan(fovRad / 2.0);
        const distH = halfW / (Math.tan(fovRad / 2.0) * Math.max(this.camera.aspect, 0.01));
        // distV/distH above are already the EXACT framing distance (model touching the edge,
        // 100% of the frame) -- for the model to occupy only a fraction of the frame (visible
        // margin around it, room for labels/legends), the distance needs to be 1/fillFraction
        // times larger.
        const margin = 1.0 / fillFraction;
        const dist = Math.max(distV, distH, 5.0) * margin;

        this.camera.far = Math.max(2000, dist * 6.0);
        this.camera.updateProjectionMatrix();

        // Aligns camera.up to the "screen up" used in the halfH calculation above -- without
        // this, OrbitControls.update() calls object.lookAt(target) using the PREVIOUS camera.up
        // (which can end up nearly parallel to the new view direction, e.g. when switching to
        // the XY top view), a degenerate case for lookAt that leaves the orientation inconsistent
        // with the computed framing.
        this.camera.up.copy(camUp);
        this.camera.position.copy(target).addScaledVector(dir, -dist);
        this.controls.target.copy(target);
        this.controls.update();
    }

    /** Zooms the camera in/out along the current line to the target, without changing view. */
    zoom(factor) {
        if (!this.camera || !this.controls) return;
        const offset = new THREE.Vector3().subVectors(this.camera.position, this.controls.target);
        const newLen = Math.max(0.5, offset.length() * factor);
        offset.normalize().multiplyScalar(newLen);
        this.camera.position.copy(this.controls.target).add(offset);
        this.controls.update();
    }

    /** Step equivalent to N mouse-wheel "ticks" on OrbitControls: its getZoomScale() is
     *  Math.pow(0.95, zoomSpeed) per tick -- reproduced here so the button gives a step that
     *  feels like manually orbiting/zooming the scene, just more noticeable than a single tick. */
    _wheelStepScale(ticks = 3) {
        const zoomSpeed = (this.controls && this.controls.zoomSpeed) || 1.0;
        return Math.pow(0.95, zoomSpeed * ticks);
    }

    zoomIn() { this.zoom(this._wheelStepScale()); }
    zoomOut() { this.zoom(1.0 / this._wheelStepScale()); }

    /**
     * "Window" zoom (AutoCAD/CAD-style): takes a rectangle in normalized screen coordinates
     * (NDC, -1..1, Y up) and reframes the camera onto it, keeping the current view direction
     * (doesn't jump to ISO/XY/etc, just zooms/recenters onto the chosen crop). Intersects the
     * rectangle's 4 corner rays with the focal plane (perpendicular to the camera direction,
     * passing through the current target) and scales the camera-target distance by the fraction
     * of the screen the rectangle occupied -- a simpler heuristic approximation than
     * _frameBounds() (doesn't reproject corners, just scales the current distance by the
     * rectangle's screen fraction).
     * @param {{x1:number,y1:number,x2:number,y2:number}} rectNDC
     */
    zoomToWindow(rectNDC) {
        if (!this.camera || !this.controls) return;

        const fraction = Math.max(Math.abs(rectNDC.x2 - rectNDC.x1), Math.abs(rectNDC.y2 - rectNDC.y1)) / 2.0;
        if (fraction < 0.01) return; // click without a real drag -- ignore

        const dir = new THREE.Vector3();
        this.camera.getWorldDirection(dir);
        const plane = new THREE.Plane().setFromNormalAndCoplanarPoint(dir, this.controls.target);

        const raycaster = new THREE.Raycaster();
        const corners = [
            { x: rectNDC.x1, y: rectNDC.y1 }, { x: rectNDC.x2, y: rectNDC.y1 },
            { x: rectNDC.x1, y: rectNDC.y2 }, { x: rectNDC.x2, y: rectNDC.y2 },
        ];
        const points = [];
        corners.forEach(c => {
            raycaster.setFromCamera(c, this.camera);
            const pt = new THREE.Vector3();
            if (raycaster.ray.intersectPlane(plane, pt)) points.push(pt);
        });
        if (points.length === 0) return;

        const newTarget = points.reduce((acc, p) => acc.add(p), new THREE.Vector3()).multiplyScalar(1.0 / points.length);

        const oldOffset = new THREE.Vector3().subVectors(this.camera.position, this.controls.target);
        const oldDistance = oldOffset.length();
        const newDistance = Math.max(0.5, oldDistance * fraction);
        const offsetDir = oldOffset.normalize();

        this.controls.target.copy(newTarget);
        this.camera.position.copy(newTarget).add(offsetDir.multiplyScalar(newDistance));
        this.controls.update();
    }
}
