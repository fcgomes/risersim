# Mapa de classes da análise estática do ANFLEX real (`trunk/src`)

> Documento de referência, produzido lendo o código-fonte real do ANFLEX (read-only, `trunk/src`) para embasar a modernização do `risersim`. Cobre: fluxo de controle da análise estática, hierarquia de elementos/formulação corrotacional, geometria inicial, cargas e movimento do topo da linha (FPSO).

## Achado mais importante: a estática roda em duas fases

O ANFLEX real **nunca** tenta resolver a estática "de verdade" (com todas as tolerâncias reais) enquanto ainda depende de regularização numérica. Ele separa isso em duas fases sequenciais, orquestradas por `cAnflexAnalysis::solve()` (`anflex_analysis.cpp:426-448`):

1. **`solve_assembly()`** (`anflex_analysis.cpp:167-230`) — um pré-solve "de montagem": cria um `cStaticIntegrator` com `m_have_artificial_stiffness=true` (linhas 191-195), resolve até convergir (ou até esgotar iterações) só para achar uma configuração de equilíbrio aproximada, apoiado na rigidez artificial. Ao final, força `m_have_artificial_stiffness=false` (linha 223) — a rigidez artificial nunca mais é usada depois disso.
2. **`solve_static()`** (`anflex_analysis.cpp:234-293`) — a análise estática real, sem rigidez artificial, partindo do estado (nós/deslocamentos) já deixado pela fase 1.

Isso importa porque o `risersim` hoje faz só **uma fase**: a rigidez artificial decai exponencialmente (`BETA=1.25`) dentro do mesmo orçamento de iterações (40) do primeiro passo de carga real. A "muleta" precisa desaparecer antes do solver ter de fato convergido, no mesmo fôlego em que a carga real está sendo aplicada — muito mais frágil do que garantir uma base sólida (fase 1, sem pressa, só com a muleta) antes de sequer tentar a fase 2 sem ela. **Essa é a explicação mais direta e evidenciada (não especulativa) encontrada até agora para por que o ANFLEX converge em malhas finas/flexíveis como o Exemplo_01a e o risersim não.**

> **Atualização (implementação testada — ver seção "Resultado do teste das duas fases" no final):** implementamos esse padrão de duas fases no risersim e **não resolveu** a divergência do Exemplo_01a real. A fase 1 (assembly) diverge exatamente na mesma iteração (~20) de sempre, independente de ter rigidez artificial disponível em todos os passos — porque a instabilidade acontece dentro do **próprio passo 1**, antes de qualquer outro passo entrar em jogo. Isso não invalida o mapeamento acima (ele é fiel ao código real do ANFLEX), mas mostra que reproduzir só a *orquestração* de duas fases não é suficiente — falta algo na própria formulação do elemento (ver detalhes no final).

## Camada de controle (Strategy pattern já usado no ANFLEX real)

| Classe | Papel | Observações |
|---|---|---|
| `cAnflexAnalysis` | Orquestrador | `solve()` → `solve_assembly()` → `solve_static()` (`anflex_analysis.cpp:426-448`) |
| `cStaticAnalysis` / `cDynamicAnalysis` | Loop de passos | **Paralelas, sem base comum** — `analysis.h` é um stub vazio (`analysis.h:14-17`). Cada uma guarda um `cIntegrator*` e um `cNonlinearSolver*`. Loop em `cStaticAnalysis::analyze()` (`static_analysis.cpp:53-120`): `for i in 1..m_num_steps`, `current_time = i*(m_analysis_time/m_num_steps)`, `setup_new_step`, `solve_current_step`, checa `convergence_test->stop()` |
| `cNonlinearSolver` (abstrata) → `cNewtonRaphson` | Iteração NR | `nonlinear_solver.h:22` (pure-virtual `solve_current_step`); única implementação `newton_raphson.h:23`. `iteration()` (`newton_raphson.cpp:51-73`): `setup_new_iteration → form_global_stiffness_matrix → form_global_load_vector → solve → update`. **Sem line search** (passo cheio, igual ao que já portamos no risersim) |
| `cIntegrator` (abstrata) → `cStaticIntegrator`/`cDynamicIntegrator` | Monta K/F | `integrator.h:32`; monta rigidez/carga, aplica condições de contorno e rigidez artificial |
| `cConvergenceTest` | Convergência | **6 critérios independentes** (ver seção própria abaixo) |
| `cLinearSolver` (abstrata) → Skyline/CSR/MKL/Pardiso | Resolve o sistema linear | Plugável via `create_solver(eSolverType)` (`anflex_analysis.cpp:71-101`); `cLinearSOE` empacota solver + armazenamento da matriz. Nenhum código específico de solver vaza pro loop de NR |

