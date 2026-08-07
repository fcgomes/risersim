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

## Isolamento bem-sucedido: existe um limiar preciso de tamanho de segmento (achado novo e forte)

Implementado `risersim/tests/diag_isolated_segment.cpp` (+ target `risersim_diag_isolated_segment` no CMake): extrai uma fatia contígua de N elementos da malha **real** do Exemplo_01a (mesmo material, mesmo espaçamento ~1m, mesma orientação quase-vertical), fixa os dois nós das pontas na posição real (lida do JSON) e deixa os nós internos totalmente livres (translação + rotação). Sem solo (leito marinho empurrado a 1.000.000 m de distância — contato impossível), sem os outros ~490 elementos do modelo completo.

**Resultado: existe um limiar nítido e monotônico entre 8 e 9 elementos.**

| Elementos | Resultado |
|---|---|
| 3, 4, 5, 6, 7, 8 | ✅ Converge nos 11 passos, tipicamente em 2 iterações por passo |
| 9 | ❌ Falha no passo 8/11 |
| 10 | ❌ Falha no passo 6/11 (precisou de 36 iterações no passo 5, quase estourando o limite de 40) |
| 12 | ❌ Falha no passo 3/11 |
| 15 | ❌ Falha no passo 2/11 |
| 20, 40 | ❌ Falha já no passo 1/11 |
| 500 (modelo completo) | ❌ Falha catastroficamente no passo 1/11 (explosão de resíduo, não apenas lentidão) |

Testado também um segmento no **meio** da linha (elementos 120-129, não só no topo) — mesmo padrão de falha por volta do mesmo tamanho. Não é uma peculiaridade da região do topo.

**Isso é a confirmação mais direta e limpa desta investigação inteira**: a dificuldade de convergência é uma propriedade **intrínseca do elemento de viga corrotacional com este material/espaçamento** (EI=21700 N·m², elementos de ~1m), que **cresce com o comprimento da cadeia de elementos encadeados** — completamente independente de solo, contato, os outros ~490 elementos do modelo completo, ou estratégia de carregamento. Repare o padrão: cadeias curtas convergem rápido (2 iterações); a partir de 9 elementos, o número de iterações necessárias por passo cresce (30-36 iterações num passo só) até estourar o limite; a partir de ~20 elementos, já falha no primeiro passo. O comportamento "explosão catastrófica" do modelo completo (resíduo indo a 10¹⁰) parece ser uma versão mais extrema do mesmo problema de fundo, amplificada pelo acoplamento de 500 elementos.

**Próximo passo concreto**: com um caso reproduzível de 9-10 elementos (pequeno o suficiente para depurar manualmente), investigar diretamente por que o número de iterações necessárias cresce com o comprimento da cadeia — candidatos: (a) acúmulo de mal-condicionamento na matriz de rigidez geométrica ao longo da cadeia, (b) a rigidez artificial (escalada por `EA/L` médio, um único valor global) ficando cada vez menos adequada para regularizar uma cadeia mais longa e mais flexível como um todo, (c) comparar contra a formulação de Crisfield do ANFLEX real (`beam.cpp:321-503`), que pode se comportar de forma mais estável para cadeias longas por alguma razão ainda não identificada.

### Correção/refinamento: o trecho testado acima era artificialmente reto e sem folga

O usuário questionou o método: as duas pontas do trecho isolado ficam com deslocamento zero — isso é estranho, e qual trecho da linha (reto ou realmente suspenso/curvado) foi escolhido? Investigando:

- **O trecho do topo (elementos 1-N) é, na prática, perfeitamente reto na geometria de entrada**: corda entre as duas pontas fixadas é igual ao comprimento de arco somado, com folga de **0,000%** (ex.: 9 elementos → arco=9,0000 m, corda=9,0000 m). Isso é local a uma catenária suave de 500 elementos — em janelas de poucos metros ela é indistinguível de uma reta.
- Isso torna o teste engastado-engastado nas duas pontas um problema clássico de **"cabo reto sob peso próprio"**: sem nenhuma perturbação transversal inicial, a rigidez tangente para o modo de flambagem/vergamento fica quase singular (não há componente de rigidez geométrica perpendicular à corda quando ela está perfeitamente alinhada). Isso é justamente o tipo de problema que a rigidez artificial (`BETA=1.25`) tenta regularizar.
- **Teste da hipótese "só falta rigidez artificial em mais passos"**: reexecutado com `ArtificialStiffnessMode::EveryStep` (em vez de só no passo 1) nos mesmos tamanhos — **não resolveu**, e piorou ligeiramente (9 elementos passou a falhar no passo 10 em vez do passo 8). Descarta essa hipótese, de forma consistente com o resultado negativo do Passo 3 no modelo completo.
- **Também testada a alternativa "corrente pendurada de um único ponto"** (`fix_mode=1`: só a primeira ponta fixa, resto — inclusive a outra ponta do trecho — totalmente livre, sem a condição de contorno artificial da "corda sem folga" na outra ponta).
- **Busca pela região com mais curvatura real de toda a linha**: varrendo janelas de 10 elementos ao longo dos 500, a maior folga (curvatura genuína) encontrada foi **0,622%** nos elementos 272-281 (nós 272-282), perto da região de touchdown (z ≈ 0-2 m, perto do leito marinho) — ordens de grandeza mais curvado que o trecho do topo.
- **Reteste engastado-engastado nesse trecho realmente suspenso/curvado**:

| Elementos (região touchdown, 272 em diante) | Resultado |
|---|---|
| 5, 6, 7, 8, 9, 10, 11 | ✅ Converge nos 11 passos |
| 12 | ❌ Falha no passo 9/11 |
| 15 | ❌ Falha no passo 1/11 (resíduo já em ~10⁴) |
| 20 | ❌ Falha no passo 1/11 (resíduo ~10⁴) |

**Conclusão refinada**: o limiar **se desloca** para cima quando o trecho tem curvatura real (de 8→9 no trecho reto do topo para 11→12 no trecho curvado de touchdown) — ou seja, a condição de contorno/folga importa, e o trecho reto e sem folga era de fato um caso artificialmente mais difícil, como o usuário suspeitou. Mas o limiar de comprimento **não desaparece**: mesmo numa região genuinamente suspensa e curvada, cadeias de 12+ elementos engastados-engastados nas pontas reais ainda falham. Isso reforça — em vez de invalidar — a conclusão de que existe uma dificuldade de convergência que cresce com o comprimento da cadeia encadeada de elementos corrotacionais, só que o limiar exato depende de quanta curvatura/folga inicial o trecho já tem para "amortecer" a necessidade de grandes rotações incrementais.

## Causa raiz encontrada e corrigida: rotação total acumulada sendo usada como se fosse deformação local

O usuário perguntou o óbvio que faltava: "é estranho, por que o ANFLEX dá certo?" Se a dificuldade fosse mesmo intrínseca ao elemento de viga corrotacional, o ANFLEX real (mesma formulação de base) também falharia no Exemplo_01a — e não falha. Isso forçou comparar o código de verdade, não só a arquitetura em alto nível.

**O bug**: em [analysis.cpp](risersim/src/analysis.cpp) (antes da correção), a força interna de cada elemento era `F_int_elem = K_elem * u_elem`, onde `u_elem` usava `node->rot` — a rotação **total acumulada** de cada nó desde t=0 (em eixos globais, somada iteração a iteração) — diretamente como se fosse a rotação de flexão local. `K_elem` é uma matriz de viga linear (EI/L etc.), válida só para ângulos **pequenos e relativos à corda atual do elemento**. Alimentá-la com a rotação total acumulada (que inclui toda a reorientação de corpo rígido do trecho desde o início, não só a deformação elástica) produz um momento fletor fantasma que cresce com o quanto aquele nó já girou no espaço — e isso cresce com o comprimento da cadeia (mais elementos = mais variação de orientação acumulada ao longo da linha). Exatamente o padrão de limiar que medimos.

**Por que o ANFLEX real não tem esse problema**: ele mantém, para cada elemento, uma tríade de referência **fixa**, calculada uma única vez em t=0 (`calc_init_rot_mt`, `beam.cpp:301`). A cada iteração, ele recompõe a orientação atual de cada nó (`update_transformations_matrices`, `beam.cpp:1021`), constrói um referencial "ghost" do elemento pela formulação de Crisfield de rotação média (`calc_transformation_mt`, `element.cpp:215`), e extrai a rotação **local/deformacional** de cada nó como o mismatch entre a orientação do nó e esse ghost frame (`calc_relative_rotations`, `matrix.cpp:499`) — só essa rotação pequena entra na matriz de rigidez (`calc_fe`, `beam.cpp:1276`). Além disso, o ANFLEX compõe a rotação acumulada de cada nó com uma regra própria (`pseudo_sum`, `integrator.cpp:697`, composição via quaternion) em vez de somar os incrementos linearmente — necessário porque rotações 3D não comutam para ângulos grandes.

**Correção aplicada no risersim** (`element_beam.hpp/cpp`, `rotation_utils.hpp`, `analysis.cpp`, `static_analysis.cpp`, `dynamic_analysis.cpp`):
- `CorotationalBeam3D` ganhou `node1_init_triad`/`node2_init_triad`, calculadas uma vez no construtor a partir da corda inicial (sem entrada de pré-curvatura manual, as duas coincidem).
- Novo método `compute_corotational_forces()` replica fielmente o pipeline do ANFLEX real: recompõe a triade atual de cada nó, monta o ghost frame por rotação média (Crisfield), extrai a rotação local de cada nó por mismatch, e só então calcula `F_int = T^T (K_local · deformação_local)` e `K = T^T K_local T`.
- `rotation_utils.hpp` (novo) implementa `compose_rotations()`, equivalente ao `pseudo_sum` do ANFLEX (composição via matriz de rotação, matematicamente igual à composição via quaternion), substituindo a soma linear ingênua `node->rot[i] += ...` nos três pontos onde isso acontecia.

**Resultado**: reexecutados os mesmos testes de segmento isolado que antes falhavam a partir de 8-12 elementos —

| Elementos (topo, reto) | Antes da correção | Depois da correção |
|---|---|---|
| 9-20 | ❌ Falha | ✅ Converge |
| 40, 100, 200 | ❌ Falha | ✅ Converge |
| 250, 300 | (não testado) | ✅ Converge |
| 350, 400, 450 | (não testado) | ❌ Falha (novo limiar, bem mais alto) |

O limiar de convergência subiu de **~8 elementos para ~300-350 elementos** — mais de 30x. Isso confirma que o bug da rotação total-vs-local era a causa dominante da divergência.

## Limiar residual em ~303-304 elementos: causa identificada (decaimento da rigidez artificial), correção com contrapartida

Isolado o novo limiar com a mesma técnica de antes: **303 elementos converge, 304 falha** — transição abrupta, de um único elemento, igual ao padrão do limiar anterior (8→9).

Investigação:
- **Não é o heurístico de "eixo global menos alinhado" trocando de eixo**: existe uma troca de eixo em algum ponto da cadeia real (elemento 265), mas cadeias que incluem essa troca (ex.: 1-303) convergem normalmente — descartado como causa direta.
- **Não é o comprimento puro da cadeia por si só**: um trecho de 218 elementos perfeitamente reto (pós-touchdown, elementos 282-499, sem curvatura nenhuma) converge sem problema isolado.
- **É aproximadamente uma contagem total de elementos**, não uma posição específica: janelas de 304 elementos deslocadas para outras partes da cadeia real (ex.: elementos 50-353, ou 197-500) falham do mesmo jeito.
- **O resíduo não cresce devagar — ele fica estável por ~10 iterações e então explode** (ex.: 601→19.335→119.560 entre as iterações 10, 20 e 30). Isso bate exatamente com o decaimento da rigidez artificial `exp(-iter/1.25)`: por volta da iteração 8-10 ela já caiu a praticamente zero, e o sistema "sem ajuda" não consegue se estabilizar sozinho numa cadeia desse tamanho.

**Teste da hipótese**: desacelerado o decaimento (constante 5.0 em vez de 1.25) — o resíduo parou de explodir (ficou decrescendo lentamente em vez de crescer), e com mais iterações disponíveis (150-200) cadeias de até 500 elementos **sem solo** passaram a convergir de forma limpa, confirmando a causa.

**Mas há uma contrapartida real**: o mesmo decaimento mais lento faz cadeias curtas (que antes convergiam em 2 iterações) precisarem de bem mais iterações dentro do mesmo orçamento — chegaram a falhar em casos de 5-20 elementos que antes eram triviais. Ou seja, não é uma correção livre de custo; é um trade-off que precisa de um esquema adaptativo (ex.: escalar a constante de decaimento com o número de elementos/DOFs da cadeia, não um valor fixo global) para não trocar um problema pelo outro. **Revertido para 1.25** (comportamento original) até esse esquema ser projetado — documentado como próximo passo, não implementado ainda.

## Achado novo e mais urgente: o modelo completo (com solo/contato) piora, não melhora, com mais iteração/decaimento mais lento

Testado o modelo completo do Exemplo_01a (500 elementos + solo real) com a correção da rotação aplicada:
- Com os parâmetros padrão (11 passos, 40 iterações), ainda diverge — mas agora numa iteração mais tardia e com um padrão diferente do bug original.
- Com decaimento mais lento (5.0) e 200 iterações — a mesma combinação que fez a cadeia de 500 elementos **sem solo** convergir perfeitamente — o modelo **com solo** piora dramaticamente: o resíduo passa de ~10³ para ~10¹²-10¹⁴, muito pior que sem essas mudanças.

Isso é um sinal forte de que o **solo/contato** é agora a causa mais provável do que resta divergindo no modelo completo — não o comprimento da cadeia (já resolvido) nem a rigidez artificial isoladamente. Isso contradiz a investigação anterior (no início desta sessão) que tinha descartado solo/contato como causa — mas aquela investigação foi feita **antes** da correção da rotação, usando segmentos isolados muito curtos (onde o bug da rotação dominava qualquer efeito do solo). Com a causa dominante corrigida, o solo/contato pode estar reaparecendo como uma causa real, agora visível.

