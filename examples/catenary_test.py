import sys
import os

# Adiciona a pasta de build ao sys.path para carregar o módulo risersim
sys.path.append(os.path.join(os.path.dirname(__file__), "..", "build", "lib", "Release"))

try:
    import risersim
    print("✅ Módulo riserSim (C++17 + Eigen 3.4.0 + pybind11) carregado com sucesso!")
except ImportError as e:
    print(f"❌ Erro ao importar risersim: {e}")
    sys.exit(1)

# 1. Propriedades de Seção de Riser Flexível (Exemplo 4 polegadas)
props = risersim.BeamMaterialProps()
props.E = 2.1e11       # 210 GPa (Aço)
props.G = 8.0e10       # 80 GPa
props.A = 0.015        # 0.015 m^2
props.IY = 5.0e-5      # 5e-5 m^4
props.IZ = 5.0e-5      # 5e-5 m^4
props.J = 1.0e-4       # 1e-4 m^4
props.rho = 25.0       # 25 kg/m

# 2. Criar Nós da Linha de Catenária
n1 = risersim.Node3D(1, 0.0, 0.0, 0.0)      # Nó do topo (plataforma)
n2 = risersim.Node3D(2, 50.0, 0.0, -50.0)   # Nó intermediário
n3 = risersim.Node3D(3, 100.0, 0.0, -100.0) # Nó do fundo (ancoragem)

# Engastar nó do topo (Nó 1) e nó do fundo (Nó 3)
n1.fix_dofs(True, True, True, True, True, True)
n3.fix_dofs(True, True, True, True, True, True)

# 3. Criar Elementos de Viga Corrotacional
e1 = risersim.CorotationalBeam3D(1, n1, n2, props)
e2 = risersim.CorotationalBeam3D(2, n2, n3, props)

e1.tension_effective = 150000.0 # 150 kN de tração efetiva
e2.tension_effective = 120000.0 # 120 kN de tração efetiva

# 4. Iniciar Análise Estática
analysis = risersim.StaticAnalysis()
analysis.nodes = [n1, n2, n3]
analysis.elements = [e1, e2]

print(f"Comprimento inicial Elem 1: {e1.current_length():.2f} m")
print(f"Comprimento inicial Elem 2: {e2.current_length():.2f} m")
print("✅ Teste de instanciação do riserSim concluído!")