## Camada de modelo/domínio

- **`cDomain`** — container raiz: `cElement**`, `cNode**`, `vector<cStruct*>` (sub-estruturas: linha, stinger, tendão, boia...), `m_prescribed_dofs` (map) / `m_restrained_dofs` (vector) — **dois mecanismos de contorno distintos** (prescrito = valor-alvo com penalidade; restringido = fixo/zero), `m_nodal_forces`/`m_static_loads`/`m_dynamic_loads`, `sGlobalData` (gravidade, densidade/viscosidade da água, nível do mar, solo)
- **`cNode`** (`node.h:44-481`) — bem mais rico que o `Node3D` do risersim: além de posição (`sCoord m_coord`), guarda `m_transl_desl`/`m_rot_desl` (+ versões "old" e delta), velocidades/acelerações, `m_transf_mt`/`m_transp_transf_mt` (matriz de transformação nodal considerando a rotação total acumulada, construída por `gen_mat_3d()`), contadores de GDL (`m_num_transl`/`m_num_rot`), estado ambiental (`m_surround`, altura de onda local), lista de variáveis de resultado
- **`cElement`** (abstrata, `element.h:86-694`) — interface virtual rica:
  - Geometria/cinemática: `update_cos`, `update_deformed_length`, `update_transformations_matrices`, `calc_deformation`
  - Rigidez: `calc_stiff_mt`, `calc_linear_stiff_mt`
  - Massa/amortecimento: `calc_mass_vector`, `get_mass_matrix` (consistente, opcional), `calc_damping_matrix`
  - Forças: `calc_internal_forces`, `calc_load` (cargas ambientais), `get_inertia_forces`, `get_damping_forces`
  - Rigidez artificial: `get_artificial_stiffness_vector/matrix`
  - Resultados: `calc_results`, `calc_element_result_vars`, `get_available_variables`

**Hierarquia concreta de elementos:**
```
cElement
├─ cBar ── cBeam ── {cBendingRestrictorBeam, cFlexiblePipeBeam}
│       └─ cTruss ── cWinch
├─ cSixDOFElement ── cBeamSD ── cBeamTL
├─ cContactElement ── {cInnerContactElement, cOuterContactElement}
├─ cScalar
├─ cRigidBodyElement
├─ cBuoyElement
├─ cTimoshenkoPipeBeam    (independente, massa consistente opcional)
└─ cMultilayerPipeBeam    (independente, massa consistente opcional)
```

`cBeam` (a que interessa para o riser): viga corrotacional **Euler-Bernoulli** (`calc_linear_stiff_mt`, `beam.cpp:1208-1272` — termos `12EI/L³`, `6EI/L²`, `4EI/L`, `2EI/L`, sem correção de cisalhamento) **+** rigidez geométrica estilo **Crisfield** (`calc_local_nonlinear_stiff_mt`, `beam.cpp:321-503`, termos P-Δ a partir das forças locais de extremidade). Massa **concentrada** (lumped, diagonal) herdada de `cBar::calc_mass_vector` (`bar.cpp:362-427`) — sem matriz de massa consistente para `cBeam` (`m_consistent_mass=false` por padrão).

## Geometria inicial / formulação corrotacional — o ponto que travou nosso warm start com MoorPy

Esse é o mecanismo mais importante de entender antes de qualquer tentativa futura de "chute inicial" externo:

