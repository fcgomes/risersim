import { ColorMapService } from '../services/ColorMapService.js';

/**
 * Riser3DRenderer.js
 * Encapsula a cena Three.js, iluminação, malha do mar/fundo, tubos do riser e bounding box.
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
        this.boundingBoxWireframe = null;

        this.initEngine();
    }

    initEngine() {
        const width = this.canvas.clientWidth || window.innerWidth || 800;
        const height = this.canvas.clientHeight || window.innerHeight || 600;

        this.renderer = new THREE.WebGLRenderer({ canvas: this.canvas, antialias: true });
        this.renderer.setSize(width, height);
        this.renderer.setPixelRatio(window.devicePixelRatio);

        this.scene.background = new THREE.Color(0x1e1e2e);

        this.camera = new THREE.PerspectiveCamera(45, width / height, 0.1, 2000);
        this.camera.position.set(60, 50, 160);

        this.controls = new THREE.OrbitControls(this.camera, this.renderer.domElement);
        this.controls.enableDamping = true;
        this.controls.dampingFactor = 0.05;
        this.controls.target.set(60, -50, 0);

        // Iluminação
        const ambientLight = new THREE.AmbientLight(0xffffff, 0.8);
        this.scene.add(ambientLight);

        const dirLight = new THREE.DirectionalLight(0xffffff, 1.0);
        dirLight.position.set(100, 200, 100);
        this.scene.add(dirLight);

        // Adiciona os grupos
        this.scene.add(this.riserGroup);
        this.scene.add(this.nodesGroup);

        // Loop de Renderização 60 FPS
        const animate = () => {
            requestAnimationFrame(animate);
            this.controls.update();
            this.renderer.render(this.scene, this.camera);
        };
        animate();

        // Redimensionamento de Janela
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
     * Renderiza o estado 3D do riser em um determinado passo
     * @param {SimulationStep} step 
     * @param {string} colormap 
     * @param {{min: number, max: number}} scalarRange 
     * @param {'dark'|'light'} currentTheme 
     * @param {string} scalarField - 'tension' | 'moment' | 'curvature' | 'vonmises' | 'mbr'
     */
    renderStep(step, colormap = 'Jet', scalarRange = { min: 0, max: 10 }, currentTheme = 'dark', scalarField = 'tension') {
        if (!step) return;

        // Limpa geometrias anteriores completamente
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

        this.updateBoundingBox(nodes, currentTheme);

        const outerRadius = 0.6;
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

    updateBoundingBox(nodes, currentTheme = 'dark') {
        if (!nodes || nodes.length === 0) return;
        if (this.boundingBoxWireframe) this.scene.remove(this.boundingBoxWireframe);

        let minX = Infinity, maxX = -Infinity;
        let minY = Infinity, maxY = -Infinity;
        let minZ = Infinity, maxZ = -Infinity;

        nodes.forEach(n => {
            minX = Math.min(minX, n.x); maxX = Math.max(maxX, n.x);
            minY = Math.min(minY, n.z); maxY = Math.max(maxY, n.z);
            minZ = Math.min(minZ, n.y); maxZ = Math.max(maxZ, n.y);
        });

        const boxMargin = 5.0;
        const marginPerpendicular = 35.0; // Distância maior no eixo perpendicular (Y)

        minX -= boxMargin; maxX += boxMargin;
        minY -= boxMargin; maxY += boxMargin;
        minZ -= marginPerpendicular; maxZ += marginPerpendicular;

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
}
