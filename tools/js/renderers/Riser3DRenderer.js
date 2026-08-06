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

        this.scene.background = new THREE.Color(0x1e1e2e);

        // far=2000/posição inicial são só um chute razoável para a escala típica do
        // Exemplo_01a -- CameraViewController.setView() reenquadra e ajusta o far plane pra
        // escala real do modelo assim que os dados carregam (ver app.js), então isso nunca
        // fica sendo o que o usuário vê de fato para um modelo de escala diferente.
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
        this.scene.add(this.envGroup);

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
     * @param {number|null} seabedDepth - Cota Z do fundo do mar (null/ausente = não desenha o plano)
     * @param {number|null} waterSurfaceZ - Cota Z da superfície do mar (null/ausente = não desenha o plano)
     */
    renderStep(step, colormap = 'Jet', scalarRange = { min: 0, max: 10 }, currentTheme = 'dark', scalarField = 'tension', seabedDepth = null, waterSurfaceZ = null) {
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

        const bounds = this.computeSceneBounds(nodes, seabedDepth, waterSurfaceZ);
        this.updateBoundingBox(bounds, currentTheme);
        this.updateEnvironmentPlanes(bounds, seabedDepth, waterSurfaceZ, currentTheme);

        // Raio do tubo proporcional ao tamanho real do modelo -- um valor fixo (0.6, calibrado
        // para a escala do Exemplo_01a, ~130m) vira uma linha de espessura sub-pixel, quase
        // invisível, num modelo bem maior (ex. Exemplo_02a, ~1800m) -- mesmo com a câmera
        // corretamente enquadrada, a linha em si fica difícil demais de enxergar/interpretar
        // (mapa_classes_anflex_estatica.md). 0.0045 * 130 ≈ 0.585, preservando a aparência
        // original para modelos da escala do Exemplo_01a.
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
     * Calcula a caixa envolvente da cena (nós do passo + fundo do mar + superfície), já com as
     * margens de exibição aplicadas -- fonte única usada tanto pelo wireframe da bounding box
     * quanto pelos planos ambientais, para que os planos fiquem sempre exatamente do tamanho da
     * caixa, nunca maiores/menores de forma independente.
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

        // Estende o alcance vertical (Y do Three.js = Z do risersim/profundidade) para sempre
        // cobrir o fundo do mar e a superfície -- não só a extensão dos nós deste passo -- assim
        // a caixa (e os planos) nunca "escondem" parte da lâmina d'água real. `1.0e5` descarta o
        // sentinela usado quando o solo está desligado via `environmental.seabed.enabled=false`
        // (empurrado a -1e6, ver main_test.cpp), que não representa uma posição real de solo.
        if (Number.isFinite(seabedDepth) && Math.abs(seabedDepth) < 1.0e5) {
            minY = Math.min(minY, seabedDepth);
            maxY = Math.max(maxY, seabedDepth);
        }
        if (Number.isFinite(waterSurfaceZ)) {
            minY = Math.min(minY, waterSurfaceZ);
            maxY = Math.max(maxY, waterSurfaceZ);
        }

        // Margens proporcionais ao span de cada eixo (com piso mínimo), em vez de uma distância
        // fixa -- um valor fixo (ex. marginPerpendicular=35 sempre) fica bem em modelos com
        // espalhamento horizontal razoável, mas domina o quadro quando um eixo é naturalmente
        // pequeno (ex. riser quase vertical, com pouco desvio lateral): a vista de topo (XY)
        // acabava mostrando uma caixa enorme e vazia em volta de uma linha minúscula.
        const spanX = maxX - minX, spanY = maxY - minY, spanZ = maxZ - minZ;
        const boxMargin = Math.max(2.0, Math.min(spanX, spanY) * 0.08);
        const marginPerpendicular = Math.max(5.0, spanZ * 0.25); // Distância maior no eixo perpendicular (Y)

        minX -= boxMargin; maxX += boxMargin;
        minY -= boxMargin; maxY += boxMargin;
        minZ -= marginPerpendicular; maxZ += marginPerpendicular;

        return { minX, maxX, minY, maxY, minZ, maxZ };
    }

    /**
     * @param {{minX,maxX,minY,maxY,minZ,maxZ}|null} bounds - ver computeSceneBounds()
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
     * Desenha os planos horizontais de referência ambiental: fundo do mar e superfície do mar.
     * Ambos semitransparentes, com o mesmo footprint X/Z (planta) da bounding box -- nunca maior
     * nem menor que ela -- na cota Z correta de cada um.
     * @param {{minX,maxX,minY,maxY,minZ,maxZ}|null} bounds - ver computeSceneBounds()
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

        // Adiciona um plano horizontal (área semitransparente) + um contorno retangular sólido na
        // borda, na cota Y dada. O contorno existe porque um plano fino visto quase de perfil
        // (ex. a vista ISO, com a câmera bem acima do centro da caixa olhando para baixo, vê o
        // plano de cima quase de raspão) fica praticamente invisível como área preenchida --
        // mas a borda, sendo uma linha (sem espessura a esconder), continua visível de qualquer
        // ângulo, igual ao wireframe da bounding box.
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

        // Descarta o sentinela usado quando o solo está desligado (environmental.seabed.enabled=
        // false empurra o fundo pra -1e6, ver main_test.cpp) -- nesse caso não há plano de fundo
        // real pra desenhar.
        if (Number.isFinite(seabedDepth) && Math.abs(seabedDepth) < 1.0e5) {
            addPlane(seabedDepth, seabedColor, 0.4, 1);
        }
        if (Number.isFinite(waterSurfaceZ)) {
            addPlane(waterSurfaceZ, waterColor, 0.28, 2);
        }
    }
}