1. A tríade nodal de referência é fixada **uma única vez**, na montagem do modelo (`cBeam::nodes_ready()`, `beam.cpp:1002-1017` → `calc_init_rot_mt`, `beam.cpp:301-317`), a partir de `curvature1`/`curvature2`.
2. **`curvature1`/`curvature2` são entrada manual do usuário no `.dat`** (`model_builder_dat.cpp`, colunas `curvature_1`/`curvature_2` lidas diretamente da tabela de entrada) — representam pré-curvatura de fabricação (ex.: tubo bobinado/"reeled"), tipicamente zero para um riser reto. **Não são derivadas da geometria 3D dos nós.**
3. **Confirmado por leitura completa do código: não existe no ANFLEX nenhum mecanismo de "ajuste de curva" (curve fitting) que calcule rotações nodais consistentes a partir de uma sequência arbitrária de posições 3D.** A única outra menção a "curvatura" no código é pós-processamento (`calc_curvature_radius`/`calc_curvatures`, calculadas a partir de momentos e rotações já resolvidas, para reportar resultado — não para inicializar o solver).
4. A cada iteração, um referencial corrotacional **separado** ("ghost frame", `cElement::calc_transformation_mt`, `element.cpp:215-249`) é recalculado a partir da posição **atual** dos nós — seu eixo local X segue a direção da corda ao vivo.
5. A deformação de flexão reportada (`calc_deformation`, `beam.cpp:1177-1204`) é o **descompasso** entre a tríade fixa (+ rotação total resolvida, `m_rot_desl`) e esse referencial baseado em posição atual.

**Consequência prática**: mover os nós para uma nova curva (ex.: uma geometria de equilíbrio calculada externamente por outra ferramenta) sem girar a orientação nodal junto quebra exatamente essa consistência — e gera momento fletor espúrio enorme. Isso vale tanto para o ANFLEX real quanto para o risersim (mesma formulação corrotacional); não é um bug de tradução, é uma propriedade da formulação. Qualquer "warm start" futuro precisaria calcular rotações nodais iniciais consistentes com a nova geometria (algo como um `calc_init_rot_mt` aplicado à geometria alvo) — o ANFLEX não tem essa peça pronta para reaproveitar.

## Cargas e movimento do topo da linha (FPSO)

- **Peso próprio/empuxo/arrasto de corrente não são um vetor de carga separado** — entram direto na formulação de força interna de cada elemento (`bar.cpp`/`beam.cpp`, usando `sGlobalData::m_gravity`, densidade do material e do fluido externo), aplicados em **magnitude total desde a 1ª iteração**. Existe um mecanismo de rampa gravitacional no código (`sLoadData::m_has_gravitational_load`/`m_gravitational_factor`) mas está **morto** — a struct é zerada e nunca preenchida em nenhum lugar de `trunk/src`.
- Cargas concentradas e de boia: vetor separado, montado só na iteração 1 de cada passo (`add_nodal_load`, `integrator.cpp:237`; `add_buoy_load`, `integrator.cpp:214`).
- **Movimento do topo = condição de contorno com penalidade, não um novo grau de liberdade.** Pipeline:
  1. Parsing (`model_builder_dat.cpp::load_static_loads`): lê `transferred_nodes`, `movement_center`, `refsys_angle`, `Amplitudes`/`Functions` por GDL, monta um `AnfLoadings::cTimeSeries`, empacota num `cLoad {node, cTimeSeries*, coords_ini}`.
  2. Cinemática (`cTimeSeries::get_movement`): a cada tempo pseudo, calcula `Amplitude × Função(t)` por GDL no `movement_center`, aplica rotação rígida (Euler/Rodrigues) para o ponto relativo do nó, rotaciona pelo `refsys_angle` — produz a posição-alvo.
  3. Aplicação a cada iteração (`cIntegrator::set_load_dofs`, `integrator.cpp:70-117`): calcula a posição-alvo, compara com a posição atual do nó, e — **só para GDLs também marcados em `m_prescribed_dofs`** — escreve um resíduo `(alvo - atual) × m_big_number` e soma `m_big_number` na diagonal da rigidez (`apply_load_stiffness`). Técnica clássica de penalidade/mola rígida, não eliminação de GDL nem elemento rígido acoplado.
- **"Passo de carga" no ANFLEX ≈ avançar o tempo pseudo usado para avaliar a função de movimento do topo** — não é uma rampa uniforme de peso/empuxo/corrente (que já entram em força total desde o início). Isso é bem diferente do que o risersim assume hoje (`load_factor` escalando tudo igualmente).
- Estático vs. dinâmico: o campo `static_movement_type`/`dynamic_movement_type` não é de fato ramificado em código — a diferença real é que a estática avalia a série temporal numa rampa até um valor final fixo (offset estático), enquanto a dinâmica usa uma tabela RAO (`dynamic_movement_type: equivalent_harmonic`) para gerar movimento oscilatório de verdade ao longo do tempo físico do integrador.