**Próximo passo recomendado**: isolar um segmento pequeno que inclua contato real com o solo (ex.: elementos perto do touchdown, 272-291, COM o solo habilitado desta vez em vez de empurrado para longe) e reproduzir a divergência num caso pequeno e depurável — a mesma metodologia que funcionou para os dois limiares anteriores.

## Causa real isolada: não é solo sozinho, é solo + corrente juntos

Seguindo o próprio roteiro acima, adicionado suporte a solo real e a corrente real (`environmental.current` do JSON: v=1,78 m/s, heading=270°, perfil de lei de potência) em `diag_isolated_segment.cpp`, e testados isoladamente (cadeia completa de 500 elementos, decaimento mais lento + 200 iterações -- os mesmos parâmetros que fizeram a cadeia pura convergir):

| Cenário (500 elementos) | Resultado |
|---|---|
| Sem solo, sem corrente | ✅ Converge |
| **Solo real, sem corrente** | ✅ Converge (inclusive um trecho de 218 elementos apoiado no solo) |
| **Corrente real, sem solo** | ✅ Converge (10-11 iterações por passo no final, mas converge) |
| **Solo real + corrente real juntos** | ❌ Diverge catastroficamente (resíduo ~10¹²) -- o mesmo padrão do modelo completo |

Isso descarta solo e corrente como causas isoladas -- cada um, sozinho, é perfeitamente administrável mesmo na cadeia inteira. A causa real é a **interação entre os dois**: a corrente aplica carga lateral significativa ao longo de toda a linha, e essa carga lateral empurra a zona de touchdown (onde o contato normal do solo e o atrito bilinear já são não-lineares) de um jeito que os dois mecanismos não-lineares não conseguem resolver juntos com o Newton-Raphson de passo cheio atual. Isso reproduz fielmente a divergência catastrófica vista no `main_test.cpp` (pipeline completo, que sempre habilita corrente quando o JSON a contém) num teste pequeno e controlado, fora da complexidade do pipeline completo (duas fases, offset de embarcação, etc.).

**Próximo passo natural**: investigar a interação solo+corrente diretamente -- candidatos: (a) o atrito bilinear (stick-slip) do solo pode estar causando uma descontinuidade que o passo cheio de Newton (sem line search) não consegue atravessar quando há carga lateral significativa empurrando contra ele; (b) talvez seja necessário um passo de carga mais fino especificamente para a fase em que a corrente e o contato com o solo interagem, em vez de aplicar 100% da carga lateral de uma vez.

## Isolamento do caso solo+corrente: limiar preciso em 32→33 elementos (caso pequeno e depurável)

Repetindo a técnica de bisseção nos parâmetros que já sabíamos combinar (solo real + corrente real, decaimento/max_iter padrão), isolado um trecho perto do touchdown (a partir do elemento 272):

| Elementos (272 em diante) | Resultado |
|---|---|
| 20, 25, 30, 31, 32 | ✅ Converge |
| 33+ | ❌ Falha, sempre no **passo de carga 2** (18,2%), não no passo 1 |

Diferente dos dois limiares anteriores, este **não corresponde a nenhuma feature geométrica especial**: os nós 295-309 (em torno do limiar) já estão todos exatamente em Z=0, assentados no fundo há muito tempo -- não há transição de contato acontecendo bem ali. É puramente uma questão de comprimento da cadeia apoiada no solo sob carga lateral de corrente.

O padrão de falha é revelador: o **passo 1 converge** (embora levando 25 iterações, com um pico intermediário de resíduo em ~1682 antes de cair -- já um sinal de dificuldade), mas o **passo 2 explode** de resíduo ~321 para ~6,87×10⁷ em só 10 iterações, e nunca mais se recupera. Isso sugere que o sistema está bem perto de um ponto de instabilidade já no passo 1, e o incremento de carga do passo 2 (mais carga lateral de corrente sendo aplicada, já que ela também é rampeada por `load_factor`) empurra para além desse ponto.

Este é agora o caso mais pequeno e mais depurável de toda a investigação (33 elementos, ~34 nós, reproduz de forma limpa e determinística) -- candidato natural para inspecionar diretamente a matriz de rigidez/resíduo nó a nó no início do passo 2, para identificar qual GDL especificamente diverge primeiro.

## Mecanismo identificado: "chattering" de contato com o solo sob Newton de passo cheio

Instrumentado temporariamente `static_analysis.cpp` para imprimir, a cada iteração com resíduo alto, qual nó/GDL tinha o maior componente de resíduo e a posição Z atual desse nó. No caso de 33 elementos (passo de carga 2, onde a divergência começa):

- Nas primeiras iterações do passo 2, o pior resíduo já aparece nos nós 273-274 (`tx`), ainda com valores moderados (~10³-10⁵) -- coerente com a carga lateral de corrente sendo aplicada.
- A partir da iteração ~10-20, o pior resíduo passa a saltar entre vários nós da região de touchdown (280, 283, 286, 290, 296, 299, 302...), com componentes de **rotação** (`ry`, `rz`) atingindo 10⁸-10⁹, e principalmente: **as posições Z desses nós, que deveriam estar assentadas em ~0 m (apoiadas no solo), aparecem oscilando entre -0.4 m e +6.6 m** -- ou seja, nós sendo levantados violentamente do fundo e depois escancarados para baixo de novo, de iteração em iteração.

Esse padrão é a assinatura clássica de **"contact chattering"**: a mola de contato do solo é **bilinear/descontínua** (rigidez alta quando penetra, zero quando separa) e o solver usa Newton-Raphson de **passo cheio, sem line search** (decisão deliberada desde o início do projeto, para replicar o ANFLEX real). Quando a carga lateral de corrente perturba um nó da zona de touchdown o suficiente, o passo cheio de Newton pode "acertar" a mola de contato do lado errado da descontinuidade, gerando uma reação enorme que chuta o nó para longe do solo na iteração seguinte -- que por sua vez gera outra reação errada na direção oposta, e o ciclo diverge em vez de convergir. Isso explica por que solo sozinho (sem perturbação lateral) e corrente sozinha (sem contato descontínuo) não disparam o problema, mas os dois juntos sim.

**Implicações para uma correção futura** (não implementada nesta sessão -- é uma mudança de comportamento que merece aprovação e teste dedicado):
- Suavizar a transição da mola de contato (ex.: rampa suave em vez de chave bilinear abrupta) reduziria a chance do passo cheio de Newton cruzar a descontinuidade de forma violenta.
- Um line search simples (ex.: cortar o passo pela metade se o resíduo aumentar) resolveria isso sem exigir mudar a formulação de contato -- mas contradiz a decisão original de replicar o passo cheio do ANFLEX; precisaria confirmar se o ANFLEX real tem algum mecanismo equivalente (ex.: um teste de penetração/separação mais conservador, ou um passo de carga mais fino nessa fase) antes de divergir dessa decisão.
- Refinar o passo de carga especificamente onde solo e corrente interagem (mais passos, incrementos menores) reduziria a perturbação por passo o suficiente para não cruzar a descontinuidade.

## Correção do atrito implementada: elimina a explosão, mas expõe um problema de convergência diferente

Implementado o modelo de atrito elástico-plástico incremental fiel ao ANFLEX real: `Node3D` ganhou estado persistente por nó (`friction_force`, `delta_disp_xy`), `SeabedInteraction::calculate_friction_1d()` passou a acumular a força incrementalmente (`f_state += k·du`, com correção de retorno ao saturar) em vez de recalcular do deslocamento total a cada chamada, e os três pontos que aplicam o incremento de Newton (`static_analysis.cpp` × 2, `dynamic_analysis.cpp` × 1) passaram a registrar o incremento desta iteração em `delta_disp_xy`, igual ao `get_delta_dx()`/`m_transl_delta` do ANFLEX real.

**Resultado no caso de 33 elementos (solo + corrente) que antes explodia catastroficamente:**

```
Antes:  passo 2, iter 10: residuo 6,87e7 -- explode, nunca recupera
Depois: passo 2, iter 30: residuo 1,97e2 -- estagna, decrescendo devagar (linear, nao quadratico)
```

A explosão sumiu -- o resíduo fica **limitado e decrescendo**, em vez de divergir sem controle. Isso confirma que o modelo de atrito baseado em deslocamento total era de fato uma causa real do chattering. Mas surgiu um problema diferente: a convergência ficou **muito lenta** (decréscimo quase linear, não a taxa quadrática esperada do Newton perto da solução) -- em 150 iterações ainda não atingiu a tolerância. Isso é consistente com múltiplos nós atingindo o limite de plasticidade perfeita do atrito ao mesmo tempo (`k_friction=0` quando a força satura), o que zera a contribuição de rigidez de atrito na matriz tangente exatamente nesses graus de liberdade -- um problema conhecido de mal-condicionamento em modelos de atrito com plasticidade perfeita, quando resolvido com Newton de passo cheio sem nenhuma técnica de estabilização (line search, comprimento de arco, etc.).

**Efeito colateral (regressão pontual)**: o caso de 32 elementos, que convergia antes da correção do atrito, passou a falhar também -- não por explosão, mas pelo mesmo motivo de estagnação lenta. O teste de caracterização (`test_static_analysis.cpp`) continua convergindo (seu cenário tem menos nós saturados simultaneamente), só que com um valor de referência diferente (atualizado: 9283,15 kN em vez de 8659,99 kN), atualizado no teste.

**Conclusão desta etapa**: a correção do atrito é fisicamente correta e claramente melhora o comportamento numérico (elimina explosão), mas revela que o gargalo real agora é a **estratégia de solução** (Newton puro, sem nenhuma técnica de estabilização) diante de múltiplos pontos de contato saturando ao mesmo tempo -- não mais um bug de modelagem isolado. As três direções já listadas anteriormente continuam válidas como próximos passos, agora com evidência mais forte:
- Um line search simples (cortar o passo se o resíduo não diminuir) atacaria diretamente esse tipo de estagnação/oscilação, sem exigir mudar a formulação de atrito ou contato.
- Um atrito com uma pequena rigidez residual pós-saturação (em vez de `k=0` exato) evitaria o mal-condicionamento local, à custa de uma pequena imprecisão física deliberada (técnica comum em códigos de contato não-linear).

## Atrito axial/lateral local implementado: melhora fidelidade, mas não resolve -- e revela o padrão comum

Confirmado, lendo `newton_raphson.cpp` e `integrator.cpp::update()` do ANFLEX real, que **não existe nenhuma técnica de estabilização numérica** (line search, amortecimento, corte de passo) -- é Newton puro, passo cheio, sempre, igual ao que o risersim já replica.

Encontrada uma diferença de parâmetros real: `Exemplo_01a_A1.xml` especifica `axial_friction=0.92`/`axial_elastic_deflection_limit=0.03m` e `lateral_friction=0.95`/`lateral_elastic_deflection_limit=0.279m` -- valores bem diferentes entre si, enquanto o risersim usava um único `friction_coeff`/`elastic_deflection_limit` isotrópico. Além disso, o atrito real do ANFLEX é decomposto nas direções **axial (ao longo da linha) e lateral (perpendicular, local)**, via `soil.cpp:calc_transf_matrix` (lateral = normal×tangente; axial = lateral×normal) -- não em X/Y globais como o risersim fazia.

Implementado: `SeabedInteraction` ganhou `axial_friction`/`lateral_friction`/`axial_elastic_deflection_limit`/`lateral_elastic_deflection_limit` distintos; `analysis.cpp` passou a calcular uma tangente média por nó (a partir da corda atual dos elementos adjacentes) e decompor o atrito em axial/lateral local antes de transformar de volta para X/Y globais, incluindo o termo cruzado `k_xy` na rigidez tangente (antes só diagonal).

**Resultado**: não resolveu o caso de 33 elementos (residual passou a oscilar caoticamente entre ~200 e ~20.000, sem convergir nem explodir de forma sustentada). E revelou uma **regressão pontual reveladora**: o caso de 20 elementos com solo (sem corrente), que antes convergia limpo, passou a **oscilar em torno de um resíduo já minúsculo** (0,02-0,06 N, relativo ~10⁻⁵ -- fisicamente já resolvido) sem nunca satisfazer o critério de convergência. Isso é "chattering" no limite elástico/plástico do atrito: como a plasticidade é perfeita (`k=0` exatamente ao saturar), um nó bem na fronteira do limite de escoamento pode alternar entre elástico e plástico a cada iteração por causa de ruído numérico, ficando preso num ciclo que nunca damping para zero.

**Conclusão consolidada desta investigação (rotação → decaimento → atrito → axial/lateral)**: cada correção foi real e válida (a de rotação foi a causa dominante, comprovada; as de atrito são mais fiéis ao ANFLEX e não pioram nada fora do regime solo+corrente), mas todas as dificuldades remanescentes têm a **mesma assinatura**: o resíduo fica **limitado, oscilando ou decrescendo devagar perto de uma descontinuidade** (contato liga/desliga, atrito elástico/plástico), nunca satisfazendo o critério de convergência dentro do orçamento de iterações -- não mais uma explosão descontrolada. Isso é exatamente o tipo de situação que um **line search simples** (cortar o passo pela metade se o resíduo não diminuir) resolveria diretamente, sem exigir mais mudanças de modelagem física -- e é a única lacuna real que sobra em relação ao ANFLEX (que também não tem essa técnica, mas aparentemente não encontra essa combinação específica de solo+corrente saturando ao mesmo tempo em uso típico).

## Line search implementado: ajuda como rede de segurança, mas não resolve o caso solo+corrente — e ensinou uma lição real

Implementado `apply_newton_step_with_line_search()` em `static_analysis.cpp`: backtracking simples (tenta o passo cheio, corta pela metade se o resíduo não melhorar, até um número máximo de tentativas), com snapshot/restore do estado do nó (`disp`, `rot`, `friction_force`) para permitir avaliar cada tentativa de forma limpa. Aplicado nos dois laços de Newton-Raphson (`solve_catenary_static` e `solve_vessel_offset`).

