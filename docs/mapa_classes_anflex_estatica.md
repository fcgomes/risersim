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

## Ver também

- `risersim/docs/opcoes_bibliotecas_opensource.md` — levantamento de bibliotecas open-source (Project Chrono, MAP++, MoorDyn-C, MoorPy) e o resultado da tentativa de warm-start com MoorPy (que motivou este documento).