## Lacunas identificadas no risersim atual

| Aspecto | ANFLEX real | risersim hoje |
|---|---|---|
| Fases da estática | 2 (assembly c/ rigidez artificial → static sem) | 1 (rigidez artificial decai dentro do mesmo passo) |
| Critérios de convergência | 6 (translação, rotação, força, momento, força/momento desbalanceados) | 2 (translação, rotação) |
| Hierarquia de elementos | `cElement` abstrata + ~12 subclasses concretas | 1 tipo hardcoded (`CorotationalBeam3D`) |
| Movimento do topo | Penalidade genérica (série temporal 6 GDL, `refsys_angle`, `movement_center`) | `VesselOffset::Near/Far` (escalar simplificado) |
| Rampa de carga | Peso/empuxo/corrente em 100% desde a 1ª iteração; "passo" = tempo da função de movimento do topo | `load_factor` escala tudo igualmente por passo |
| Solver linear | Abstração plugável (Skyline/CSR/MKL/Pardiso) | `Eigen::SparseLU` fixo |
| Container do modelo | `cDomain` (coleções por conceito de domínio, `new`/`delete` manual) | `RiserModel` (vetores simples) |

## Recomendação de modernização (arquitetura + bibliotecas)

O objetivo de "modernizar" aqui é reaproveitar a **separação em camadas** que o ANFLEX já validou em produção — não reinventar a física, só trazer a estrutura comprovada para C++ atual (RAII, smart pointers, Eigen) em vez de `new`/`delete` manual e matrizes escritas à mão:

- **`LinearSolver`/`LinearSOE`**: manter como interface abstrata (como o ANFLEX já faz), mas com backend em **Eigen** — `Eigen::SparseLU` como default (já em uso no risersim), com `Eigen::ConjugateGradient` como alternativa para modelos grandes. Sem necessidade de reimplementar skyline/CSR à mão.
- **`NonlinearSolver`/`NewtonRaphson`**: manter o padrão puro (sem line search) já portado fielmente no risersim.
- **`Integrator`/`StaticIntegrator`/`DynamicIntegrator`**: separar de `NewtonRaphson` a responsabilidade de "montar K e F para esta iteração" — hoje isso está entranhado numa função só (`solve_catenary_static`). Essa separação **habilita naturalmente** a Fase 1 do roadmap (duas fases assembly/static), porque cada fase vira só uma configuração diferente do mesmo `Integrator` (com/sem rigidez artificial), reaproveitando o mesmo `NewtonRaphson`.
- **`ConvergenceTest`**: portar os 6 critérios (já sabemos ler `tolerance`/`max_unbalanced`/`convergence_criterium` do AML, ver `risersim/docs/opcoes_bibliotecas_opensource.md`).
- **`Element`/`BeamElement`**: base abstrata enxuta (rigidez/massa/força interna/resultados) + `BeamElement` como wrapper fino sobre a matemática já validada do `CorotationalBeam3D` — sem alterar essa matemática.
- **`Model`/`Domain`**: container com `std::vector<std::unique_ptr<Node>>`/`std::vector<std::unique_ptr<Element>>`, sem `new`/`delete` manual.
- **`PrescribedMotion`**: generalizar `VesselOffset` para o padrão de penalidade do ANFLEX (posição-alvo por série temporal + mola de rigidez grande) — mais fiel para representar o caso real "Cross" do curso (offset de FPSO com `refsys_angle`/`movement_center`).

## Roadmap faseado (priorizado pelo que mais provavelmente resolve a divergência primeiro)

1. **Duas fases estáticas (assembly → static)** — prioridade máxima. Ataca a causa raiz com evidência real do ANFLEX, não especulação. Implementar dentro do `static_analysis.cpp` atual, sem refatorar toda a arquitetura ainda: (a) fase "assembly" só com rigidez artificial, rodando até convergir ou até um número generoso de iterações; (b) fase "static" limpa (rigidez artificial desligada), partindo do resultado de (a). Testar no Exemplo_01a real.
2. **`ConvergenceTest` com 6 critérios** (hoje só 2), usando campos já extraídos do AML.
3. **Refactor OOP** (`Element` abstrato + `BeamElement` wrapper, `Integrator` abstrato, `Model`/`Domain`) — traz a separação de camadas do ANFLEX real para dentro do risersim, sem tocar a matemática do elemento. O passo 1 já força um primeiro movimento nessa direção (separar integrator do loop de NR).
4. **`PrescribedMotion` genérico por penalidade** (substituindo `VesselOffset::Near/Far`).
5. **Abstração de solver linear plugável** (`Eigen::SparseLU` default, `Eigen::ConjugateGradient` como alternativa para modelos grandes).