**Primeiro resultado (critério estrito, só aceita se o resíduo não crescer)**: quebrou casos que antes convergiam perfeitamente sem solo nem corrente (ex.: 300 elementos), com o teste de caracterização caindo para um valor ~124x errado. Investigando com um traço detalhado (`RISERSIM_DEBUG_LS`), a causa ficou clara: o passo cheio de Newton pode legitimamente piorar o resíduo por várias iterações seguidas longe da solução — isso é normal e o Newton puro se autocorrige (como o ANFLEX real, que não tem nenhum line search, comprova). Um critério que rejeita qualquer piora força o solver para uma sequência de passos pequenos que, cumulativamente, seguem uma **trajetória diferente e pior** do que o passo cheio teria seguido — backtracking "gatilho fácil" pode genuinamente piorar um caso bem-comportado, não é uma rede de segurança de graça.

**Ajuste**: relaxada a tolerância de aceitação para até 100x o resíduo anterior (rede de segurança só contra explosão catastrófica de verdade, não contra a piora transitória normal do Newton). Isso eliminou a regressão nos casos sem solo/corrente (verificado em bateria: 5 a 300 elementos, todos voltam a convergir igual a antes).

**Mas com esse limiar frouxo o suficiente para não regredir, o line search também deixa de intervir a tempo nos casos catastróficos** (33 e 500 elementos com solo+corrente): um fator de 100x por iteração ainda permite crescimento astronômico ao longo de várias iterações compostas (100¹⁰ é um número absurdo) — o mecanismo simplesmente não tem força para conter uma explosão que se estende por 10-20 iterações sem reduzir a tolerância a ponto de regredir os casos bons.

**Conclusão honesta**: o line search, do jeito simples implementado (checagem de norma total do resíduo, sem informação de direção/gradiente), não consegue servir simultaneamente como (a) rede de segurança eficaz contra a explosão de contato+corrente e (b) não-regressivo em casos bem-comportados — são objetivos em tensão direta com esse desenho. Resolver isso de verdade precisaria de uma técnica mais sofisticada: um critério tipo Armijo (usando a derivada direcional, não só a norma do resíduo), limitar o passo por nó/GDL em vez de um único fator escalar global, ou atacar a causa raiz da explosão diretamente (ex.: suavizar a descontinuidade de contato, como já cogitado antes). Mantido o line search no código (não regride nada, e ainda funciona como rede de segurança genuína contra explosões muito além de 100x), mas documentado claramente que **não resolve** o caso solo+corrente — isso permanece um problema aberto.

## Solver linear: RCM + LDLT, fechando a lacuna com o ANFLEX real

Investigando o solver linear real do ANFLEX (a pedido do usuário), descobri que:
- **O default real é Skyline** (`eSolverType::COL3D`), não MKL/Pardiso — confirmado em três pontos: construtor de `cAnflexAnalysis` (`anflex_analysis.cpp:41-52`), reset da GUI (`cb-analysis.cpp:1524`), e o executável de linha de comando/lote (`main_dll.cpp:450`, `string solver = "col3d"`). Nenhum exemplo do repositório sobrescreve isso. MKL/Pardiso existem como opção explícita, não como o que roda por padrão.
- **Mas o ANFLEX reordena os nós via Reverse Cuthill-McKee por padrão** (`model_builder_dat.cpp:5222-5231`, `reorderer` default = `"reverse_cuthill_mckee"`, via Boost Graph Library), com o algoritmo de Sloan como alternativa — aplicado na construção do modelo, antes de qualquer solver rodar. Isso reduz a banda da matriz para o Skyline (e para os outros dois backends também, ainda que eles tenham seu próprio reordenamento por cima).
- **Todos os três backends exploram a simetria** da matriz de rigidez (Skyline via perfil/Cholesky-LDLᵀ; MKL DSS e Pardiso em modo simétrico/simétrico-indefinido) — nenhum trata `K` como matriz geral não-simétrica.
- **Nenhum dos três é iterativo/pré-condicionado** — todos são solvers diretos. Isso descarta "precisão do solver linear" como causa de qualquer divergência investigada nesta sessão — o sistema linear é resolvido a cada iteração com a mesma precisão essencialmente exata dos dois lados.

**Implementado no risersim** para fechar as duas lacunas de fidelidade:
- `rcm_reorder.hpp` (novo): Reverse Cuthill-McKee implementado direto (BFS por grau crescente, sem trazer Boost como dependência). Usado em `Analysis::assign_equation_numbers()` — numera os GDLs seguindo a ordem RCM em vez da ordem de `model->nodes`, sem reordenar o modelo em si (só a numeração das equações).
- `EigenSimplicialLDLTSolver` (novo, `linear_solver.hpp`/`.cpp`): explora a simetria de `K_global` via `Eigen::SimplicialLDLT`, igual aos três backends reais. Virou o **default** (`Analysis` constructor), substituindo `EigenSparseLUSolver`.

**Validação**: RCM é uma pura permutação (não muda a física) — o teste de caracterização passou com o **mesmo** valor de referência de antes, confirmando que não alterou o resultado numérico. A troca para LDLT também passou com o mesmo valor de referência, e testada contra toda a bateria de regressão (5 a 300 elementos sem solo/corrente, mais os casos com solo) — converge exatamente nos mesmos casos que o SparseLU convergia, e não falha de forma diferente (sem `DecompositionFailed`) nos casos já problemáticos (solo+corrente, ainda não resolvidos). `EigenSparseLUSolver` continua disponível como alternativa mais robusta (com pivoteamento) caso `K_global` fique genuinamente indefinida em algum cenário futuro onde o LDLT falhe — o Pardiso real lida com essa mesma situação via escalonamento/permutação MPS (`mtype=-2`), que o `SimplicialLDLT` do Eigen não replica.

## Documentação em Doxygen (inglês) e migração para C++20

A pedido do usuário, todo o código (`include/`, `src/`, `tests/`) recebeu comentários Doxygen em inglês (estilo Javadoc `/** @brief */`), incluindo tradução dos comentários de racional que antes só existiam em português neste documento/código, e das docstrings do binding Python (`bindings.cpp`). Adicionado `risersim/Doxyfile` (configuração curada, não o default completo) + `docs/mainpage.dox` (página inicial em inglês com a tabela de camadas da arquitetura, os achados de fidelidade principais e os problemas abertos) + um target `docs` opcional no CMake (`cmake --build build --target docs`, só registrado se o Doxygen estiver instalado). Validado gerando o site com Doxygen de verdade (container descartável): zero warnings após corrigir duas coisas — uma tag de config não suportada pela versão instalada, e uma string de uso (`<start_elem_id>`) sendo interpretada como tag HTML por engano (corrigido com um bloco `@code`).

Em seguida, elevado `CMAKE_CXX_STANDARD` de 17 para 20 (GCC 11.4 do `Dockerfile`, Ubuntu 22.04, com suporte sólido a C++20 exceto módulos/coroutines completos, que o projeto não usa). Junto com a subida do padrão, removido o hack de portabilidade `M_PI`/`_USE_MATH_DEFINES` de `config.hpp` (existia só porque MSVC não expõe `M_PI` por padrão, enquanto GCC/Clang o expõem como extensão não-padrão do `<cmath>`) em favor de `std::numbers::pi` (`<numbers>`, C++20), que é portável e `constexpr` em qualquer compilador conforme ao padrão. Atualizados os 12 pontos de uso (`current_profile.hpp`, `element_beam.hpp`/`.cpp`, `hydrodynamics.hpp`, `dynamic_analysis.cpp`, `main_test.cpp`, `diag_isolated_segment.cpp`).

**Validação**: rebuild completo via Docker sem warnings novos; teste de caracterização (Catch2) passou com o **mesmo** valor de referência de antes; bateria de diagnóstico isolado (20 elementos com solo real, 300 elementos sem solo) convergiu exatamente como antes. Confirma que a mudança de padrão + a troca do `M_PI` foram comportamento-preservantes, como esperado (troca puramente de representação de uma constante, não de lógica).

## Passo 5 do roadmap concluído: interface `Element` abstrata

Implementada a interface `Element` (`element.hpp`), espelhando `cElement` do ANFLEX real, mas deliberadamente enxuta: só o que o loop de montagem genérico realmente usa (acesso a nós, rigidez tangente + força interna, matriz de massa) — não a superfície gigante do `cElement` real (carga de fluido, wake effect, amortecimento, mapa de variáveis de resultado etc.), que não se aplica aqui.

**Decisão de desenho**: o plano original previa `Element` (abstrata) → `BeamElement` (wrapper sobre a matemática do `CorotationalBeam3D`, + a tríade de referência fixa que faltava). Essa tríade já foi implementada diretamente dentro de `CorotationalBeam3D` (foi a correção da causa raiz da divergência, ver seção acima) — então, no momento de implementar este passo, a lacuna que o wrapper existiria para preencher já não existe mais. Criar um `BeamElement` separado hoje seria só uma casca repassando chamadas, sem nenhum comportamento próprio — uma abstração prematura. Em vez disso, `CorotationalBeam3D` implementa `Element` diretamente (`class CorotationalBeam3D : public Element`), o que também é mais fiel ao próprio ANFLEX (`cBeam` deriva de `cElement` diretamente, sem uma camada intermediária).

**O que a interface abstrai**: `num_nodes()`, `node(i)`, `assemble(K, F_int)` (rigidez + força interna, tamanho dinâmico `Eigen::MatrixXd`/`VectorXd`) e `mass_matrix(rho_water)`. Dados específicos de viga (propriedades de material, tensão efetiva, curvatura/von Mises/MBR) continuam só em `CorotationalBeam3D` — não foram forçados para dentro da interface genérica, do mesmo jeito que `cBar`/`cBeam` reais têm bastante estado além do `cElement` base.

**Uso real, não só decorativo**: os dois laços de montagem por elemento que existiam — `Analysis::assemble_system()` (rigidez/força interna estática+dinâmica) e o laço de massa em `DynamicAnalysis::solve_time_domain_dynamic()` — foram generalizados para usar `elem->assemble()`/`elem->mass_matrix()`/`elem->num_nodes()`/`elem->node(i)` em vez de código fixo em 12 GDLs e acesso direto a `node1`/`node2`. Isso significa que os dois laços já funcionam sem alteração para um futuro segundo tipo de elemento (ex.: um elemento de treliça/cabo), desde que ele implemente `Element`. `RiserModel::elements` continua `vector<CorotationalBeam3D*>` (não virou `vector<Element*>`) — trocar o tipo do container exigiria cast em toda a fiação que usa dados específicos de viga (`main_test.cpp`, `bindings.cpp`, `diag_isolated_segment.cpp`, exportação de resultados), sem nenhum benefício concreto hoje, já que só existe um tipo de elemento.

**Validação**: novo `tests/test_element.cpp` compara `assemble()`/`mass_matrix()` contra `compute_corotational_forces()`/`global_mass()` **bit a bit** (igualdade exata, não aproximada — são a mesma matemática, só copiada para um tamanho dinâmico) numa configuração de amostra não-trivial (elemento inclinado, esticado, com deslocamento e rotação não-nulos nos dois nós). Bateria de regressão completa (teste de caracterização + diagnóstico de segmento isolado) confirmou resultado numérico idêntico ao de antes do refactor.

Também rodado `risersim_test_main` (modelo sintético de 40 elementos, estático + dinâmico) para exercitar o laço de massa generalizado de `DynamicAnalysis`, que não é coberto pelos outros dois testes. A análise dinâmica não converge em vários passos — mas isolado revertendo temporariamente só esse laço para a versão antiga (hardcoded, 12 GDLs) e rodando de novo, o **mesmo** padrão de não-convergência aparece, confirmando que é um problema pré-existente da análise dinâmica em si (nunca fez parte da bateria de regressão validada nesta sessão, que sempre focou em estática), não uma regressão introduzida por este refactor.

## Passo 6 do roadmap concluído: `RiserModel` com `unique_ptr`

`RiserModel::nodes`/`elements` migraram de `vector<Node3D*>`/`vector<CorotationalBeam3D*>` com `delete` manual em `clear()` para `vector<unique_ptr<Node3D>>`/`vector<unique_ptr<CorotationalBeam3D>>`. `CorotationalBeam3D::node1`/`node2` continuam ponteiros crus **não-donos** apontando para dentro desse armazenamento — igual ao próprio `cDomain` do ANFLEX real, onde elementos referenciam nós por índice/ponteiro sem possuí-los.

**Investigação prévia necessária**: o plano original descrevia este passo como "mecânico, baixo risco" — verdade para o núcleo C++, mas havia uma complicação real: os bindings pybind11 (`bindings.cpp`) expunham `RiserModel.nodes`/`.elements` como propriedades **leitura-e-escrita** (`model.nodes = [n1, n2, ...]`), o que não é seguro de sustentar com `vector<unique_ptr<T>>` (pybind11 não tem como "roubar" a posse de um objeto Python já existente para dentro de um novo `unique_ptr` automaticamente). Antes de decidir a forma final, verifiquei se algo no repositório realmente dependia desse padrão de escrita — nenhum script em `risersim/tools/` importa o módulo `risersim` (pybind11) nem usa esse padrão; o fluxo de produção real (`run_from_aml.py`) sempre chama o executável C++ via `subprocess`. Com essa confirmação, troquei `nodes`/`elements` para propriedades **somente leitura** (retornam uma projeção de ponteiros não-donos) e adicionei `RiserModel.add_node(...)`/`.add_element(...)` — construção owned diretamente no C++, devolvendo uma referência para o Python usar. Os exemplos no docstring do módulo e da `StaticAnalysis` foram atualizados para o novo padrão (o exemplo antigo já estava desatualizado antes disso: usava `sa.nodes = [...]` numa classe que nunca teve esse atributo).

`CorotationalBeam3D`'s próprio construtor pybind11 (`risersim.CorotationalBeam3D(id, node1, node2, props)`, construção standalone independente de `RiserModel`) e as funções utilitárias de módulo `build_catenary_nodes`/`build_catenary_elements` (que devolvem ponteiros com posse transferida para o Python, também independentes de `RiserModel`) não precisaram mudar — só ganharam uma nota no docstring esclarecendo que não são mais atribuíveis a `RiserModel.nodes`/`.elements` (que agora são somente leitura).