## Resultado do teste das duas fases (Passo 3 implementado e testado)

Implementado em `risersim/src/static_analysis.cpp`: `StaticAnalysis::solve()` agora roda `solve_catenary_static()` duas vezes — fase 1 ("assembly") com `ArtificialStiffnessMode::EveryStep` (rigidez artificial disponível, decaindo por iteração, em **todos** os passos de carga, não só o primeiro) usando `load_steps`/`max_iter_per_step` normais; fase 2 ("static") com `ArtificialStiffnessMode::Never`, carga total num único passo, partindo do estado que a fase 1 deixou. A flag mora no `StaticIntegrator` (Passo 2), então as duas fases reusam a mesma função sem duplicar lógica.

**Resultado no Exemplo_01a real: não resolveu.**

- **Fase 1 (assembly)** diverge com os **resíduos idênticos, bit-a-bit**, aos do modo de fase única de sempre (`1.2791e+03 → 1.0447e+03 → 6.4445e+07 → 6.9411e+10` nas iterações 0/10/20/30) — porque a instabilidade acontece **dentro do próprio passo 1**, e nenhum outro passo chega a ser tentado. Disponibilizar rigidez artificial em passos futuros não muda nada sobre o que acontece no passo 1, já que ele é sempre o primeiro a rodar.
- **Fase 2 (static)**, partindo do estado corrompido que a fase 1 deixou (deslocamentos absurdos, resíduo de fase 1 chegou a ~7×10¹⁰), começa **ainda pior** (resíduo inicial de 1,19×10¹²) e também diverge.
- O caso sintético de teste (catenária simples, sem rotação livre — ver `risersim/tests/test_static_analysis.cpp`) continua convergindo normalmente com as duas fases, com resultado praticamente idêntico ao de antes (diferença de ~0,0001% na tração final) — confirma que a mudança não quebrou o caminho que já funcionava, só não resolveu o que estava quebrado.

**O que isso ensina**: o mapeamento da arquitetura de duas fases do ANFLEX é fiel ao código-fonte real (seção acima), mas replicar só a *orquestração* (rodar duas vezes, com/sem rigidez artificial) não é suficiente — a causa raiz não está em "quantas vezes" ou "em quantos passos" a rigidez artificial fica disponível, está em **algo que acontece dentro da própria iteração ~20 do passo 1**, antes de qualquer segunda fase ter chance de ajudar. Isso descarta com evidência forte a hipótese de que uma orquestração diferente do solver resolveria o problema, e reforça — pela segunda vez nesta investigação (a primeira foi o warm start do MoorPy) — que a causa está na formulação/implementação do próprio elemento de viga corrotacional (`element_beam.cpp`), não em como ele é orquestrado.

**Próximo passo recomendado**: voltar à investigação original, ainda pendente — isolar o `CorotationalBeam3D` num teste mínimo (2-3 elementos, mesma proporção de malha fina/EI baixo/quase-vertical do Exemplo_01a) para observar diretamente o que acontece com `local_geometric_stiffness()`/`transformation_matrix()`/`local_material_stiffness()` por volta da iteração 15-20, fora do contexto de 500 elementos acoplados. Um candidato concreto a investigar nesse teste isolado: comparar a formulação de rigidez geométrica (`local_geometric_stiffness`) do risersim contra a formulação de Crisfield do ANFLEX real (`cBeam::calc_local_nonlinear_stiff_mt`, `beam.cpp:321-503`) — são abordagens diferentes para o mesmo problema (rigidez geométrica não-linear), e uma diferença ali é a suspeita mais concreta ainda não descartada nesta sessão.

## Ver também

- `risersim/docs/opcoes_bibliotecas_opensource.md` — levantamento de bibliotecas open-source (Project Chrono, MAP++, MoorDyn-C, MoorPy) e o resultado da tentativa de warm-start com MoorPy (que motivou este documento).