**Simplificação real**: como `vector<unique_ptr<T>>` já não é copiável e já move corretamente por padrão, o construtor de movimento e o operador de atribuição de movimento escritos à mão em `RiserModel` foram removidos — `= default` já faz a coisa certa.

**Validação**: bateria de regressão completa (teste de caracterização + `test_element.cpp` + diagnóstico de segmento isolado de 20 e 300 elementos + `main_test` completo) — todos convergem exatamente como antes, mesmos valores de referência (dentro da tolerância já estabelecida do teste de caracterização).

## Passo 4 do roadmap concluído: `ConvergenceTest` com os 6 critérios reais do ANFLEX

Lido `convergence_test.h`/`.cpp` do ANFLEX real para entender os 6 critérios exatos (`eConvergenceCriterion`): Translation, Rotation, ForcesNorm, MomentsNorm, UnbalancedForces, UnbalancedMoments. Achados principais:

- **Translation/Rotation são sempre ativos** (`// Criterio de deslocamento sempre eh usado`), os outros 4 são opt-in via AML (`m_use_force_criterium`, `m_check_unbalanced_force`, `m_check_unbalanced_moment`).
- A fórmula de Translation/Rotation é `norma_da_correcao_desta_iteracao / norma_acumulada_desde_o_inicio_do_passo` — **exatamente** o que o risersim já calculava inline em `solve_catenary_static` (o `split_norms` lambda + `ratio_transl`/`ratio_rot`), confirmando que essa parte já estava correta; só faltava a organização em classe.
- ForcesNorm/MomentsNorm: resíduo de força/momento desta iteração, normalizado pelo resíduo da **primeira** iteração do passo (não por um valor absoluto fixo).
- UnbalancedForces/UnbalancedMoments: o maior valor **absoluto** de força/momento residual em qualquer GDL individual (não normalizado) — com uma válvula de escape: dentro das últimas 3 iterações do orçamento, satisfazer só esse critério já é suficiente, mesmo que os outros ainda não tenham convergido (evita falhar um passo por causa de uma razão de incremento que decai devagar mas já é pequena, bem no limite de iterações).
- Critério de convergência de cada `sConvergenceCriterion` usa `<=` (não `<`); o código inline do risersim usava `<` — troquei para `<=` ao portar, fiel ao ANFLEX (a diferença é inobservável na prática, igualdade exata em ponto flutuante é essencialmente impossível de atingir).

**Implementado**: `convergence_test.hpp`/`.cpp` com a classe `ConvergenceTest`, `enum class ConvergenceCriterion` (6 valores), e `ConvergenceCriterionState`/`ConvergenceConfig`. Deliberadamente mais enxuto que o `cConvergenceTest` real (sem a infraestrutura de `cStatusWatcherAnalysis`, mensagens de erro formatadas, hooks de teste `_TESTS`/`_DIFZERO` — nada disso existe no risersim). `StaticAnalysis::solve_catenary_static` foi refatorado para usar `ConvergenceTest` no lugar do `split_norms` lambda inline; os 4 critérios novos (ForcesNorm/MomentsNorm/UnbalancedForces/UnbalancedMoments) ficam **desabilitados por padrão** (`ConvergenceConfig` default), preservando o comportamento atual exatamente — ficam prontos para quando houver leitura de config real do AML/JSON habilitando-os.

**Não mexido, deliberadamente**: `solve_vessel_offset` continua com seu próprio critério simples (`rel_R < 1e-4`, resíduo relativo direto) — é um critério genuinamente diferente em espírito (não é a razão de incremento), então não foi forçado para dentro do `ConvergenceTest`.

**Validação**: novo `tests/test_convergence_test.cpp` (3 casos, 12 assertions) cobre especificamente: (a) Translation/Rotation sempre ativos e bloqueados na primeira iteração (`iter>=1`), (b) ForcesNorm/MomentsNorm normalizando pelo resíduo da 1ª iteração, (c) a válvula de escape do UnbalancedForces só valendo perto do limite de iterações. Bateria de regressão completa (teste de caracterização + `test_element.cpp` + diagnóstico de segmento isolado de 20 e 300 elementos) — todos convergem como antes.

## Passo 7 do roadmap concluído (e roadmap original todo fechado): `PrescribedMotion`

Investigado como o ANFLEX real impõe movimento prescrito (`dof.h`'s `sPrescribedDOF`, `cIntegrator::set_load_dofs`/`apply_load_stiffness`, `cDomain::calc_big_number`, `cBeam::get_big_number`). O mecanismo real é a técnica do **"número grande"** (mola de penalidade): em vez de eliminar o GDL (Dirichlet rígido), soma-se um valor de rigidez enorme (`big_number`, escolhido pelo ANFLEX como o módulo de elasticidade E do material — já ordens de magnitude maior que qualquer termo real de rigidez translacional/rotacional) na diagonal de K naquele GDL, e o valor de força interna naquele GDL é **sobrescrito** (não somado) por `(deslocamento_atual - alvo) * big_number` — uma mola virtual bem rígida puxando o GDL para a posição-alvo a cada iteração.

**Antes desta mudança**, `solve_vessel_offset` movia `top_node->disp` diretamente por fora do sistema linear, com o nó de topo permanentemente engastado (`eq_numbers = {-1,...}`) — funciona, mas é uma técnica diferente da real, e não generaliza: só serve para um nó com todos os 6 GDLs eliminados, não para prescrever um GDL individual em qualquer nó (a peça que falta para, no futuro, alimentar o movimento do topo com uma série temporal **medida** em vez de um offset sintético — ver plano original, seção "Como isso se conecta ao objetivo maior").

**Implementado**: `prescribed_motion.hpp` (`PrescribedMotion`, header-only) — nó + quais dos 6 GDLs estão ativos + alvo de translação/rotação; `apply()` soma `big_number` à diagonal de K e sobrescreve `F_int` naquele GDL. `Analysis` ganhou `prescribed_motions` (lista vazia por padrão — sem custo/mudança de comportamento a menos que populada) e `assemble_system` aplica a penalidade após a montagem normal de elementos/solo, com `big_number` calculado como o maior `E` entre os elementos do modelo (fiel ao `cBeam::get_big_number()` real).

**Cuidado com o sinal**: com a convenção `Residual = F_ext - F_int` e `u_novo = u_antigo + K⁻¹·Residual` (usada em todo o risersim), a fórmula correta é `F_int = (atual - alvo) * big_number` — o oposto do que uma leitura ingênua da fórmula literal do ANFLEX (`presc_desl - desl`) sugeriria, porque a convenção de sinal de "força resistente" do ANFLEX não é necessariamente a mesma do `F_int` do risersim. Deduzido a partir de primeiros princípios (não copiado cegamente) e validado com um teste de unidade que verifica o sinal explicitamente, além do teste de ponta a ponta abaixo — um erro de sinal aqui faria a mola de penalidade empurrar para **longe** do alvo, divergindo.

**`StaticAnalysis::solve_vessel_offset` migrado**: o nó de topo passa a ter GDLs livres de verdade durante a fase de offset (chamando `assign_equation_numbers()` de novo), guiado por um `PrescribedMotion`, com o alvo rampando de `disp_inicial` até `disp_inicial + offset` — e restaurado ao engastamento original (`eq_numbers` salvos e restaurados, `prescribed_motions` limpo) ao final, preservando exatamente o contrato de GDL que o resto do código (ex.: `DynamicAnalysis`, que reusa o mesmo `RiserModel`) espera do nó de topo. `apply_newton_step_with_line_search` não exclui mais o nó de topo (`excluded_node=nullptr`) — ele agora recebe correções de Newton como qualquer outro nó livre.

**Bug pré-existente corrigido de brinde**: a fórmula antiga de rampa (`top_node->disp += offset*step/steps² `, dentro de um loop de `step=1..steps`) na verdade convergia para só ~50-55% do offset pedido para `steps` grandes (bug aritmético, não intencional), em vez do offset completo. A nova implementação rampa corretamente para o alvo exato no passo final — confirmado no teste de ponta a ponta (offset pedido 2.0 m, nó de topo termina em exatamente 2.0 m). Nada na bateria de regressão rastreada usava `solve_vessel_offset` numericamente (só `solve_catenary_static`), então esse não era um valor de referência protegido.

**Escopo deliberado**: só a análise **estática** foi migrada (é o caso "Cross" citado no plano original, que é estático). O movimento prescrito do topo em `DynamicAnalysis` (onda harmônica) continua com sua própria técnica direta (`top_node->disp = ...` fora do sistema, nó explicitamente excluído do loop de Newton) — migrar isso também introduziria risco numa parte do código já documentada como tendo problemas de convergência pré-existentes não relacionados a esta sessão; fica como trabalho futuro se algum dia for necessário.

**Validação**: `tests/test_prescribed_motion.cpp` (3 casos) — (a) fórmula da penalidade isolada, incluindo o sinal explicitamente; (b) GDLs inativos/fixos corretamente ignorados; (c) teste de ponta a ponta: `solve_catenary_static` seguido de `solve_vessel_offset` num modelo sintético pequeno, confirmando convergência rápida (7-10 iterações por passo) e que o nó de topo termina exatamente no offset pedido, além de confirmar que `eq_numbers`/`prescribed_motions` voltam ao estado original depois. Bateria de regressão completa (caracterização + `test_element` + `test_convergence_test` + diagnóstico isolado de 20/300 elementos + `main_test`) — tudo convergindo exatamente como antes.

Com isso, **todos os 8 passos do roadmap de modernização original estão concluídos**. Os três problemas de convergência documentados ao longo da sessão (limiar ~300-350 elementos, solo+corrente em cadeias longas, chattering perto do limite elástico/plástico no caso de 20 elementos) continuam em aberto — não fazem parte do roadmap de arquitetura, são investigações de física/numérica separadas.

## Bug real encontrado e corrigido: `tol=100.0` usado como tolerância de força vs. razão adimensional

Investigando por que o viewer 3D mostrava a linha "dobrando" na zona de touchdown mesmo com a estática reportando sucesso: `StaticAnalysis::tol` (default `100.0`) era documentado em `bindings.cpp` como "residual-norm convergence tolerance (N)", mas era passado direto para `ConvergenceConfig::transl_tol`/`rot_tol`, que `ConvergenceTest` usa como limite de uma **razão adimensional** (`ratio_transl = correção desta iteração / correção acumulada no passo`, tipicamente em `[0,~1]`). Um limite de `100.0` faz esse critério virar um no-op -- todo passo "convergia" em 1-2 iterações com resíduo de força de centenas de milhões a bilhões de N, nunca de fato em equilíbrio.

**Corrigido**: `tol` (default e todos os call sites que hardcodeavam `100.0`) trocado para `0.01`, um valor são para a razão adimensional. Documentação/docstrings atualizadas para descrever o parâmetro corretamente.

**Importante**: isso **não é o mesmo bug** da divergência do Exemplo_01a documentada nas seções acima -- o Exemplo_01a sempre leu sua própria tolerância do XML real (`AnalysisData/Static/x_tol=0.001`), nunca dependeu do default `100.0`. Confirmado rodando o Exemplo_01a real com o binário corrigido: **continua divergindo**, exatamente como documentado (residual crescendo a partir da iteração ~20 do passo 1). O bug do `tol=100.0` afetava só o modelo sintético de fallback (usado quando nenhum AML é fornecido) e dois testes que hardcodeavam esse default -- todos corrigidos e revalidados (343 asserts, teste de caracterização com novo valor de referência real: 1101,98 kN em vez de 3539,19 kN, que estava congelado sob a convergência falsa).

## Retomando a investigação: causa real do "chattering" de 20 elementos identificada (não é o atrito saturando)

Reinvestigado o caso de 20 elementos (trecho 272+, solo real, sem corrente) que estagna oscilando num resíduo minúsculo (~0,03 N, relativo ~1,5e-5) sem nunca convergir -- documentado antes como suspeita de "chattering no limite elástico/plástico do atrito". Instrumentado temporariamente (`RISERSIM_DEBUG_RESIDUAL=1`, ver `static_analysis.cpp`) para identificar o DOF com maior resíduo a cada iteração:

**Achado**: o pior resíduo, iteração após iteração, é sempre o GDL `ty` (lateral local) do **mesmo nó** (286), alternando de sinal a cada iteração (-0,0227 → +0,0259 → -0,0229 → +0,0261...) -- um ciclo-limite de período 2 clássico. O nó está com penetração ínfima no solo (~5e-5 m, praticamente na superfície exata) e o estado de atrito lateral (`friction_force[1]`) está bem longe da saturação (~0,002 N, contra um limite de saturação muito maior) -- **descartando a hipótese original de saturação de atrito** como causa deste caso específico.

**Tentativa 1 (não resolveu sozinha)**: adicionada rigidez residual pós-saturação ao atrito (`SeabedInteraction::friction_residual_stiffness_fraction`, default 2% da rigidez elástica, em vez de `k=0` exato ao saturar -- regularização padrão em código de contato/plasticidade, sugerida como próximo passo nas seções acima). Não teve efeito neste caso específico (números idênticos bit-a-bit, confirmando que o atrito nunca satura aqui), mas é uma correção real e bem fundamentada para quando o atrito de fato satura (caso solo+corrente abaixo muda de trajetória com ela) -- mantida.

**Tentativa 2 (resolve este caso específico)**: os 6 critérios de convergência do `ConvergenceTest` (portados no passo 4 do roadmap) sempre tiveram uma "válvula de escape": nas últimas 3 iterações do orçamento, satisfazer só o critério de força/momento desbalanceado máximo (`UnbalancedForces`/`UnbalancedMoments`) já basta, mesmo que a razão de translação/rotação nunca se estabilize -- só que nenhum caller nunca habilitava esses 4 critérios opcionais. Exposto via `StaticAnalysis::enable_unbalanced_criteria`/`unbalanced_force_tol`/`unbalanced_moment_tol` (default desligado, zero mudança de comportamento a menos que habilitado explicitamente) e ligado no teste isolado (`diag_isolated_segment.cpp`, novo parâmetro posicional).

**Resultado**: com `enable_unbalanced_criteria=true` (tol=1 N / 1 N.m), o caso de 20 elementos deixa de travar no passo 1/11 -- avança até o **passo 5/11** antes de falhar por um motivo diferente (carga mais alta). É uma melhora real e mensurável, não uma correção completa.

**Testado contra o caso solo+corrente (32/33 elementos)**: nem a rigidez residual de atrito nem a válvula de escape resolvem esse caso -- o resíduo ali é genuinamente grande (~5.000-9.700 N, não uma oscilação de ruído numérico perto da convergência), confirmando que são **dois mecanismos diferentes**: (a) um ciclo-limite de pequena amplitude perto do touchdown quando só o solo está ativo (agora mitigável via a válvula de escape), e (b) a divergência catastrófica solo+corrente já documentada extensivamente acima, que continua sem solução.

**Confirmado no Exemplo_01a real completo (500 elementos)**: com os dois fixes aplicados, ainda diverge -- o problema dominante no modelo completo é o mecanismo (b), não (a).

**Validação**: bateria completa (343 asserts Catch2, cadeia de 300 elementos sem solo/corrente, modelo sintético do `main_test`) -- tudo convergindo exatamente como antes (ambos os fixes desligados por padrão para os callers existentes, exceto a rigidez residual de atrito, que é sempre ativa mas só muda trajetória quando o atrito de fato satura).

**Próximo passo em aberto**: o mecanismo (b) -- solo+corrente -- continua sendo o problema central do Exemplo_01a real. As direções já listadas (line search tipo Armijo/direcional, limitar o passo por nó/GDL em vez de um fator escalar global, suavizar a transição liga/desliga do contato vertical -- hoje só a penetração é suavizada, a transição pen>0/pen<=0 continua uma chave dura) continuam válidas e nenhuma foi implementada ainda.

## Corrente ignorada/reduzida em elementos enterrados: implementado, real, mas não resolve o caso solo+corrente

A pedido do usuário, investigado se o ANFLEX real ignora a corrente em elementos apoiados/enterrados no solo (hipótese direta para o problema solo+corrente). Confirmado lendo o código real:

- `cMorison::calc_external_flow` (`morison.cpp:141-145`) retorna força hidrodinâmica **zero** para `BURIED_ELEMENT`/`DRY_BURIED_ELEMENT` (elemento com os dois nós "enterrados").
- `cNode::set_surrounding_state` (`node.cpp:1030-1047`) só marca um nó como "enterrado" (`m_surround=3`) quando ele está **mais de 1 metro abaixo da linha do solo** (`zn < SEAB - 1.0`) -- não basta estar apoiado na superfície (penetração de centímetros continua `WET_ELEMENT`, corrente cheia).
- Para elementos parcialmente enterrados (`WET_BURIED_ELEMENT`), a força é reduzida proporcionalmente ao trecho não enterrado (`m_wet_length`).

**Confirmado: o risersim não tinha nenhum equivalente** -- `StaticIntegrator::assemble_load_vector` (`static_integrator.cpp`) aplicava a força de corrente incondicionalmente, com o comprimento total do elemento, independente da posição em relação ao solo.

**Implementado**: mesma lógica no `assemble_load_vector` -- calcula a fração do elemento acima do limiar `seabed_depth - 1.0`, zera a força se os dois nós estão enterrados além disso, interpola linearmente se só um está (o piso do risersim é plano, então a interpolação linear em Z é exata, ao contrário da geometria mais geral do ANFLEX real que não precisou ser replicada).

**Resultado**: testado nos casos de 32/33 elementos (solo+corrente) e no Exemplo_01a completo (500 elementos) -- **resíduos idênticos aos de antes da correção**, em todos os casos. Instrumentação (`RISERSIM_DEBUG_RESIDUAL`) confirma por quê: nesses casos os nós na zona de divergência nunca ficam mais de 1m abaixo do solo -- ficam bem próximos da superfície (penetração ~5e-5 m) ou até flutuando acima dela (ex.: nó 273 chega a z=+1,42 m, claramente não enterrado). O pior resíduo (`tx`) salta entre **vários nós diferentes a cada iteração** (273, 296, 291, 274, 285, 298, 295, 304...), com atrito substancial (dezenas de N) saturado simultaneamente em vários deles -- confirma o padrão já documentado ("salta entre vários nós da zona de touchdown"), não um problema de um nó específico afundando demais.

**Conclusão**: a correção é real, fiel ao ANFLEX, e fica no código (zero mudança de comportamento quando a condição de enterro não é atingida -- toda a bateria de regressão, 343 asserts, continua passando) -- mas **não é a causa** da divergência solo+corrente investigada aqui. O mecanismo real continua sendo a interação difusa entre múltiplos pontos de atrito saturando ao mesmo tempo sob carga lateral de corrente, como já apontado nas seções anteriores. As direções ainda não tentadas (line search direcional tipo Armijo, limitar o passo por nó/GDL, suavizar a transição liga/desliga do contato vertical) continuam sendo os candidatos mais promissores.

## Bug real encontrado e corrigido: "step 0" exportado não era a geometria real de entrada quando a Fase 1 diverge

Investigando o Exemplo_02a (a pedido do usuário, "vamos rodar o exemplo_02"): o viewer 3D mostrava a linha com uma "onda"/dobra visível perto do touchdown, mesmo depois de corrigir a câmera e o raio do tubo (que eram bugs reais, mas separados -- ver seções abaixo). O usuário perguntou se as coordenadas estavam sendo lidas certo.

**Investigação, nó a nó, em cada etapa do pipeline**:
- `extract_nodes()` (Python) -- correto, bate exatamente com o HDF5 bruto do modelo.
- `input_simulation.json` gerado -- correto, mesmos valores.
- `catenary_results.json` (saída do solver, `static_steps[0]`) -- **diferente**, e não corresponde a NENHUM nó da entrada (não é reordenação, é um valor calculado que não existe no arquivo de entrada).

**Causa raiz**: `StaticAnalysis::solve()` roda em duas fases (Fase 1 "assembly" com rigidez artificial, Fase 2 "static" sem). Quando a Fase 1 falha em convergir (caso do Exemplo_02a: diverge no passo 1/11), o código **deliberadamente não reseta `node->disp`** antes de tentar a Fase 2 -- para aproveitar o "progresso" da Fase 1 como chute inicial (comentário original: "proceeding to the static phase from the state reached"). Mas `solve_catenary_static()` **sempre** limpa `history` e recaptura seu próprio "step 0" no início de cada chamada, a partir do estado atual do modelo naquele momento -- então o "step 0" capturado no início da Fase 2 não é a geometria real de 0% de carga, é **o último estado que a Fase 1 deixou depois de divergir por 40 iterações**, rotulado erradamente como `load_factor: 0.0` no resultado exportado. Confirmado comparando contra a geometria real do ANFLEX (arquivo `..._results_static.h5`, tanto a referência quanto o passo 11 resolvido): perfeitamente lisa, sem nenhuma onda -- provando que o bug era nosso, não um artefato real do modelo.

**Corrigido**: `StaticAnalysis::solve()` agora captura a geometria pristina (0% de carga, antes de qualquer iteração de Newton em qualquer fase) uma única vez, no início, via nova função auxiliar `capture_snapshot()` (extraída da lógica antes duplicada em `solve_catenary_static()`). Depois que as duas fases rodam, `history[0]` é sobrescrito com essa captura verdadeira -- garantindo que o "step 0" exportado seja sempre honesto sobre o que realmente é, mesmo que ambas as fases falhem completamente.

**Validação**: bateria completa (343 asserts Catch2, sem mudança de comportamento nos casos que já convergiam) + Exemplo_02a real -- ângulo máximo de curvatura entre nós vizinhos na geometria exportada caiu de >23° para **0,59°** em toda a malha de 834 nós, batendo com a suavidade da geometria real do ANFLEX.

**Bugs relacionados, encontrados na mesma investigação** (viewer 3D, não o solver):
- Câmera do viewer (`CameraViewController.js`/`Riser3DRenderer.js`) tinha distância/far-plane hardcoded para a escala do Exemplo_01a (~130m) -- um modelo de ~1800m de lâmina d'água (Exemplo_02a) aparecia como uma fatia minúscula e distorcida. Corrigido: distância de câmera e far-plane agora proporcionais ao bounding box real do modelo carregado, com auto-enquadramento (`setView('ISO', ...)`) no carregamento.
- Raio do tubo 3D também hardcoded (0.6, calibrado para ~130m) -- virava uma linha sub-pixel, quase invisível, em modelos maiores. Corrigido: proporcional ao span real do modelo.
- Leitor `xml_h5_reader.py` tinha o nome do grupo XML/HDF5 hardcoded como `"group1"` (minúsculo); o Exemplo_02a exporta com `"Group1"` (maiúsculo), causando falha silenciosa (modelo com 0 nós/elementos) e crash (segfault) no `main_test`. Corrigido: nome do grupo descoberto dinamicamente.

## Retomando solo+corrente: três hipóteses testadas contra o ANFLEX real, nenhuma resolve sozinha

A pedido do usuário ("focar no ANFLEX real — análise estática e tipo de elemento — e criar um
plano de testes"), nova rodada de investigação lendo diretamente o código-fonte real do ANFLEX
(`c:/fred/proj/anflex/trunk/src`, não a reimplementação) em vez de só inferir comportamento.

**Elemento de viga: descartado como causa.** `cBeam` real (`src/beam.h`/`beam.cpp`) é uma viga
corrotacional Euler-Bernoulli com tríade de referência fixada na montagem
(`calc_init_rot_mt`, `beam.cpp:301`) — o `CorotationalBeam3D` do risersim já implementa o
equivalente exato (`node1_init_triad`/`node2_init_triad`, fixados na construção). Não há gap de
formulação de elemento a fechar.

**Hipótese 1 (testada): rampa de carga.** O ANFLEX real usa uma "time function" com rampa em
meio-cosseno (`cRampFunction::get_ramp`, `libs/anf_movements/src/ramp_function.cpp:30-42`:
`0,5*(1-cos(π·t/rampa))`) para escalar peso/empuxo/corrente durante a estática — não uma rampa
linear. O solo em si não tem função do tempo própria (`soil_uncoupled.cpp`/`soil_coupled.cpp` não
referenciam `cTimeFunction`); só "sente" a rampa indiretamente via o deslocamento que resulta das
forças externas rampadas. **Implementado** (`static_analysis.cpp:265`, `load_factor =
0,5*(1-cos(π·t))`). Sozinha, não resolveu — mas mudou a assinatura da falha de "explosão
catastrófica imediata" para "quase convergência antes de explodir" em vários casos.

**Hipótese 2 (testada): rigidez de atrito nunca amolece no ANFLEX real.** Em
`cUncoupledSoil::calc_unidimensional_friction` (`soil_uncoupled.cpp:144-173`) e
`cCoupledSoil::calc_friction` (`soil_coupled.cpp:108-162`), a rigidez tangente `k=fl/u_limit` é
calculada uma vez e nunca reduzida, mesmo quando a força é corrigida de volta pro limite de
Coulomb (`phi>0`) — só a força/estado inelástico muda, nunca a rigidez. O risersim reduzia a
rigidez para uma fração residual (2%) ou zero exato ao saturar (`friction_residual_stiffness_fraction`,
já removido). **Implementado**: `calculate_friction_1d` agora sempre retorna a rigidez elástica
plena, igual ao ANFLEX real. Isoladamente, transformou a explosão catastrófica do caso mínimo (33
elementos) num *chattering* limitado (oscilação 1e1-1e3, sem convergir em 40 iterações) — pior
localmente que o amolecimento anterior nesse caso específico, mas sem a explosão total.

**Descoberta adicional: Exemplo_01a usa solo desacoplado, Exemplo_02a usa solo ACOPLADO.**
Confirmado nos `.aml` reais: `%SOIL.UNCOUPLED` (Exemplo_01a) vs. `%OPTION.SOIL.COUPLED`
(Exemplo_02a, `Exemplo_02a.aml:228`). O modelo acoplado (`cCoupledSoil::calc_friction`) usa uma
superfície de escoamento combinada (`norm_f=sqrt(Fx²+Fy²)` capado em `sqrt(fl_ax²+fl_lat²)`, com
retorno radial de `lambda` compartilhado) em vez de dois springs 1D independentes. **Implementado**:
`SeabedInteraction::calculate_friction_coupled` (nova, réplica linha-a-linha da fórmula real) +
`SoilModel::{Uncoupled,Coupled}` selecionável via `environmental.seabed.soil_model` no JSON,
aplicado no branch certo em `analysis.cpp:105-118`. Também descoberto e corrigido de passagem:
`main_test.cpp` nunca lia `axial_friction`/`lateral_friction`/`axial_elastic_deflection_limit`/
`lateral_elastic_deflection_limit` do JSON — usava um `friction_coeff`/`0,05 m` isotrópico para
as duas direções, mesmo quando os valores reais divergem bastante entre si (ex. Exemplo_02a:
axial=1,2/0,03m vs. lateral=1,2/0,56m). Agora lido do JSON quando presente.

> **Nota de cautela (achada numa leitura posterior, ver
> [`mapa_aml_exemplos_e_web_interface.md`](mapa_aml_exemplos_e_web_interface.md)):** a inferência
> "Exemplo_02a → acoplado" acima se apoiava só na presença da tag `%OPTION.SOIL.COUPLED` no
> `.aml`. Uma leitura mais recente de `interfaces/src/aml.cpp` encontrou que essa tag legada está
> num caminho de migração **morto** no código-fonte atual: a variável que a aplicaria de volta aos
> solos (`s_temp_soil_coupled`, `aml.cpp:65`) nunca é atribuída em lugar nenhum, e a própria tag
> `%OPTION.*` está fora do mapa de leitura (`fmap`) — o bloco inteiro é ignorado silenciosamente por
> `cAml::readAML()`. Com esse código-fonte, todo solo carregaria como `uncoupled` por padrão,
> independente da tag legada. Isso não invalida necessariamente o resultado acima (é possível que a
> versão do `interfaces/src` que gerou o XML real do Exemplo_02a fosse diferente da que temos hoje,
> ou que o solo acoplado tenha sido setado por outro caminho não identificado), mas a premissa
> "Exemplo_02a é acoplado" não deveria mais ser tratada como confirmada só pela tag do `.aml` —
> precisaria reverificar contra o comportamento real do `Exemplo_02a_A1.xml` já exportado antes de
> confiar nela de novo.

**Resultado combinado (rampa + rigidez plena + atrito acoplado + valores axiais/laterais reais)
no Exemplo_02a completo**: a Fase 1 (rigidez artificial) chega muito perto de convergir — resíduo
estável e baixo (~800-1800 N) até a iteração ~20, depois explode subitamente entre as iterações
20-30. O mesmo padrão aparece na Fase 2 e no Exemplo_01a com o atrito desacoplado: **estável até
~iter 20, "parede" repentina depois**, não mais uma divergência gradual desde o início.

**Hipótese 3 (testada e REFUTADA): contato normal hiperbólico saturante vs. mola linear pura.**
Inspecionando nó a nó a explosão (iter 20-30 acima), achado um nó penetrando >2m no "solo" já no
primeiro passo de carga (2%) — fisicamente absurdo pra um contato quase-rígido. Causa aparente:
`calculate_seabed_reaction` usa um modelo hiperbólico saturante (`f(pen)=pen/(1/k+pen/f_ultima)`)
cujo `ultimate_bearing_force` **nunca era lido do JSON**, ficando travado no default de 5000 N —
muito pequeno pra um riser real pesado. O ANFLEX real não tem esse conceito: `cUncoupledSoil`/
`cCoupledSoil` usam uma mola linear pura, sem saturação (`m_forces[2] = -k*pen`,
`soil_uncoupled.cpp:90-91`). **Testado trocar para a mola linear pura, igual ao ANFLEX real** —
resultado: **piorou**, a Fase 2 passou a explodir para ~1e30 em vez de ~1e12 (força ilimitada faz
qualquer iterada ruim virar catástrofe maior). **Revertido** — a saturação hiperbólica do risersim,
apesar de não existir no ANFLEX real, funciona como uma rede de segurança real contra iteradas
ruins, não é a causa raiz. Mantida, com `ultimate_bearing_force` elevado para um teto muito mais
alto (1e7 N, antes 5000 N) que não deveria mais amarrar sob carga realista.

**Testado: mais iterações não resolvem.** Com o teto de iterações elevado de 40 para 150 no
Exemplo_02a, o resíduo **não converge nem decresce monotonicamente** — oscila caoticamente entre
ordens de grandeza (7,9e12 → 1,2e12 → 4e10 → 7,1e8 → ...). Confirma que não é "faltam iterações",
é uma instabilidade genuína no entorno da iteração ~20-100+.

**Estado atual**: as três hipóteses testadas hoje (rampa suave, rigidez de atrito plena, atrito
acoplado) individualmente e em conjunto mudam a assinatura da falha (de explosão imediata para
"quase convergência" ou chattering limitado) mas nenhuma resolve sozinha nem em combinação. A
mudança mais promissora identificada (contato normal linear puro) foi testada e refutada
experimentalmente. **Ainda não tentado**: suavizar a transição liga/desliga `pen>0`/`pen<=0` em si
(hoje só a *penetração* é suavizada, o cruzamento de zero continua uma chave dura) — candidato
natural dado que a "parede" em ~iter 20-30 tem a assinatura de muitos nós mudando de estado juntos;
inspecionar diretamente quantos nós cruzam `pen=0` simultaneamente nessa janela de iterações antes
de mudar mais código.

## Correção da Hipótese 1: peso não é rampeado no ANFLEX real -- só a corrente, por uma curva própria

Retomando solo+corrente numa nova rodada. A Hipótese 1 acima (rampa em meio-cosseno aplicada junto
a peso/empuxo/corrente) estava **parcialmente errada**, descoberto lendo `bar.cpp`/`beamSD.cpp`
diretamente em vez de só `ramp_function.cpp`: `cBar::calc_weight_load` (`bar.cpp:785-786`, idêntico
em `beamSD.cpp:1225-1226`) só aplica o fator de rampa (`AFATDB`/`m_gravitational_factor`) se
`m_has_gravitational_load` (`IWDB`) for verdadeiro -- e esse campo **nunca é atribuído em lugar
nenhum do código-fonte** (`grep` completo em `src/`, só é lido). É código morto: **peso próprio
(+ empuxo) sempre entra com magnitude total, sem rampa nenhuma, desde o primeiro passo**, em todo
o ANFLEX real.

A corrente, por sua vez, **é** rampeada -- mas por uma curva própria e independente, não a mesma
rampa estrutural. `Currents/Cor_S/static_function_id` aponta pra uma função (`Functions/StaTfDef`
no Exemplo_01a) cujos pontos reais (lidos do HDF5, `Functions/StaTfDef/points`) são
`(X=0,Y=0), (X=1,Y=0), (X=11,Y=1)` -- a corrente fica praticamente **zerada durante todo o primeiro
passo de carga** (`static_steps=11`) e só cresce gradualmente até o valor total no último passo. Não
é a mesma curva de meio-cosseno usada antes, e não tem relação nenhuma com o peso.

**Implementado**: peso desacoplado de qualquer rampa (`StaticIntegrator::assemble_load_vector`,
`static_integrator.cpp`, magnitude total sempre); corrente ganhou sua própria curva de rampa real,
lida do XML/HDF5 (`xml_h5_reader.py::extract_current_ramp()`, normalizada pro domínio `[0,1]` de
fração-de-passos e escrita em `environmental.current.ramp_x`/`ramp_y` no JSON) e interpolada
linearmente por passo em `StaticAnalysis::solve_catenary_static` (`static_analysis.cpp`); quando o
JSON não tem essa curva (modelos sintéticos, JSONs antigos), cai de volta na mesma rampa de
meio-cosseno de sempre para a corrente -- comportamento inalterado nesse caso.

**Resultado no Exemplo_01a completo (500 elementos, solo+corrente reais)**: a Fase 2 (carga total
num único passo, sem rigidez artificial) -- que antes explodia catastroficamente e nunca recuperava
-- agora **converge de verdade**, dado um orçamento maior de iterações (300 em vez do padrão 40 lido
do XML): resíduo sobe a ~3e7 nas primeiras iterações e depois **decresce establemente** até
convergir na iteração 190 (T_eff=257,4 kN no topo, valor plausível). É uma mudança qualitativa real
-- de "explode e nunca recupera" (o padrão de todas as tentativas anteriores desta investigação,
incluindo com 150-200 iterações no Exemplo_02a) para "pico controlado e convergência genuína,
só que mais devagar que o padrão de 40 iterações real do ANFLEX".

**Trade-off real encontrado e aceito deliberadamente** (peso deixar de ser rampeado é incondicional,
afeta qualquer caso, não só solo+corrente): dois casos que antes convergiam limpo com 40 iterações
passaram a precisar de mais:
- **300 elementos sem solo/corrente**: resíduo cai a ~1e-4 (praticamente zero) mas oscila sem nunca
  satisfazer o critério de razão de incremento -- o mesmo "chattering perto do limite" já
  documentado antes (seção "Retomando a investigação" acima), resolvido de verdade pela válvula de
  escape já existente (`enable_unbalanced_criteria`).
- **30 elementos perto do touchdown, só solo (sem corrente)**: mais sério -- falha no passo 6 com
  resíduo genuinamente grande (~1,5e4), e a válvula de escape **não** resolve, nem com 200 iterações.
  Atribuível ao peso pular pra 100% já no passo 1 em vez de entrar gradualmente, tornando a
  regularização por rigidez artificial do primeiro passo mais difícil nesse caso específico.

**Decisão do usuário**: aceitar e documentar como trade-off conhecido, sem mudar o default de
`max_iterations` nem investigar mais fundo o caso de 30 elementos nesta rodada. Fidelidade real ao
ANFLEX (peso sempre pleno) prevalece sobre manter esses dois casos convergindo com o orçamento
padrão de iterações.

**Lacuna que permanece em aberto**: mesmo no caso alvo (Exemplo_01a completo), risersim precisa de
~190 iterações onde o `num_max_iter=40` real do XML sugere que o ANFLEX real converge dentro desse
orçamento -- uma diferença de eficiência/fidelidade que esta correção não fecha. Candidatos não
investigados: o `BETA` de rigidez artificial do ANFLEX pode decair *entre* passos (não ser resetado
a cada novo passo como o risersim faz hoje), fazendo os 11 passos funcionarem como um refinamento
gradual da regularização em vez de (só) um ramp de carga -- ainda não confirmado lendo o código real.

## Correção estrutural: respeitar `%ASSEMBLY.USING` real em vez de forçar duas fases sempre

Investigando o gap de eficiência (risersim precisando de ~240+ iterações vs. as 87 reais do
Exemplo_01c pro mesmo caso solo+corrente), leitura cuidadosa de `newton_raphson.cpp`,
`integrator.cpp`, `static_integrator.cpp` e `beam.cpp::calc_artificial_stiffness` descartou
diferença de fórmula -- a rigidez artificial (`avg_EA_L * exp(-iter/1,25)`, rotacional=0,05x) já
bate exatamente com o ANFLEX real, e nenhum dos dois lados tem relaxação/projeção no update de
Newton.

**Causa real, estrutural**: `StaticAnalysis::solve()` sempre rodava um pipeline de duas fases fixo
-- Fase 1 "assembly" (rigidez artificial em todo passo) seguida de Fase 2 "static" (100% da carga
num único passo). O Exemplo_01a/01c real tem `%ASSEMBLY.USING.FALSE` no `.aml`/`.pml` -- confirmado
lendo `anflex_analysis.cpp`, o ANFLEX real **nunca roda a fase de assembly** pra esse modelo, só uma
única rampa suave de 11 passos, rigidez artificial gated a `step==1` (nunca reativada depois,
`static_integrator.cpp:196`) -- nunca há um salto de "todo o resto da carga num passo só".

**Implementado**: `StaticAnalysis::use_assembly_phase` (default `true`, preserva o comportamento de
duas fases pra qualquer JSON sem o dado real -- zero risco pra suíte de testes/modelo sintético).
Quando `false`, `solve()` roda uma única chamada de `solve_catenary_static` com
`ArtificialStiffnessMode::OnlyFirstStep` -- modo que **já existia** (era inclusive o default
documentado de `solve_catenary_static`, "risersim's historical behavior"), só nunca era invocado
por `solve()`. Fonte do dado real: `%ASSEMBLY.USING.TRUE`/`.FALSE` só existe no texto `.aml`/`.pml`
(convenção autodescritiva, sem linha de valor separada); nova função `extract_assembly_flag()` em
`xml_h5_reader.py`, chamada por `run_from_aml.py` (que já tem o caminho do `.aml` em mãos) e
injetada em `analysis_options.static.use_assembly_phase` no JSON.

**Resultado no Exemplo_01a real**: confirmado `use_assembly_phase=false` extraído corretamente do
`.aml`. Com o orçamento padrão de 40 iterações/passo, o passo 1 ainda não converge -- mas o padrão
mudou de "duas fases brigando" pra um resíduo pequeno (~7-8 N, `Rel R (ref) ~4,5e-4`, já abaixo da
tolerância nominal) que oscila devagar sem nunca satisfazer o critério de razão de
incremento -- a mesma assinatura de "chattering perto de um resíduo minúsculo" já documentada antes
(seção "Retomando a investigação", resolvida lá pela válvula de escape `enable_unbalanced_criteria`).
Testado com orçamento maior (300 iterações, só diagnóstico): passos 1-5 convergem (260, 10, 46, 65,
176 iterações -- lento e irregular, mas convergindo de verdade, sem explosão), passo 6 falha em 300.
É uma melhora real (antes, com o pipeline de duas fases, nem chegava no passo 6) mas ainda longe das
~8 iterações/passo do ANFLEX real -- a causa provável remanescente é a mesma oscilação perto do
critério de incremento já mapeada, agora afetando vários passos, não só um caso isolado de 20-30
elementos. `enable_unbalanced_criteria` existe no código mas não está exposto via JSON/`main_test.cpp`
-- candidato natural pra uma próxima correção, ainda não implementada nesta rodada.

**Bateria de não-regressão**: `risersim_tests` (343 asserts) sem mudança de referência (default
`true` preserva o pipeline de duas fases intacto); `risersim_diag_isolated_segment` (300 sem
solo/corrente, 30 elementos touchdown solo-only, 500 corrente-only) -- resultados idênticos aos de
antes desta mudança, como esperado (essa ferramenta não passa por `use_assembly_phase`).

## Ligando a válvula de escape real (`%ANALYSIS_CASE.STATIC.CONVERGENCE_CRITERIUM`) -- melhora, não resolve

Continuando a investigação do gap de eficiência: o AML real do Exemplo_01a tem
`%ANALYSIS_CASE.STATIC.CONVERGENCE_CRITERIUM = 'DISP_AND_FORCE'` e
`%ANALYSIS_CASE.STATIC.MAX_UNBALANCED = 1.0` -- confirmação direta e real de que o ANFLEX de
verdade usa o critério combinado (deslocamento + força/momento desbalanceado máximo), com o mesmo
valor de tolerância (1.0) que já era o default hardcoded da válvula de escape do risersim
(`StaticAnalysis::enable_unbalanced_criteria`/`unbalanced_force_tol`/`unbalanced_moment_tol`,
implementada numa investigação anterior mas nunca lida do AML real).

**Implementado**: `extract_static_convergence_criterium()` em `xml_h5_reader.py` (mesmo padrão de
`extract_assembly_flag()`, lendo o `.aml`/`.pml` direto -- esse dado também não existe no XML/H5),
chamada por `run_from_aml.py`, escrevendo `analysis_options.static.enable_unbalanced_criteria`/
`unbalanced_force_tol`/`unbalanced_moment_tol` no JSON quando o critério real inclui força.
`AnalysisOptionsConfig`/`main_test.cpp` ganharam os campos correspondentes, default `false`/`1.0`
(zero mudança de comportamento sem o dado real).

**Resultado no Exemplo_01a real, orçamento padrão de 40 iterações/passo (o mesmo do XML real)**:
- **Passo 1**: converge em 38 iterações (antes falhava sempre) -- via a válvula de escape mesmo
  (`norm_R=758,8 N`, ainda um resíduo relativamente alto, mas dentro do critério de força
  desbalanceada máxima).
- **Passo 2**: converge em 13 iterações, resíduo genuinamente pequeno (1,62 N) -- convergência
  "de verdade", não só a válvula de escape.
- **Passo 3**: ainda falha -- resíduo cresce a ~1,2-1,5e4 e **oscila nesse patamar** (não explode
  sem limite) até esgotar as 40 iterações.

É uma melhora real e mensurável (antes desta correção, nem o passo 1 convergia dentro do orçamento
padrão) mas não fecha o gap completo contra as ~8 iterações/passo do ANFLEX real.

**Achado colateral, revela a fragilidade da válvula de escape**: testado com orçamento maior (300
iterações, só diagnóstico) -- contraintuitivamente, o **passo 1 passou a falhar** onde antes
convergia com 40. Causa: a válvula de escape só é avaliada nas **últimas 3 iterações do orçamento**
configurado (`ConvergenceTest`, já documentado). Com `max_iter=40`, a janela de escape cai nas
iterações 38-40, pegando por sorte um momento do ciclo de oscilação onde o pior resíduo pontual já
estava abaixo da tolerância; com `max_iter=300`, a mesma janela desliza pra 298-300, um ponto
diferente (e aparentemente pior) do mesmo ciclo oscilatório. Ou seja, a convergência via válvula de
escape hoje depende de "sorte" de onde a janela de 3 iterações cai dentro de um ciclo de chattering,
não de uma garantia real -- consistente com a conclusão já registrada antes ("não resolve, é uma
rede de segurança parcial").

**Estado em aberto**: a causa raiz do chattering nos passos 3+ permanece a mesma já mapeada
(oscilação perto de uma descontinuidade solo+corrente, sem nenhuma técnica de estabilização
direcional tipo Armijo ou limitação de passo por nó/GDL implementada). Não investigado mais fundo
nesta rodada -- ver as direções já listadas nas seções anteriores (line search direcional, limitar
o passo por nó/GDL, suavizar a transição liga/desliga do contato).

**Bateria de não-regressão**: `risersim_tests` (343 asserts) sem mudança; `risersim_diag_isolated_segment`
(300 sem solo/corrente, 30 touchdown solo-only, 500 corrente-only) -- resultados idênticos aos de
antes (essa ferramenta não passa por `enable_unbalanced_criteria`).

## Atrito axial/lateral real do solo nunca chegava ao pipeline principal: corrigido, melhora real mas parcial

Lendo a interface gráfica real (`trunk/interfaces/src`, ver
[`mapa_classes_anflex_interface.md`](mapa_classes_anflex_interface.md)) para mapear como o modelo é
construído e o AML/PML/XML/DAT são gerados, achamos uma lacuna concreta que tinha passado
despercebida por toda esta investigação: **`tools/xml_h5_reader.py` nunca extraía os parâmetros de
atrito anisotrópico do solo do XML real** (`axial_friction`, `lateral_friction`,
`axial_elastic_deflection_limit`, `lateral_elastic_deflection_limit`, flag `coupled`) — só um
`lateral_friction` isotrópico ia para o JSON.

Consequência concreta, confirmada lendo `risersim/src/simulation.cpp:32-38`: como as chaves
`axial_friction`/`axial_elastic_deflection_limit`/etc. nunca existiam no JSON,
`EnvironmentalConfig` ficava nos sentinelas -1.0 (`model.hpp:31-34`), e `SeabedInteraction` nunca
era sobrescrita — ficava no que o **construtor** já preenche por padrão
(`seabed.hpp:50-53`): `axial_friction = lateral_friction = friction_coeff` (0,95, o único valor
real que já vinha do XML) e `axial_elastic_deflection_limit = lateral_elastic_deflection_limit =
0,05 m` (default isotrópico do construtor, não o valor real do modelo). Ou seja: **toda rodada do
pipeline completo (`run_from_aml.py` → `risersim_test_main`) contra o Exemplo_01a real ao longo
desta investigação inteira usou atrito isotrópico (0,95/0,05 m nas duas direções)**, nunca os
valores reais e bem diferentes entre si que o `Exemplo_01a_A1.xml` de fato tem (`axial_friction=
0,92`/`axial_elastic_deflection_limit=0,03 m` vs. `lateral_friction=0,95`/
`lateral_elastic_deflection_limit=0,2779 m`, confirmado lendo `Exemplo_01a_A1.xml:784-800`). A
ferramenta de diagnóstico isolado (`diag_isolated_segment.cpp`) já usava os valores reais, mas
**hardcoded manualmente** no próprio arquivo de teste — nunca vindos do JSON gerado pelo pipeline
principal.

**Corrigido**: `xml_h5_reader.py` agora lê `Soils/Solo/axial_friction`,
`.../lateral_friction`, `.../axial_elastic_deflection_limit`, `.../lateral_elastic_deflection_limit`
e `.../coupled` (mapeado para `"coupled"`/`"uncoupled"`) diretamente do XML, escrevendo-os em
`environmental.seabed` só quando de fato presentes na fonte (sem forçar um valor "de mentirinha"
quando ausentes — nesse caso o comportamento isotrópico de fallback de antes continua intacto).
Bateria de regressão completa (343 asserts Catch2) sem mudança, como esperado (mudança puramente
aditiva).

**Resultado no Exemplo_01a real** (mesmo `enable_unbalanced_criteria=true`/`max_unbalanced=1.0`
lido do AML real, mesmos 40 iterações/passo — única variável isolada é o atrito passar a ser o
real e anisotrópico em vez do isotrópico 0,95/0,05 m):

| Passo (Current Factor) | Antes (atrito isotrópico 0,95/0,05 m, seção "Ligando a válvula de escape real") | Depois (atrito real: axial 0,92/0,03 m, lateral 0,95/0,2779 m) |
|---|---|---|
| 1 (0%) | Converge em 38 iterações (via válvula de escape) | Converge em **25** iterações |
| 2 (10%) | Converge em 13 iterações | Converge em **9** iterações |
| 3 (20%) | ❌ Falha — resíduo cresce a ~1,2-1,5×10⁴ e oscila nesse patamar | ✅ **Converge em 19 iterações** (com um pico intermediário a 7,1×10⁴ na iteração 10, recuperando depois) |
| 4 (30%) | (nunca chegou a rodar) | ❌ Falha — resíduo cresce a 1,69×10⁵ (iter 10) → 1,50×10⁴ (iter 20) → 1,20×10⁴ (iter 30), mesma assinatura de patamar alto sem explodir |

É uma melhora real e mensurável — o modelo avança um passo de carga inteiro a mais (do passo 3 para
o passo 4) antes de esbarrar na mesma assinatura de divergência já documentada (resíduo que cresce
e estagna num patamar alto, ~10⁴, sem explodir sem limite) — **mas não resolve**: o passo 4 falha
com um padrão qualitativamente idêntico ao que já travava o passo 3 antes desta correção. Isso é
consistente com o padrão já visto ao longo de toda esta investigação (rotação → decaimento →
atrito → rampa de carga → critério de convergência): cada correção de fidelidade real empurra o
limiar um pouco mais longe, nenhuma sozinha fecha o gap completo contra as ~8 iterações/passo do
ANFLEX real. A causa raiz continua sendo a mesma já identificada e não implementada: falta uma
técnica de estabilização direcional (line search tipo Armijo, limitar o passo por nó/GDL, ou
suavizar a transição liga/desliga do contato vertical).

**Achado colateral, fora de escopo desta correção**: a mesma leitura da interface encontrou que
`cAssembly` (fase de assembly real) tem seu próprio orçamento de convergência
(`MAX_ITERATION=20` default, tolerância/critério próprios), independente do `cAnalysis` estático
(`MAX_ITERATION=40`) — mas `StaticAnalysis::solve()` hoje reusa o orçamento da fase estática para
as duas fases quando `use_assembly_phase=true`, e `xml_h5_reader.py` não extrai
`AnalysisData/Assembly/...` do XML nenhum. Não afeta o Exemplo_01a (`%ASSEMBLY.USING.FALSE`), mas
é um candidato real para investigar em qualquer exemplo que tenha o assembly ligado — ver
[`mapa_classes_anflex_interface.md`](mapa_classes_anflex_interface.md).

## Limitar o passo de Newton por norma física: ajuda casos isolados, mas piora o modelo completo

Primeira tentativa das três técnicas ainda não experimentadas listadas no
[roadmap](roadmap.md) (Eixo 1a). Implementado `StaticAnalysis::enable_step_limiting`/
`max_translation_step_m`/`max_rotation_step_rad` (default `false`/`0.5`/`0.3`, zero mudança de
comportamento a menos que habilitado): `apply_newton_step_with_line_search()`
(`static_analysis.cpp`) agora calcula, **antes** do laço de backtracking já existente, o maior
`alpha` inicial que mantém o deslocamento/rotação de todo nó dentro desse teto — em vez de sempre
começar o backtracking em `alpha=1` (passo cheio) e só cortar depois se o resíduo não melhorar. A
ideia: quando a rigidez tangente local de um nó fica quase singular (contato+atrito saturando ao
mesmo tempo), `K⁻¹·Resíduo` pode produzir um `step_dU` gigante mesmo para um resíduo modesto — um
nó sendo arremessado vários metros numa única iteração, o padrão já documentado de "chattering".
Capar o passo pela própria norma física ataca isso na origem, antes do line search por resíduo (um
teto frouxo de 100x, ver seção acima) sequer ter chance de intervir.

**Bateria de não-regressão**: `risersim_tests` (343 asserts) sem nenhuma mudança de valor de
referência, como esperado (default desligado).

**Resultado no caso isolado de 33 elementos (touchdown, solo+corrente reais)** — combinado com
`enable_unbalanced_criteria` (já necessário mesmo sem step limiting, ver seções acima), varrendo o
teto de deslocamento/rotação de 0.05 m até 0.001 m:

| Teto (m/rad) | Resultado |
|---|---|
| 0.05 (ou mais frouxo) | Idêntico a sem step limiting — nunca chega a ativar o corte nesse caso |
| 0.01 | Passos 1-3 convergem, passo 4 falha (resíduo oscilando ~2,4-16,7×10³) |
| **0.002-0.004** | **Passos 1-5 convergem, passo 6 falha** (resíduo oscilando ~500-1400 N, sem explodir) — melhor resultado, uma faixa estável, não um ponto isolado de sorte |
| 0.001 (mais apertado) | Piora — passo 5 falha (mais cedo que com 0.002) |
| 0.002 com `max_iter=150` (em vez de 40) | Piora — passo 3 falha (mais cedo). Mesma fragilidade já documentada da válvula de escape: mais iterações desliza a janela de "últimas 3" pra outro ponto do ciclo de chattering, não necessariamente melhor |

Isolado, é uma melhora real e mensurável: o caso de 20 elementos touchdown solo-only (que falhava
no passo 1 baseline, por uma diferença de resíduo minúscula — 1,28 N, perto da tolerância) passa a
**convergir completamente** com teto 0.002. O caso de 33 elementos avança do passo 2 (sem step
limiting, só com a válvula de escape) até o passo 5.

**Mas o mesmo teto piora o caso de 300 elementos sem solo/corrente** (que já convergia quase por
completo no baseline — falhava só no passo 3, com resíduo já em ~1,2×10⁻⁴, essencialmente zero,
perto da tolerância): com teto 0.002 passa a falhar já no **passo 1**, com resíduo bem mais alto
(~5,5×10³) — uma regressão clara, não uma melhora.

**Confirmado no Exemplo_01a completo real** (500 elementos, com a correção de atrito anisotrópico
já aplicada nesta sessão): o mesmo padrão se repete, pior ainda —
- Teto 0.002/0.002: passo 1 (que convergia em 25 iterações sem step limiting) passa a **falhar**,
  resíduo estagnando em ~6,5×10³.
- Teto 0.05/0.05 (bem mais frouxo, "inerte" no caso isolado de 33 elementos): passo 1 volta a
  convergir em 25 iterações (idêntico ao baseline), mas o **passo 2** (que convergia em 9 iterações
  sem step limiting) passa a **falhar**, resíduo crescendo até ~5,8×10³.

**Conclusão honesta**: o mecanismo funciona exatamente como projetado (verificado por varredura, não
só por leitura de código) e ajuda genuinamente casos isolados pequenos, mas **capar o passo
uniformemente para o modelo inteiro** penaliza justamente as correções grandes e legítimas que
outras partes da malha de 500 elementos precisam para avançar — o mesmo teto que resolve o
touchdown atrapalha o resto. Isso não é o mesmo tipo de fragilidade já visto no line search por
resíduo (que também não resolveu, mas pelo menos não regredia casos bons quando frouxo o
suficiente) — aqui, qualquer teto apertado o bastante pra ajudar o touchdown já é apertado o
bastante pra atrapalhar em outro lugar do modelo completo, uma faixa útil estreita demais que não
sobrevive à escala real do modelo.

O código fica no repositório (opt-in, `enable_step_limiting=false` por padrão, zero risco pra
qualquer caller existente) — é uma ferramenta real de diagnóstico/ajuste fino por caso, não uma
correção de propósito geral. **Não resolve o bug solo+corrente no modelo completo.** Próximo passo
recomendado: uma das outras duas técnicas do roadmap (line search direcional tipo Armijo, ou
suavizar a transição liga/desliga do contato vertical) — ou uma versão *seletiva* deste mecanismo
(aplicar o teto só a nós perto de uma transição de contato/atrito ativa, não à malha inteira),
fora de escopo desta rodada.

> **Nota retroativa**: os dois testes acima (caso isolado de 33 elementos e Exemplo_01a completo)
> rodaram sob a corrente **uniforme e superestimada** descrita na seção seguinte (o perfil real
> nunca era consultado por profundidade) — ou seja, o "chattering" que o step limiting tentava
> conter era, pelo menos em parte, causado por uma carga de corrente fisicamente errada demais
> perto do touchdown, não só por uma dificuldade numérica intrínseca do Newton. Isso não invalida
> o achado (a técnica genuinamente não generalizava bem pro modelo completo, medido como estava),
> mas explica por que ela nunca teria sido suficiente sozinha — e por que vale reconsiderar,
> depois da correção abaixo, se ainda é necessária.

## Bug real encontrado e corrigido: perfil de corrente sempre avaliado na superfície -- resolve o bug solo+corrente

A pedido do usuário ("vamos revisar os dados agora com AML e XML, vamos verificar se não está
faltando nenhum dado ou se tem algum dado com unidade errada"), uma auditoria direta comparando o
XML real, `xml_h5_reader.py` e o consumo em C++ encontrou dois bugs encadeados na extração/uso do
perfil de corrente -- juntos, a causa raiz de boa parte (talvez toda) a divergência solo+corrente
investigada ao longo deste documento inteiro.

**Bug 1 (`xml_h5_reader.py::extract_current_profile()`)**: o perfil real no XML
(`Currents/Cor_S/profile/values`, confirmado em `Exemplo_01a_A1.xml:928-935`) é tabulado como
"profundidade a partir do leito marinho" (0=fundo, crescente até a superfície) -- ex.: `(0m, 90°,
0.34 m/s)` no fundo até `(265m, 270°, 1.78 m/s)` na superfície. A função só reordenava os
**índices** (mais raso primeiro, pra `Simulation::run()` usar `.front()` como valor de superfície)
mas nunca transformava os **valores** de profundidade -- o array `depths_m` ficava
`[265, 225, ..., 0]`, ainda na convenção "altura acima do leito", só que descendente.

**Bug 2 (`current_profile.hpp::get_velocity()`/`get_heading()`)**: essas funções calculavam
`depth_from_surface = std::max(0.0, -z)`, assumindo implicitamente que a superfície do mar está em
`z=0` e o leito em `z` negativo -- a convenção do modelo **sintético** de fallback. Mas
`ModelBuilder` alinha `seabed_depth_z` ao Z **real** dos nós lidos do XML/H5
(`model_builder.cpp:258-263`), que no frame nativo do AML fica com o leito perto de `z≈0` e a
**superfície em `z≈+265`** -- exatamente o oposto. Como todo `z` real deste modelo é `>=0`,
`depth_from_surface = max(0, -z)` dava **sempre 0**, para qualquer nó, em qualquer profundidade.

**Efeito combinado**: `interp1(depths_m, velocities_ms, depth_from_surface=0)` -- com uma tabela
descendente (bug 1) e uma consulta sempre em `x=0` (bug 2) -- caía sempre no primeiro ramo de
`interp1()` (`x <= x_table.front()`) e devolvia `velocities_ms.front()` = **1.78 m/s / 270°, o
ponto de SUPERFÍCIE, para todo elemento da linha inteira**, inclusive os que estão encostados no
leito marinho na zona de touchdown (onde o valor real é 0.34 m/s). Como a força de arrasto escala
com v², isso aplicava até **~27x mais carga lateral que a real** exatamente na região onde
solo+atrito já são mais delicados -- coerente com o padrão de "chattering" perseguido a sessão
inteira (rotação → decaimento → atrito → rampa de carga → critério de convergência → step
limiting), sem que nenhuma dessas correções anteriores pudesse ter revelado o problema, já que
todas eram testadas sob essa mesma carga de corrente uniformemente superestimada.

Corrigidos os dois: `extract_current_profile()` agora transforma cada ponto para
`profundidade_da_superfície = profundidade_total_da_água - altura_acima_do_leito` antes de
guardar (mantendo a mesma ordem de índices, que já ficava correta por coincidência);
`CurrentProfile` ganhou um novo campo `water_surface_z` (default `0.0`, preserva exatamente o
comportamento do modelo sintético, que nunca seta esse campo), e `get_velocity()`/`get_heading()`
passaram a calcular `depth_from_surface = water_surface_z - z` em vez de `-z`;
`Simulation::run()` agora propaga `model->environmental.water_surface_z` (já calculado por
`ModelBuilder`, só nunca chegava até o `CurrentProfile`) pro novo campo.

**Bateria de não-regressão**: `risersim_tests` (343 asserts) sem nenhuma mudança de valor de
referência -- o modelo sintético usado nos testes automatizados nunca seta `water_surface_z`
(fica no default `0.0`, idêntico à convenção "superfície em z=0" que o código sempre assumiu),
então esse caminho é bit-a-bit inalterado. O bug só existia para modelos reais derivados de
XML/H5, que nunca tiveram cobertura automatizada de regressão para o comportamento por
profundidade da corrente.

**Resultado no Exemplo_01a completo real** (500 elementos, com a correção de atrito anisotrópico
já aplicada nesta sessão, **sem** step limiting habilitado):

```
✅ Step  1 Converged in 25 iterations! (norm_R = 0.61 N)
✅ Step  2 Converged in  4 iterations! (norm_R = 0.004 N)
✅ Step  3 Converged in 38 iterations! (norm_R = 7.49 N)
✅ Step  4 Converged in  4 iterations! (norm_R = 0.63 N)
✅ Step  5 Converged in 26 iterations! (norm_R = 1.01 N)
✅ Step  6 Converged in 38 iterations! (norm_R = 128 N)
✅ Step  7 Converged in 38 iterations! (norm_R = 531 N)
✅ Step  8 Converged in 38 iterations! (norm_R = 109 N)
✅ Step  9 Converged in 38 iterations! (norm_R = 332 N)
✅ Step 10 Converged in 38 iterations! (norm_R = 60 N)
✅ Step 11 Converged in 38 iterations! (norm_R = 51 N)

🎉 STATIC CATENARY ANALYSIS CONVERGED SUCCESSFULLY!
  [TOP] X=-47.73 m, Z=257 m
  [T_eff TOP] 217.3 kN
```

**Todos os 11 passos convergem** -- o bug de convergência estática solo+corrente do Exemplo_01a,
investigado exaustivamente ao longo deste documento inteiro (rotação total-vs-local, decaimento da
rigidez artificial, atrito elástico-plástico incremental, atrito axial/lateral real, rampa de
carga real da corrente, critério de convergência combinado, `%ASSEMBLY.USING` real, atrito
anisotrópico do solo, e por fim o perfil de corrente por profundidade), **está resolvido**. A
tração efetiva no topo (217,3 kN) é um valor fisicamente plausível para este riser.

Vale registrar o padrão da investigação inteira: cada correção anterior (rotação, decaimento,
atrito, rampa, critério combinado) era real, bem fundamentada e mediu uma melhora genuína -- mas
nenhuma delas *sozinha* fechava o gap porque a causa dominante remanescente (corrente uniforme e
superestimada na zona de touchdown) continuava lá, atrapalhando qualquer uma delas. Isso é consistente
com a "auditoria de dados" ter sido a abordagem certa depois de esgotar as hipóteses puramente
numéricas/algorítmicas -- o problema não estava em como o Newton-Raphson lidava com a
não-linearidade, estava em qual carga ele via em primeiro lugar.

## Pendências resolvidas: step limiting, outros consumidores de `CurrentProfile`, e um novo bug de massa dinâmica

Três pendências deixadas na seção anterior, resolvidas na mesma rodada:

**1. Step limiting não é mais necessário.** O resultado acima (11/11 passos convergindo) não usa
`enable_step_limiting`. Confirma a suspeita já registrada: o "chattering" que o step limiting
tentava conter era, pelo menos em grande parte, sintoma da corrente uniformemente superestimada,
não uma dificuldade numérica intrínseca do Newton. O código continua no repositório (opt-in,
desligado por padrão) como ferramenta de ajuste fino, sem uso conhecido no momento.

**2. `CurrentProfile` é usado também pela análise dinâmica -- já corrigido automaticamente.**
`DynamicAnalysis(const Analysis&)` (`dynamic_analysis.hpp:59-74`) copia `current` por valor de
`StaticAnalysis` (`current = static_analysis.current;`), então o `water_surface_z` corrigido já
chega à dinâmica sem nenhuma mudança adicional -- confirmado lendo o construtor, não só supondo.
Não foi encontrado nenhum bug análogo em `hydrodynamics.hpp`/força de onda: a força de onda neste
código não é um carregamento distribuído dependente de profundidade (Airy/Morison ao longo da
linha inteira) -- é só um deslocamento prescrito no nó de topo (`disp_z = amplitude*sin(omega*t)`,
`dynamic_analysis.cpp:69`, representando o RAO/heave do casco), então não há eixo `z` de onda para
ter a mesma confusão de convenção.

**3. Novo bug real encontrado ao investigar a dinâmica: `rho` (peso submerso) reaproveitado como
massa estrutural.** Rodando o Exemplo_01a completo com dinâmica habilitada pela primeira vez desde
que a estática passou a convergir, a dinâmica falhava em **todos os 20 passos de tempo**. Auditando
os dados de massa (mesma disciplina desta seção): `element_beam.hpp:24` documentava `rho` como
"Structural mass density (kg/m^3)" (default 7850, aço), mas `model_builder.cpp` (comentário
original: *"Maps density directly from the XML's real submerged weight, for pure physical
consistency"*) na verdade deriva `rho` do **peso já submerso** (líquido de empuxo) do XML --
correto para a fórmula de peso estático (`w_dry = rho*A*g`, com `water_density=0` quando o modelo
vem de JSON real, pra não subtrair empuxo duas vezes), mas fisicamente errado para inércia: empuxo
reduz o peso líquido de um corpo, mas nunca reduz sua massa inercial (F=ma). E
`CorotationalBeam3D::total_linear_mass()` (`element_beam.hpp:135-140`, usada só por
`DynamicAnalysis`, nunca pela estática) reaproveitava esse mesmo `rho` pra massa. Pro Exemplo_01a:
`rho` (peso submerso) ≈ 1587 kg/m³ vs. `rho_structural` real (derivado do peso **seco**, também já
calculado no Python mas descartado) ≈ 3790 kg/m³ -- a massa dinâmica estava subestimada em **~2,4x**.

**Corrigido**: novo campo `rho_structural` em `BeamMaterialProps` (default 7850, igual a `rho`,
pra modelos sintéticos ficarem exatamente como antes), usado por `total_linear_mass()` no lugar de
`rho`; `xml_h5_reader.py` agora calcula e escreve `rho_structural` a partir do peso seco real
(`weight_dry_kNm`, já computado, só nunca exposto no JSON); `model_builder.cpp` lê o novo campo com
fallback pra `elem_props.rho` quando ausente (preserva o comportamento -- com o mesmo bug -- de
JSONs antigos, em vez de piorar). `risersim.BeamMaterialProps` (binding Python) ganhou a mesma
propriedade. Bateria de regressão (343 asserts) sem mudança -- `total_linear_mass()` só é exercida
pela dinâmica, que não tem cobertura automatizada nenhuma ainda.

**Resultado no Exemplo_01a completo** (com todos os fixes desta sessão): a dinâmica passa a
convergir em **15 dos 20 passos de tempo** (antes: 0 de 20) -- os passos que ainda falham são 1, 5,
6, 7 e 8 (resíduo crescendo: 326→354→666→1276 N nos passos 5-8). Testado se era só falta de
orçamento de iteração (`max_iterations` de 20 pra 60): **não ajudou**, os mesmos 5 passos continuam
falhando, com resíduos ainda maiores -- descarta "faltam iterações" como explicação, mesma
assinatura já vista na estática (mais iterações desliza a janela de convergência pra um ponto pior
do ciclo, não resolve). É uma melhora real e substancial, mas **a dinâmica ainda não está
totalmente resolvida** -- fica como próximo passo dedicado do Eixo 1b (não investigado mais fundo
nesta rodada: candidatos óbvios a checar primeiro são o amortecimento de Rayleigh
(`alpha_rayleigh`/`beta_rayleigh`, ainda hardcoded no construtor de `DynamicAnalysis`, nunca lidos
do JSON) e a magnitude/fase da força de onda nos primeiros ciclos).

**Achado colateral importante -- o sistema está bem na borda da convergência via válvula de
escape.** Durante essa investigação, comparando duas rodadas do Exemplo_01a que deveriam ser
idênticas (só a presença ou não da chave `rho_structural` no JSON, um campo que a estática **nunca
lê**), a trajetória de convergência da fase estática mudou visivelmente passo a passo (ex.: passo 2
levou 4 iterações numa rodada e 17 na outra) -- mas o resultado físico final foi **idêntico**
(T_eff = 217,3 kN nas duas). Isso confirma que o sistema, mesmo convergindo, está operando bem
perto de uma bifurcação numérica onde qualquer perturbação incidental (nada relacionado à física
em si) pode deslocar exatamente qual passo precisa da válvula de escape -- consistente com todo o
histórico de "chattering" já documentado neste arquivo. Não é uma regressão nem um bug novo, mas é
um lembrete de que comparações passo-a-passo entre rodadas devem olhar o resultado físico final
(tração, posição), não o número exato de iterações de cada passo individual, que tem ruído
inerente.

## Ver também

- [`roadmap.md`](roadmap.md) — plano de trabalho consolidado (Eixo 1a é este bug).
- [`mapa_classes_anflex_interface.md`](mapa_classes_anflex_interface.md) — mapa da interface
  gráfica real (`trunk/interfaces/src`): construção de modelo, leitura AML/PML, exportação DAT/XML.
- `risersim/docs/opcoes_bibliotecas_opensource.md` — levantamento de bibliotecas open-source (Project Chrono, MAP++, MoorDyn-C, MoorPy) e o resultado da tentativa de warm-start com MoorPy (que motivou este documento).
