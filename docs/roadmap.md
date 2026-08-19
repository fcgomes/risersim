# Roadmap do `risersim`

> Documento de planejamento, não de investigação (diferente dos três `mapa_*.md`). Consolida os
> seis objetivos que o usuário definiu em 2026-08-07 — resolver o bug da estática, rodar a análise
> dinâmica, implementar itens faltantes, interface de entrada de dados, interface de controle de
> simulação (projetos/disparo/acompanhamento) e pós-processamento — em eixos com dependências e uma
> ordem sugerida. Não é uma decisão fechada; serve de base para decidir, um eixo de cada vez, o que
> planejar em detalhe a seguir.

## Eixo 1 — Confiabilidade do motor (bloqueante para qualquer resultado em que se possa confiar)

### 1a. Fechar o bug estático solo+corrente — ✅ RESOLVIDO

Depois de exaustivamente investigado (rotação total-vs-local, decaimento da rigidez artificial,
atrito elástico-plástico, atrito axial/lateral real, rampa de carga real, critério de convergência
combinado, `%ASSEMBLY.USING` real) sem fechar o gap, e depois de uma tentativa de estabilização
numérica (limitar o passo por norma física) que ajudava casos isolados mas piorava o modelo
completo, a causa raiz acabou sendo um **bug de dados**, não numérico: o perfil de corrente por
profundidade nunca era de fato interpolado — dois bugs encadeados (`xml_h5_reader.py` guardando a
profundidade na convenção errada + `CurrentProfile` assumindo superfície em `z=0` quando o modelo
real tem o leito em `z≈0`) faziam toda consulta cair no valor de **superfície**, aplicando até
~27x mais carga lateral que a real na zona de touchdown. Corrigido — ver
[`mapa_classes_anflex_estatica.md`](mapa_classes_anflex_estatica.md), seção "Bug real encontrado e
corrigido: perfil de corrente sempre avaliado na superfície". **Os 11 passos de carga do
Exemplo_01a completo convergem agora**, sem precisar do step limiting.

Lição para o resto do roadmap: depois de esgotar hipóteses numéricas/algorítmicas razoáveis, uma
auditoria direta de dados (comparar o valor real no XML contra o que de fato chega no C++, campo a
campo) foi o que resolveu — vale aplicar essa mesma disciplina antes de partir para técnicas mais
sofisticadas em qualquer outra frente (ex. 1b abaixo).

**Pendências resolvidas** (ver `mapa_classes_anflex_estatica.md`, seção "Pendências resolvidas"):
step limiting confirmado desnecessário (resultado atual não o usa); `CurrentProfile` confirmado
usado também pela dinâmica, já corrigido automaticamente por herança de valor
(`DynamicAnalysis(const Analysis&)`), sem bug análogo na força de onda (não é dependente de
profundidade neste código). Achado colateral: o sistema convergido está bem perto de uma
bifurcação numérica — a trajetória passo-a-passo (não o resultado físico final) é sensível a
detalhes incidentais, então comparações futuras devem olhar o resultado final, não iterações
por passo.

**Confirmação adicional** (auditoria de nomenclatura/conversão, ver
`mapa_aml_exemplos_e_web_interface.md`, achado 2): eliminar o `rho` fabricado (densidade
"peso-equivalente") em favor da densidade seca real + `water_density` real reforçou o achado
acima -- mesmo uma diferença de ~0,0006% no peso líquido por metro foi suficiente pra fase de
assembly (pré-solve) falhar em convergir dentro do orçamento padrão de 40 iterações nalguns runs,
exigindo mais iterações (150 testado) pra chegar num ponto de partida bom o bastante pra fase
final. O resultado físico final continua correto e estável (T_eff≈217,3-217,4 kN) -- só o
orçamento de iteração da assembly pode precisar de folga maior em modelos reais como este. Vale
considerar, quando o Eixo 1a for revisitado, aumentar o default de `static.max_iterations` da
assembly (ou torná-lo independente do valor do XML) em vez de manter os 40 vindos de
`AnalysisData/Static/num_max_iter`.

### 1b. Investigar a análise dinâmica — ✅ RESOLVIDO (Exemplo_01a converge os 20 passos completos)

Começada nesta rodada, seguindo a mesma disciplina que resolveu 1a (auditoria de dados antes de
técnica numérica). Rodando o Exemplo_01a completo com dinâmica pela primeira vez desde que a
estática passou a convergir: **todos os 20 passos de tempo falhavam**. Auditoria encontrou um bug
real e concreto: `total_linear_mass()` (massa/inércia dinâmica) reaproveitava `rho` -- que na
verdade é um valor "equivalente de peso submerso" (líquido de empuxo), correto só para a fórmula
de peso estático -- como se fosse densidade estrutural real. Empuxo reduz peso líquido mas nunca
reduz massa inercial; pro Exemplo_01a isso subestimava a massa dinâmica em **~2,4x**. Corrigido
(novo campo `rho_structural`, derivado do peso seco real do XML, já calculado mas antes
descartado) — ver `mapa_classes_anflex_estatica.md`, seção "Pendências resolvidas".

**Resultado**: a dinâmica passa a convergir em **15 dos 20 passos** (antes: 0). Real e
substancial, mas não resolvido -- passos 1 e 5-8 ainda falham (resíduo crescendo, não platô);
testado orçamento maior de iterações, não ajudou (mesma assinatura de "janela de escape desliza
pra pior" já vista na estática).

**Atualização** (ver `mapa_aml_exemplos_e_web_interface.md`, seção "Auditoria de conversões de
valor", achado 4): o amortecimento de Rayleigh JÁ era lido corretamente do JSON pelo C++
(`simulation.cpp:100-101` — a suspeita anterior de "hardcoded no construtor" estava desatualizada);
o gap real era `xml_h5_reader.py` nunca extrair o valor verdadeiro do XML (`mass_damping`/
`stiffness_damping` por material, já calculados pelo ANFLEX real), hardcodando `alpha=0.05,
beta=0.005` sempre. Corrigido -- os 7 XMLs de exemplo disponíveis têm `consider_damping="no"`
(amortecimento real = 0), então o Exemplo_01a hoje roda com o dado fiel. Verificado: não muda o
resultado estático, e o padrão de convergência dinâmico ficou essencialmente igual (resíduos um
pouco maiores sem o amortecimento fabricado que antes ajudava marginalmente) -- ou seja, esse não
era o fator limitante da dinâmica. A contagem exata de passos convergidos variou entre verificações
consecutivas mesmo sem nenhuma mudança de dado de entrada (4/20 nesta rodada vs. 15/20 documentado
antes) -- confirma que é a mesma sensibilidade de "beira de bifurcação" já registrada em 1a, não
uma regressão.

**Atualização 2** (ver `mapa_aml_exemplos_e_web_interface.md`, seção "Movimento de topo real"):
implementado o movimento real de topo (RAO + JONSWAP "equivalent harmonic", novo módulo
`vessel_motion.hpp`/`.cpp`), substituindo a onda regular Z-só que existia antes. Achado colateral:
a suspeita de que faltava migrar o movimento prescrito do topo pra `PrescribedMotion` (a técnica
de penalidade da estática) era falsa pista -- o nó de topo tem todos os GDL eliminados
(`prescribed_dofs: [-1]*6`), então atribuir `disp`/`rot` diretamente já era a técnica certa; o gap
real era a ausência da RAO/JONSWAP em si, não a técnica de imposição. Durante a implementação, um
bug real de interpretação do campo `Rao/offset` do XML (usar a posição real do nó, ~47m de braço
de alavanca, em vez do offset casco→CM real, poucos metros) produzia uma amplitude de heave de
30m -- corrigido pra usar `offset` direto (~10,3m). Usuário questionou se 10m ainda não era muito;
investigação independente (reprodução em Python) achou que a transferência geométrica estava
amplificando um pitch real da tabela RAO (~0,34 rad/m, incomumente alto) via o braço de ~13m --
fórmula conferida e correta, mas a discrepância entre a posição real do nó (X=-47,73m) e
`movement_center` (X=0) sugere que o referencial de `movement_center`/`offset` pode não ser o
mesmo da malha do riser, sem como confirmar contra nenhum consumidor XML real. Decisão do
usuário: desligar a transferência geométrica por enquanto (movimento aplicado no CM) -- caiu pra
~0,85m, ordem de grandeza mais conservadora; fórmula fica implementada e comentada, pronta pra
reativar quando o referencial for confirmado. Resultado: estática inalterada em todas as versões,
resíduos dinâmicos na mesma ordem de grandeza de antes em todas elas (nunca mais a explosão
numérica de 10²⁰ da primeira versão com bug), mas a dinâmica ainda não converge nos 20 passos
completos -- não era (sozinho) o fator limitante.

**Atualização 3** (ver `mapa_aml_exemplos_e_web_interface.md`, seção "Movimento de topo real",
rodada 3): a dúvida de referencial da Atualização 2 foi resolvida lendo o código-fonte real direto
-- `model_builder_dat.cpp`/`anf_movements.cpp`/`save-dat.cpp` confirmam que `cm_position` =
`movement_center + offset` (somados) e que o braço de alavanca vem de `(posição real do nó
pós-estática) - cm_position`, exatamente a abordagem da primeira implementação (revertida antes
por engano) -- o `.dat` não é um formato paralelo desconectado, é a ponte real entre a interface e
o solver, e `model_builder_dat.cpp` é o único lugar em `trunk/src` que constrói
`cEquivalentHarmonic`. Reativada a transferência geométrica com a fórmula correta. Resultado ao
rodar o Exemplo_01a real: heave volta a **30,17m** (praticamente igual à primeira versão -- confirma
que aquele valor era física real do ANFLEX, não um bug) e a dinâmica volta a divergir
catastroficamente (resíduos a 10²⁴, mesma ordem da primeira versão). A fórmula agora bate 100% com
o ANFLEX real, mas isso expõe -- sem mascarar -- que o solver dinâmico do risersim não aguenta um
movimento de topo dessa magnitude nos parâmetros atuais (rampa de 5s dentro de uma janela de
simulação de só 1s/20 passos, Newton sem line search). Ou seja: o "achado 2" (transferência
desligada) era um mascaramento, não uma correção -- a causa real do não-convergimento inclui esse
movimento de topo genuinamente grande, que só ficou visível agora que o dado está fiel.

**Atualização 4**: revisão pedida pelo usuário sobre ordem de translação/rotação achou e corrigiu
um problema real (não a causa raiz, mas uma correção de fidelidade válida): `dynamic_analysis.cpp`
compunha a rotação do nó de topo prescrito via `compose_rotations()` (produto de Rodrigues,
não-linear), enquanto o mecanismo real de movimento prescrito (`integrator.cpp::set_load_dofs:81-93`)
soma componente a componente (`presc_desl[i] += movements[i]`, sem composição não-linear) --
consistente com o método "harmônico equivalente" já ser linearizado do início ao fim. Corrigido
(`top_node->rot = static_rots.front() + vessel_rot`), suíte sem regressão. Resultado ao rerodar:
praticamente idêntico (heave 30,17m, divergência na mesma ordem de grandeza) -- **não era a causa**.
Achado mais importante dessa rodada: o resíduo já explode a partir do passo 2 (t=0,1s), quando a
rampa de 5s ainda deixa o movimento prescrito quase nulo (fator de rampa ~0,001) -- ou seja, a
causa provavelmente NÃO é a magnitude do movimento de topo (~30m), é algo que quebra o solver
mesmo com um deslocamento de topo praticamente zero. Isso muda o foco do próximo passo: não é mais
"por que um movimento de 30m diverge", é "por que o Newton dinâmico diverge quase imediatamente
mesmo com o topo essencialmente parado" -- um problema mais estrutural (massa/rigidez/Newmark-beta
em si), coerente com o histórico desta seção (a dinâmica nunca convergiu nos 20 passos completos em
nenhuma versão testada, mesmo antes de qualquer trabalho de movimento de topo).

**Atualização 5** (causa raiz encontrada e corrigida, parcialmente): isolei experimentalmente
(a pedido do usuário) que topo genuinamente PARADO (`vessel_motion.enabled=false` E
`wave.amplitude_m=0`) converge nos 20 passos completos -- confirmado até sob um build com
AddressSanitizer+UBSan (descartando bug de memória). Qualquer movimento variando no tempo, por
menor que seja, já quebra. Isso apontou pro laço de montagem da matriz de massa
(`dynamic_analysis.cpp:187-205`): o nó de topo tem `eq_numbers=[-1]*6` (GDL eliminado), então
qualquer termo de massa consistente do elemento que o toque -- inclusive o acoplamento inercial
`M_BA·a_topo` com o primeiro nó livre -- é descartado na montagem. Perguntei "e o ANFLEX faz de
que forma?" antes de corrigir: confirmado em `domain.cpp:546-578` que o ANFLEX real usa **método
de penalidade** ("big number") pro movimento prescrito, não eliminação de GDL -- o nó nunca sai do
sistema, então esse termo nunca desaparece lá. Corrigido reaproveitando `PrescribedMotion`
(`prescribed_motion.hpp`), já implementada e testada -- é o mesmo mecanismo que
`StaticAnalysis::solve_vessel_offset` já usa pro offset estático, só que agora também na dinâmica
(nó de topo ganha GDL reais + mola de penalidade, `compose_rotations()` correto pro estado do
Newton, alvo do achado 4 mantido). Suíte sem regressão (361 asserts). **Resultado real**: melhora
genuína, mas parcial -- resíduos ficam limitados (milhares) até o passo 12 (antes: explosão já no
passo 4), mas a partir do passo 13 volta a divergir (10²² no passo 20), ainda sem convergir os 20
passos completos. Ou seja, a causa identificada era real e a correção ajudou de fato, mas não é a
única coisa em jogo -- 30m de heave + 7,6m de surge numa janela de 1s/20 passos (contra um período
de onda de ~11s, nem um ciclo completo) continua sendo um movimento muito agressivo pro passo de
tempo usado.

**Atualização 6** (usuário desconfiou: "estou achando muito sensível essa estática, será que
temos alguma invasão de memória, algum lixo no caminho?"): tinha razão. ASan/UBSan (usados na
Atualização 5) não pegam esse tipo de bug por padrão -- rodei **Valgrind memcheck
--track-origins=yes** contra o caso determinístico de falha estática (`vessel_motion.enabled=
false`, que não deveria sequer tocar a estática) e achou de cara: `analysis.cpp:24-28` fazia
`node_tangent_sum[node] += ex` num `unordered_map<Node3D*, Eigen::Vector3d>` -- `operator[]`
default-constrói a entrada na primeira inserção, e o construtor padrão de `Eigen::Vector3d` **não
zera os coeficientes** (documentado no próprio Eigen, por performance), ao contrário de
`std::array`/`double`. Lixo de memória sendo somado a `ex` na primeira ocorrência de cada nó. Esse
vetor alimenta a decomposição de atrito do solo (axial/lateral), que entra na matriz de rigidez --
o rastro do Valgrind confirmou a contaminação chegando até dentro da fatoração de Cholesky. Só
importa pra nós com `k_seabed>0` (zona de toque do fundo, que o Exemplo_01a tem). **Corrigido**:
`try_emplace(node, Eigen::Vector3d::Zero())` antes de cada `+=`. Suíte sem regressão. **Resultado**:
o caso que sempre falhava deterministicamente agora **converge limpo** sob build Release normal,
estática E dinâmica completas nos 20 passos -- boa parte da sensibilidade "beira de bifurcação"
documentada nesta sessão inteira não era sensibilidade numérica genuína, era lixo de memória
mesmo. Combinando com a Atualização 5 (acoplamento inercial), o caso real de 30m de heave também
melhorou mais: passos 1-14 convergem todos limpos agora (antes: só até o 12), divergência começa
só no 15 -- ainda não fecha os 20 passos, mas o avanço acumulado dos dois bugs reais é substancial.

**Recomendação pro próximo passo**: com massa dinâmica, Rayleigh damping, movimento de topo,
composição de rotação, o acoplamento inercial do nó prescrito E memória não-inicializada já
corrigidos/confirmados, os candidatos de auditoria de dado/formulação/memória conhecidos estão
esgotados. Dois caminhos concretos: (a) isolar um caso mínimo com um movimento de topo mais modesto
(ou uma duração/passo de tempo mais realista, já que 1s/20 passos nem completa um ciclo de onda de
11s) e comparar diretamente contra `cDynamicAnalysis`/`cDynamicIntegrator` do ANFLEX real
(`trunk/src`), mesma metodologia que achou a rotação total-vs-local na estática; (b) investigar se
falta algo tipo line search/sub-passos no Newton dinâmico (o real ANFLEX também não tem line search,
confirmado em `newton_raphson.cpp` -- mas pode ter outra técnica de robustez, ex. redução automática
de `dt` quando o passo não converge, que o risersim hoje não tem).

**Atualização 7** (fechado -- os 20 passos completos convergem, pela primeira vez em toda essa
investigação): segui o caminho (a) acima -- plano completo aprovado pelo usuário, comparando o
tratamento de superfície livre do risersim contra o ANFLEX real (`trunk/src/nl_hidrostatic.{h,cpp}`,
`trunk/src/morison.cpp`) pra achar o gatilho exato do passo 15 (t=0,75s). Achados e correções, em
ordem:

- **Empuxo constante sem rigidez tangente** (gap real, confirmado por comparação de código): o
  risersim aplicava empuxo constante por elemento, independente de Z, em 4 lugares duplicados
  (`static_integrator.cpp`, `static_analysis.cpp` ×2, `dynamic_analysis.cpp`) -- sem nenhuma rigidez
  associada. Portei `cNL_Hidrostatics` quase 1:1 como `hydrostatics.hpp` (força + rigidez tangente
  por extremidade, 5 regimes de submersão) e pluguei em todos os 4 call-sites de força mais um novo
  `Analysis::assemble_buoyancy_stiffness()` (rigidez, somada em `K_global` nos loops de Newton
  estático e dinâmico, mesmo padrão do `C_global = alpha*M + beta*K`). Simplificação assumida (fase
  1 do plano, documentada no header): superfície plana (`water_surface_z` único, não por-extremidade
  variando com a onda) e clearance vertical de centro-de-linha em vez da distância perpendicular ao
  eixo inclinado que o ANFLEX real usa. Melhoria real e validada (zero regressão, 361 asserts), mas
  **não era a causa da divergência do passo 15** -- ver próximo item.
- **Erro meu de diagnóstico, corrigido antes de seguir**: assumi a convenção Z do risersim sintético
  (superfície em Z=0) pra interpretar a trajetória do nó 274. Depois de implementar e testar o fix de
  empuxo acima e ver resíduo bit-a-bit idêntico ao baseline, chequei os dados reais do modelo:
  `water_surface_z≈265`, `seabed_depth≈0` -- convenção oposta (a nativa do ANFLEX, que é a real pra
  qualquer modelo vindo de XML/H5). O nó 274 estava cruzando o **leito**, não a superfície. Reportei
  o erro ao usuário antes de continuar; o fix de empuxo foi mantido (correto e validado por conta
  própria), só reclassificado como não sendo a causa deste bug específico.
- **Causa raiz real** (isolada com instrumentação de debug temporária, depois removida): no passo 15,
  uma correção de Newton sem limite algum, calculada enquanto um nó ainda estava ~1,75cm FORA do
  contato com o leito (`k_seabed=0` ali -- sem resistência local nenhuma pro Newton), empurrou esse nó
  1,6m através do leito numa única iteração. A resposta de força interna elementar/estrutural
  resultante (não a mola do leito em si) explode o resíduo. É um problema de globalização/tamanho de
  passo do Newton, comum a qualquer modelo simples de contato unilateral -- o próprio ANFLEX real tem
  a mesma descontinuidade de rigidez de contato na mola linear dele (confirmado via
  `newton_raphson.cpp`: sem adaptação de `dt`/retry nenhuma) -- ou seja, **não é um gap de física
  faltando em relação ao ANFLEX**, é um problema numérico do solver.
- **Correção**: portei pro loop dinâmico o mesmo mecanismo já existente no estático
  (`StaticAnalysis::enable_step_limiting`/`apply_newton_step_with_line_search`) -- (1) limitador de
  passo (`max_translation_step_m=0.5`, `max_rotation_step_rad=0.3`) e (2) busca de linha por
  backtracking residual, até 5 cortes pela metade. A tolerância do backtracking teve que ser mais
  apertada que a do estático: `norm_trial <= res_norm*100.0` (a mesma do estático) testada e
  confirmada **ineficaz** aqui -- deixava passar exatamente o passo que explode; `norm_trial <=
  res_norm*2.0` testado e confirmado suficiente. Refatorei o loop de Newton dinâmico em torno de uma
  lambda `assemble_at(U_trial)` reutilizável (monta F_ext/K_global/F_int a partir de um vetor de
  estado absoluto, não incremental -- diferente do estático, não precisa snapshot/restore entre
  tentativas de backtracking).
- **Instrução explícita do usuário, implementada**: "não faz sentido progredir a simulação se temos
  um passo que falha" -- `stop_on_first_non_convergence` (`DynamicAnalysis`/`AnalysisOptionsConfig`)
  mudou o default de `false` pra `true`: um passo não-convergido pára o laço de tempo inteiro em vez
  de continuar rodando passos seguintes sobre um estado fisicamente sem sentido.

**Resultado final**: rodando o `Exemplo_01a` completo e real (XML+H5 reais, config real do ANFLEX --
20 iterações, 1s de duração, dt=0,05s, tolerância 1e-3, sem nenhum afrouxamento artificial), estática
E dinâmica convergem os 20 passos completos pela primeira vez em toda essa investigação. Catch2 361
asserts / 15 casos, zero regressão, verificado a cada incremento.

**Em aberto, não decidido ainda**: as fases 4 (força de Morison, já escrita em `hydrodynamics.hpp`
mas nunca instanciada) e 5 (massa adicionada escalando com comprimento molhado) do plano original
ficaram pendentes -- eram contingentes no checkpoint da fase 3, que seguiu por outro caminho (leito/
limitador de passo/backtracking) em vez do previsto. Como a divergência que motivou o plano já está
resolvida, vale decidir com o usuário se ainda compensa perseguir Morison/massa molhada agora (physics
gap real, mas não bloqueante) ou se o Eixo 1b deve ser considerado fechado por ora.

## Eixo 2 — Pipeline de dados (desbloqueia mais casos de teste reais, baixo risco)

### 2a. Religar `aml_reader.py` ao schema real do `ModelBuilder` — ✅ RESOLVIDO (refinamentos menores em aberto)

Bloqueio real fechado na Atualização 17 (dinâmica do Far converge os 70s/1400 passos reais
completos). Restam só refinamentos, não mais bloqueio: gap residual de ~5-7% em heave/roll do Far
(Atualizações 15/16, pausado -- resíduo numérico, não bug identificado), detector de chattering
período-N genérico (baixa prioridade agora que o retry `LSTEPITER` resolve o caso que motivava
isso), e correlacionar a onda do RAO de topo com a onda de Morison na linha (mudança maior, não
crítica).

Achado em [`mapa_aml_exemplos_e_web_interface.md`](mapa_aml_exemplos_e_web_interface.md): hoje, 23
dos 30 exemplos disponíveis (todos sem pasta `_analysis/` com XML/H5 pré-exportado) rodam
silenciosamente com o modelo sintético de fallback — `aml_reader.py` produz um schema que
`ModelBuilder` nunca entende. Consertar isso (gerar `model.nodes`/`model.elements` a partir dos
segmentos já extraídos, resolver corrente/solo por ID como `xml_h5_reader.py` já faz) é um trabalho
bem-escopado e de baixo risco (não toca o C++, só o parser Python), e destrava candidatos reais
novos ao bug solo+corrente — em especial `DNV_Check.aml` (solo e corrente confirmados casando por
ID, caminho estático).

**✅ Resolvido**: `to_risersim_json()` agora gera o schema real (nós/elementos/seção sintetizados a
partir dos segmentos), com corrente/solo/onda resolvidos por ID real (não mais "primeiro do
arquivo"). Um AML pode ter vários `%LOAD_CASE` (ex. Near/Far/Transverse/Cross do `Exemplo_01a`),
cada um com sua própria corrente+onda — `_resolve_load_case(load_case_id)` seleciona pelo ID real
(`--load-case-id` no `run_from_aml.py`), com transparência (nome/ID resolvido impresso). Como só
"Cross" tem XML+H5 real exportado neste exemplo, `--force-aml-path` força o caminho `.aml` puro
(malha sintética) mesmo quando existe uma pasta `_analysis/`, necessário pra rodar os outros load
cases (antes disso, `--load-case-id` era silenciosamente ignorado sempre que havia XML+H5 --
descoberto rodando Near/Far/Transverse pela primeira vez).

**Atualização 1** (movimento real de topo RAO+JONSWAP, ligado no caminho `.aml` puro): até aqui,
`vessel_motion` (o mecanismo real de topo já portado e validado pro caminho XML+H5,
`vessel_motion.hpp`) nunca era populado por `aml_reader.py` -- fallback sempre em onda regular só
em Z. Pesquisei o pré-processador real (`trunk/interfaces/src`, não `trunk/src`/`trunk/libs` --
esses só consomem o `.dat`/XML já traduzido, o parser de texto `%KEYWORD` só existe no
pré-processador) pra portar: posição global do ponto de conexão (`%CONNECTION.LOCAL_COORDINATES` +
`%FLOATING.SHIP`, rotação `90-azimute` + translação pela origem do FPSO --
`connection.cpp:189-281`), offset estático real (`%FLOATING.LOADS.STATIC`'s `%TIME_SERIES_LOAD`,
avaliado num parser genérico de tabela de função no tempo real da análise estática), e a tabela RAO
**embutida** no próprio `.aml` (não é arquivo externo -- `%RAO 'FPSO.RAO'` é só um rótulo, a tabela
inteira vem inline, formato confirmado contra `interfaces/src/rao.cpp`). Validação forte: a fórmula
da posição da conexão, calculada à mão antes de escrever código, bateu **exatamente** com o
`[TOP] X=-47.73 m, Z=257 m` já observado no caso real "Cross" (XML+H5).

**Atualização 2** (malha inicial via catenária real, usando MoorPy): o `.aml` não tem coordenadas de
nó, mas tem os parâmetros de um problema de contorno de catenária real
(`%LINE.CATENARY.ANGLE`+`%LINE.AZIMUTH`+comprimento+profundidade) -- o pré-processador real resolve
isso com uma biblioteca externa fechada (`tec_line`, não está neste repo). `moorpy.Catenary.catenary(
XF,ZF,L,EA,W)` (já usado e validado em `spikes/mooring_validation/`, ~1% de erro vs. ANFLEX real) só
resolve "dado o vão, qual a tração" -- não tem modo "dado o ângulo, ache o vão". Implementei
`_solve_catenary_geometry()`: usa `catenary()` como solver interno de uma busca (bisseção) no vão
horizontal até bater o ângulo real no topo. **Validação forte**: a âncora prevista bateu com a âncora
real exportada (XML+H5, caso "Cross") a nível de **centímetro** num vão de ~296m
(`(-271.44,-248.97,0.00)` previsto vs. `(-271.40,-248.94,~0)` real).

**Bug real achado e corrigido durante a validação**: a fórmula de `movement_center` (Atualização 1)
subtraía a posição do nó de topo direto; o certo é subtrair o **gap** entre a posição real da conexão
e onde o nó 1 efetivamente está (`connection_global - top_node_position`), não `top_node_position`
isolado -- sem isso, ao mudar o nó de topo pra posição real (Atualização 2), o braço de alavanca
dobrava em vez de cancelar. Pego checando os amplitudes impressos (100-762m de heave, fisicamente
absurdo), não só "convergiu com sucesso" -- convergência sozinha não bastava pra pegar esse bug.

**Atualização 3** (causa raiz do "Far mostra amplitude muito maior" da lista acima, achada e
corrigida): não era ressonância real nem bug de busca de frequência -- era falta de conversão de
unidade. O ANFLEX real (`model_builder_dat.cpp:242-250`) converte a **amplitude** (não só a fase)
dos GDL rotacionais da RAO (roll/pitch/yaw) de graus/m pra radiano/m antes de usar; `vessel_motion.cpp`
convertia a fase mas nunca a amplitude. Como os momentos espectrais (`m0` etc.) envolvem
amplitude², o erro (~57x, `180/π`) compõe quadraticamente -- e é pior justamente quando o pico do
espectro de onda coincide com um pico de RAO rotacional, o que acontece quase exatamente para "Far"
(ambos perto de ω≈0,40 rad/s), explicando por que só esse caso mostrava números absurdos. Confirmado
comparando contra o `.SAI` real gerado rodando o Fortran legado via WSL (`anf_i`/`anf_s`/`anf_d`,
binários Linux em `anf_analysis/fortran/bin/`) para o caso Far do `Exemplo_01c`: a tabela real
"RESPONSE AMPLITUDE OPERATOR (TRANSFERRED)" do `.SAI` está explicitamente em unidades **"(M/M,
RAD/M)"**, com valores pequenos (~0,049 rad/m) muito diferentes do valor bruto do arquivo `.RAO`
(graus/m, ~6,77). Corrigido em `interpolate_heading()` (`vessel_motion.cpp`): amplitude dos GDL
rotacionais (`dof>=3`) agora escalada por `π/180` antes de montar a curva complexa usada na
transferência de ponto e nos momentos espectrais. Um teste (`test_vessel_motion.cpp`, o caso do
braço de alavanca pitch→heave) precisou de ajuste porque seu RAO sintético assumia amplitude=1.0
como "1 rad" direto -- reescalado por `180/π` no próprio teste pra preservar a mesma intenção.
Suíte: 361/361 sem regressão.

**Achado de processo, sem relação com o bug acima** (pego só ao rodar o pipeline real de ponta a
ponta, não pela suíte Catch2): depois do fix e da suíte passando, rodar `run_from_aml.py` no caso
Far pela linha de comando continuava mostrando o heave de ~1400m antigo. Causa: `risersim_test_main.exe`
(o binário que `run_from_aml.py` de fato invoca) não tinha sido recompilado depois do fix em
`vessel_motion.cpp` -- só `risersim_tests.exe` (a suíte) tinha sido reconstruído nesta sessão. Ou
seja, a suíte verde mascarava um binário de produção desatualizado. Recompilado via `MSBuild` da
instalação "Visual Studio 18" (não a "2022" -- o projeto usa toolset `v145`, que só a instalação 18
tem). Depois do rebuild, Far real: heave 1381m → **26,9m**, roll 19,1 rad (absurdo, >2π) → **0,33
rad**, surge → **3,49m** -- ordem de grandeza correta, ~51x de melhora, batendo com o que o harness
Python standalone já tinha indicado antes do fix chegar no binário real. **Lição**: ao validar um
fix de C++ pelo pipeline real (não só pela suíte de testes), confirmar a data de modificação do
binário específico invocado, não assumir que "a suíte passou" implica "o binário usado pelo restante
do sistema já reflete o fix".

**Atualização 4** (causa raiz do gap residual de ~3x e da não-convergência dinâmica do Far, ambos
achados e corrigidos -- investigação por comparação direta, ponto a ponto, contra as tabelas reais
"RESPONSE AMPLITUDE OPERATOR (TRANSFERRED)" e "LOCAL DISTANCE (USED FOR TRANSFERENCE)" de um `.SAI`
real, geradas rodando o Fortran legado via WSL tanto pro Far quanto pro Cross do `Exemplo_01c`):
dois bugs genuínos e independentes na transferência geométrica CM→ponto de fixação de
`interpolate_heading()`/construtor de `VesselMotion`, nenhum dos dois relacionado ao bug de
graus→radianos da Atualização 3.

1. **`offset` não deveria entrar nesta conta.** O código somava `config.offset_m` a
   `cm_position_m` antes de calcular o braço de alavanca, por analogia direta com
   `model_builder_dat.cpp:4373-4386` (que de fato faz `cm_position[i] += cm_offset[i]`) -- mas essa
   leitura ignorava que, no MESMO trecho real (`model_builder_dat.cpp:4335-4359`), o
   `node_position` do OUTRO lado da subtração recebe o EXATO MESMO offset somado
   (`node_position[i] += offset_coord[i] + offset_coord_assembly[i]`) antes de entrar na mesma
   conta -- no real, o offset é somado nos dois lados do delta e cancela. No pipeline do risersim,
   `attachment_point_global` (a posição real pós-estática do nó de topo) não carrega esse offset da
   mesma forma (é idêntica em todos os load cases), então somar offset só do lado do CM introduzia
   um desbalanceamento que o real nunca tem.
2. **A rotação usava o ângulo com o sinal trocado.** A fórmula em si já batia com
   `cMatrixTransform::inv_transform` (`matrix_transform.cpp:248-259`, aplica R(-theta), a
   transposta de `set_z_matrix`) -- o problema era o `theta` usado: `refsys_angle_deg` (105° pro
   Exemplo_01a) é o valor já validado contra o campo `refsys_angle` exportado no XML real, mas o
   `m_ref_sys_angle`/`floating_angle` que `cAnfMovements`/`cHybridMovement` de fato passam pra
   `inv_transform` internamente (`anf_movements.cpp:66-84`, `hybrid_movement.cpp:59`) tem o sinal
   OPOSTO a esse valor exportado. Como `cEquivalentHarmonic` repassa esse mesmo `floating_angle`
   sem modificar tanto pra escolher a direção da RAO quanto pra girar o braço de alavanca
   (`equivalent_harmonic.cpp:45-54`), a correção precisou negar o ângulo nos dois lugares de uma
   vez (não só na rotação).

Os dois bugs juntos foram achados comparando `x_local`/`y_local` calculados (antes: variavam por
load case, sem bater com nada reconhecível) contra a "LOCAL DISTANCE" real, que é **idêntica nos
dois load cases** (Far e Cross) e bate byte-a-byte com as `%CONNECTION.LOCAL_COORDINATES` cruas do
próprio `.aml`: `(65.0000, -32.0000, -8.0000)`. Corrigido: `cm_global` não soma mais `offset_m`;
`refsys_angle_rad_` agora guarda o `floating_angle` já negado (`-config.refsys_angle_deg`), usado
consistentemente nos três pontos do arquivo que giram entre o referencial local da RAO e o global
(seleção de heading, braço de alavanca no construtor, e a rotação final local→global em
`get_motion()`). Suíte: 361/361 sem regressão (o teste do braço de alavanca usa `refsys_angle_deg
=0`, insensível ao sinal).

**Resultado, comparando contra as tabelas reais "JOINT MOVEMENTS" (STD final por GDL) do `.SAI`**:

| GDL (unidade) | Cross real | Cross calculado (razão) | Far real | Far calculado (razão) |
|---|---|---|---|---|
| surge (m) | 0,3139 | 0,3149 (1,00x) | 2,567 | 2,57 (1,00x) |
| sway (m) | 0,19502 | 0,1879 (0,96x) | 1,8743 | 1,867 (1,00x) |
| heave (m) | 0,97067 | 0,9458 (0,97x) | 9,2905 | 7,96 (0,86x) |
| roll (°) | 0,37943 | 0,3584 (0,94x) | 9,7258 | 7,821 (0,80x) |
| pitch (°) | 1,0613 | 1,0307 (0,97x) | 3,2506 | 3,210 (0,99x) |
| yaw (°) | 0,1270 | 0,1233 (0,97x) | 1,1368 | 1,111 (0,98x) |

Cross agora bate em todos os 6 GDL dentro de ~3-6%; Far bate muito melhor que antes em surge/sway/
pitch/yaw (~1-2%), com heave/roll ainda ~15-20% subestimados (razão não uniforme entre GDL, gap bem
menor que o ~3x/~2x anterior, causa não identificada). **Efeito colateral direto**: a dinâmica do
Far, que antes parava de convergir em t≈9,65s mesmo com a amplitude já saneada pela Atualização 3,
agora **converge os 60s/1200 passos completos** sem nenhuma mudança no solver -- confirma que o
gap residual (heave ~27m, ~3x acima do real) era agressivo o bastante pra ainda quebrar o Newton
dinâmico. **Transverse (load case 135), que divergia na dinâmica (passo 4/t=0,2s) segundo o achado
anterior desta seção, também converge limpo agora** -- mesmo efeito colateral, sem investigação
separada necessária.

**Achado de processo, sem relação com os bugs acima** (pego só ao rodar o pipeline real de ponta a
ponta, não pela suíte Catch2): depois do fix da Atualização 3 e da suíte passando, rodar
`run_from_aml.py` no caso Far pela linha de comando continuava mostrando o heave de ~1400m antigo.
Causa: `risersim_test_main.exe` (o binário que `run_from_aml.py` de fato invoca) não tinha sido
recompilado depois do fix em `vessel_motion.cpp` -- só `risersim_tests.exe` (a suíte) tinha sido
reconstruído nesta sessão. Ou seja, a suíte verde mascarava um binário de produção desatualizado.
Recompilado via `MSBuild` da instalação "Visual Studio 18" (não a "2022" -- o projeto usa toolset
`v145`, que só a instalação 18 tem). Depois do rebuild, Far real: heave 1381m → **26,9m**, roll
19,1 rad (absurdo, >2π) → **0,33 rad**, surge → **3,49m** -- ordem de grandeza correta, ~51x de
melhora, batendo com o que o harness Python standalone já tinha indicado antes do fix chegar no
binário real. **Lição**: ao validar um fix de C++ pelo pipeline real (não só pela suíte de testes),
confirmar a data de modificação do binário específico invocado, não assumir que "a suíte passou"
implica "o binário usado pelo restante do sistema já reflete o fix".

**Em aberto, não resolvido ainda**:
- **Gap residual de ~15-20% no heave/roll do Far** (ver tabela acima) -- razão não uniforme entre
  GDL (surge/sway/pitch/yaw batem em ~1-2%), então não parece ser um terceiro bug de escala; pode
  ser o mesmo ~1% de diferença de grid já documentado (`dw=(wf-wi)/n` vs `/(n-1)`) composto de
  forma não-linear onde a RAO tem um pico mais agudo (roll do Far, ver
  `mapa_aml_exemplos_e_web_interface.md`), ou outra coisa ainda não identificada. Testado
  convergência de `nwave` (100→20000, já usando o código corrigido desta rodada): resultado já
  está convergido em ~500, então NÃO é falta de resolução do grid JONSWAP -- descarta essa
  hipótese especificamente.
  **Tentativa feita e revertida**: reli `movext.f:159-198` (a fórmula real do extremo de
  Rayleigh) esperando achar um terceiro bug de escala -- `STDPM=sqrt(AREAMOV/2)` (não
  `sqrt(AREAMOV)` puro) e `FMAX=sqrt(2*ln(TINTV*XFZ))` com `XFZ` baseado em m0/m2 (não m2/m4).
  Reescrevi `vessel_motion.cpp` pra bater literalmente com isso -- resultado **piorou**: as
  razões (antes 0,76-1,25 no Far, 0,94-1,00 no Cross) viraram uniformemente ~0,68-0,71 nos dois
  casos (~1/√2, todos os 6 GDL igualmente subestimados). Como a versão anterior já estava muito
  mais perto do real (principalmente no Cross), revertido (`git checkout`). A uniformidade exata
  do erro introduzido (1/√2 em todos os GDL) sugere que a leitura de `movext.f` tem um fator de 2
  errado em algum lugar -- provavelmente `AREAMOV`/`XM0` (o "m0" do Fortran, de `movgharfloa.f`,
  ainda não lido nesta rodada) não é a mesma grandeza que o `m0` deste código calcula por
  trapézio, e por isso o "/2" de `STDPM=sqrt(AREAMOV/2)` não se aplica do mesmo jeito aqui. Não
  investigado mais a fundo -- próxima tentativa, se valer a pena, deveria ler `movgharfloa.f`
  primeiro pra confirmar a definição exata de `AREAMOV` antes de mexer na fórmula de novo.

**Mudança de metodologia (achado nesta rodada, antes de continuar o gap acima)**: `trunk/anf_analysis`
contém o C++ REAL do ANFLEX (histórico SVN próprio, `$Revision$`/`$LastChangedBy$` nos headers,
last-changed 2015-2016) -- não é um wrapper fino sobre o Fortran, é uma reimplementação C++ completa
e ativamente mantida, em duas árvores: `anf_analysis/src` (motor: `domain.cpp`, `dynamic_analysis.cpp`,
`static_integrator.cpp`, `dynamic_integrator.cpp`, etc., praticamente os mesmos nomes de arquivo já
citados via `trunk/src` nesta investigação inteira) e `anf_analysis/anf_movements/src` (biblioteca de
movimento de topo/RAO/onda: `hybrid_movement.cpp`, `equivalent_harmonic.cpp`, `jonswap_spectrum.cpp`,
`rao.cpp`, `matrix_transform.cpp`). `trunk/src` (usado até aqui) é uma cópia mais antiga (datada de
2016-10-03 vs. 2016-11-10 do `anf_analysis/src`) com os MESMOS nomes de arquivo mas conteúdo diferente
-- funcionalmente equivalente pro que já foi lido até agora, mas `anf_analysis` é a árvore mais
completa e mais nova. **Decisão do usuário**: priorizar o C++ de `anf_analysis` daqui pra frente;
Fortran (`anf_analysis/fortran/*.f`) só quando o C++ não tiver a rotina ou o caso for complexo demais
pra entender só pelo C++.

**Atualização 8** (aplicando a mudança acima ao gap do Far -- achado inicial promissor, mas
descartado depois de implementado e testado, ver abaixo):
lendo `anf_analysis/anf_movements/src/hybrid_movement.cpp:100-136` (o construtor de `cHybridMovement`,
onde os momentos espectrais e o extremo de Rayleigh são calculados de fato) em vez do Fortran: a
fórmula em si (`m_amp_std=sqrt(m0)`, `m_amp_s=2*m_amp_std`, `Tm=2π·sqrt(m2/m4)`,
`N=t/Tm`, `rayleigh_factor=sqrt(0.5·ln(N))`, `amp_max=amp_s·rayleigh_factor`) bate **exatamente** com
o que `vessel_motion.cpp:240-256` já calcula hoje -- confirma, com a fonte real (não mais inferência
via Fortran), que a versão atual (pós-reversão da Atualização 4) estava certa e a tentativa revertida
(acima) é que introduzia o erro 1/√2. Mas o construtor real tem uma diferença estrutural que
`vessel_motion.cpp` NÃO replica: `hybrid_movement.cpp:69-81` restringe o laço de componentes de onda
usado no cálculo dos momentos (m0/m2/m4) ao intervalo `[rao_min, rao_max]` -- a faixa de frequência
REALMENTE tabelada na RAO (`m_rao_table->get_min_fre()/get_max_fre()`), descartando qualquer
componente de onda fora dela. `vessel_motion.cpp:215-238` integra sobre a faixa CONFIGURADA inteira
(`%WAVE.FIRST_FREQUENCY`/`LAST_FREQUENCY`, 0,2-3,0 rad/s no Exemplo_01a) e usa `interp_linear()`
(`vessel_motion.cpp:118-130`), que EXTRAPOLA como constante (`y.front()`/`y.back()`) fora da faixa da
RAO, em vez de excluir esses pontos. Conferido contra o `.aml` real: a tabela RAO do Exemplo_01a vai
só até **1,20 rad/s** (`exemplos/Curso/Exemplo_01/Exemplo_01a/Exemplo_01a.aml:756-760`), bem abaixo do
teto de integração configurado (3,0 rad/s) -- ou seja, hoje incluímos ~60% do domínio de integração
(1,2 a 3,0 rad/s) com um valor de RAO extrapolado (constante) que o código real simplesmente não usa
ali. Como `m4` pesa por `ω⁴`, essa cauda espúria pesa desproporcionalmente mais nela do que em `m0`,
e como o efeito depende de quão achatada/alta é a última amostra tabelada de cada GDL (heave/roll do
Far, notados como tendo um pico mais agudo -- ver achado anterior nesta seção), é consistente com o
gap ser não-uniforme entre GDL e maior no Far.

**Implementado e testado -- hipótese DESCARTADA, gap continua aberto**: clipei o laço de
`vessel_motion.cpp` à faixa `[frequencies_rad_s.front(), frequencies_rad_s.back()]` da própria RAO
(`mov_ini`/`mov_fin`, mesmo algoritmo de `hybrid_movement.cpp:69-81`, inclusive o mesmo off-by-one de
fronteira quando um ponto da grade de onda cai EXATAMENTE no limite da RAO -- exigiu ajustar a
referência independente de `test_vessel_motion.cpp` pra bater; suíte 361/361 sem regressão).
Recompilei `risersim_test_main.exe` (não só a suíte) e rodei o Far real com um debug print temporário
(`RISERSIM_VESSEL_MOTION_DEBUG`, removido depois) pra imprimir os 6 GDL antes/depois do fix.
**Resultado: byte-idêntico ao valor pré-fix** (surge=2,57 sway=1,867 heave=7,96 roll=7,82° pitch=3,21°
yaw=1,111° -- mesmos números da tabela da Atualização 4). Causa: minha leitura anterior do `.aml` só
cobriu as primeiras ~48 das 59 linhas de frequência da tabela RAO (`Exemplo_01a.aml:712`, cabeçalho
"25 59") -- a tabela real do heading 0° vai até **10,0 rad/s**, bem além do teto de integração
JONSWAP configurado (3,0 rad/s), confirmado lendo `frequencies_rad_s` de volta do
`input_simulation.json` gerado (min 0,0628, max **10,0**, não 1,2 como eu tinha lido antes). Ou seja,
a RAO tabelada é mais LARGA que o domínio de integração, não mais estreita -- o clip nunca exclui
nada na prática pra este caso real, e por isso o fix não muda o resultado. **Mantido mesmo assim**
(código correto, fiel ao algoritmo real, zero regressão, zero mudança de comportamento nos casos já
validados) -- só não é a causa do gap. Gap do heave/roll do Far **continua sem causa identificada**;
próxima hipótese, se valer a pena perseguir, precisa ser outra (a suspeita de grid JONSWAP já foi
descartada antes; frequência de corte da RAO agora também descartada com evidência direta).

**Atualização 5** (causa raiz do `T_eff` estático do "Cross" via `.aml` puro, ~994 kN vs. 217 kN
real, achada e corrigida -- ✅ **RESOLVIDO**): não era o formato do chute inicial por si só (testado
diretamente: rodar com o `warm_start` já existente, aplicado como `disp` por cima da malha reta
-- `model_builder.cpp:200-221` -- deu o MESMO `T_eff`, 994,2 kN, provando que a forma inicial não
era a causa). A causa real: `model_builder.cpp:149` deriva `L_unstretched` (comprimento natural/
não-esticado de cada elemento, usado no cálculo de deformação axial) da distância EUCLIDIANA entre
as coordenadas de entrada dos nós -- correto quando a malha de entrada já é a geometria real
instalada (caminho XML+H5), mas `_synthesize_mesh()` (caminho `.aml` puro) coloca os nós numa
RETA entre o topo e a âncora reais, e uma corda reta é sempre mais curta que o cabo real (que tem
sag) -- medido: corda de 392,3m pra um cabo real de 500m (~22% mais curto). Cada elemento achava
que seu comprimento natural era ~0,785m em vez de ~1,0m -- esticar o espaçamento real disso gerava
uma deformação axial artificial enorme e, com EA=360 MN, uma tração muitas vezes maior que a real.
`moorpy_warm_start.py`/`build_from_risersim_json.py` (o spike MoorPy já validado) tinham o MESMO
bug, por sinal: também inferiam o comprimento da linha somando distâncias nó-a-nó da malha reta de
entrada (documentado no próprio spike como assumindo "geometria já instalada", nunca validado pro
caminho `.aml`) -- por isso corrigir só a malha inicial (sem consertar o spike) não bastava. Duas
correções: (1) `aml_reader.py::to_risersim_json()` agora inclui um campo `model.total_length_m`
real (soma dos segmentos declarados, não re-derivável de coordenadas de uma corda reta); (2)
`build_from_risersim_json.py` usa esse campo quando presente, em vez de sempre somar distâncias
nó-a-nó; (3) `run_from_aml.py` agora aplica a correção MoorPy automaticamente no caminho `.aml`
puro (antes: só disponível como script manual separado) -- e, diferente do `warm_start` (que só
sobrepõe `disp`), sobrescreve as PRÓPRIAS coordenadas dos nós antes de `model_builder.cpp` calcular
`L_unstretched`, corrigindo o comprimento na raiz. **Resultado**: malha corrigida soma ~499,9m
(era 392,3m); `T_eff` do Cross = **217,5 kN** (real: 217,3 kN, ~0,1% de erro) -- Near/Far/
Transverse também convergem pra `T_eff`~218-220 kN cada (antes: ~950-1050 kN, bem mais dispersos
entre si, outro sinal de que eram artefato de malha, não física real). Zero mudança em C++.

**Achado novo, em aberto**: com o `T_eff` estático agora correto (bem mais baixo, ~218 kN em vez de
~1000 kN), a dinâmica do Near e do Far pararam de convergir (Far: não converge no passo 79/t=3,95s,
`res_norm=430`; antes desta correção, ambos convergiam completos) -- só o Transverse continua
convergindo limpo. Não investigado ainda -- pode ser sensibilidade do Newton dinâmico a uma
mudança real e substancial na configuração de equilíbrio (mesma família de problema do Eixo 1b,
robustez do solver dinâmico), não necessariamente um bug novo introduzido por esta correção.

**Verificação de regressão feita nesta rodada** (todos os 4 load cases do `Exemplo_01a`, binário
`risersim_test_main.exe` recompilado a cada mudança): Near/Far/Transverse (`--force-aml-path`,
estática) convergem, com posição global da conexão consistente `(-47,73, -54,50, 257,00)` m e
`refsys=105,0°` nos três (offsets com sinal correto por caso). Cross (caminho real XML+H5, sem
`--force-aml-path`, que na verdade está em `Exemplo_01a_analysis/Exemplo_01a_A1.{xml,h5}`, não em
`Exemplo_01c_analysis` como uma tentativa inicial supôs por engano) segue sem regressão: T_eff
estático convergido = **217,3 kN**, batendo o valor real já documentado. Dinâmica: Near/Far/
Transverse/Cross convergem os passos completos (Far: 60s/1200 passos; antes da Atualização 4, Far
parava em t≈9,65s e Transverse divergia no passo 4). Catch2: 361/361.

**Atualização 6** (investigação da não-convergência dinâmica do Near/Far achada na Atualização 5 --
dois bugs reais de robustez do Newton dinâmico achados e corrigidos, um terceiro achado documentado
e deixado em aberto por ser física, não bug de solver): reproduzi o caso Far (`--duration 30
--dt 0.05`, 600 passos) e instrumentei o loop (`RISERSIM_DYN_DEBUG`, removido antes do commit final)
pra imprimir, por iteração de Newton, o DOF de maior resíduo e a posição/atrito do nó dono dele --
mesma técnica já usada pro solver estático (`RISERSIM_DEBUG_RESIDUAL`, `static_analysis.cpp:401`).

1. **Passo 79 (t=3,95s): "chattering" de contato, não divergência.** O resíduo ficava preso num
   ciclo de período 2 EXATO (430,229 ↔ 179,177, repetindo indefinidamente até estourar o
   orçamento de 20 iterações) -- o Z do nó 300 cruzava o leito marinho por frações de milímetro a
   cada iteração (contato liga/desliga), exatamente o mesmo padrão "chattering" já documentado pro
   solver ESTÁTICO (`seabed.hpp:106`, `static_analysis.hpp:33-48`) mas nunca replicado no
   dinâmico. Causa raiz específica do dinâmico: o *line search* de backtracking (`docs/roadmap.md`
   item 1b, commit `c6a517d`) reutiliza a matriz de amortecimento `C_global` do TOPO da iteração
   pra avaliar cada tentativa (`Reuses this iteration's M_global/C_global for every trial`,
   documentado como "an accepted approximation") -- mas exatamente no instante em que o contato
   liga/desliga, `K_global` (e portanto `C_global = alpha*M + beta*K`) muda bastante, então o
   resíduo que o backtracking mede pra decidir aceitar/rejeitar o passo já está desatualizado em
   relação ao resíduo real que a próxima iteração vai encontrar -- deixando passar exatamente a
   correção que cruza o contato inteiro de uma vez. **Corrigido**: recalcula `C_trial` a partir do
   `K_global` fresco de cada tentativa (`trial_state.K_global`), em vez de reusar o `C_global`
   antigo. Isso sozinho já reduziu bastante o resíduo (472→184→76→49→35→32→29 em 7 iterações) mas
   não bastou sozinho pra convergir de vez -- a posição do nó, porém, já tinha convergido pra um
   ponto fixo fisicamente desprezível (250µm → 113µm → 43µm → 8,5µm → 4,2µm → 0,35µm de erro), só o
   critério de resíduo relativo (o ÚNICO critério de parada que o solver dinâmico tinha) continuava
   oscilando. Por isso um segundo fix: um critério de convergência por INCREMENTO (não força),
   idêntico em espírito ao critério translação/rotação que `ConvergenceTest` já usa no estático mas
   que o dinâmico nunca teve -- se a correção aceita numa iteração é menor que 0,1mm/0,0001rad em
   todo DOF, aceita como convergido mesmo que o resíduo de força não tenha zerado (a posição já não
   está mais se movendo, então continuar iterando é só ruído numérico). Com os dois fixes, o passo
   79 converge limpo.
2. **Passo 87 (t=4,35s): mesmo padrão de "chattering", amplitude maior (~1mm), sem decair.** Nó 297
   preso num ciclo de período 2 estável só que desta vez SEM convergir gradualmente (os dois
   estados se repetiam quase exatamente a partir da iteração 8, sem encolher) -- o fix de
   incremento acima não pega esse caso porque a oscilação não é pequena o bastante. Reconhecido
   como o caso clássico de ponto fixo de Newton oscilando entre dois estados que "cercam" a raiz
   verdadeira (o mesmo problema que motiva bisseção/regula-falsi em vez de Newton puro quando a
   raiz está entre dois pontos que o método visita repetidamente). **Corrigido**: detector de ciclo
   de período 2 -- guarda o resíduo/estado de 2 iterações atrás; se o resíduo atual bate (±0,01%)
   com o de 2 iterações atrás depois de pelo menos 5 iterações (evita disparo em coincidências
   iniciais), aceita a MÉDIA dos dois estados `U` que alternam como a solução -- na prática, uma
   bisseção do ciclo. Resultado: "período-2 chattering detectado no passo 87 iter 8 -- aceitando o
   estado com bisseção" e a simulação segue adiante.
3. **Passo 109 (t=5,45s): tração efetiva NEGATIVA em vários elementos (`T_eff` até -877 kN),
   achado real e diferente dos dois acima, NÃO CORRIGIDO** -- o resíduo cresce de forma irregular
   (não é mais um ciclo de período 2 limpo; os valores mudam a cada iteração: 14501→20172→16923→
   5506→9388→...→11278) com o pior DOF alternando de nó mas sempre em elementos com tração quase
   zero ou negativa (linha "afrouxando"/"slack" sob a excursão grande de heave/surge do topo real,
   já que o `T_eff` deste caso ficou bem mais baixo depois da Atualização 5, ~218kN vs. ~1000kN
   antes -- linha mais frouxa = mais fácil de ficar com tração negativa sob movimento dinâmico
   grande). Fisicamente, tração efetiva perto de zero ou negativa faz o termo de rigidez
   geométrica (rigidez "de corda", proporcional à tração) da barra ficar perto de zero ou negativo,
   e como a rigidez à flexão deste cabo é muito pequena (`EI=21,7 kN.m²`), o elemento fica quase
   sem resistência transversal -- um problema conhecido de dinâmica de linhas de ancoragem frouxas
   ("slack line snap"), não um artefato numérico do solver como os dois casos acima. NÃO é o mesmo
   tipo de bug -- resolver de verdade provavelmente exige uma técnica dedicada (piso mínimo de
   tração, amortecimento artificial perto de T≈0, ou uma formulação de cabo que trate afrouxamento
   explicitamente), não uma correção pontual do Newton. Deixado em aberto.

**Resultado**: Near passa a convergir os 600 passos completos (antes: não investigado, falhava);
Transverse e Cross seguem convergindo limpo (sem regressão); Far avança do passo 79 pro passo 109
(~40% a mais de simulação) antes de esbarrar no problema de tração negativa (item 3 acima, não
relacionado aos bugs de chattering corrigidos). Catch2: 361/361, sem regressão. Instrumentação de
debug (`RISERSIM_DYN_DEBUG`) foi removida antes do commit -- só ficaram os três fixes em si.

**Atualização 7** (item 3 acima, tentativa feita e revertida): antes de tentar qualquer fix,
pesquisei o Fortran real (`anf_analysis/fortran/ANFLEX_COMUNS/bpngkn.f`, `bpngknl.f`, `bpmatg.f` --
a rotina real de rigidez geométrica de viga) pra confirmar se o ANFLEX real trata compressão/tração
negativa de algum jeito especial antes de portar qualquer coisa às cegas. **Achado**: não trata --
`bpngkn.f` usa a força axial (`P1`) igual ao risersim, sem piso, sem `IF (P1.LT.0)`, sem trocar de
formulação -- ou seja, esta instabilidade não é uma lacuna do risersim vs. o real, é uma
característica da própria formulação de viga que o ANFLEX real também tem (só o elemento "cabo"
puro, sem rigidez a flexão, tem uma checagem -- e essa checagem é um `STOP` fatal quando a rigidez
axial some, `arigm3.f:223-228`, não uma correção graciosa). O único mecanismo genérico de ajuda ao
Newton que o real tem é uma "rigidez artificial" (`bpflex.f:852-909`): decai exponencialmente com o
número da iteração dentro de cada passo, não é disparada por tração negativa especificamente -- e o
risersim já porta uma versão disso pro solver ESTÁTICO (`StaticIntegrator`,
`static_integrator.cpp:107-142`, mesma forma: `k = avg(EA/L) * exp(-iter/1.25)`), mas o DINÂMICO
nunca teve. Portei a mesma técnica pro loop dinâmico (rigidez artificial adicionada em
`assemble_at()`, decaindo por iteração) e testei no Far -- **piorou**: passou a falhar no passo 104
em vez do 109 (antes desta tentativa). Revertido (`git checkout`). Diferente da tentativa
documentada na Atualização 4 (que tinha um motivo claro pro erro, 1/√2 uniforme), aqui não ficou
óbvio por que piorou -- possivelmente a rigidez artificial, adicionada em TODA iteração de TODO
passo (não só nos que precisam), muda a trajetória de convergência dos passos anteriores ao 109 o
suficiente pra deixar o passo 109 numa condição inicial pior, já que cada passo dinâmico herda o
estado exato do anterior (diferente do estático, onde artificial stiffness só é usada na fase de
"assembly" isolada, nunca na fase "real" limpa -- `ArtificialStiffnessMode::Never`). Não investigado
mais a fundo. Próxima tentativa, se valer a pena, deveria escopar a rigidez artificial SÓ aos passos/
elementos com tração já negativa (não global/sempre-ligada como o port direto fez), ou considerar
alguma das outras técnicas já listadas no item 3 (piso mínimo de tração, formulação de cabo com
afrouxamento explícito).

**Atualização 8** (item 3, segunda tentativa -- implementada, testada, também sem efeito):
seguindo a mudança de metodologia desta sessão (priorizar `anf_analysis/src` C++ real em vez de
Fortran, ver Eixo 2a), li `dynamic_integrator.cpp:516-517`/`static_integrator.cpp:206-207` do
ANFLEX real em vez do `bpflex.f` já lido antes -- achado concreto: o real só aplica rigidez
artificial no **PASSO 1** (`if(m_have_artificial_stiffness && step == 1) apply_artificial_stiffness();`,
mesmo gate em estático e dinâmico), nunca nos passos seguintes -- bem mais restrito que a tentativa
da Atualização 7 (toda iteração de todo passo). Extraí a fórmula de decaimento já portada em
`StaticIntegrator` pra uma função livre (`add_artificial_stiffness()`, `static_integrator.hpp/cpp`)
e portei pro `assemble_at()` do dinâmico, gated exatamente por `step == 1` -- suíte 361/361 sem
regressão. **Resultado real (Far, `--duration 30 --dt 0.05`): idêntico byte-a-byte ao baseline**
(mesmo chattering período-2 no passo 87/iter 8 com `res=1759.62`, mesma falha no passo 109 com
`res_norm=7730.44`). Causa: o movimento de topo real (`vessel_motion.cpp:286`) tem sua PRÓPRIA
rampa suave (mesma função `0.5*(1-cos(...))` do fallback) -- no passo 1 (t=0,05s) o fator de rampa
já é essencially zero, então o passo 1 converge trivial em pouquíssimas iterações sem nunca precisar
de correção do Newton na prática (a checagem de convergência usa só o resíduo de força, que não
depende de `K_global`/rigidez artificial nenhuma) -- ou seja, o mecanismo real de rigidez artificial
no passo 1 existe pra outra coisa (ajudar a transição estática→dinâmica em geral), não tem
oportunidade de influenciar este caso específico. **Mantido mesmo assim** (fiel ao real, zero
regressão, zero mudança de comportamento) -- é a segunda tentativa seguida sem efeito no item 3;
pausado aqui, aguardando decisão do usuário sobre se vale continuar investigando esse item
específico ou seguir pra outra coisa.

**Atualização 9** (achado decisivo + terceira tentativa, melhoria real mas parcial): antes de
tentar mais nada, comparei contra uma rodada REAL do Fortran via WSL já feita numa sessão anterior
(`exemplos/Curso/Exemplo_01/Exemplo_01c/Exemplo_01c_analysis/Exemplo_01c_A1_FarDD.SAI`, dinâmica
completa 70s/dt=0,05, mesma config) -- nunca conferida contra este item específico até agora.
**O ANFLEX real não tem NENHUMA dificuldade no passo 109/t=5,45s**: converge em 3 iterações, como
todos os passos vizinhos (2-5 iterações, sem chattering) do início ao fim dos 70s/1400 passos.
Descarta de vez a hipótese "talvez seja um limite físico/numérico que o real também tem" -- não é;
o `risersim` tem uma fragilidade real e específica que o real não tem.

Isso motivou a terceira tentativa: instanciar o Morison (`hydrodynamics.hpp`, escrito há tempos mas
nunca ligado ao solver dinâmico -- só o movimento de topo via RAO entra dinamicamente hoje, a linha
em si só recebe arrasto de CORRENTE, constante, nenhuma força hidrodinâmica de ONDA). Implementado
em `dynamic_analysis.cpp::assemble_at()`: gerado um estado de mar JONSWAP real (uma realização de
fase aleatória por análise, não por passo) via `JONSWAPSpectrum::generate_wave_components()` +
`AiryWaveKinematics`, aplicado por elemento via `MorisonForce` (arrasto + inércia). Detalhe de
correção importante: a aceleração estrutural passada pra `calculate_force_per_length()` é
deliberadamente ZERO, não a real -- o termo `-(Cm-1)*rho*A*a_struct` do Morison é matematicamente a
MESMA contribuição de massa adicionada que `mass_matrix()` já bota no lado esquerdo da equação
(`m_added = rho_water*outer_area()*props.Ca`, com `Ca=Cm-1`); usar a aceleração real duplicaria essa
massa adicionada (uma vez via `M_global`, outra via `F_ext`). Suíte 361/361 sem regressão.

**Resultado real (Far)**: passa do passo 109 pro **116** (t=5,45s → 5,80s) -- melhoria real, mas
pequena, longe do comportamento limpo do real (2-5 iterações até o passo 1400). Testei com uma
segunda semente de fase aleatória (7 em vez do default 42): passo **118** (t=5,90s) -- na mesma
faixa, confirma que não é ruído de fase específico (não é "essa fase em particular ajuda", é uma
melhoria real mas modesta e consistente entre realizações). **Suspeita não confirmada de por que a
melhoria é pequena**: o movimento de topo (RAO "harmônico equivalente", frequência única
determinística) e o Morison na linha (decomposição de fase aleatória multi-componente) usam DUAS
realizações INDEPENDENTES do "mesmo" mar JONSWAP -- fisicamente deveriam vir da mesma onda real
(correlacionadas), mas aqui não estão, o que pode fazer a força de onda na linha brigar com o
movimento do topo em vez de reforçá-lo em alguns instantes. Não investigado mais a fundo -- exigiria
repensar a representação do mar (uma realização única alimentando tanto o RAO equivalente quanto o
Morison), mudança maior. **Mantido mesmo assim** (fisicamente real, correto na formulação, melhoria
mensurável, zero regressão) -- terceira tentativa seguida sem fechar o item 3 por completo, mas a
primeira que de fato mudou o resultado na direção certa. Pausado aqui.

**Atualização 10** (2026-08-16, quarta tentativa -- bug real achado e corrigido, mais dois
mitigadores parciais explorados e deixados como decisão do usuário): antes de perseguir as duas
pistas já anotadas (correlacionar a onda do topo com a do Morison; mapear os binários `.HIS`/`.MOV`
reais), investiguei as duas primeiro -- ambas esbarraram em complicação antes de virar trabalho
concreto (`.MOV` acabou sendo só um eco de texto da config do corpo flutuante, não histórico;
`.HIS` é binário de verdade mas a rotina achada que escreve nele, `bpwrde.f`, é do módulo de
análise em domínio da FREQUÊNCIA, não do domínio do tempo -- não serve pra comparar tração por
passo do caso Far transiente). Por decisão do usuário, troquei pra instrumentar o próprio
`risersim` no passo que falha.

- **Achado**: o sinal do passo que falha é mais grave do que documentado até aqui -- tração
  efetiva em elementos perto do touchdown chega a **±1-10 MN** entre iterações (a tração real de
  topo é ~218 kN), fisicamente absurdo. Com `EA=360 MN` e elementos de ~1m, poucos centímetros de
  deslocamento relativo entre nós vizinhos já geram isso.
- **Bug real achado e corrigido**: `node->friction_force` (`seabed.hpp`) é estado PERSISTENTE
  atualizado incrementalmente (`f_state += k*du`) -- mas o laço de backtracking do solver dinâmico
  (`dynamic_analysis.cpp`, a mesma técnica que já existe no estático) chama `assemble_at()` uma vez
  por tentativa de `alpha`, e cada chamada muta `friction_force` em definitivo, INCLUSIVE tentativas
  REJEITADAS -- sem tirar/restaurar um snapshot antes de cada tentativa (o estático já faz isso,
  via `NodeStateSnapshot`/`restore()`; o dinâmico nunca teve o equivalente). O `du` de uma tentativa
  rejeitada é relativo ao estado deixado pela tentativa ANTERIOR (também rejeitada), não ao estado
  realmente aceito no início da iteração -- cada backtrack composto contamina ainda mais o estado de
  atrito. Corrigido: snapshot de `friction_force` de todos os nós antes do laço de backtracking,
  restaurado no início de cada tentativa. Sem regressão (405/405; casos já validados como Cross
  seguem convergindo idênticos -- a correção é matematicamente um no-op quando a primeira tentativa
  já é aceita, só muda comportamento quando o backtracking de fato tenta mais de uma vez).
  **Resultado isolado** (config padrão, sem mais nada): passo 116 → **128** (t=5,8s → 6,4s).
- **Dois mitigadores parciais adicionais, testados mas NÃO mantidos no código** (efeito real, mas
  com trade-offs que ficam pra decisão do usuário antes de tornar permanentes):
  1. Cap de translação do limitador de passo dinâmico (hoje hardcoded 0,5m, nunca configurável)
     mais apertado (0,01m) + orçamento de iteração maior (20→200-400): sozinho leva de 116 pro
     **145**: mas combinado com a correção do atrito acima, NÃO soma (144, quase igual) --
     indício de que o cap apertado já limita o dano que o bug de atrito causava, então corrigir os
     dois juntos é redundante, não aditivo.
  2. Tolerância do detector de chattering período-2 (`res_hist`, hoje `1e-4` relativo, nunca
     configurável) mais frouxa: combinada com a correção do atrito (cap padrão, mais iterações),
     `5%` leva de 156 pro passo **210** (t=10,5s, quase o dobro do baseline original) -- mas o
     colapso final vira catastrófico (resíduo ~1e92) em vez de um estouro comum; `1%` dá resultado
     PIOR (175) e também colapsa catastroficamente (~1e100) -- não é monotônico, sugere que uma
     tolerância frouxa demais pode aceitar uma média de dois estados genuinamente diferentes (não
     um par período-2 de verdade), plantando uma semente ruim que floresce em outro passo adiante.
- **Estado atual do código**: só a correção do bug de atrito (item acima) ficou -- é a única das
  três mudanças que é uma correção sem ambiguidade, não um trade-off. Os dois mitigadores foram
  revertidos (ficam documentados aqui, com os números, caso o usuário queira reativá-los como
  parâmetros configuráveis via JSON, no mesmo espírito do `enable_step_limiting` do estático).
- **Ainda em aberto**: mesmo só com a correção do atrito, o passo 128 falha com um padrão de
  chattering quase-período-2 (resíduo oscilando ~30mil↔226mil em DOFs laterais da zona de
  touchdown) apertado demais pro detector de 1e-4 pegar -- não é mais o "slack line" simples
  documentado antes (tração levemente negativa), é uma dinâmica de contato mais rica que ainda não
  tem causa raiz identificada. Próxima hipótese, se valer a pena: expandir o detector de
  período-2 pra período-N genérico.

**Atualização 11** (2026-08-16, quinta tentativa -- correção real de consistência, resultado nulo
neste caso): pedido do usuário pra testar se `C_trial` recomputado por tentativa (a técnica que já
fechou o chattering dos passos 79/87, Atualização 6) também se aplicava aqui. Ao reler o código com
calma: **já se aplica** -- `C_trial` já é recalculado do zero a partir do `K_global` fresco de cada
tentativa de backtracking desde a Atualização 6; o comentário logo acima é que tinha ficado
desatualizado (dizia "reusa M_global/C_global... aproximação aceita", sem refletir mais o código
abaixo). Mas essa releitura revelou um paralelo real que ninguém tinha notado: `M_global`
(matriz de massa) era montada UMA VEZ no topo de cada iteração e reaproveitada em TODAS as
tentativas de backtracking daquela iteração, com um comentário afirmando "não depende de Z" --
falso: `local_mass_matrix()` (`element_beam.cpp`) usa `current_length()`, que muda com a posição
dos nós tanto quanto a rigidez geométrica que já motivou recalcular `K`/`C` por tentativa. Extraído
`assemble_mass()` (lambda, mesmo padrão de `assemble_at()`) e chamado tanto no topo da iteração
quanto dentro do laço de backtracking, produzindo `M_trial` fresco por tentativa. Suíte sem
regressão (405/405); Cross seguiu convergindo idêntico. **Resultado no caso Far**: nenhuma
mudança -- passo de falha e resíduo final ficaram byte-idênticos ao baseline só-com-o-fix-de-atrito
(passo 128, `res_norm=9.81756e6`), confirmando que a massa nunca diverge o bastante entre
tentativas pra mudar uma decisão de aceitar/rejeitar neste caso específico (o comprimento dos
elementos na zona de touchdown varia pouco demais, mesmo sob uma tentativa ruim, pra M pesar).
**Mantida mesmo assim** (correção de consistência real e barata, sem regressão, mesmo padrão já
validado pra K/C) -- resultado nulo aqui, mas pode importar em outro caso com deformação mais
extrema por tentativa. O comentário desatualizado foi corrigido pra refletir que K/C/M são todos
recalculados por tentativa agora.

**Atualização 12** (2026-08-16, sexta tentativa -- achado grande, causa raiz provável do gap
inteiro): usuário pediu pra verificar os dados/cálculo do movimento em vez de continuar mexendo no
solver. Reli `vessel_motion.cpp::get_motion()`: a rampa que traz o movimento de zero até a
amplitude plena usa `ramp_time_s_ = 5.0` FIXO, nunca ligado a nenhum dado real.

- **Achado real**: a tabela real "JOINT MOVEMENTS - WAVE AND MOVEMENT CORRELATED" de um `.SAI`
  real (`Exemplo_01c_A1_FarDI.SAI:811-818`) imprime, por GDL, `AMPLITUDE`, `PERIOD`, `PHASE` E
  **`RAMP`** -- uma coluna nunca conferida até agora. Pro Far: `PERIOD=15,348s` (nosso cálculo:
  15,24s, ~0,7% de diferença -- confirma que a frequência equivalente em si já estava correta),
  `RAMP=30,696s`. Conferido nos outros 3 casos reais também (Cross: período 11,022/rampa 22,044;
  Near: 10,717/21,434; Transverse: 10,325/20,650) -- em TODOS, **rampa = 2 × período**, exato.
- **Tentativa 1, descartada**: ler o C++ real (`model_builder_dat.cpp:461-480/5223-5225`,
  `cWave::get_wave_ramp()`) mostrou que o default real É `0,10 × duração total da análise
  dinâmica` -- implementei isso (`ramp_time_s = 0.10 * duration_s`, threaded desde
  `dynamic_analysis.cpp` até o construtor de `VesselMotion`). **Quebrou o caso Cross** (que sempre
  convergiu limpo): `%ANALYSIS_CASE.TIME_DOMAIN.TOTAL_TIME` do `Exemplo_01a.aml` é só 1,0s (valor
  de teste/exemplo), então a rampa virou 0,1s -- amplitude plena em 2 passos de tempo, choque
  imediato na estrutura. Revertido antes de qualquer commit.
- **Investigação da duração real por trás do `.SAI`**: achei `TOTAL TIME = 70.0000` impresso no
  próprio `FarDI.SAI` -- mas veio de `Exemplo_01c.aml` (não `Exemplo_01a.aml`, que só tem o valor
  de 1,0s de brinquedo -- são dois arquivos de exemplo diferentes sobre o mesmo modelo físico).
  Com duração real = 70s, a fórmula "10% da duração" daria rampa=7,0s -- não bate com os 30,696s
  reais (30,7/70=0,44, não 0,10). **A fórmula "10% da duração" está confirmada no código real, mas
  não é o que gerou ESSES dados específicos** -- não achei o campo/cálculo exato de onde
  `get_wave_ramp()` tirou 30,696s pra esse caso (não é um campo exportado no `.aml`, que não tem
  nenhuma ocorrência de "RAMP" em lugar nenhum). O padrão "2×período" continua batendo exato nos 4
  casos reais, então é essa relação (baseada no período do próprio movimento equivalente, não na
  duração total escolhida pra rodar a simulação) que foi implementada.
- **Implementado**: `ramp_time_s_` agora é calculado DENTRO do construtor de `VesselMotion`
  (`2 * (2*pi/omega_eq_)`), depois que a frequência equivalente é conhecida -- não depende mais de
  nenhum parâmetro externo (`dynamic_analysis.cpp` não passa mais rampa nenhuma, só o construtor
  resolve sozinho). Suíte sem regressão (405/405). Cross convergeu limpo com rampa calculada
  =22,17s (bate com o real 22,044s). Near e Transverse também sem regressão.
- **Resultado no caso Far**: o passo de falha foi de **t=5,8s pro passo 442, t=22,1s** -- quase
  4x mais simulação, com um resíduo final pequeno (776 N, não mais explosões de milhões) --
  claramente o maior salto de todo esse eixo de investigação, e resultado IDÊNTICO rodando com
  duração total de 30s ou 70s (confirma que a rampa agora é intrínseca ao movimento, não à duração
  escolhida pra rodar). Testado orçamento maior de iterações (20→100): melhora um pouco mais
  (passo 453, t=22,65s) mas não fecha -- resíduo na explosão final ficou maior (100687 N), sinal
  de que o que resta pode ser um problema genuinamente diferente do que já foi caçado até aqui,
  não mais o mesmo padrão de chattering pequeno.
- **Em aberto**: ainda não converge o run completo, mas a causa provável do gap inteiro (rampa
  fixa de 5s completamente desconectada da física real, aplicando o movimento pleno rápido demais)
  parece ter sido a principal — o restante que falta agora, no novo ponto de falha (t≈22s, perto
  de onde a excursão do topo atinge seu primeiro extremo, dado T≈15,3s), precisa de investigação
  própria, sem a distorção anterior.

**Atualização 13** (2026-08-17, comparação sistemática contra `anf_movements` real -- fecha uma
hipótese com certeza matemática, abre uma nova pista concreta): usuário pediu pra comparar
diretamente o cálculo do movimento entre `anf_movements` (C++ real) e `risersim`, retomando o gap
de heave/roll do Far documentado acima como "causa não identificada".

- **Comparação ponto-a-ponto contra o `.SAI` real, alta precisão**: com um print de debug
  temporário (revertido), confirmado que a curva RAO TRANSFERIDA calculada por `vessel_motion.cpp`
  (amplitude E fase, heave e roll, em 6 frequências entre 0,38-0,43 rad/s) bate **byte-a-byte** (6
  algarismos significativos) com a tabela real "RESPONSE AMPLITUDE OPERATOR (TRANSFERRED)" do
  `.SAI` (`Exemplo_01c_A1_FarDI.SAI:311-372`). Também confirmado que os parâmetros JONSWAP usados
  (alpha=0,004726 gamma=1,674175 period=15,35) batem exatos com os impressos no `.SAI`
  (`:535-537`: PEAKEDNESS=1,6742 ALPHA=0,0047260) e com `%EQUIVALENT_HARMONIC.MAXIMIZATION=HEAVE`
  do `.aml` real. **Ou seja: a leitura da tabela RAO, a interpolação de heading, a transferência de
  corpo rígido e os parâmetros de onda estão TODOS corretos** -- não é mais hipótese, é
  confirmado numericamente contra os dados reais.
- **Tentativa de fix -- mantida, mas não fecha o gap**: comparando `hybrid_movement.cpp:69-101`
  (C++ real) contra `vessel_motion.cpp`, a integração dos momentos espectrais (m0/m2/m4) usava um
  grid UNIFORME de `jonswap_nwave` pontos (100, dw~0,028 rad/s) em vez de nós de quadratura mais
  finos onde a RAO varia rápido. Troquei pra usar os próprios pontos de frequência da tabela RAO
  como nós de quadratura (mais finos, 0,01 rad/s, perto da ressonância de roll). **Verificado
  numericamente que isso É uma correção real** (mais perto do integral contínuo verdadeiro: contra
  uma referência de 200 mil pontos usando a mesma curva RAO já confirmada idêntica à real, o grid
  antigo tinha ~4,6% de erro no m0 do heave por superestimar a área perto de ω=0,2 rad/s -- onde
  S(ω)~ω⁻⁵ é convexo e o trapézio superestima --, o novo grid tem ~1,2%). Suíte sem regressão
  (405/405), sem regressão no Far real (mesmo passo de falha 442/t=22,1s de antes). **Mas -- achado
  importante -- não é a causa do gap real**: mesmo o integral de referência de 200 mil pontos, na
  MESMA curva RAO/parâmetros já confirmados idênticos aos reais, dá amp_max(roll)=0,1323 rad
  (7,58°) e amp_max(heave)=7,66 m -- ainda longe do real (roll=9,7258°=0,16975 rad, heave=9,2905
  m). **Nenhum esquema de quadratura pode legitimamente convergir pra um valor ~2,7x (roll)
  diferente do integral contínuo da MESMA função já verificada idêntica à real** -- descarta
  resolução de grid como causa raiz com uma certeza bem mais forte que a tentativa anterior
  (Atualização 4, que só tinha testado convergência até nwave=20000 sem uma referência analítica
  independente pra comparar).
- **Pista nova, concreta, não fechada**: lendo `movgharfloa.f:244-274` (a rotina Fortran legada que
  o roadmap já apontava como próximo passo desde a Atualização 4/8) -- `AREAMOV(dof)` NÃO é uma
  integral trapezoidal contínua, é uma **SOMA discreta sobre os componentes reais da onda**:
  `AREAMOV += (AMPMVE_i * AMP_i)²` onde `AMPMVE_i` é a RAO interpolada na frequência do componente
  `i` e `AMP_i` é a AMPLITUDE PRÓPRIA daquele componente de onda (não um peso `S(ω)·dω` calculado
  aqui, mas um valor já pronto, vindo de qualquer que seja o esquema de discretização da onda
  escolhido -- `spectrum.cpp` tem 3 métodos, `discretize_constant_period/amplitude/frequency`, cada
  um distribuindo os componentes de um jeito diferente). Confirmado que a onda real do Far usa
  `NWAV=100` (`%WAVE.SPECTRUM_SUBDIVISION` no `.aml`, mesmo valor que `jonswap_nwave` já assume) --
  **mas ainda não identificado QUAL dos 3 métodos de discretização é usado nem como `AMP_i` de cada
  componente é calculado** (não é simplesmente `sqrt(2·S(ω_i)·Δω_i)` com `Δω` uniforme -- os campos
  do `.aml` `%WAVE.LEFTEND_FREQUENCY=5`/`RIGHTEND_FREQUENCY=10`/`RIGHTEND_INTERVAL=0,1` sugerem
  parâmetros de um esquema específico ainda não decifrado). Isso muda o tipo de discrepância que
  procuramos: não é mais "grid fino demais vs grosso demais" (já descartado com o cálculo acima),
  é "SOMA sobre um conjunto específico e não-trivial de componentes de onda reais" vs. o que
  `risersim` faz hoje (integral contínuo aproximado) -- podem divergir mesmo com o número de
  componentes igual, se a DISTRIBUIÇÃO das frequências/amplitudes dos componentes for bem diferente
  perto de uma ressonância estreita como a do roll.
- **Pista da soma discreta -- testada e também descartada**: usuário pediu pra focar no C++ real
  (`anf_analysis`/`anf_movements`), não no Fortran. Os campos do `.aml`
  (`LEFTEND_FREQUENCY=5`/`RIGHTEND_FREQUENCY=10`/`RIGHTEND_INTERVAL=0,1`) mapeiam exatamente nos
  parâmetros `nesq`/`ndir`/`dwmaxd` de `cSpectrum::discretize_constant_amplitude()`
  (`spectrum.cpp:166-267`, C++ puro) -- confirma que essa é a discretização real usada (não
  Fortran). Repliquei essa função + `extrem()` (o refinamento de cauda) byte-a-byte em Python: 100
  componentes NÃO-uniformes, deliberadamente adensados perto do pico espectral (que fica quase
  exatamente na mesma frequência da ressonância de roll do Far, ~0,40 rad/s -- 22 dos 100 pontos
  caem só na janela [0,35-0,45]). Usando ESSE grid real como nós de quadratura pro cálculo de
  `hybrid_movement.cpp` (que só reaproveita a frequência dos componentes, `w[i]`, avaliando sua
  PRÓPRIA `S(ω)` -- não usa a amplitude do componente da onda em si): amp_max(roll)=0,1314 rad --
  **essencialmente idêntico** a todas as outras tentativas de grid (0,131-0,134), não os 0,170 rad
  reais. Confirma outra vez, agora com o grid VERDADEIRO (não uma aproximação), que quadratura não
  é a causa.
- **Achado grande: o `.SAI` real provavelmente não vem do C++**. Procurando onde a string exata
  "JOINT MOVEMENTS - WAVE AND MOVEMENT CORRELATED" (cabeçalho da tabela real) é impressa, achei DUAS
  fontes: `bdeffun.f` (Fortran) e `anf_movements/src/anflex_data.cpp:653`
  (`print_regular_movement_approach`, C++, formato idêntico ao do `.SAI` real). Mas
  `print_regular_movement_approach` só é chamada de DENTRO do próprio `anflex_data.cpp`
  (`set_equivalent_harmonic:740`, `set_equivalent_wave` equivalente), escrevendo num arquivo de
  SAÍDA LATERAL (`"equivalent_harmonic_approach.txt"`), não no `.SAI` principal -- e o único outro
  call site (`main.cpp:412`) é o `main()` de teste/demo standalone da própria biblioteca
  `anf_movements`, não o pipeline do solver completo. **Nenhum call site liga essa função C++ ao
  `.SAI` real** -- ou seja, o número que essa sessão (e sessões anteriores) vem comparando como
  "verdade" no `.SAI` muito provavelmente foi gerado pelo caminho Fortran legado
  (`bdeffun.f`/`movgharfloa.f`/`movext.f`, cujo `AREAMOV` já demonstrado ser uma SOMA discreta
  sobre componentes de onda, estruturalmente diferente do integral espectral que
  `hybrid_movement.cpp` calcula -- Atualização 13 acima), não pelo C++ que `vessel_motion.cpp`
  replica.
- **Reformulação do problema**: `vessel_motion.cpp` já foi verificado, de várias formas
  independentes, como um port fiel do algoritmo C++ real (`hybrid_movement.cpp`/
  `equivalent_harmonic.cpp`/`jonswap_spectrum.cpp`/`rao_table.cpp`) -- curva RAO, parâmetros
  JONSWAP, fórmula de momentos espectrais/Rayleigh, e agora até o grid de discretização de onda,
  todos conferidos byte-a-byte ou algoritmo-a-algoritmo contra a fonte. Se o `.SAI` de fato vem do
  Fortran (não confirmado 100%, já que não fui além do C++ por pedido do usuário), o "gap" de
  heave/roll pode não ser mais um bug do risersim -- pode ser uma divergência real entre o Fortran
  legado e o C++ atualmente mantido, que nem o próprio ANFLEX reconcilia entre os dois caminhos.
- **Correção importante**: a frase acima ("~2,7x" no m0 do roll) tinha um erro de conta -- elevei
  ao quadrado uma razão que já era a razão de m0, não de amplitude. O gap real, corrigido: amplitude
  do roll/heave sai **~15-23% baixa**, não ~2,7x -- bem mais parecido com a tabela original de razões
  documentada antes desta rodada (Atualização 4: heave 0,86, roll 0,80/0,94 no Far/Cross) do que a
  história dramática que cheguei a escrever.
- **Compilado e testado o `anf_movements` real (não o Fortran)**: usuário pediu explicitamente pra
  compilar. Montado um projeto CMake mínimo (`anf_movements_probe/`, fora do `risersim`) linkando só
  o necessário do C++ real (`rao_table.cpp`, `rao.cpp`, `cubic_spline_function.cpp`,
  `linear_function.cpp`, `piecewise_function.cpp`, `tdma.cpp`, `matrix_transform.cpp`,
  `jonswap_spectrum.cpp`, `trigonometric_functions.cpp`, `statistics.cpp`, com `-DANFLEX`), com stubs
  vazios só pros símbolos Fortran nunca de fato chamados nesse caminho (`durandd`/`ran2`/`wnumb`/
  `extrem`). Compilou limpo com o mesmo MSVC "Visual Studio 18" do risersim. Descoberta no caminho:
  `model_builder_dat.cpp:5609` confirma que o carregador REAL de RAO da produção usa
  `AnfLoadings::CUBIC_SPLINE` (não linear, que é o que `vessel_motion.cpp` usa) -- uma pista nova e
  plausível, já que o pico de roll é bem estreito e spline vs. linear diverge mais forte fora dos
  nós tabelados.
  - Achado bônus: o `.iwv` REAL usado pra gerar esse `.SAI` (`.../includes/OSW.iwv`, mesmo
    TZ/HS/GAMMA/ALFAJ já confirmados) mostra **ICGO=2 (CONSTANT_FREQUENCY)**, NESQ=0, NDIR=0 --
    **não** CONSTANT_AMPLITUDE como eu tinha deduzido antes lendo só os campos do `.aml`
    (`LEFTEND_FREQUENCY`/`RIGHTEND_FREQUENCY`/`RIGHTEND_INTERVAL`, que aparentemente não mapeiam
    onde eu assumi). Achar o arquivo real evitou continuar investigando em cima de uma premissa
    errada.
  - Rodei o `cRAO_Table` genuíno (spline cúbico real) direto contra o `FPSO.RAO` real (o mesmo
    arquivo usado pra gerar o `.SAI` de referência, 25 direções × 59 frequências, direção 150°
    isolada), com o `cJonswapSpectrum` genuíno e os parâmetros já confirmados. **Resultado: quase
    idêntico ao que `vessel_motion.cpp` (interpolação linear) já dá** -- roll amp_max
    0,131-0,142 rad (razão 0,77-0,84 contra o real 0,170 rad) em 3 grids de quadratura diferentes,
    contra 0,131-0,134 rad da interpolação linear. **Cubic-spline vs. linear não é a causa** -- a
    diferença entre os dois métodos de interpolação é pequena (~1-8%), muito menor que o gap
    inteiro (~15-23%).
- **Conclusão desta sub-rodada**: usando classes C++ REAIS e compiladas (não mais inferência lendo
  fonte, nem reimplementação em Python) -- `cRAO_Table` genuíno com spline cúbico genuíno,
  `cJonswapSpectrum` genuíno, dados reais (`FPSO.RAO`) -- o mesmo shortfall de ~15-23% em heave/roll
  aparece de novo, praticamente idêntico ao que `vessel_motion.cpp` já calculava. Isso é uma
  confirmação bem mais forte que qualquer coisa anterior nesta investigação de que a interpolação
  RAO e a quadratura dos momentos espectrais, ISOLADAMENTE, não são a causa. Faltava rodar o
  pipeline completo `cEquivalentHarmonic`/`cHybridMovement` de verdade -- feito a seguir.

**Atualização 14** (2026-08-17, pipeline REAL completo, ponta a ponta -- salto grande no gap):
usuário pediu explicitamente pra fazer exatamente como o `anf_movements` faz, não mais isolar
`cRAO_Table`/`cJonswapSpectrum`. Expandido o `anf_movements_probe/` pra compilar e rodar o
`cIrregularWave`/`cEquivalentHarmonic` reais, ponta a ponta:
- Achado no caminho: `discretize_constant_frequency` usa `durandd`/`ran2` de verdade (não Fortran
  -- o próprio `anf_movements` já tem uma transcrição C++ verificada em `random_numbers.cpp`,
  comentada como "Rotina transcrita do Anflex em Fortran para o C"), o RNG clássico Numerical
  Recipes (L'Ecuyer + embaralhamento Bays-Durham) -- usado diretamente, sem stub.
- Parseados os 25 headings inteiros do `FPSO.RAO` real (não mais só a direção 150° isolada) num
  `cRAO` genuíno (`CUBIC_SPLINE`, confirmado real via `model_builder_dat.cpp:5609`), com um
  `cIrregularWave` genuíno usando os parâmetros reais do `OSW.iwv` (JONSWAP, `CONSTANT_FREQUENCY`,
  NWAVE=100, NESQ=NDIR=0, NSIMU=1 -- literalmente o valor real do arquivo, não um chute) e um
  `cEquivalentHarmonic` genuíno (HEAVE maximization, t=10800s, mesma posição/CM/floating_angle já
  confirmados byte-exatos).
- **Resultado**: roll amp_max sai **93,7%** do real (antes: 77-84% em toda tentativa anterior,
  incluindo com spline cúbico real isolado), heave **95,2%** (antes ~83-87%), demais GDL 98-106%.
  Período 15,317s vs real 15,348s (~0,2% de diferença, também bem mais próximo que antes).
  Verificado que o resultado é ESTÁVEL contra o RNG (testado NSIMU=1,2,3,4,5,7,100 -- todos dão
  praticamente o mesmo valor, ~93-95% pro roll -- uma anomalia isolada de 60% observada no meio do
  processo foi rastreada a um binário desatualizado por uma sequência de rebuild malfeita, não a
  sensibilidade real ao RNG).
- **Conclusão**: rodando o pipeline C++ real de ponta a ponta (não mais peças isoladas), o gap de
  heave/roll cai de ~15-23% pra **~5-7%** -- uma discrepância pequena o bastante pra ser plausível
  como resíduo de detalhe de implementação não replicado (ex. `NVERT`/malha hiperbólica, marcada
  como "sem efeito" nos comentários mas não 100% confirmada nesta rodada) ou uma divergência residual
  real Fortran-vs-C++ (achado da Atualização 13). **`vessel_motion.cpp` está, na prática, correto** --
  o C++ real da ANFLEX, rodado de verdade com os dados reais, chega bem perto do próprio `.SAI` de
  referência, e MUITO mais perto do que qualquer aproximação usada nesta investigação. Ferramenta
  (`anf_movements_probe/`, fora do `risersim`) mantida no repo pra qualquer verificação futura.

**Atualização 15** (2026-08-17, tentativa de fechar o ~5-7% residual -- sem sucesso, mas gap
descartado como não-explicado por nenhum parâmetro explícito): usuário pediu pra investigar o
resíduo. Achada uma TERCEIRA árvore de código relevante: `interfaces/src/anfmov.cpp` -- o
executável batch que de fato processa `.aml` puro (distinto de `anf_analysis/src/
model_builder_dat.cpp`, que é o caminho usado por um formato de projeto baseado em `cReaderNode`/
XML, não o `.aml` clássico com diretivas `%WAVE.*`/`%EQUIVALENT_HARMONIC.*`). Auditados, campo a
campo, contra o `.aml`/`.SAI` reais do `Exemplo_01c`:
- `GRAVITY=9.81` (`%GLOBAL.GRAVITY`) e `SWL=265.0` (`%GLOBAL.SEABED.DEPTH`) -- confirmados, batem
  com o que o probe já usava.
- `MAXIMIZATION=HEAVE` para o load case 'Far' -- confirmado via `%EQUIVALENT_HARMONIC.MAXIMIZATION`
  no `.aml` (não é mais um hardcode não-verificado).
- `w_seed`/`nsimu` -- confirmados como o mesmo campo `%WAVE.RANDOM_VALUE=1.0` alimentando os dois
  parâmetros do construtor (`anfmov.cpp:680,695`), batendo com o que o probe já usava.
- `NESQ`/`NDIR`/`DWMAXD` -- `anfmov.cpp:250-252` zera esses três explicitamente quando
  `disc_type == CONSTANT_FREQUENCY` (nosso caso), **mesmo o `.aml` tendo `LEFTEND_FREQUENCY=5`/
  `RIGHTEND_FREQUENCY=10`** -- confirma (de um ângulo novo) o achado anterior de que esses campos do
  `.aml` são irrelevantes pra esse `ICGO`, não uma leitura errada.
- `delta` -- `anfmov.cpp` calcula `(wfin-wini)/nwave = 0.028` e passa explicitamente; o probe passa
  `spectrum=nullptr`, o que aciona o mesmo cálculo internamente no construtor (`irregular_wave.cpp:
  125`, só ativo quando `m_spectrum == NULL`) -- resultado idêntico por caminhos diferentes,
  confirmado lendo o código, não é uma divergência.
- **Achado real, testado empiricamente**: `anfmov.cpp:732,736` usa interpolação **LINEAR** pra RAO,
  não `CUBIC_SPLINE` (contradiz a Atualização 13, que tinha confirmado `CUBIC_SPLINE` olhando pro
  `model_builder_dat.cpp:5609` -- código que agora sabemos ser de uma árvore diferente, não
  necessariamente a que gerou este `.SAI`). Trocado `CUBIC_SPLINE`→`LINEAR` no probe e rebuildado:
  o resultado **piorou** (roll 93,7%→88,2%, heave 95,2%→92,0%, sway também se afastou). Revertido
  pra `CUBIC_SPLINE`, que empiricamente fica mais perto do `.SAI` real independente de qual delas o
  `anfmov.cpp` realmente usa.
- **Conclusão desta rodada**: todo parâmetro explícito auditável (gravidade, SWL, semente, NESQ/
  NDIR/DWMAXD, delta, DOF de maximização, método de interpolação) bate com o valor real ou, quando
  testada a alternativa, essa alternativa piora o resultado -- não há mais nenhum "parâmetro óbvio
  errado" a corrigir no probe. O gap residual de ~5-7% provavelmente não vem de um valor de entrada
  incorreto, e sim de algo mais estrutural: possivelmente o `.SAI` real não veio nem do
  `model_builder_dat.cpp` nem do `anfmov.cpp` tal como lidos aqui (pode haver um quarto caminho, ou
  uma versão/branch diferente do binário real usado pra gerar esse exemplo específico), ou uma
  divergência numérica residual genuína entre a implementação Fortran histórica e o port C++ atual
  que nenhuma auditoria de parâmetros vai capturar. Dado o esforço já investido e o tamanho pequeno
  do gap remanescente, a investigação foi pausada aqui -- `vessel_motion.cpp` permanece confirmado
  como correto na prática (Atualização 14), e não há mais evidência de um bug fixável no risersim.

**Atualização 16** (2026-08-17, item 3 -- método HHT-α implementado e testado, hipótese refutada
por experimento direto): usuário pediu pra atacar de vez o item 3 (tração negativa/divergência do
Far em t≈22s, aberto desde a Atualização 6). Duas investigações paralelas (agentes Explore)
descartaram três hipóteses e apontaram uma quarta como a mais provável explicação de por que o
Fortran real não treme no touchdown enquanto o risersim diverge:

- Retry de passo com `dt` reduzido (`badina.f:2163-2171`, `LSTEPITER`) existe no real, mas está
  **desligado** na rodada de referência (`NEWRIG=2>0` faz o card nunca ser lido).
- Elemento de cabo tension-only (`bpcabo.f`/`truss.cpp`) existe na árvore real, mas o `.DAT` de
  referência só usa `PORTICO` (viga não-linear), nenhum `CABO`.
- Suavização de contato com o leito -- confirmado **idêntica** ao risersim (mola linear
  liga/desliga dura nos dois, `soil_uncoupled.cpp`/`bpsolo.f`).
- **Método Alfa (HHT-α) ativo por padrão** (`ALFA=-0,1`, `bldanagr3.f:77`, ativo na rodada de
  referência inteira já que `NEWRIG=2` nunca lê o card de override) -- amortecimento numérico
  controlado de alta frequência, mecanismo clássico (Hilber-Hughes-Taylor 1977) pra exatamente
  este tipo de problema de contato/rigidez quase-singular. O risersim usava Newmark comum com
  `gamma=0,55`/`beta=0,28` ad hoc (nunca documentado/derivado de nenhuma fonte real).

**Implementado**: HHT-α de verdade (não só ajuste de `gamma`/`beta`) em `dynamic_analysis.cpp`,
fórmula confirmada lendo `bcalfa.f:100-114` e os pontos onde `FORALFA`/`FORESTAT` entram no
resíduo em `badina.f` (`gamma=0,5-α`, `beta=0,25*(1-α)²`, resíduo `(1+α)*(F_ext-F_int)_trial -
α*(F_ext-F_int)_prev_aceito - M*A - C*V`, `K_eff` com o mesmo peso `(1+α)` em `K`/`C`). Novo campo
de config `hht_alpha` em `AnalysisOptionsConfig` (`model.hpp`), lido do JSON em
`model_builder.cpp`, default `-0,1` (o próprio default real). Suíte Catch2: 405/405, zero
regressão.

**Resultado real -- hipótese refutada, não confirmada**: testado o Far real (`--duration 30 --dt
0.05`) com `hht_alpha` em quatro valores:

| `hht_alpha` | passo de falha | t (s) | resíduo final |
|---|---|---|---|
| 0,0 (Newmark puro) | 446 | 22,3 | 13499 |
| -0,1 (default real) | 430 | 21,5 | 7493 |
| -1/3 (extremo típico) | 421 | 21,05 | 42028 |
| +0,1 (sinal invertido, diagnóstico) | 28 | 1,4 | 94937 (catastrófico) |

A tendência é monotônica e o teste de sinal invertido se comporta exatamente como a teoria prevê
(amortecimento no sinal errado quebra quase imediatamente) -- confirma que a implementação está
correta, não é bug. Mas o resultado real é o oposto do esperado: **quanto mais amortecimento
HHT-α (mais negativo o α), PIOR** o caso Far fica (falha mais cedo), não melhor. O default real do
ANFLEX (`α=-0,1`) piora ligeiramente o baseline (passo 430 vs. 442-446), e `α=0` (mais perto do
Newmark ad hoc antigo) continua sendo o melhor ponto testado. **Sem regressão em Near/Transverse/
Cross** (os três continuam convergindo os 600/601 passos completos com `α=-0,1` default, `T_eff`
do Cross idêntico ao já validado, 217,3 kN).

**Conclusão**: a hipótese "HHT-α explica por que o Fortran real não treme aqui" está **refutada
por experimento direto**, não apenas não-confirmada -- o mecanismo existe e está ativo na rodada
de referência real, mas replicá-lo fielmente no risersim não resolve (e piora ligeiramente) o
problema específico do touchdown do Far. A causa real da robustez do Fortran nesse ponto continua
sem explicação identificada; nenhuma das 4 hipóteses investigadas nesta rodada (retry de passo,
elemento de cabo, suavização de contato, HHT-α) explica sozinha. Código mantido (é uma melhoria
formal real sobre o Newmark ad hoc anterior, fiel ao mecanismo real do ANFLEX, configurável, zero
regressão nos casos que já funcionavam) -- decisão pendente com o usuário sobre o valor default
(`-0,1` fiel ao real vs. `0,0` que não piora o único caso ainda quebrado) e se vale perseguir o
`LSTEPITER` (retry com `dt` reduzido) como próxima hipótese, já que essa é a única das 4 ainda não
testada diretamente no risersim (as outras 3 foram descartadas por auditoria, não por experimento).

**Decisão do usuário** (via `AskUserQuestion`): default de `hht_alpha` fica em `0,0` (o ponto que
não piora nenhum caso conhecido); implementar e testar `LSTEPITER` como próxima tentativa.

**Atualização 17** (2026-08-17, item 3 -- `LSTEPITER` implementado, ✅ **RESOLVIDO**: o Far converge
os 70s/1400 passos completos pela primeira vez em toda essa investigação): extraído o corpo do
laço de Newton por passo (`dynamic_analysis.cpp`, antes ~500 linhas inline no `for (step...)`) pra
uma função recursiva `try_advance(t_start, dt_sub, depth)`, replicando o mecanismo real
`LSTEPITER`/`LSTEPBACK` (`badina.f:2163-2171`): quando o Newton não converge dentro do orçamento de
iterações, em vez de parar o laço de tempo inteiro, rebobina `U`/`V`/`A`/`F_static_prev` pro
último estado aceito e tenta de novo com `dt` pela metade -- recursivamente, até
`dynamic_max_step_halvings` níveis (novo campo em `AnalysisOptionsConfig`, default 4 = `dt` tão
fino quanto `dt_s/16`; 0 = comportamento antigo, sem retry). `c1`/preditor/`K_eff`/bisecção
período-2/trial de backtracking, todos parametrizados por `dt_sub` (não mais o `dt_s` fixo do
laço externo). O laço externo (`step`/`time=step*dt_s`) continua exatamente como antes -- grid
fixo de log/export/`step==1` da rigidez artificial -- só a FORMA de avançar de `step-1` pra `step`
passa a admitir sub-passos internos. Suíte: 405/405, zero regressão.

**Resultado real**: rodando o Far com a duração REAL (70s/1400 passos, mesma do `.SAI` de
referência) -- **converge os 1400 passos completos**, primeira vez em toda essa investigação
(desde a Atualização 6, ~10 tentativas atrás). 444 retries no total ao longo da rodada inteira,
esmagadora maioria (412) resolvidos com só UM nível de halving (`dt=0,025s`), 32 precisaram de
dois níveis (`dt=0,0125s`), nenhum chegou perto do orçamento máximo de 4. Primeiro retry em
t≈22,25s -- exatamente o ponto onde o baseline sem retry falhava (`t≈22,3s` com `hht_alpha=0`),
confirmando que é o MESMO fenômeno de touchdown/tração-quase-zero já diagnosticado, só que agora
com uma saída real em vez de travar. **Zero regressão**: Near/Transverse/Cross continuam
convergindo limpo (600/601 passos), nenhum dos três precisou de nenhum retry (confirma que o
mecanismo só age quando genuinamente necessário, não muda nada nos casos que já funcionavam).

**Conclusão**: diferente das 3 hipóteses anteriores descartadas por auditoria e do HHT-α (refutado
por experimento), `LSTEPITER` -- a única das 4 hipóteses que não tinha evidência prévia de que
explicava a robustez do Fortran real neste caso específico -- foi a que efetivamente fechou o item
3. Isso não significa que o mecanismo REAL usado pelo Fortran de referência seja este (ele estava
confirmado desligado lá, `NEWRIG=2` -- Atualização 16); significa que replicar essa técnica de
robustez genérica do ANFLEX resolve o problema por conta própria, independente de reproduzir
exatamente o caminho que o Fortran de referência tomou. O item 3 do Eixo 1b/2a, aberto desde a
Atualização 6, está **resolvido**.

**Atualização 18** (2026-08-17): novo tipo de comparação, nunca feita antes nesta investigação --
deslocamento nodal estático (a FORMA da catenária inteira), não o movimento do topo/RAO (já validado
em rodadas anteriores) nem a convergência (já validada). Metodologia: `.SAI` real tem uma tabela
`NODAL DATA` (echo, `NearSI.SAI` etc.) com a posição XYZ final de cada nó nomeado real (confirmado
empiricamente ser a posição JÁ CONVERGIDA, não uma referência pré-carga -- somar a tabela
`NODAL DISPLACEMENTS` do `SS.SAI` por cima dá resultado sem sentido físico, ex. a âncora "andando"
3,85m de sua posição real). Comparado contra `node_positions` final do HDF5 do risersim
(`--static-only`), pareando por comprimento de arco acumulado (541 nós reais vs 529 do risersim,
comprimentos totais batem em 522,00m vs ~521,95m).

**Achado**: nos três casos (Near/Transverse/Cross), o trecho apoiado no leito (a partir de s/L≈0,53-
0,54) bate quase exato (erro horizontal médio ~0,09m). O trecho suspenso em catenária diverge
consistentemente -- pior no Near (médio 5,81m, máximo 8,23m), moderado no Cross (2,71m/4,66m), menor
no Transverse (1,77m/3,34m). O perfil de profundidade (Z) está correto em todo o trecho (erro médio
~0,2m); é especificamente a posição HORIZONTAL (deriva lateral induzida por corrente) que diverge.

**Hipótese testada**: `CurrentProfile::get_drag_force_per_meter` (agora `get_drag_force_per_length`,
`current_profile.hpp`) aplicava a força de arrasto em direção global fixa (`heading` da tabela),
usando o módulo COMPLETO da velocidade de corrente, sem projetar na direção perpendicular ao eixo
local do elemento -- diferente do `MorisonForce` de onda (`hydrodynamics.hpp`), que já faz essa
projeção, e diferente do real `cMorison::calc_distributed_load`
(`anf_analysis/src/morison.cpp:49-124`), que projeta velocidade/aceleração relativas em componentes
normal E tangencial ao elemento antes de aplicar a lei de arrasto quadrática (`normal_rel_vel`/
`tan_rel_vel`, com coeficientes `m_normal_drag_coef`/`m_tangential_drag_coef` separados). Corrigido:
`get_drag_force_per_length(z, D, rho, elem_axis)` agora projeta a velocidade de corrente
perpendicular a `elem_axis` antes da lei quadrática (sem termo tangencial, mesma simplificação já
usada pelo `MorisonForce` de onda -- "standard practice for risers/mooring lines" já documentado
ali), retornando um vetor 3D completo (incluindo componente Z, que antes não existia -- um elemento
inclinado sob corrente horizontal gera força de arrasto com componente vertical, que
`static_integrator.cpp`/`dynamic_analysis.cpp` agora também aplicam em `eq1_z`/`eq2_z`, antes
ignorado). Suíte: 405/405, zero regressão.

**Resultado**: teoricamente mais correto (bate com a formulação real do `cMorison`), mas efeito
PEQUENO na prática -- erro horizontal médio do Near caiu de 5,81m pra 5,72m (~1,5%), Transverse de
1,77m pra 1,70m, Cross praticamente inalterado (2,71m→2,71m). **Mantido** (é a formulação certa,
consistente com o resto do código, não piora nada), mas **não explica a maior parte do gap** --
causa raiz do erro de deriva horizontal na catenária suspensa segue **em aberto**.

**Hipóteses descartadas nesta mesma rodada, com evidência direta**:
- **Deriva média de onda na estática real**: descartada. O deck Fortran real
  (`Exemplo_01c_A1_NearS.DAT:163-165`, flags `IWAVE ICURR IDEAD ...`) tem `IWAVE=0` explícito --
  onda desligada na análise estática real, só corrente (`ICURR=1`) + peso (`IDEAD=1`), exatamente o
  que o risersim já faz no `--static-only`.
- **Convenção de profundidade da corrente invertida**: descartada. O arquivo `.cur` real usado pelo
  Fortran (`includes/Cor_SW.cur`, convenção "Z=altura acima do leito", 0=leito) bate espelhado
  ponto-a-ponto com os blocos `%CURRENT` do `.aml` (convenção "profundidade abaixo da superfície",
  0=superfície) -- mesmos 8 valores de velocidade E ângulo, sem inversão de sinal nem erro de
  mapeamento entre os dois formatos.
- **Coeficiente de arrasto (Cd) via Reynolds automático**: descartada. `%MATERIAL.MORISON.DRAG =
  1.0` está explícito no `.aml` (não `0.0`, que dispararia o cálculo automático via Reynolds em
  `cMorison::calc_distributed_load`, `anf_analysis/src/morison.cpp:88-98`) -- real e risersim usam
  o mesmo `Cd=1,0` fixo.

**Mais uma descartada, também com evidência direta**: risersim convergindo pra um equilíbrio
geometricamente diferente do real por sub-convergência (múltiplos quase-equilíbrios não seria
implausível numa linha com EI muito baixo, ~21,7 kN·m²). Testado: re-rodado o Near com 60 passos de
carga (vs 11 do real), `tolerance=1e-8` (vs 1e-3 -- afeta diretamente `transl_tol`/`rot_tol`, o
critério de razão de incremento, `static_analysis.cpp:330-332`), `max_iterations=200`. Convergiu em
9 iterações no último passo com norma de resíduo ~3,6e-4 N (desprezível pra um sistema com trações
em escala kN) -- e o erro horizontal médio no trecho suspenso ficou **idêntico**: 5,726m vs 5,724m
antes (11 passos/tol=1e-3). Não é sub-convergência nem múltiplos equilíbrios -- risersim converge
de forma tight pro MESMO formato, que diverge do real por uma diferença de física/formulação
genuína, não um artefato numérico.

**Rigidez estrutural (EI/EA) do elemento -- também descartada**, via agente Explore dedicado a
comparar `CorotationalBeam3D` (`element_beam.cpp`) contra o elemento real (confirmado ser `cBeam`/
`beam.cpp`, `%MATERIAL.FINITE_ELEMENT 'beam'` no `.aml` -- não é o Timoshenko, essa linha não usa
cisalhamento):
- Coeficientes da rigidez geométrica batem em forma (`6/5·P/L`, `L/10·P/L` etc., `beam.cpp:361,368`
  vs `element_beam.cpp:66-67`); termos extras do real (`ZI/YI`, ordem `I/(A·L²)`) são ~1e-3 a 1e-4
  relativos ao termo principal -- desprezíveis.
- A "tração efetiva" com termos de pressão interna/externa (`p_e*A_e - p_i*A_i`) que alimenta a
  rigidez geométrica do risersim (`tension_effective`, `element_beam.hpp:155-160`) tem seu
  equivalente real (`FR`, `bar.cpp:1037-1085`) usado só pra RELATÓRIO no real -- a rigidez de fato
  usa tração pura sem pressão (`beam.cpp:1310-1354`). E no risersim, `p_e`/`p_i` nunca são
  atribuídos em lugar nenhum do solver (só expostos via bindings pra ferramentas externas) --
  ficam sempre 0.0. Ou seja, `tension_effective ≡ tension_true` nos DOIS códigos nesse caso: não é
  uma diferença, é um empate (hipótese inicial do agente, de dupla-contagem de pressão hidrostática,
  descartada).
- Comprimento no denominador (`current_length()` no risersim vs `m_original_length` no real,
  `beam.cpp:1246,343`) é uma diferença real confirmada, mas a deformação elástica axial é ~1e-5
  (EA=360MN) -- 3+ ordens de magnitude pequena demais pra explicar 8m em 522m.

**Conclusão desta rodada**: 6 hipóteses testadas (1 mantida com efeito real mas pequeno --
projeção perpendicular da corrente; 5 descartadas com evidência direta -- onda estática,
convenção de profundidade, Cd/Reynolds, convergência/múltiplos-equilíbrios, rigidez EI/EA do
elemento). Causa raiz segue **em aberto**. Próximas hipóteses candidatas, não testadas ainda,
sugeridas pelo agente: (1) a mecânica do "ghost frame"/atualização de tríade corrotacional
(`compute_corotational_forces`, `element_beam.cpp:168-219`, vs `calc_init_rot_mt`/`BTTIL`/
`element.cpp:215-257` do real) -- MAIS provável que seja sutil o bastante pra escapar de uma
auditoria de fórmula isolada; (2) `%CURRENT.ANGLE` varia com profundidade nos dados reais (perfil
completo já confirmado propagado corretamente ponto-a-ponto, mas pode haver erro de
interpolação/sinal em algum lugar do caminho de aplicação da força não auditado a fundo ainda).

**Atualização 19** (2026-08-18, hipótese 1 acima testada -- descartada em 3 partes, mas achado um
gap real e menor no processo): comparação fórmula-por-fórmula direta contra o C++ real (não só
revisão de forma, como as rodadas anteriores desta seção fizeram para EI/EA):
- **`calc_transformation_mt`** (`element.cpp:225-267`, o "ghost frame" de Crisfield) vs
  `compute_corotational_forces()`'s ghost-frame block (`element_beam.cpp:180-197`) -- **match
  exato**, termo a termo (`PX`/`PY`/`PZ`, `A1`/`A2`/`A3`, `I2`/`I3`).
- **`pseudo_sum`** (`math_utils.cpp:134-194`, composição de quatérnion) vs `compose_rotations()`
  (`rotation_utils.hpp`) -- **match exato**: mesma convenção de composição (`qp = q_delta ⊗
  q_old`, equivalente a `R_new = R_delta * R_old` em forma matricial).
- **`update_transformations_matrices`** (`beam.cpp:1055-1063`, `m_node_tm = m_node_init_tm *
  trans(m_transf_mt)`, com `m_transf_mt` construído por `gen_mat_3d(m_rot_desl)` a partir da
  rotação total acumulada do nó, `node.cpp:973-985`) vs `node1_tm = node1_init_triad() *
  rodrigues(node->rot).transpose()` -- **match estrutural exato**.

**Achado real, não testado antes**: `cBeam::calc_init_rot_mt()` (`beam.cpp:300-325`) dá a cada
elemento uma "torção de nascença" (`teta_i`/`teta_j`, via `bttil()`) calculada a partir de
`curvature_1`/`curvature_2` -- valores lidos DIRETO de uma coluna do arquivo de entrada
(`columns_reader.get_double_column("curvature_1")`, `model_builder_dat.cpp:1646`), ou seja,
computados pelo gerador de malha fechado (`tec_line`, já documentado indisponível neste repo), não
pelo solver. O risersim, em contraste, sempre dava aos dois nós de um elemento o MESMO triedro
inicial (`node1_init_triad_ = node2_init_triad_ = build_frame_from_chord(...)`, `element_beam.hpp`
-- já documentado como simplificação deliberada no próprio comentário da classe) -- ou seja, curvatura
inicial zero sempre, mesmo numa malha inicial genuinamente curva (a catenária/apoio-no-leito real,
corrigida via MoorPy). Bate com o padrão do gap (trecho reto no leito ≈ correto, trecho curvo no
vão suspenso diverge).

**Implementado**: já que `tec_line` está indisponível, `curvature_1`/`curvature_2` são estimadas
por diferença finita discreta (curvatura de Menger, 3 pontos consecutivos da malha inicial já
corrigida) em `ModelBuilder::load_from_json()` (`discrete_signed_curvature()`, novo), projetada no
eixo Y local do próprio elemento -- mesmo escopo/convenção do `curvature_1`/`curvature_2` reais.
`CorotationalBeam3D` ganhou dois parâmetros novos (`curvature1`/`curvature2`, default 0.0 -- toda
chamada anterior ao fix se comporta identicamente) e replica `bttil()`/`calc_init_rot_mt()` pra
construir `node1_init_triad_`/`node2_init_triad_` com a torção correta em vez de sempre coincidirem.
Suíte: 405/405, zero regressão.

**Resultado, medido isoladamente (mesmo build, só esse fix ligado/desligado) contra o Exemplo_01a
real (Near/Far/Transverse/Cross)**: tração no topo desloca ~0,02% em TODOS os 4 casos (ex. Cross:
213,930→213,887 kN); deslocamento nodal máximo em toda a linha inteira é de **14cm**, a maioria na
faixa de 1-4cm. **Correto (mais fiel ao ANFLEX real), mas efeito numérico pequeno demais pra
explicar o gap de 5-8m documentado nesta seção** -- não é a causa raiz. **Mantido de qualquer forma**
(decisão do usuário: correto, testado, zero regressão, mesmo sem resolver o gap).

Achado colateral (não relacionado ao fix): o baseline `T_eff` do Cross documentado alhures nesta
sessão (217,5 kN) já não bate nem SEM este fix (213,93 kN, mesma build) -- já tinha se afastado por
alguma mudança anterior não identificada. Vale re-auditar esse baseline separadamente.

**Conclusão**: as 3 partes da mecânica corrotacional (ghost frame, composição de rotação,
atualização de tríade) mais a torção inicial agora estão todas verificadas/testadas -- nenhuma
explica o gap de 5-8m. **Investigação pausada** (decisão do usuário) -- precisa de uma pista
genuinamente nova pra continuar (ex. instrumentar os dois códigos com prints e comparar valor por
valor, iteração por iteração, como foi feito com sucesso pro gap de heave/roll do Far).

**Atualização 20** (2026-08-18, mesma sessão -- usuário pediu "os valores de curvatura estão
corretos?", achado um bug real de eixo, corrigido e remedido -- **não fecha o gap, mas fecha o
capítulo da torção inicial com evidência direta contra dado real, não só T_eff**):

**Verificação direta contra o dado real do `tec_line`** (nunca feita antes -- as rodadas anteriores
só mediam o EFEITO agregado do fix, não os valores de curvatura em si): o `.DAT` real gerado pelo
ANFLEX pra este mesmo modelo (`Exemplo_01c_A1_NearS.DAT`, achado em
`exemplos/.../Exemplo_01c_analysis/includes/L1_beam.ele`) tem as colunas `CURV1`/`CURV2` originais
que o `tec_line` calculou -- dado que eu não sabia que existia até procurar. Portei
`discrete_signed_curvature()` (a estimativa por diferença finita) e `build_frame_from_chord()` (o
eixo Y local que ela projeta) pra Python e comparei elemento por elemento contra essas 540
curvaturas reais (`includes/L1.nod` dá a malha inicial, mesma usada pelo `tec_line`):
- **A fórmula de curvatura discreta em si bate quase exatamente** com o valor real do `tec_line`
  quando projetada no eixo Y que o ANFLEX real usa (razão real/estimado entre 0,9997 e 1,0004 na
  maioria dos elementos, erro médio ~1,3e-5).
- **Mas `build_frame_from_chord()` usa uma convenção de eixo Y diferente da real** -- a heurística
  própria do risersim ("eixo global menos alinhado com a corda") nunca tinha sido comparada
  diretamente contra `nMathUtils::calc_angbeta`/`calc_local_mt_rot` (`math_utils.cpp:419-500`), a
  fórmula real que define ROTGF (usada tanto pra construir os triedros de nascença quanto, no
  ANFLEX real, pra tracking corrotacional contínuo -- as 3 partes já validadas na Atualização 19
  eram sobre a mecânica de COMPOSIÇÃO, não sobre esta escolha de eixo específica). Projetando o
  MESMO vetor de curvatura 3D no eixo Y errado do risersim, o valor sai sistematicamente ~35-40%
  menor que o real, e com **sinal invertido em 18 de 298 elementos (6%)** do vão suspenso do caso
  Near.

**Corrigido**: `build_frame_from_chord()` (`element_beam.cpp`) reescrito pra replicar
`calc_angbeta`/`calc_local_mt_rot` exatamente, em vez da heurística antiga. É a ÚNICA função usada
tanto pelo construtor (triedros de nascença, `ex0` inicial) quanto por `transformation_matrix()`
(rotaciona a matriz de massa local, chamada a cada passo) -- corrigir aqui mantém as duas
consistentes automaticamente, e o tracking corrotacional contínuo (`compute_corotational_forces()`)
nunca chama esta função depois de t=0 (só compõe rotações a partir do triedro de nascença), então a
mudança não pode introduzir um descasamento entre o triedro "de nascença" e o "atual".

**Regressão**: 2 dos 20 casos de teste (`test_multiline.cpp`, os dois testes que passam por
`ModelBuilder::load_from_json()` com uma catenária sintética de malha muito grossa e um TDP quase
vertical) pararam de convergir em 300 iterações -- diagnosticado como dificuldade genuína (não bug):
a torção de nascença agora reflete a curvatura real (antes artificialmente amortecida pelo bug de
eixo), e pra essa malha grossa (8m/elemento) isso é uma perturbação geométrica inicial maior.
Convergiu em 1339 iterações com `max_iter_per_step` elevado pra 2000 (residual já estava em ~1e-5,
só oscilando perto da tolerância de incremento) -- malhas reais do `tec_line` refinam exatamente
essas regiões (0,25-1m, não 8m), então produção não paga esse custo. Comentário explicativo deixado
nos dois testes. Suíte: 405/405 restaurado.

**Remedido contra o dado real (não só T_eff -- posição nodal absoluta, mesma metodologia da
Atualização 19: `NODAL DATA` inicial do `.SAI` + `NODAL DISPLACEMENTS` do último passo convergido =
posição final real, comparada por fração de comprimento de arco)**: caso Near, T_eff idêntico ao
build anterior (217,1 kN) e **forma convergida praticamente idêntica** entre o build com o bug de
eixo e o build corrigido -- erro horizontal médio 4,246m→4,248m, máximo 13,273m→13,273m (diferenças
na 4ª casa decimal). Ou seja: mesmo com os valores de curvatura ~35-40% errados (às vezes com sinal
trocado) sendo substituídos pelos corretos, a forma final da catenária sob corrente não muda de
forma perceptível -- confirma de forma direta e definitiva que a torção de nascença, seja qual for
sua magnitude exata, **não é o mecanismo por trás do gap de 5-8m**, fechando esta linha de
investigação com uma evidência mais forte que a da Atualização 19 (que só tinha T_eff agregado).

**Mantido**: mesmo não fechando o gap, o fix de eixo é uma correção real (documentada e verificada
contra dado real do `tec_line`, não uma hipótese) e melhora a fidelidade da mecânica corrotacional
como um todo (não só a torção de nascença -- `transformation_matrix()`/matriz de massa também
passam a usar a convenção real). **Investigação do gap de 5-8m continua pausada** -- mesma
conclusão da Atualização 19, agora com uma hipótese a mais eliminada com evidência direta.

**Tentativa de retomada, mesma sessão: técnica do `anflex_cmd.exe` (probe do solver real) não
deslanchou, pausada de novo.** Achado promissor: `anflex_cmd.exe` (raiz do repo,
`src/main_dll.cpp`/`main_cmd.cpp`, já compilado em `build/bin/Release/`) é um CLI real, sem GUI,
sem Fortran, que roda o solver ANFLEX de verdade ponto-a-ponto -- a mesma técnica que resolveu o
gap de heave/roll do Far ([[project_far_heave_roll_gap]]). Duas tentativas limpas (`.DAT` puro do
Exemplo_01c, depois -- corrigido pelo usuário, já que `cReader` é baseado em pugixml -- o XML+H5
real do Exemplo_01a/Cross) queimaram ~7-8min de CPU real cada (confirmado via contador de CPU
subindo, não travado/ocioso) sem NENHUMA saída, e foram mortas. Causa parcialmente explicada
(stdout do Windows fica totalmente bufferizado sem console interativo, `main_cmd.cpp` não tem
`fflush`), mas isso não distingue "solver denso legado genuinamente lento" de "travado antes mesmo
de começar a resolver". **Pausado sem resolver a ambiguidade** (decisão do usuário) -- duas opções
não tentadas pra uma próxima rodada: deixar rodar bem mais tempo sem matar, ou instrumentar
`static_analysis.cpp`/`model_builder_dat.cpp` com prints com `fflush` explícito antes de rodar de
novo. Detalhe completo na memória (`project_static_catenary_current_gap.md`).

### 2b. Suporte a múltiplas zonas de solo por segmento (opcional, avaliar sob demanda)

Achado no `Boiao/P52_Boiao.aml` (uma linha atravessando 3 solos diferentes ao longo do próprio
comprimento) — nem `aml_reader.py` nem `xml_h5_reader.py` correlacionam isso hoje, ambos assumem
solo único global. Só vale a pena se um caso de teste real desse tipo entrar em uso.

### 2c. Suporte a múltiplas linhas (corpo flutuante compartilhado) — ✅ IMPLEMENTADO, convergência real verificada

Pedido do usuário (prioridade explícita), movido do backlog. Caso real: `Multilinhas.aml` -- 7
linhas presas ao MESMO corpo flutuante (`%FLOATING.SHIP`, 6 GDL) via 7 pontos de conexão locais
distintos, não acoplamento linha-linha. Pesquisa prévia (2 agentes Explore + 1 Plan) mapeou tanto
a arquitetura real do ANFLEX (`interfaces/src/`: `cReference`/`cConnection`/`cLine`, corpo
flutuante é um SUBTIPO de referência, não entidade separada) quanto o estado do risersim dos dois
lados -- decisão do usuário: espelhar fielmente essa hierarquia no schema JSON (`references`/
`connections`/`lines`), não uma versão minimalista; frontend/web run-manager explicitamente fora
de escopo desta rodada.

**Fase 1 -- mecânica C++ multi-anexo** (`model.hpp`: novos `ReferenceInfo`/`ConnectionInfo`/
`LineInfo` + `RiserModel::resolve_line_attachments()`; `simulation.cpp`/`dynamic_analysis.cpp`/
`static_analysis.cpp`: os 3 pontos hardcoded em `nodes.front()` como "o" nó de topo viraram loops
por linha). Achado no caminho: plumbing profundo do solver (numeração de equação RCM,
`compute_rcm_order()`) já era 100% genérico pra componentes desconexos -- zero mudança necessária
ali, só ganhou teste dedicado. Achado um bug de correção física real (não relacionado à
numeração): `compute_stress_and_curvature()`'s vizinho prev/next (usado pra curvatura/momento/von
Mises/MBR) era escolhido só por posição no array (`elements[i-1]`/`[i+1]`), não por adjacência
topológica real -- com múltiplas linhas, elementos de linhas diferentes ficam lado a lado no array
plano sem se tocar; corrigido pra checar `node2==node1` de verdade antes de usar como vizinho (4
pontos: `capture_snapshot()`, `solve_vessel_offset()`, `dynamic_analysis.cpp`, todos compartilhando
a mesma lógica antes errada). Novo `tests/test_multiline.cpp`: duas correntes catenárias
totalmente desconexas resolvidas juntas batem, ponto a ponto, contra cada uma resolvida sozinha
(prova zero cross-talk); modelo sem `lines[]` continua com comportamento idêntico ao de sempre.

**Fase 2 -- parsing JSON no `ModelBuilder`** (`model_builder.cpp`): resolução de ID generalizada
de índice-direto-no-array (`node1_id - 1` como posição) pra mapa `id -> Node3D*` (mesmo padrão já
usado pelo bloco de warm-start) -- necessário pra IDs globalmente únicos mas não densos entre
linhas. Extraída `parse_vessel_motion_config()` (antes inline só em `environmental.vessel_motion`)
pra ficar reutilizável também em `references[].vessel_motion`. Fixture JSON de 2 linhas parseia e
converge; fixture single-line (sem `lines[]`) prova retrocompatibilidade total.

**Fase 3 -- síntese multi-linha em `aml_reader.py`, testada contra o `Multilinhas.aml` real**:
`_extract_all()`/`_parse_floating_ships()`/`_parse_connections()` já parseavam tudo multi-entrada
(achado surpreendente -- não começava do zero); o gargalo estava só downstream
(`to_risersim_json(line_index=0)` sempre resolvia UMA linha). Novo `to_risersim_json_multiline()`
(`tools/aml_reader.py`) + `build_config_from_aml_multiline()` (`risersim_runner.py`) +
`run_from_aml.py --all-lines`. Achado e resolvido no caminho: a correção de malha via MoorPy
(`_apply_moorpy_mesh_correction()`, corrige a corda reta pra geometria de catenária real --
sem ela `L_unstretched` sai curto e a tração estática fica várias vezes maior que a real, mesmo
bug já documentado no Eixo 2a) assume que `model.elements` forma UMA corrente contínua
(`node_order = [elements[0].node1_id] + [e.node2_id for e in elements]`) -- quebraria correndo
através de fronteiras entre linhas no array plano mesclado. Resolvido aplicando a correção POR
LINHA (`ANFLEXAMLReader._moorpy_correct_line_mesh()`, nova, roda a mesma lógica MoorPy numa
fatia JSON de uma linha só, isolada, dentro do loop) em vez de tentar ensinar a ferramenta
existente sobre fronteiras de linha.

**Verificação real** (2026-08-16): `exemplos/Multilinhas/Multilinhas.aml` real, via `run_from_aml.py
--all-lines --static-only`: as 7 linhas resolvem conexão+catenária reais (moorpy) com sucesso,
schema gerado estruturalmente correto (14497 nós/14490 elementos, IDs globalmente únicos e sem
sobreposição entre linhas, confirmado inspecionando o JSON gerado -- linha N vai de node_id
X a Y sem invadir a faixa da linha seguinte), `references`/`connections`/`lines` corretos (1
corpo flutuante compartilhado, 7 conexões de topo + 7 de âncora). H5 exportado sem crash mesmo
no modelo grande.

**Achado inicial: a estática não convergia** -- resíduo divergia já no 1º passo de carga. Isolado
via teste A/B: rodando a MESMA linha 1 sozinha pelo caminho single-line já existente e não tocado
(`to_risersim_json()`, sem nenhuma mudança desta rodada) reproduz a *mesma* divergência -- prova
que não era um bug do multi-linha, e sim algo pré-existente no motor pra esse tipo de geometria
(águas muito profundas, 2200m vs. 100-265m dos exemplos já validados; linha muito longa, 5265m;
EI muito baixo, 6,27 kN·m²; malha com ~2070 elementos/linha, bem além de qualquer caso já testado).

**✅ Investigado e resolvido, sem mudança de C++** (2026-08-16): instrumentação de debug temporária
(removida depois) achou a causa real -- não é o "chattering" de contato/atrito já documentado (o
DOF de maior resíduo, no trecho onde a explosão começa, é sempre uma ROTAÇÃO, em nós longe do
leito, com atrito zerado). É mal-condicionamento: EI tão baixo com malha tão fina deixa a rigidez
rotacional quase singular, e o passo de Newton cheio "chuta" a rotação longe demais. O mecanismo
que resolve já existia (`StaticAnalysis::enable_step_limiting`, desligado por padrão) -- só
precisava de calibração: cap de rotação bem mais apertado que seu próprio default (0,01 rad vs.
0,3 rad) elimina a explosão por completo; a tolerância de força/momento desbalanceados (usada como
"escape hatch" perto do limite de iteração, ver `convergence_test.cpp`) também precisou afrouxar
(5→30 N) pra não ficar perseguindo um resíduo já fisicamente desprezível por centenas de
iterações. Confirmado convergindo as 7 linhas individualmente E o modelo combinado (7 linhas, 11
passos de carga cada) -- todas batendo no mesmo `T_eff=2361 kN` (forte prova de consistência
física, já que as 7 linhas são geometricamente equivalentes, só giradas). Testado também que essas
configurações são inofensivas num caso já validado (Exemplo_01a/Cross, EI=21,7 kN·m²): T_eff
idêntico ao baseline (217,3 kN), convergindo em MENOS iterações -- ou seja, o gatilho da
auto-detecção não precisa ser cirúrgico, só separar os dois pontos reais já encontrados.

**Implementado**: `ANFLEXAMLReader._static_robustness_overrides(ei_nm2)` (`tools/aml_reader.py`),
chamado tanto em `to_risersim_json()` quanto em `to_risersim_json_multiline()` -- se
`EI < 15.000 N·m²` (limiar entre os dois pontos reais, 6.270 instável / 21.700 inofensivo), liga
`enable_step_limiting`+cap 0,01 rad, afrouxa `unbalanced_force_tol`/`moment_tol` pra 30 N, e sobe
`max_iterations` pro maior entre o valor real do `.aml` e 400 (cobre o pior caso observado, o
passo 1 partindo da malha reta inicial, que precisou de 387). Zero mudança em C++/`model.hpp`
(os defaults do mecanismo continuam os mesmos pra todo o resto). Reverificado ponta a ponta pelo
pipeline real (`run_from_aml.py --all-lines`, sem nenhuma edição manual de JSON): converge os 11
passos, mesmo `T_eff=2361 kN` de antes; `Exemplo_01a` via caminho `.aml` puro confirmado SEM
disparar a auto-detecção (EI=21,7 kN·m² > limiar), comportamento idêntico ao de sempre. Catch2:
405/405 (mudança só em Python).

**Fora de escopo desta rodada** (confirmado com o usuário antes de começar): frontend (`Riser3DRenderer.js`/`DataLoaderService.js`/tabela de resultados -- hoje assumem uma corrente única contígua, precisam de conectividade real exportada no H5 primeiro); seleção de linha no gerenciador de rodadas web (`risersim_projects.py`/`run_server.py`, sem conceito de "linha" hoje); caminho XML+H5 (`xml_h5_reader.py`, sem export real multi-linha existente).

### 2d. Suporte a múltiplos tipos de elemento — ✅ CONCLUÍDO (Fases 0-4, todas implementadas)

Usuário perguntou como o `risersim` está em relação aos tipos de elemento reais do ANFLEX
(2026-08-18). Levantamento (3 agentes Explore) achou ~9 famílias reais (`cBeam` + 5 variantes,
`cTruss`/`cWinch`, `cScalar` [genérico, também usado como flexjoint], `cContactElement`,
`cBuoyElement`, `cRigidBodyElement`), todas realmente parseadas por `cModelBuilderDAT`. O risersim
implementava só UM tipo (`CorotationalBeam3D`), hardcoded incondicionalmente em
`RiserModel::add_element()`. Cruzando contra os 31 `.aml` reais deste repo (mesmo critério "sob
demanda" já usado no resto do backlog): `cScalar` (6 arquivos, inclusive como o ANFLEX real
implementa flexjoint) e `cTruss` (7 arquivos) são os únicos com uso real repetido; `cWinch` (4) e
`cBuoyElement` (3) secundários; `cRigidBodyElement`/`cContactElement`/variantes de viga têm **zero**
ocorrências em qualquer `.aml` deste repo -- fora de escopo, não implementar especulativamente.
Plano completo (4 fases) em `C:\Users\fcgom\.claude\plans\cozy-cooking-kazoo.md`.

**Fase 0 (arquitetura, pré-requisito) -- implementada**: `Element` (`element.hpp`) já era uma
interface polimórfica de verdade (`num_nodes()`/`node()`/`assemble()`/`mass_matrix()`, os dois
loops de montagem genéricos já passavam por ela) -- o bloqueio real era `RiserModel` armazenar
`vector<unique_ptr<CorotationalBeam3D>>` e `add_element()` hardcodear esse tipo. Virou
`vector<unique_ptr<Element>>` + `add_beam_element()`/`add_scalar_element()` por tipo. `Element`
ganhou 2 hooks novos com default neutro (`update_effective_tension()` retorna 0.0,
`characteristic_stiffness()` retorna 0.0) -- únicos genuinamente reutilizáveis por qualquer tipo
futuro. Os ~8 pontos de código que ainda chamavam membros específicos de `CorotationalBeam3D` fora
da interface genérica (peso/empuxo/corrente em `static_integrator.cpp`/`static_analysis.cpp`/
`dynamic_analysis.cpp`, rigidez de empuxo/`characteristic_stiffness` em `analysis.cpp`, resultados
por elemento em `capture_snapshot()`/snapshot dinâmico) agora fazem `dynamic_cast<CorotationalBeam3D*>`
e pulam elementos que não sejam viga -- fisicamente correto (um conector/mola não tem peso próprio
nem empuxo nem tração no sentido axial de uma viga, igual ao `cScalar` real, que não tem
`calc_load`). Resultados por elemento (tração/momento/curvatura/von Mises/MBR) de um elemento
não-viga viram placeholder zero (mantém os arrays alinhados por índice com a conectividade do H5) --
renderização de resultado pra tipos não-viga fica como trabalho futuro documentado, não quebrado
silenciosamente. `rcm_reorder.hpp` (também usava `node1()`/`node2()` direto) generalizado para
`num_nodes()`/`node(i)`, conectando todos os pares de nós de um elemento (idêntico ao caso de 2 nós
de hoje, funciona pra qualquer contagem futura). Bindings Python (`bindings.cpp`): `.elements`
continua expondo só vigas (filtro `dynamic_cast`), decisão de expor outros tipos adiada até um
consumidor real precisar. **Verificação**: 405/405 (Catch2) idêntico, T_eff do caso Near
(Exemplo_01c) idêntico ao valor pré-refactor (217,1 kN) -- confirma refactor puro, zero mudança de
comportamento pro caminho beam-only.

**Fase 1 (`ScalarElement`, mola/flexjoint genérico 6-GDL) -- implementada**: novo
`include/risersim/curve_function.hpp` (`PiecewiseLinearCurve`, curva linear-por-partes com valor +
derivada, cobre tanto mola linear simples quanto uma curva realmente não-linear como a válvula
ESDV real) e `include/risersim/element_scalar.hpp` (`ScalarElement`). Simplificação deliberada em
relação ao `cScalar` real: em vez do referencial local por 3 pontos auxiliares do `.DAT` real
(schema JSON do risersim não tem esse conceito ainda), usa
`CorotationalBeam3D::build_frame_from_chord()` na corda inicial do próprio elemento, fixo na
construção -- reaproveita a mesma convenção de eixo já verificada contra dado real do ANFLEX nesta
sessão, em vez de inventar uma segunda. Deslocamento relativo translacional via projeção direta;
rotação relativa via composição própria de rotações (`rodrigues`/`AngleAxisd`, não subtração ingênua
de vetores) -- ver docstring da classe pra derivação completa do padrão "mola entre dois pontos"
(mesmo `K_global = T^T*K_local*T` que a viga já usa). Sem massa (`mass_matrix()` sempre zero,
igual ao `cScalar` real) e sem carga hidrodinâmica (excluído pelo filtro `dynamic_cast` da Fase 0,
fisicamente correto). `ModelBuilder::load_from_json()` ganhou dispatch por `element_type` (default
`"beam"`, retrocompatível) e um `scalar_properties` JSON (`stiffness_x/y/z/rx/ry/rz`, mola linear
por GDL -- curva genuinamente não-linear via JSON fica pra quando um caso real precisar). Testes
novos em `test_element.cpp` (4 casos: `node()`/`num_nodes()`, repouso força-zero, alongamento axial
reproduzindo o par de força clássico de mola de 2 pontos com sinal conferido, massa sempre zero).
**Verificação**: 585/585 (Catch2, 405 antigos + 180 novos), ALL_BUILD limpo (`risersim_test_main`,
`risersim_tests`, módulo pybind), T_eff do Near reconfirmado idêntico após a Fase 1 também.

**Fase 2 (`TrussElement`, cabo/amarração 3-GDL/nó) -- implementada (2026-08-18)**: novo
`include/risersim/element_truss.hpp`. Réplica direta de `cTruss::calc_stiff_mt`/
`calc_internal_forces` (`truss.cpp:142-352`): `axial_force = strain*E*A + initial_tension`,
`strain = (L_deformado - L_referencia)/L_referencia`, rigidez `K3 = (E*A/L_ref)*cos⊗cos +
(axial_force/L_ref)*I` em blocos `[[K3,-K3],[-K3,K3]]`. Decisão confirmada com o usuário antes desta
rodada (plano em `cozy-cooking-kazoo.md`): **sem clamp de compressão**, réplica exata do `cTruss`
real (que é uma barra bidirecional normal apesar do nome). Como todo `Element` monta num layout
12x12/12x1 fixo (6 GDL/nó, ver `element.hpp`), as linhas/colunas rotacionais (3-5, 9-11) ficam
zero -- reflete fielmente que `cTruss` real não tem GDL rotacional nenhum (`m_num_dof=3`).
Contato com o solo continua funcionando (esse loop é por NÓ, não por tipo de elemento). Massa: mass
matrix concentrada simples (`rho*A*L_ref/2` por nó, só translação -- mesmo termo líder de
`cBar::calc_mass_vector`, sem fluido interno/massa adicionada hidrodinâmica). 5 testes novos em
`test_element.cpp`.

**Peso próprio/empuxo/corrente pra `TrussElement`/`WinchElement` -- implementado (2026-08-18,
rodada seguinte)**: era a lacuna documentada acima ("Simplificação deliberada, MAIOR que a do
`ScalarElement`") -- diferente de `cScalar` (que também não tem essas cargas no ANFLEX real), o
`cTruss` real HERDA `cBar::calc_weight_load` (classificado como carga EXTERNA no real, mesma
categoria de `calc_load`, não força interna -- por isso a implementação segue o padrão já usado
pra `CorotationalBeam3D`, não o padrão "força interna" usado pro `BuoyElement`). `TrussProps` ganhou
`D_outer` (default 0.0 = sem envelope hidrostático/arrasto, retrocompatível com qualquer JSON
anterior a este campo). Réplica direta do loop peso/empuxo/corrente já usado pra viga (`w_dry =
rho*A*g`, `Hydrostatics(D_outer, L, water_density)` pra empuxo escalado por submersão + rigidez
tangente correspondente, arrasto de corrente via `CurrentProfile` -- que já tem seu próprio Cd
global de modelo, não precisou de um `Cd` por elemento), adicionado em 4 pontos
(`static_integrator.cpp::assemble_load_vector`, `static_analysis.cpp` x2, `dynamic_analysis.cpp`)
+ `Analysis::assemble_buoyancy_stiffness()`. `WinchElement` ganha de graça (os loops casam em
`TrussElement*`, que `WinchElement` também é). AINDA fora: arrasto/inércia de onda (Morison) --
mesma lacuna documentada do `BuoyElement`, genuinamente mais complexo, sem caso real validado ainda.
2 testes novos em `test_static_analysis.cpp` (peso/empuxo via `assemble_load_vector`, rigidez via
`assemble_buoyancy_stiffness`), verificados recomputando com a mesma classe `Hydrostatics` usada
pela implementação. **Nota sobre retrocompatibilidade**: só empuxo e arrasto de corrente são
gateados por `D_outer` (zero por padrão = sem efeito, JSON antigo sem esse campo fica exatamente
como antes NESSAS duas parcelas). O PESO SECO (`rho*A*g`) não é gateado -- é incondicional, igual
ao `cTruss` real -- então qualquer elemento `truss`/`winch` já existente que configure `rho`/`A`
(mesmo sem `D_outer`) passa a ter peso próprio agora, onde antes não tinha nenhum. Mudança de
comportamento intencional (era exatamente a lacuna sendo fechada), não uma regressão -- mas
modelos reais com `truss`/`winch` regenerados antes desta rodada podem mostrar uma leve mudança de
forma (a linha agora cai sob o próprio peso) na próxima vez que forem resolvidos.

**Fase 3 (`WinchElement`, extensão pequena da Fase 2) -- implementada (2026-08-18)**: novo
`include/risersim/element_winch.hpp`, deriva de `TrussElement` e sobrescreve só
`reference_length()` (comprimento de referência não-esticado passa a ser
`initial_length()*payout_fraction(tempo)`, via uma curva `PiecewiseLinearCurve` -- default
`constant(1.0)`, comportamento idêntico a um Truss simples se nenhuma curva for configurada).
Simplificação em relação ao `cWinch` real: uma curva só (real tem `static_function`/
`dynamic_function` separadas e compostas, `winch.cpp:124-153`) -- cobre o caso comum (recolher até
um comprimento alvo, depois manter), recolhimento ativo DURANTE uma simulação dinâmica fica pra
quando um caso real precisar. **Achado de arquitetura**: nem `Element::update_effective_tension()`
nem `Analysis::assemble_system()` tinham como saber o "tempo atual" -- não existia esse conceito no
laço de montagem genérico. Resolvido com um hook novo, mesmo padrão da Fase 0
(`Element::set_time(double)`, default no-op) + um campo público `Analysis::current_time` (default
0.0, zero mudança de comportamento pra quem nunca configura) que `StaticAnalysis` seta pra fração
do passo de carga `t` (mesma convenção já usada pra rampa de corrente) e `DynamicAnalysis` seta pro
tempo real decorrido, antes de cada `assemble_system()`. 2 testes novos em `test_element.cpp`.

**Verificação (Fases 2+3)**: 707/707 assertions (Catch2, 585 antigos + 122 novos), ALL_BUILD limpo
(`risersim_test_main`, `risersim_tests`, módulo pybind, `risersim_diag_isolated_segment`) --
inclusive o próprio teste de convergência estática completo (1339 iterações, mesmo resultado de
antes), forte evidência de zero regressão no caminho beam-only (nada em `CorotationalBeam3D` foi
tocado). `ModelBuilder::load_from_json()` ganhou `element_type: "truss"`/`"winch"` +
`truss_properties`/`winch_properties` (`E`/`A`/`rho`/`initial_tension`, e pro winch um
`payout_curve: {"time":[...], "fraction":[...]}` opcional).

**Fase 4 (`BuoyElement`, elemento de 1 nó) -- implementada (2026-08-18)**: novo
`include/risersim/element_buoy.hpp`. Achado central que resolveu a preocupação original do plano
("precisa de ajuste nos hooks da Fase 0/2"): o `cBuoyElement` real classifica sua rigidez/força
hidrostática restauradora (submersão da linha d'água) como **INTERNA**
(`calc_stiff_mt`/`calc_internal_forces`, mesmo grupo de métodos puros virtuais que o `cTruss` usa
pra sua própria força elástica) -- não como carga externa. Isso significa que essa parte encaixa
direto em `Element::assemble()`/`mass_matrix()` sem nenhum hook novo, desde que `water_surface_z`
seja capturado uma vez na construção (o nível d'água não muda ao longo de uma rodada do risersim
hoje -- elevação de onda ainda não alimenta hidrostática por elemento em lugar nenhum, mesma lacuna
já documentada pra viga). Fórmulas portadas sinal-a-sinal do real (`buoy_element.cpp:143-245`), com
uma verificação de sinal via equilíbrio físico direto que INICIALMENTE sugeriu um sinal diferente do
que o real usa -- descartada em favor do porte literal depois que o mesmo tipo de checagem, aplicada
ao `TrussElement` (já validado, testes passando), se mostrou não-confiável isoladamente. `Kzz`/`Krot`
usam `rho_water*g*área` explícito (convenção de densidade de massa do risersim, mesmo padrão de
`Hydrostatics`/`assemble_buoyancy_stiffness()`) em vez do `área*m_water_density` cru do real (que,
sem dividir por g, só faz sentido fisicamente se o `m_water_density` real ali já for densidade de
PESO, não de massa -- mesma formula final, só a convenção de unidade traduzida).

**Simplificações deliberadas**: referencial local FIXO (=identidade, sem rastrear orientação da
boia -- real atualiza `m_transf_matrix` a cada iteração quando `num_dofs>3`); SEM peso próprio nem
Morison/arrasto/onda (`calc_load`'s termo dominante além do peso) -- peso é adicionado como carga
EXTERNA (não cabe em `assemble()`, que só vê o próprio nó), via o mesmo padrão
`dynamic_cast<BuoyElement*>` já usado pro peso de viga, em 4 pontos (`static_integrator.cpp`,
`static_analysis.cpp` x2, `dynamic_analysis.cpp`); Morison/onda/corrente fica de fora, mesma lacuna
documentada do Truss/Winch (genuinamente complexo, precisa de cinemática de onda que o risersim
ainda não consome plenamente nem pra viga). Massa: `mass_matrix()` = massa estrutural
(`weight/g`) + massa adicionada (`rho_water*volume*Ca` por eixo) + inércia rotacional estrutural
configurável -- direto de `calc_mass_vector`, sem a rotação pelo referencial (que aqui é identidade).
`ModelBuilder` ganhou `element_type: "buoy"` (schema de nó único, `node_id` em vez de
`node1_id`/`node2_id`) + `buoy_properties` (`weight`/`volume`/`z_area`/`Ca`/`moment_inertia`) --
construído DEPOIS da seção de parâmetros ambientais (não junto com viga/scalar/truss/winch), já que
precisa de `water_surface_z` resolvido. 8 testes novos em `test_element.cpp`.

**Verificação (Fase 4)**: 734/734 assertions (707 antigos + 27 novos), ALL_BUILD limpo (inclusive
módulo pybind e `risersim_diag_isolated_segment`) -- o teste de convergência estática completo
(1339 iterações) reproduziu resultado idêntico de novo, zero regressão.

**Plano de 4 fases concluído.** Peso/empuxo/corrente pra `TrussElement`/`WinchElement` (a lacuna da
Fase 2) foi fechada na rodada seguinte, mesmo dia -- ver o parágrafo "Peso próprio/empuxo/corrente
pra `TrussElement`/`WinchElement` -- implementado" logo após a Fase 2 acima. Lacunas conhecidas
remanescentes: Morison/onda/corrente pra `BuoyElement` (Fase 4) e nenhuma integração JSON-round-trip
end-to-end pra `ScalarElement`/`TrussElement`/`WinchElement`/`BuoyElement` ainda (só testes
unitários da fórmula) -- nenhuma bloqueante, ambas documentadas por tipo acima.
`cRigidBodyElement`/`cContactElement`/variantes alternativas de viga seguem fora de escopo (zero uso
real neste repo).

## Eixo 3 — Interfaces (podem começar em paralelo aos eixos 1-2, escopadas ao que o motor já suporta)

### 3a. Interface de entrada de dados — ✅ v1 IMPLEMENTADA (2026-08-17)

Arquitetura original (dois JSONs, compilador JS no navegador) **revisada** com o usuário antes de
implementar: em vez de "uma linha só, referências por nome, compilador em JS", o desenho final
(schema v3, ver `docs/mapa_aml_exemplos_e_web_interface.md` se quiser o histórico da discussão)
ficou:

- **IDs em vez de nomes** — todo objeto (solo/material/corrente/onda/linha/segmento/caso de
  carga/análise) tem um `id` inteiro; referências cruzadas usam esse id, espelhando o `.aml` real
  (`%SOIL.ID`, `%MATERIAL.ID`, etc.).
- **Solo por segmento**, não por linha — `segments[].soil_id`, como `%LINE.SEGMENT.SOIL_ID` real.
- **Múltiplas linhas/correntes/ondas/análises/casos de carga** desde o v1 — o motor já suportava
  isso (`references[]`/`connections[]`/`lines[]`, Eixo 2c), não fazia sentido restringir a uma de
  cada. Cada linha continua com o topo fixo no espaço (sem corpo flutuante compartilhado — RAO/
  movimento de topo real fica pra uma fase 2, decisão confirmada).
- **Análise → lista de casos de carga** (não o contrário): uma configuração de solver
  (`analyses[].load_case_ids`) se aplica a vários cenários de corrente/onda — uma rodada é o par
  `(analysis_id, load_case_id)`.
- **Compilador único no backend (Python)**, não JS no navegador — decisão explícita: a geometria
  de catenária (achar o vão dado o ângulo+comprimento) só tem solução validada via MoorPy em
  Python (`solve_catenary_geometry()`, já usado pelo caminho AML), reimplementar em JS seria
  trabalho novo e arriscado. **AML reroteado**: `aml_reader.py` para de produzir a JSON de
  simulação diretamente — agora produz `to_interface_json()` (a JSON de interface), e o mesmo
  compilador (`risersim_runner.py::build_config_from_interface()`) atende tanto projetos
  importados de `.aml` quanto projetos criados do zero pelo editor. XML+H5 continua indo direto
  pra JSON de simulação (malha já resolvida, sem etapa de catenária pra deduplicar).

**Implementado**:
- `tools/catenary_geometry.py` (novo) — `solve_catenary_geometry`/`correct_line_mesh_via_moorpy`/
  `synthesize_mesh`/`compute_rayleigh_alpha`/`compute_rayleigh_beta`/`static_robustness_overrides`/
  `build_section_properties`, extraídos de `aml_reader.py` (refactor puro).
- `aml_reader.py::to_interface_json()` — traduz o `.aml` pro schema de interface, preservando IDs
  reais; não resolve geometria (isso migrou pro compilador).
- `risersim_runner.py::build_config_from_interface()`/`list_interface_runs()` — compilador único;
  `build_config_from_aml()`/`build_config_from_aml_multiline()` viraram wrappers finos sobre ele.
- Backend (`risersim_projects.py`/`run_server.py`): `create_blank_project`/`update_interface`,
  `create_run(analysis_id, load_case_id)` (com fallback de compatibilidade pra quem só passa
  `load_case_id`), rotas novas (`POST /api/projects/blank`, `GET`/`PUT /api/projects/<id>/interface`,
  `GET /api/projects/<id>/runs-catalog`). Todo projeto (inclusive os vindos de `.aml`) ganha um
  `source/interface.json` auto-derivado na criação — só projetos "em branco"
  (`source.interface_editable`) aceitam `PUT`.
- Frontend: `tools/web/editor.html`/`js/editor_app.js` (novo) — formulário de 8 abas (Site/Solos/
  Materiais/Correntes/Ondas/Linhas+Segmentos/Casos de Carga/Análises), tabelas editáveis genéricas
  (`renderEditableTable()`) pros 6 catálogos + segmentos de cada linha. `project.html`/
  `preprocessor.html` ganharam o seletor de duas etapas (análise → caso de carga, só um dos dois
  reaproveitado quando há mais de uma combinação — mesmo comportamento condicional de antes) e uma
  4ª aba "✏️ Editar" (só visível pra projetos `interface_editable`). `dashboard.js` ganhou a aba
  "✏️ Em branco" no modal de novo projeto, com um modelo default mínimo (1 solo/material/linha/
  análise, geometria conservadora o bastante pra convergir de cara).

**Verificação real**: regressão completa do reroteamento AML→interface→simulação contra os 4 casos
do Exemplo_01a (Near/Far/Transverse/Cross) — `T_eff` byte-idêntico ao baseline (217.2/220/219.6/
213.9 kN) nos dois caminhos (`ANFLEXAMLReader` direto e via `risersim_projects.py`). Teste de
múltiplas linhas + múltiplas análises com JSON de interface escrita à mão (2 linhas independentes,
2 análises cobrindo 1 e 2 casos de carga respectivamente) — as 3 combinações convergem, linha 1
bate o baseline exato, linha 2 (geometria diferente) converge com `T_eff` distinto e fisicamente
coerente. Backend testado via `ProjectStore` puro (blank + AML-derivado, incluindo o bloqueio de
`update_interface` num projeto não-editável) e via servidor real (`run_server.py` local + Chrome
headless `--dump-dom`): projeto "em branco" real criado, `editor.html` carrega e popula as 8 abas
com dados reais sem erro de console; projeto vindo de exemplo real mostra a aba "Editar" oculta e o
seletor de duas etapas com as 4 combinações certas (Near/Far/Transverse/Cross sob a análise "A1").
Bug real achado e corrigido nesse processo: segmentos vindos de uma JSON de interface derivada de
AML não têm `id` próprio (só a linha tem) — quebrava o botão de apagar segmento no editor
(`data-row-id="undefined"`); corrigido com backfill defensivo de id antes de renderizar. Catch2:
405/405 sem regressão (nenhum arquivo C++ tocado em todo este eixo).

**Atualização 1** (preview 3D ao vivo, pedido pelo usuário — deixa de ser "fora de escopo"):
painel 3D na aba "Linhas" (`Riser3DRenderer`/`CameraViewController`, os mesmos já usados pelo
`preprocessor.html`, reaproveitados sem alteração), gatilho decidido com o usuário: só aparece
depois que existe pelo menos uma linha; mostra imediatamente mar+solo+bounding box (calculados só
a partir do topo da linha e `global.water_depth_m`, sem round-trip) e, com debounce de 600ms após
parar de editar, a malha real via novo endpoint stateless `POST /api/interface/preview`
(`build_config_from_interface()`, nada salvo em disco — seguro chamar a cada edição). Erros de
compilação (ex. `material_id` ainda não criado) aparecem inline, sem interromper a digitação.
Verificado via screenshot real (Chrome headless `--screenshot`, servidor local): caixa aparece
instantânea, depois a catenária real (25° da vertical) substitui o placeholder.

**Atualização 2** (layout redesenhado -- "a cara do pós-processador, só que editável", pedido
explícito do usuário): o preview 3D deixou de ser um painel dentro da aba "Linhas" e virou o
mesmo esqueleto de 3 regiões do `preprocessor.html`/`posprocessor.html` (`css/shared.css` --
sidebar de câmera/zoom | canvas 3D | painel com abas), espelhado: painel de dados (as 8 abas do
formulário) na ESQUERDA, sidebar de câmera + canvas 3D sempre visível na DIREITA -- editar o
modelo agora tem o desenho inteiro ao lado, em qualquer aba, não só em "Linhas". Mudanças reais:
- `editor_app.js` monta um `Riser3DRenderer`/`CameraViewController`/`ZoomWindowController`
  persistentes desde o início (não mais lazy, já que o canvas nunca fica oculto), reaproveitando
  `bindCameraToolbar()`/`initThemeToggle()` exatamente como as outras duas páginas.
- O preview passou a mostrar o MODELO INTEIRO (todas as linhas) em qualquer aba -- editar um
  material ou solo pode mudar a geometria (peso entra na equação da catenária) tanto quanto editar
  uma linha, então não fazia sentido restringir a visualização a uma aba.
- Reframe da câmera passou a ser explícito (só no load inicial e ao adicionar/apagar linha), não
  mais em toda edição -- com o canvas sempre visível, reenquadrar a cada tecla digitada faria a
  câmera "pular" enquanto o usuário orbita a cena pra inspecionar algo.
- Drag-resize do painel precisou de lógica própria (`initReversedPanelResizer()`), já que
  `ui/PanelResizer.js` assume o painel de dados à DIREITA do resizer (como no pré/pós-processador)
  -- aqui está à esquerda, então o sinal do delta de arraste é invertido.
- Bordas do `#data-panel`/`#sidebar` (herdadas de `shared.css`, pensadas pro arranjo original)
  ajustadas via `editor.css` pro espelhamento; legenda de diâmetro (`#colorbar-legend` e
  vizinhos) ganhou o mesmo dimensionamento 140px do pré-processador (`shared.css` deixa isso
  deliberadamente de fora, por página).
Verificado via Chrome headless `--screenshot` (servidor local, projeto "em branco" real): canvas
3D com bounding box + catenária real visível simultaneamente nas abas "Site", "Materiais" e
"Linhas" -- confirma que o preview não é mais exclusivo de uma aba.

**Atualização 3** (protótipo de grid tipo planilha, pedido do usuário -- colar de Excel/Sheets +
unidade configurável por coluna): a aba "Materiais" trocou o `renderEditableTable()` genérico por
**Tabulator** (MIT, `tabulator-tables@5.5.2` via CDN, mesmo padrão de dependência que Three.js/
Plotly já usam) -- só essa aba, como protótipo antes de decidir se vale espalhar pras outras
(Solos/Correntes/Ondas/Casos de Carga/Análises continuam no editable-table de sempre).
- **Colar de planilha**: módulo de clipboard nativo do Tabulator (`clipboard: true,
  clipboardPasteAction: "range"`) -- selecionar a célula de destino e Ctrl+V com dados copiados do
  Excel/Sheets funciona sem código customizado.
- **Unidade por coluna**: 5 colunas (diâmetro externo/interno, peso seco/molhado, EA) ganharam um
  seletor de unidade embutido no próprio cabeçalho (`unitColumn()`, m/mm/in/ft e kN·kgf·lbf·tf
  conforme o grupo) -- o modelo (`this.model.materials`) continua em SI internamente, só o
  formatter/editor da célula convertem na exibição/edição; trocar a unidade só re-renderiza
  (`table.redraw(true)`), não muda o dado guardado.
- **Bug de layout achado e corrigido**: Tabulator mede a largura do contêiner na hora de montar a
  grade -- como isso acontece durante `renderAll()` (aba "Site" ainda ativa, painel "Materiais"
  com `display:none`), a tabela nascia com largura zero e nunca se recuperava sozinha ao trocar de
  aba (mesma classe de bug que os gráficos Plotly já tinham no pré-processador). Corrigido com
  `switchToTab()` chamando `table.redraw(true)` sempre que a aba vira "materials".
- Tema (cores dark/light do Tabulator) mapeado pros tokens `--surface`/`--border`/`--accent` etc.
  já existentes, em vez de carregar o CSS do tema padrão da biblioteca.
Verificado via Chrome headless `--screenshot` + `--dump-dom` (servidor local): grid renderiza com
os seletores de unidade certos no cabeçalho, sem "undefined"/"NaN" vazando pras células, e sobrevive
a ser carregado direto na aba "Materiais" via `?tab=materials` (o cenário que expõe o bug de
largura zero, se o fix não estivesse funcionando). Colar de verdade a partir de uma planilha real
não foi testado neste ambiente (sem acesso a clipboard real em Chrome headless) -- fica pra
confirmação manual do usuário.

**Atualização 4** (generalização do grid tipo planilha -- discussão de risco com o usuário sobre
usar o Tabulator "pronto" vs. caseiro, decidida a favor do Tabulator especificamente pela
virtualização: as tabelas de catálogo podem crescer a centenas/milhares de linhas, e renderizar
tudo de uma vez em HTML puro -- o que o `renderEditableTable()` antigo fazia -- engasgaria bem
antes disso; escrever virtualização de scroll caseira do zero foi julgado um projeto arriscado por
si só, então a decisão foi manter uma biblioteca madura pra essa parte específica em vez de
reinventá-la). Extraído `tools/web/js/ui/SpreadsheetTable.js` -- `createSpreadsheetTable(container,
items, columns, {onDelete, onChange})`, com tipos de coluna unificados (`text`/`number`/`unit`/
`select`/`checkbox`/`list`) e coluna de ID/apagar automáticas. **Todas** as abas de catálogo (Solos/
Materiais/Correntes/Ondas/Casos de Carga/Análises) e a tabela de Segmentos de cada linha migraram
pra esse componente -- `renderEditableTable()` foi removido. Duas melhorias generalizadas nessa
migração:
- **Fix de largura zero virou genérico**: em vez do `switchToTab()` checando `if key ===
  'materials'` (como na Atualização 3), `SpreadsheetTable.js` usa um `ResizeObserver` no próprio
  container -- corrige automaticamente qualquer tabela nascida numa aba escondida, não só
  Materiais, sem o `EditorApp` precisar saber qual aba é qual.
- **Refresh entre abas dependentes**: quando um catálogo referenciado (solo/material/corrente/
  onda) é editado ou apagado, a tabela que o referencia (Segmentos ou Casos de Carga) chama
  `.refresh()` pra atualizar os rótulos exibidos nos dropdowns -- consolidado em métodos
  `on*Changed()` por catálogo.
A tabela de Segmentos é a exceção ao padrão "cria uma vez, atualiza local": como está amarrada ao
array `segments` da linha ATIVA (referência diferente a cada troca de linha no seletor), é
destruída e reconstruída em toda chamada de `renderSegments()`, em vez de reapontar uma instância
existente pra um array novo (evita um bug de closure obsoleta que reapontar traria).
Verificado via Chrome headless `--screenshot` em 6 abas (Solos/Correntes/Ondas/Casos de Carga/
Análises/Linhas) carregadas via deep-link direto (`?tab=<x>`, o cenário que expõe o bug de largura
zero) contra um projeto com todos os catálogos populados -- todas renderizam corretamente, os
dropdowns de referência cruzada mostram os rótulos certos (ex. caso de carga "Near" mostrando
"Cor_NE (1)"/"Onda_1 (1)"), sem "undefined"/"NaN". Bug cosmético achado e corrigido: colunas
inteiras (`static.steps`/`static.max_iterations`) mostravam "20.000"/"300.000" com o default de 3
casas decimais -- setado `decimals: 0` nessas duas.

**Atualização 5** (bug real achado pelo usuário no uso real, não em teste): abrindo o editor pela
aba "✏️ Editar" de `project.html` (via `<iframe id="edit-frame">`), o cabeçalho inteiro do
`editor.html` (marca + título "RiserSim - Editor de Modelo" + seu PRÓPRIO botão de tema) renderizava
duplicado por cima do cabeçalho de `project.html` -- `editor_app.js` era a única das quatro páginas
com iframe (pré-processador/pós-processador/editor) que não tinha a checagem `window.self !==
window.top` que esconde o cabeçalho próprio quando embutida (`preprocessor_app.js::initUI()`/
`app.js::initUI()` já tinham). Faltou copiar esse guard ao criar `editor_app.js` do zero nesta
sessão. Corrigido em `initViewport()`, mesmo padrão exato das outras duas páginas. Verificado via
Chrome headless contra `project.html?project=<id>&view=edit` (o caminho real de uso, não
`editor.html` direto) -- um único cabeçalho, um único botão de tema, título do projeto aparece uma
vez só acima das abas do editor.

**Atualização 6** (aba "Linhas" virou duas tabelas, pedido do usuário): o formulário de campos
soltos + um único seletor `<select>` foi substituído por **duas tabelas** no mesmo padrão das
outras abas (`createSpreadsheetTable()`) -- "Linhas" (nome, topo X/Y/Z, ângulo de catenária,
azimute, uma linha por linha) e "Segmentos" (mostrando os segmentos da linha clicada na tabela de
cima). Sem seletor separado nem botão "apagar esta linha": apagar é o mesmo 🗑 por linha que todo
catálogo já tem, sem confirmação (consistente com Solos/Materiais/etc., que também não confirmam).
- **Seleção de linha**: `SpreadsheetTable.js` ganhou suporte a `selectable`/`onRowSelected`
  (Tabulator `selectable: 1` -- no máximo uma linha destacada por vez, estilo rádio) -- clicar
  numa linha troca qual `segments[]` a tabela de baixo mostra.
- **Posição do topo (`top_position_m: [x, y, z]`)**: como é um array, não um objeto plano, a
  resolução por dot-notation do Tabulator não serve -- `SpreadsheetTable.js` ganhou um tipo de
  coluna novo, `type: 'custom'` (`getValue`/`setValue` contra a linha inteira via
  `cell.getData()`), reaproveitável pra qualquer campo futuro que não seja uma propriedade de
  objeto simples.
- **Bug real achado e corrigido, não trivial**: o `background` da linha selecionada usando uma cor
  translúcida (`var(--accent-soft)`, um `rgba()`) deixava o TEXTO da linha invisível -- mas só na
  linha que passou por `table.selectRow()`, nunca em hover nem numa linha comum; reproduzível e
  consistente entre os dois temas, causa raiz não totalmente entendida (possível bug de composição
  do Tabulator/Chrome headless com fundo translúcido numa linha re-renderizada por seleção). Isolado
  por eliminação (testando com CSS desabilitado, com cor sólida hardcoded, sem `box-shadow`) até
  achar que o problema era especificamente a translucidez -- corrigido trocando por
  `color-mix(in srgb, var(--accent) 18%, var(--surface))`, que é opaco. Comentário deixado no CSS
  pra não cair na mesma pegadinha de novo.
Verificado via Chrome headless: seleção inicial (primeira linha automaticamente destacada com
segmentos certos), clique numa linha diferente (simulado via `table.selectRow(id)` programático
através de um iframe de teste) trocando a tabela de segmentos corretamente, `+ Nova linha`
adicionando e selecionando a nova linha, apagar a linha ativa via seu próprio 🗑 escolhendo
corretamente a próxima linha ativa -- tudo confirmado nos dois temas e contra o projeto real do
usuário no Docker.

**Atualização 7** (bounding box "sliver" num riser quase plano, achado pelo usuário testando de
verdade): num riser com pouco/nenhum deslocamento lateral (azimute alinhado, típico de um projeto
recém-criado), o eixo perpendicular da caixa delimitadora ficava um fatia finíssima ao lado dos
outros dois eixos, cheios (~100m+), difícil de orbitar/enquadrar. A causa era estrutural, não um
bug -- `Riser3DRenderer.js::computeSceneBounds()` já tinha uma margem proporcional ao próprio span
desse eixo (`marginPerpendicular`), mas o PISO dela era uma distância fixa (5m), não proporcional
aos OUTROS eixos, então continuava minúscula perto de uma caixa de 100m+. Corrigido com a ideia do
próprio usuário: depois de calcular a caixa, nenhum eixo pode ficar abaixo de 25% do maior eixo --
quem ficar é re-centralizado e alargado até esse piso. Função é **compartilhada** entre editor,
pré- e pós-processador (`Riser3DRenderer.js` não é específico do Eixo 3a), então o fix vale pros
três visualizadores, não só o editor. Verificado via Chrome headless: vista ISO mostra uma caixa
proporcionalmente 3D (não mais uma folha fina) e a vista de topo (XY) continua correta (não
reabriu o problema antigo, já documentado no código, de "caixa enorme e vazia" que a margem
proporcional original tinha resolvido).

**Atualização 8** (segunda linha "desenhada errado", achado pelo usuário testando de verdade --
bug real, não visual): ao adicionar uma segunda linha, o visualizador 3D às vezes desenhava um
segmento diagonal esquisito ligando as duas linhas, e a própria segunda linha aparecia cortada
(faltando o trecho final). Causa raiz: `Riser3DRenderer.js::renderStep()` desenhava cada elemento
ligando `nodes[i]` a `nodes[i+1]` -- ou seja, por POSIÇÃO no array, não pelo `node1_id`/`node2_id`
real do elemento. Isso só coincide com a conectividade real para uma única linha contínua; um
modelo com 2+ linhas concatena as listas de nós/elementos de cada linha (`risersim_runner.py`'s
`node_id_offset`/`elem_id_offset`), e cada linha extra soma mais um nó "inicial" sem elemento
correspondente antes dele -- a partir daí todo elemento fica desalinhado por essa diferença: um
cilindro espúrio conectando a âncora da linha 1 ao topo da linha 2 (o "segmento esquisito"), e o
último elemento real de cada linha nunca chega a ser desenhado (dropado no fim do loop). Com uma
segunda linha bem mais longa (comprimento default de `+ Nova linha`, 500m vs. os ~150m do projeto
do usuário), o efeito fica muito mais visível -- reproduzido isolado com um projeto de teste (linha
1 original + `addLine()` padrão): o trecho apoiado no leito, que deveria se estender por ~350m,
simplesmente não aparecia, restando só aquele cilindro espúrio parecendo (por coincidência de
posição) uma versão reta da linha 1.

Corrigido em duas partes: `Riser3DRenderer.js::renderStep()` agora monta um `Map` id→nó e resolve
`elem.node1_id`/`node2_id` de verdade quando presentes, caindo de volta pra indexação posicional
só quando ausentes (mantém o comportamento de sempre para os resultados JÁ SOLUCIONADOS do
pós-processador -- `DataLoaderService.js`, lendo HDF5 -- que nunca carregam conectividade real, só
valores por elemento ordinal; exportar isso exigiria mudar `SimulationExporter` em C++, fora de
escopo aqui). `editor_app.js::updatePreviewNow()` e `preprocessor_app.js::getSyntheticStep()`
(as duas outras origens de dados não-solucionados, que JÁ têm os ids reais disponíveis) passaram a
repassar `node1_id`/`node2_id` no lugar de descartá-los no `.map()`. Como `Riser3DRenderer.js` é
compartilhado (editor/pré/pós-processador), o pré-processador também se beneficia -- um projeto
AML com múltiplas `%LINE` importado hoje já cairia no mesmo bug, só que ninguém tinha reparado
ainda. Verificado via Chrome headless: reprodução isolada (linha 1 original + `addLine()` padrão)
mostrando a linha 2 completa e corretamente formada; regressão do projeto real do usuário (2 linhas
com azimutes diferentes) idêntica a antes; regressão de um projeto de exemplo real de 1 linha só
(`Exemplo_01a` no pré-processador) sem mudança nenhuma. Docker reconstruído e saudável.

**Atualização 9** (fecha o gap do pós-processador deixado em aberto na Atualização 8): o HDF5
(`SimulationExporter::export_hdf5()`, C++) passou a exportar conectividade real -- `node_ids` (id
real por índice de `node_positions`) e `element_node1_ids`/`element_node2_ids` (par de ids reais por
índice de `element_*`), escritos uma vez na raiz do arquivo (`write_connectivity()`, novo) a partir
de `Analysis::model` (já disponível em `Simulation::export_results()`, nenhum parâmetro novo
precisou ser passado). `DataLoaderService.js::parseHDF5Group()` agora lê esses três datasets (se
existirem -- arquivo mais antigo sem eles cai no fallback posicional de sempre) e repassa pro
`BeamElement3D`/`Node3D` de cada elemento/nó, então `Riser3DRenderer.js::renderStep()` (Atualização
8) resolve por id de verdade também no pós-processador, não só no editor/pré-processador. Verificado
em 3 camadas: `risersim_tests.exe` 405/405 sem regressão; `risersim_test_main.exe` local contra um
modelo de 2 linhas escrito à mão, HDF5 resultante com os 3 datasets novos e valores corretos
(elemento na fronteira entre as duas linhas pula direto pro id real do topo da linha 2, sem o
cilindro fantasma; último elemento da linha 2 presente, não mais dropado); e um projeto real de 2
linhas criado e rodado via Docker (endpoint `/api/projects/blank` + `/api/projects/<id>/runs`, o
mesmo caminho de produção que `run_worker.py` usa) -- HDF5 do worker confirmado com os datasets
novos, e o pós-processador (`posprocessor.html`) desenhando as duas linhas corretamente contra esse
resultado real. Assinatura do `SimulationExporter::export_hdf5()` não mudou (usa
`Analysis::model`, já público) -- o binding Python exposto em `bindings.cpp` (não usado por nenhum
`.py` do repo) continua compilando sem alteração.

**Atualização 10** (backend do pipeline ganha os 4 tipos de elemento novos do Eixo 2d -- só
backend, UI do editor fica pra rodada seguinte, escopo confirmado com o usuário): usuário perguntou
como a interface web acompanha o motor C++ ter ganhado `ScalarElement`/`TrussElement`/
`WinchElement`/`BuoyElement` (Eixo 2d) -- achado: nem o editor nem o compilador Python tinham
qualquer conceito de `element_type`, sempre produziam elementos beam implicitamente, e
`aml_reader.py` já vinha colapsando silenciosamente materiais reais truss/scalar/winch pra um
único beam (confirmado com dados reais, ver abaixo).

- **Schema**: `materials[]` ganhou `finite_element_type` (`beam`/`truss`/`scalar`/`winch`, default
  `beam` -- espelha o próprio `%MATERIAL.FINITE_ELEMENT` real, campo NO material, não uma entidade
  separada). `truss`/`winch` reaproveitam os campos JÁ EXISTENTES de material (diâmetro/peso/
  rigidez axial -- confirmado que `risersim_runner.py::_material_props()` já deriva E/A/rho de
  forma genérica, nada específico de viga) + `initial_tension_kN` novo (só truss). `scalar` ganhou
  3 campos novos -- `scalar_stiffness_translational_kNm`/`_torsional_kNm_per_rad`/
  `_bending_kNm_per_rad` -- **corrigido contra dado real** (`exemplos/Boiao/P52_Boiao.aml`,
  materiais 123-124, bloco `%MATERIAL.FLEXIBLE_JOINT`): um flexjoint real do ANFLEX tem só 3
  valores (translacional isotrópico, torcional no eixo do próprio flexjoint, flexão isotrópica no
  plano transversal), não 6 valores por eixo como uma primeira tentativa (não confirmada contra
  dado real) tinha assumido -- mapeado pro schema de 6 campos do `ScalarProps` C++ como
  translacional→x=y=z, torcional→rx, flexão→ry=rz. Novo catálogo `curves[]` (`{id, name, time_s,
  fraction}`, mesmo padrão id-referenciado de materiais/solos) alimenta `winch_payout_curve_id`
  **no material** (não no segmento -- também corrigido contra dado real,
  `exemplos/Curso/Exemplo_03/Estática/Exemplo_03_E.aml` materiais 713-714,
  `%MATERIAL.TIME_FUNCTION_ID` referenciando um bloco `%TIME_FUNCTION` separado). Novo catálogo
  `buoys[]` (`{id, name, weight_kN, volume_m3, z_area_m2, Ca[3], moment_inertia[3]}`) +
  `segments[].buoy_id` -- suposição de design não verificável contra dado real (ver abaixo): a boia
  se conecta ao ÚLTIMO nó da subcadeia de elementos do segmento.
- **Compilador** (`catenary_geometry.py`/`risersim_runner.py::build_config_from_interface()`):
  4 funções novas espelhando `build_section_properties()` (`build_truss_properties`/
  `build_winch_properties`/`build_scalar_properties`/`build_buoy_properties`); o laço por
  segmento agora despacha por `finite_element_type` do material referenciado em vez de sempre
  montar `section_properties`; um `buoy_id` de segmento vira um elemento `buoy` extra anexado ao
  último nó da subcadeia. `solve_catenary_geometry()`/`correct_line_mesh_via_moorpy()` continuam
  usando só o primeiro segmento (mesma simplificação de sempre) -- ganharam um erro claro se esse
  primeiro segmento for `scalar` (sem rigidez axial/peso pra resolver a forma da catenária).
- **`aml_reader.py`** -- a correção mais trabalhosa: cada categoria de material real usa uma tag de
  ABERTURA diferente (`%MATERIAL.FLEXIBLE_LINE` pra beam/truss/winch, `%MATERIAL.FLEXIBLE_JOINT`
  pra scalar, `%MATERIAL.RIGID_TUBE`/`%MATERIAL.STIFFENER`/`%MATERIAL.STRESS_JOINT` também viram
  beam -- confirmado varrendo `exemplos/` real), então o padrão de "ocorrência múltipla por tag
  fixa" que solos/correntes/ondas já usam não serve -- novo `_scan_material_blocks()` varre o
  arquivo bruto por marcadores `%MATERIAL.END` (mesma classe de solução que
  `_scan_wave_spectrum_types()` já usa pra `%WAVE.JONSWAP`/`%WAVE.REGULAR`, generalizada pra um
  conjunto de tags de abertura não-enumerável de antemão). `to_interface_json()` agora preserva
  CADA material real (antes: só o primeiro `%MATERIAL.FLEXIBLE_LINE` encontrado, resto
  descartado); `%LINE.SEGMENT.BUOY_ID` (já lido, nunca propagado) passa a chegar no segmento de
  saída.
- **Gap real encontrado, não fechado**: `%LINE.SEGMENT.BUOY_ID` está em `-1` (não usado) em TODO
  `.aml` real já pesquisado neste repo (Boiao, Manifold, ESDV, Ruptura) -- o mecanismo real de
  anexar uma boia aparenta ser outro (o lado XML/H5 do ANFLEX real lê boias de uma seção
  "BuoyElement" separada, sem relação com segmentos de linha -- `model_builder_dat.cpp:4455`
  `load_buoys()`). Suporte a boia via `.aml` real fica sem verificação end-to-end possível por
  ora; o caminho de JSON de interface escrita à mão (ou gerada pelo futuro editor) funciona
  normalmente. Registrado como suposição de design explícita, não como bug corrigido.
- **Verificação**: (1) script ad hoc construindo uma JSON de interface à mão com 1 linha
  (beam→truss→scalar→winch, mais um `buoy_id`) via `build_config_from_interface()`, conferindo
  `element_type`/`*_properties` corretos por elemento e ausência de ids duplicados -- passou de
  primeira depois de 2 bugs reais encontrados e corrigidos (ver "achados extras" abaixo). (2) O
  `P52_Boiao.aml` REAL inteiro (16 materiais, 9 beam + 5 truss + 2 scalar) importado via
  `to_interface_json()` e compilado via `build_config_from_interface()` até o fim, produzindo
  14652 elementos reais (7737 beam + 6906 truss + 9 scalar) sem nenhum erro -- primeira vez que
  esse arquivo passa pelo pipeline inteiro (AML→interface→compilador) preservando seus tipos de
  elemento reais; resolver o equilíbrio não-linear completo (convergência real) NÃO foi rodado
  (arquivo grande, fora do escopo desta verificação de schema/compilador). (3) Regressão dos 4
  casos de carga do `Exemplo_01a.aml` (Near/Far/Transverse/Cross) reconfirmada -- todos continuam
  compilando pra uma lista de elementos 100% `beam` (500 elementos cada, zero warnings novos),
  confirmando que o novo schema é aditivo/opcional e não muda nada pra um modelo beam-only.

  **Achados extras durante a verificação** (bugs reais, pré-existentes, encontrados e corrigidos
  neste processo, não hipotéticos): `spikes/mooring_validation/build_from_risersim_json.py`
  (usado por `correct_line_mesh_via_moorpy()` pra melhorar o palpite inicial da malha) lia
  `e["section_properties"]` incondicionalmente pra TODO elemento -- quebrava com `KeyError` assim
  que uma linha tinha um elemento truss/scalar/winch/buoy; corrigido pra ler a chave certa por
  `element_type` e pular scalar/buoy (sem E/A/peso axial equivalente) do cálculo da média
  representativa da linha. `catenary_geometry.py`'s aviso de fallback (`correct_line_mesh_via_
  moorpy`, catch-all) imprimia um emoji que quebra com `UnicodeEncodeError` num console Windows
  padrão (cp1252) -- transformava uma exceção capturada de propósito (contrato documentado de
  "melhor esforço, nunca propaga") numa queda real; trocado por um marcador ASCII simples.

**Fora de escopo do v1** (explícito): movimento de topo real/RAO, upload de `.RAO`, corpo
flutuante compartilhado entre linhas (fase 2 futura); editar/reabrir um projeto importado de AML/
XML no editor (só projetos "em branco" são editáveis); turret/ruptura; boia via importação de
`.aml` real (ver "gap real encontrado" na Atualização 10 -- ainda sem caso de teste real
disponível).

**Atualização 11** (UI do editor pros 4 tipos de elemento -- fecha o item que a Atualização 10
deixou explicitamente de fora): schema/compilador/importador AML já existiam (Atualização 10); só
faltava o `editor.html`/`editor_app.js` em si saber criar/editar Truss/Winch/Scalar/Buoy, em vez de
só produzir materiais beam-only. Nenhuma mudança de schema/compilador foi necessária -- confirmado
por leitura direta que os nomes de campo que o compilador já lê (`finite_element_type`,
`initial_tension_kN`, `winch_payout_curve_id`, `scalar_stiffness_translational_kNm`/
`_torsional_kNm_per_rad`/`_bending_kNm_per_rad`, `buoys[].weight_kN`/`volume_m3`/`z_area_m2`/`Ca`/
`moment_inertia`, `segments[].buoy_id`) já eram exatamente os que a UI precisava produzir.

- **2 catálogos novos**, mesmo padrão de Solos/Correntes/Ondas: aba "📈 Curvas" (`curves[]`, tempo×
  fração, referenciada pelo `winch_payout_curve_id` de um material winch) logo depois de
  "Materiais"; aba "🎈 Boias" (`buoys[]`, peso/volume/área-Z/Ca/momento de inércia) logo antes de
  "Linhas" (é o catálogo que a tabela de Segmentos referencia).
- **Materiais ganhou 5 colunas** (sempre visíveis, só lidas pelo compilador quando o tipo do
  material as usa -- mesmo padrão que as colunas de Rayleigh já tinham antes desta rodada): "Tipo"
  (`finite_element_type`, select fixo beam/truss/scalar/winch), "Tração inicial" (truss/winch, com
  seletor de unidade como EA já tem), 3 colunas de rigidez scalar, e "Curva de recolhimento"
  (winch, select **anulável** apontando pro catálogo Curvas).
- **`SpreadsheetTable.js::selectColumn()` ganhou `nullable`** -- único componente genuinamente novo
  de código (os `select` existentes, `material_id`/`soil_id`/`current_id`/`wave_id`, são todos
  obrigatórios; `winch_payout_curve_id` e o novo `segments[].buoy_id` são os primeiros
  campos-referência realmente opcionais do editor). Opção "(nenhuma)" prependida quando `nullable`,
  grava `null` (não string vazia) quando selecionada.
- **Segmentos ganhou a coluna "Boia"** (`buoy_id`, select anulável apontando pro catálogo Boias).
- **Preview 3D**: `updatePreviewNow()` generalizado pra ler `D_outer` da chave de propriedades
  certa por `element_type` (antes: sempre `section_properties`, assumindo beam) e excluir
  scalar/buoy do cálculo de min/max da legenda de diâmetro (sem isso, o `0` sintético de um
  elemento sem diâmetro real puxaria a barra de cores pra baixo artificialmente).
  `Riser3DRenderer.js::renderStep()` ganhou um ramo pro elemento `buoy` -- só tem `node_id` (1 nó,
  compartilhado com o elemento estrutural adjacente), não `node1_id`/`node2_id`; sem tratamento
  especial cairia no fallback posicional e desenharia um cilindro espúrio (mesma classe de bug já
  corrigida pra multi-linha, Atualização 8) -- desenhado como esfera marcadora fixa (amarelo,
  `0xffcc33`) em vez de cilindro.
- **Backfill defensivo** em `load()` (mesmo padrão do backfill de `id` em segmentos AML-derivados)
  pra projetos criados antes desta mudança não mostrarem colunas em branco; `dashboard.js`'s modelo
  default de projeto "em branco" ganhou `curves: []`/`buoys: []`/`buoy_id: null` explícitos.
- `WEB_VERSION`: `1.6.0` → `1.7.0`.

**Verificação real** (servidor local + Chrome headless via CDP, sem Docker -- rodada não toca C++
nem `run_worker.py`/`run_server.py`, só frontend): projeto "em branco" real criado via
`/api/projects/blank`; sequência completa testada via protocolo CDP direto (não só screenshot --
`Runtime.consoleAPICalled`/`Runtime.exceptionThrown` capturados programaticamente, zero exceções e
zero erros de console do início ao fim): criar curva na aba Curvas, trocar o tipo do material pra
`winch` e apontar pra ela, criar boia na aba Boias, setar `buoy_id` no segmento -- o preview
recompilou com sucesso (`preview-error` vazio) produzindo 51 elementos com `element_type` `winch`/
`buoy` corretos. Screenshot confirma visualmente a esfera amarela marcando a boia exatamente no nó
esperado (âncora do segmento único desta linha de teste), sem cilindro espúrio. Regressão: projeto
"em branco" recém-criado, sem nenhuma mutação, carregado do zero -- material default mostra "Viga
(beam)" na coluna Tipo, segmento mostra "(nenhuma)" na coluna Boia, preview 3D idêntico ao
comportamento anterior a esta rodada (mesma catenária, mesma cor por diâmetro), zero
exceções/erros de console.

**Atualização 12** (sub-abas por tipo na aba "Materiais", pedido do usuário testando a Atualização
11 na prática -- achou confuso ter as 5 colunas novas sempre visíveis numa tabela só, mesmo só
relevantes pra um subconjunto dos materiais): entre "acinzentar campos irrelevantes" e "sub-abas
por tipo, cada uma só com seus próprios campos", o usuário escolheu a segunda. A coluna "Tipo" foi
removida -- o tipo fica implícito pela sub-aba onde o material é criado.

- 4 sub-abas dentro de "Materiais" (**Vigas**/**Cabos**/**Juntas**/**Guinchos**), continuam
  operando sobre o MESMO `this.model.materials[]` (um `id` global só, sem catálogos separados --
  mesmo design do próprio ANFLEX real, `%MATERIAL.FINITE_ELEMENT` é campo NO material). Vigas
  mantém exatamente as 13 colunas de antes desta rodada inteira de mudanças de tipo de elemento
  (zero mudança pra quem só usa viga); Cabos/Guinchos ficam sem Cm/Cd/EI/GJ (truss/winch não têm
  Morison nem rigidez de flexão/torção); Juntas só tem as 3 rigidezes scalar.
- **Achado ao desenhar a divisão**: o amortecimento de Rayleigh (T1/T2/ξ1/ξ2) NÃO é por tipo de
  elemento -- `risersim_runner.py:619-629` usa só o material do PRIMEIRO segmento da PRIMEIRA
  linha pra esses 4 valores, sejam quais forem os tipos de todos os outros materiais (um único
  alpha/beta global, limitação de schema pré-existente, não desta rodada). Como qualquer tipo pode
  ser esse primeiro material, essas 4 colunas continuam em TODAS as sub-abas -- e o hint da aba
  ganhou uma frase explicando essa regra, que antes não estava documentada em lugar nenhum da UI.
- `SpreadsheetTable.js::createSpreadsheetTable()` ganhou um parâmetro `filter: (item) => boolean`
  -- filtragem VISUAL via `table.setFilter()` (Tabulator), não uma cópia do array; as 4 tabelas de
  sub-aba compartilham o array real (`this.model.materials`), então add/delete/geração de `id`
  continuam operando sobre o catálogo de verdade em qualquer sub-aba, sem reconciliar nada entre
  elas.
- Criar um material numa sub-aba já grava o `finite_element_type` certo; Juntas ganhou default de
  rigidez não-zero (antes: 0.0 em tudo, que na prática seria uma dobradiça livre -- nunca é o que
  "adicionar uma junta" deveria significar).

**Incidente durante a verificação, sem relação com o código**: matando processos por PID que
pareciam estar segurando a porta 8000 (suspeita de round-robin entre servidor antigo/novo, mesmo
padrão de gotcha já documentado nesta sessão), um dos PIDs matados era na verdade o backend do
Docker Desktop em si -- derrubou o daemon inteiro (`docker ps` passou a falhar). Os containers
`risersim-web-1`/`risersim-worker-1` foram parados (não apagados) nesse processo. Corrigido:
Docker Desktop relançado, `docker compose up -d` religou os containers com o mesmo volume/imagem de
antes, sem perda de dado. **Lição**: nunca matar um PID achado via `netstat` sem antes confirmar via
`tasklist`/`wmic` qual processo/imagem ele é -- numa porta compartilhada com Docker Desktop no
Windows, um PID "óbvio" pode não ser o servidor de teste. Verificação real desta rodada foi refeita
inteira contra um servidor local numa porta DEDICADA (8010, nunca usada por Docker) para eliminar
essa ambiguidade por completo, em vez de reusar a 8000.

**Verificação real**: mesmo método CDP da Atualização 11, servidor local porta 8010 (ver acima):
alternar as 4 sub-abas confirmando que cada uma mostra só suas colunas certas (Vigas: 13 colunas
originais; Cabos: sem Cm/Cd/EI/GJ; Guinchos: idem + curva; Juntas: só as 3 rigidezes); criar um
material em cada sub-aba (`Cabo_1`/`Junta_1`/`Guincho_1`) confirmando que aparece SÓ na sua própria
sub-aba e com `id` sequencial correto no array real; apontar o guincho pra uma curva criada na aba
Curvas e confirmar `winch_payout_curve_id` gravado certo; apagar o material truss pela sua própria
sub-aba e confirmar que sai do array real (`this.model.materials`, 4→3). Zero exceções/erros de
console (`Runtime.exceptionThrown`/`Runtime.consoleAPICalled` via CDP) em toda a sequência.
Screenshots confirmam visualmente o layout das 4 sub-abas nos dois temas. Rebuild + restart do
container Docker `web`/`worker` aplicado depois da verificação local, confirmado servindo o código
novo (`MATERIAL_SUBTABS`/`addMaterialOfType` presentes no JS servido).

### 3b. Interface de controle de simulação (projetos, disparo, acompanhamento)

A peça arquitetural mais nova de todo o roadmap — hoje não existe nenhum backend real, só
`tools/dev_server.py` (servidor de arquivos estáticos) e execução manual do binário C++ via
Docker/CLI. Precisa de desenho próprio antes de qualquer código, cobrindo pelo menos:
- Onde e como um "projeto" (modelo + histórico de rodadas) é persistido — sistema de arquivos é
  provavelmente suficiente para começar, sem precisar de banco de dados.
- Como uma simulação é de fato disparada a partir do navegador — precisa de um serviço/API por trás
  (o navegador não pode invocar `risersim_test_main` diretamente); decidir se roda local, em
  Docker, ou outro lugar.
- Como o progresso é acompanhado em tempo real — o solver já imprime por iteração/passo no stdout
  (`std::cout` com `std::endl`, ver `static_analysis.cpp`), então captura/streaming desse output
  (polling de arquivo de log ou algo como Server-Sent Events) é o caminho mais direto, sem precisar
  instrumentar o C++ para emitir eventos estruturados.

**Recomendação**: tratar como uma rodada de planejamento própria (Explore + Plan) quando chegar a
vez, dado que introduz infraestrutura nova (backend) que hoje não existe.

#### Fase 1 — ✅ IMPLEMENTADA: backend de projetos/rodadas + dashboard/project.html

Sistema de arquivos como fonte de verdade (`risersim/projects/<id>/{project.json,
input_simulation.json, runs/<run-id>/{run.json, input_simulation.json, stdout.log,
catenary_results.json/.h5}}`), sem banco de dados — ver `tools/risersim_projects.py`
(`ProjectStore`). Dois processos separados no `docker-compose.yml`, comunicando só pelo volume
compartilhado: `web` (`tools/run_server.py`, API REST + estático, substituto drop-in de
`dev_server.py`) e `worker` (`tools/run_worker.py`, loop serial que roda `risersim_test_main` uma
rodada de cada vez). Frontend: `dashboard.html`/`project.html` (grid de projetos, criação por
exemplo pré-descoberto de `trunk/exemplos/`, disparo de rodada, log ao vivo via polling de
`stdout.log`). Testado end-to-end (Docker build + Catch2 sem regressão + fluxo completo via
headless Chrome) antes desta rodada de trabalho.

#### Fase 2 — ✅ IMPLEMENTADA: proveniência de versões, hash de modelo, acesso a pré/pós, import por upload

Continuação direta da Fase 1, quatro blocos (ver histórico de planejamento — usuário levantou os
quatro pontos numa mesma conversa):

- **A. Proveniência de versões por rodada** — três campos gravados em `run.json`, cada um evoluindo
  independentemente: `solver_fingerprint` (sha256 do binário `risersim_test_main`, calculado só
  pelo `worker` — só ele tem acesso ao binário compilado — em
  `run_worker.py::compute_solver_fingerprint()`, preenchido quando a rodada começa a rodar, não na
  criação); `schema_version` (constante `SCHEMA_VERSION` em `xml_h5_reader.py`, gravada dentro do
  próprio `input_simulation.json` por `to_risersim_json()`, lida de volta do snapshot já copiado
  por `risersim_projects.py::create_run()`); `web_version` (constante única `WEB_VERSION`, novo
  módulo `tools/risersim_version.py`, cobre interface+pré+pós-processador porque são o mesmo
  deploy — exposta em `GET /api/version`, gravada em `project.json` e em todo `run.json` na
  criação). Motivação real: o próprio bug de memória não-inicializada corrigido nesta sessão
  (mesmo config, binário diferente, resultado diferente) é exatamente o cenário que
  `solver_fingerprint` existe para tornar rastreável.
- **B. Hash do modelo + dedupe de rodadas** — `model_hash` (sha256 do `input_simulation.json`
  congelado em cada rodada, calculado em `create_run()`). `POST /api/projects/<id>/runs` ganhou um
  parâmetro `force` (bool); sem ele, se já existir uma rodada TERMINADA
  (`converged`/`failed`) do mesmo projeto com o mesmo `model_hash`
  (`ProjectStore.find_run_by_model_hash()`), a API responde `409` com o `run_id` da duplicata em
  vez de criar outra à toa — evita gastar a fila serial (uma rodada de cada vez) sem necessidade,
  já que o solver é determinístico dado o mesmo binário. `force=true` bypassa. `project.js` trata o
  409 com `confirm()` e reenvia com `force=true` se o usuário confirmar; tabela de rodadas ganhou
  uma badge discreta `= run-X` nas linhas com `model_hash` repetido.
- **C. Acesso direto a pré/pós-processamento a partir do projeto** — pós já existia; pré-processador
  ganhou `preprocessor_app.js::resolveInputUrl()` (mesmo padrão de `app.js::resolveResultsUrl()`):
  `?project=<id>` → `GET /api/projects/<id>/input` (novo endpoint, input do NÍVEL DO PROJETO, "o
  que uma rodada nova usaria agora"); `?project=<id>&run=<run-id>` → rota genérica de resultados
  por rodada já existente, só adicionando `"input_simulation.json"` ao dict
  `RESULT_FILENAMES` de `run_server.py` (o snapshot já é copiado por `create_run()`, só faltava
  servir). `project.html` ganhou link "🔍 Ver entrada" (nível projeto, cabeçalho) e, por linha da
  tabela de rodadas, "🔍 Entrada usada" (sempre disponível, ao contrário de "Ver resultados" que
  exige `status=converged`).
- **D. Import de caso via upload + área de origem unificada** — novo endpoint
  `POST /api/projects/upload` (multipart: `name`, `xml_file`, `h5_file`, `aml_file` opcional),
  reaproveitando `build_config_from_xml_h5()` tal e qual (sem duplicar a lógica de compilação).
  `create_project()` passou a copiar os arquivos de origem (XML+H5+AML) pra dentro de
  `projects/<id>/source/{model.xml,model.h5,model.aml}` via `_store_source_files()` — caminho de
  código único usado tanto pelo fluxo por exemplo pré-descoberto quanto pelo de upload, resolvendo
  o gap onde `source.xml_path` de projetos-por-exemplo apontava pra fora do diretório do projeto
  (dependente do mount de `trunk/exemplos` continuar no mesmo lugar). `dashboard.html` ganhou uma
  segunda aba "📤 Importar arquivo" no modal "Novo Projeto" (file pickers, submit via `FormData`).

**Verificação real** (2026-08-08, Docker Desktop 29.6.2 no Windows, `docker compose build` + `up`):
rebuild de `web`+`worker` OK (C++ do stage `builder` veio do cache, zero mudança); suíte Catch2
rodada explicitamente no stage `builder` — **361 assertions em 15 test cases, todas passando**
(mesmo número já documentado, confirma zero regressão — nenhum arquivo em `src/`/`include/`/
`tests/` foi tocado nesta rodada). Fluxo completo via API real (`curl`) + headless Chrome
(`chrome --headless=new --dump-dom`, mais um harness HTML same-origin pra simular cliques reais
no modal, já que não havia Playwright/Selenium disponíveis no ambiente):
`GET /api/version` → `{"web_version":"1.0.0"}`; projeto criado por exemplo
(`Curso/Exemplo_01/Exemplo_01a`) com `source/model.{xml,h5,aml}` de fato copiados pro diretório do
projeto (confirmado via `docker compose exec`); rodada disparada e processada pelo worker —
`run.json` final com `model_hash`, `schema_version:1`, `web_version:"1.0.0"` e
`solver_fingerprint` (preenchido só depois do worker pegar a rodada, valor batendo com o log de
startup do worker) todos presentes; segunda tentativa de rodada do mesmo projeto sem `force` →
`409` com corpo `{"error":"já existe uma rodada terminada com o mesmo model_hash","run_id":"run-
20260808-204642","status":"failed"}`; com `force:true` → `201`, nova rodada criada, e a badge
`= run-20260808-204642` apareceu na linha da rodada duplicada em `project.html` (confirmado via
dump-dom); `preprocessor.html?project=<id>` e `?project=<id>&run=<run-id>` carregaram os dados
certos nos dois casos (501 nós/500 elementos, sem avisos, em ambos); upload manual de um par
XML+H5+AML de `trunk/exemplos/Curso/Exemplo_02/Exemplo_02a/` via `POST /api/projects/upload`
(`curl -F`) criou o projeto, copiou os três arquivos de fato pra `projects/<id>/source/`, e a
rodada disparada nesse projeto rodou pelo mesmo pipeline do worker (estática convergida, T_eff≈1027
kN, dinâmica em andamento nos mesmos termos de uma rodada por exemplo) — comportamento idêntico ao
fluxo por exemplo, só a origem dos arquivos muda. Aba "📤 Importar arquivo" testada via clique real
simulado num harness same-origin: abre o modal, alterna painéis (`display:none`/`""`), atualiza
classe `active` dos botões e `dashboard.sourceTab`, tudo correto.

#### Fase 3 — ✅ IMPLEMENTADA: caso de carregamento por rodada, projeto a partir de `.aml` puro, limpeza de UI

Rodada de trabalho puxada pelo usuário navegando a interface real (Docker) e apontando lacunas
concretas, não planejada de antemão.

- **Caso de carregamento por rodada** — até aqui, "projeto" era 1:1 com um caso já resolvido (uma
  pasta XML+H5 = um caso); o `--load-case-id` do `run_from_aml.py` nunca tinha sido ligado ao
  backend web. Decisão do usuário (entre duas arquiteturas propostas): **projeto = modelo físico
  (o `.aml`), rodada escolhe o `%LOAD_CASE`** — o mesmo modelo mental já usado via CLI a sessão
  inteira, agora navegável dentro do MESMO projeto. Implementado extraindo
  `build_config_from_aml()`/`list_aml_load_cases()`/a correção de malha MoorPy pra
  `risersim_runner.py` (compartilhadas entre `run_from_aml.py` e o backend web, zero mudança de
  comportamento no CLI); `ProjectStore.create_run()` ganhou `load_case_id` opcional, monta e
  hasheia o config certo por rodada (`DuplicateRunError` substitui a checagem de dedupe que antes
  só olhava o projeto); nova rota `GET /api/projects/<id>/load-cases`; seletor de caso na criação
  de rodada (`project.html`/`preprocessor.html`) e coluna "Caso" nas tabelas de rodada.
- **Projeto a partir de `.aml` puro** — perguntado pelo usuário ao notar que só 7 dos 31 exemplos
  (os com pasta `_analysis/` já exportada) podiam virar projeto. `discover_aml_only_examples()`
  (`risersim_runner.py`) varre `trunk/exemplos/` por `.aml` sem XML+H5 correspondente (24 dos 31);
  `create_project()`/`_store_source_files()` (`risersim_projects.py`) e as rotas de criação
  (`run_server.py`) passaram a aceitar `aml_path` sozinho; `dashboard.html`/`dashboard.js` marcam
  exemplos aml-only na lista e mostram o seletor de caso já na criação do projeto.
- **Limpeza de UI do pós-processador** — aba "📁 Carregar" (upload manual, redundante com o caminho
  `?project=&run=` principal) removida; controles de animação (play/pause/slider) movidos da aba
  lateral pra uma barra flutuante compacta sobre o canvas 3D (estilo player de vídeo, só ícones,
  cor no gradiente laranja da marca — `--brand-accent`/`--brand-accent-strong`), reaproveitando o
  padrão de overlay HTML já usado por `#colorbar-legend`/`#zoom-rect`.
- `WEB_VERSION`: `1.0.0` → `1.3.1`.

**Verificação real** (2026-08-15, mesmo stack Docker): rebuild de `web` OK a cada mudança; todos os
4 casos de carregamento do Exemplo_01a criados como rodadas do mesmo projeto, T_eff consistente com
o já validado via CLI (~217-220 kN); projeto criado a partir de `Exemplo_01b.aml` (aml puro) —
rodada falhou na estática por limitação pré-existente e não-relacionada (EI≈0,01 kN·m², reproduzida
identicamente via CLI); projeto criado a partir de `DNV_Check.aml` (aml puro) — estática convergiu
limpa (18 iterações), dinâmica confirmada avançando via log ao vivo antes de abortada (rodada grande
demais pra esperar terminar, não necessário pra validar o pipeline). Markup/JS servidos conferidos
via `curl` contra o container real a cada mudança.

### 3c. Pós-processamento

Já existe uma base sólida (`posprocessor.html`, viewer 3D, gráficos Plotly). O trabalho principal
aqui é integrar com o conceito de "projeto/rodada" do item 3b (carregar resultados de uma rodada
específica do histórico, em vez de sempre apontar para um arquivo fixo) — depende de 3b existir
primeiro para fazer sentido completo, mas a base de visualização já não precisa de retrabalho.

#### Fase 1 — ✅ IMPLEMENTADA: envoltórias + histórico no tempo

Puxada pelo usuário comparando com o que o ANFLEX real oferece no pós-processamento. Pesquisa no
C++ real (`anf_analysis/src/post_processor.cpp`/`results_structs.h`/`results_variables.h`)
confirmou que envoltória (mín/máx por nó de elemento, sobre um range de passos) e histórico no
tempo (série completa por nó/elemento escolhido pelo usuário) são recursos de primeira classe lá —
e que toda a série temporal do `risersim` já vive na memória do navegador
(`FEASimulation.staticSteps`/`.dynamicSteps`, populados por `DataLoaderService.js`), então os dois
recursos foram implementados **só no frontend**, sem tocar em C++/exportação.

- **Envoltória**: `FEASimulation::getElementEnvelope(field)` (novo método) itera `activeSteps` e
  retorna mín/máx por posição ordinal do elemento. Checkbox "📉 Mostrar envoltória" (aba
  Visualização) sobrepõe 2 traces tracejados (mín/máx) aos 3 gráficos de perfil já existentes
  (Tração/Momento/von Mises) — decisão do usuário: overlay nos gráficos existentes, não uma aba
  separada.
- **Histórico no tempo**: nova aba de viewport "🕒 Histórico no Tempo" +
  `TimeHistoryChartController.js` (novo). Seleção do ponto de interesse é feita clicando numa linha
  da tabela de Elementos/Nós já existente (aba "📊 Tabela") — decisão do usuário, em vez de um
  dropdown — o que também destaca o nó/elemento escolhido na cena 3D
  (`Riser3DRenderer::updateSelectionHighlight()`, reaproveitando o `nodesGroup`, existente mas
  nunca antes populado). Nó selecionado mostra X/Y/Z vs. tempo; elemento selecionado mostra o campo
  escalar atualmente escolhido em "🎯 Grandeza para Colorir" vs. tempo, com uma linha vertical
  marcando o passo ativo do slider.
- `WEB_VERSION`: `1.3.1` → `1.4.0`.

**Verificação real** (2026-08-16, Docker): rebuild de `web` OK; testado via um harness HTML
same-origin (mesma técnica já usada na Fase 2 do Eixo 3b, já que não há Playwright/Selenium no
ambiente) contra uma rodada real convergida do caso Far (`Exemplo_01a`, 21 passos dinâmicos, 501
nós/500 elementos): clique em linha de elemento → `selectedPoint` correto, classe `selected-row`
aplicada, marcador 3D criado; clique em linha de nó → histórico com 3 traces (X/Y/Z); clique de
novo na mesma linha → deseleciona; toggle de envoltória → Tração 1→3 traces, Momento+Curvatura
3→4, von Mises 2→4, todos os traces extras corretos. Zero erros de console capturados em todo o
fluxo. Confirmado visualmente por screenshot (tema escuro) que o layout/cores seguem o padrão
visual já estabelecido.

#### Fase 2 — ✅ IMPLEMENTADA: correções de layout dos gráficos + tabela de Elementos compacta

Puxada pelo usuário usando a Fase 1 na prática. Três achados de layout, seguidos de uma
reorganização da tabela de Resultados.

- **Título/legenda sobrepostos pelas barras flutuantes**: os 4 `div`s de gráfico (`.viewport-layer`)
  preenchiam 100% da altura do `#canvas-container` desde y=0, ficando por baixo de
  `#viewport-tabs` (topo) e `#playback-overlay` (base), ambos `position:absolute`. Tentativa 1
  (`padding-top`/`padding-bottom` no CSS) não funcionou -- `clientHeight`, que o Plotly lê pra se
  dimensionar, INCLUI padding, então o gráfico continuava se desenhando no tamanho cheio e o
  excesso (onde ficava o eixo X) era cortado pelo `overflow:hidden` do container, fazendo o eixo
  sumir por completo. Corrigido de vez trocando pra `position:absolute` com `top`/`bottom` (mesmo
  padrão dos outros overlays) -- isso sim encolhe a caixa real antes do Plotly medir.
- **Legenda acima do título** (ordem invertida): a legenda usava coordenadas `paper` (relativas à
  área do gráfico, `y:1.12`) enquanto o título usava posição automática do Plotly -- a legenda
  acabava renderizando mais alto que o título. Corrigido fixando os dois em coordenadas
  `container` (`yref:'container'`) com o título mais perto do topo e a legenda abaixo dele, em
  `ProfileChartsController.js` e `TimeHistoryChartController.js`.
- **Envoltória mín. "invisível"**: as curvas de mín./máx. usavam a mesma cor da série principal (só
  com opacidade reduzida) -- quando o valor mínimo histórico de um elemento coincidia com o do
  passo atualmente exibido (comum logo após trocar de modo estático/dinâmico), a curva tracejada
  ficava desenhada exatamente por baixo da sólida, parecendo ausente. Corrigido com cores fixas e
  distintas (`#ec4899` máx./`#06b6d4` mín.) em vez de derivadas da cor de cada série, garantindo
  contraste mesmo quando os valores coincidem.
- **Tabela de Elementos compacta (Proposta C, escolhida entre 3 propostas apresentadas)**: a tabela
  de 8 colunas (ID/Nó/Status/Tração/Momento/Curvatura/von Mises/MBR) já exigia scroll horizontal no
  painel lateral. Reduzida a 4 colunas "manchete" (ID/Nó/Status/Tração); as 4 grandezas restantes
  (Momento/Curvatura/von Mises/MBR) passam a aparecer num painel de detalhe (`#element-detail-card`,
  reaproveitando as classes `control-group`/`stat-card` já existentes) que se popula ao clicar numa
  linha -- o mesmo mecanismo de seleção (`selectPoint()`) já usado pelo destaque 3D e pelo histórico
  no tempo da Fase 1. Para não perder a capacidade de achar rapidamente "o pior elemento" que a
  tabela larga dava de graça, os 3 cabeçalhos restantes ganharam ordenação por clique (▲/▼/⇅),
  persistente durante a navegação entre passos. Decisão do usuário: uma "tabelão com tudo" pode ser
  adicionada depois, sob demanda -- não faz parte deste incremento.
- `WEB_VERSION`: `1.4.0` → `1.4.1`.

**Verificação real** (2026-08-16, mesmo harness HTML same-origin contra a mesma rodada Far):
gráficos conferidos visualmente por screenshot em cada etapa (título acima da legenda, eixo X
visível, barra de playback sem sobrepor nada; envoltória mín./máx. com cores distintas em dois
passos diferentes, inclusive um onde mín.==passo atual). Tabela: 4 colunas confirmadas, ordenação
ascendente/descendente testada na coluna Tração (elemento 1, maior tração, ordena primeiro em
descendente; elementos da zona de touchdown, menor tração, primeiro em ascendente) com indicador
correto; clique em linha popula o painel de detalhe com os valores corretos, mantém destaque 3D e
histórico no tempo funcionando; painel de detalhe some ao trocar para a visão de Nós. Zero erros de
console em todo o fluxo.

#### Fase 3 — ✅ IMPLEMENTADA: seletor de grandeza dedicado pro histórico no tempo

Usuário perguntou se dava pra escolher a grandeza do histórico. Até aqui, o histórico de um
elemento reaproveitava o valor de "🎯 Grandeza para Colorir" (aba Visualização, campo que também
colore a cena 3D) -- funcionava, mas escondido, e com um bug real: trocar aquele seletor enquanto
a aba "Histórico no Tempo" estava ativa forçava a navegação de volta pra aba de perfil
correspondente (`switchViewportView()`'s condição `activeViewportView !== '3d'` não excluía
`'history'`), tirando o usuário da aba sem pedir.

- Novo seletor `#history-field-select`, independente do de coloração 3D (`this.historyField`,
  novo estado em `app.js`, default `'tension'`), numa barra flutuante (`#history-field-bar`) só
  visível durante a aba Histórico e só quando o selecionado é um ELEMENTO (nó não tem grandeza --
  histórico de nó já é X/Y/Z fixo).
- Bug de navegação corrigido: `activeViewportView !== '3d' && activeViewportView !== 'history'`.
- **Achado de layout, pego só depois de screenshot** (não pela suíte funcional): a barra flutuante
  original, ancorada `position:absolute` no canto direito da mesma linha das abas, colava direto
  em cima do texto "🕒 Histórico no Tempo" assim que a 5ª aba entrou na fileira -- espaço
  insuficiente pra ambos convivirem numa tela de largura comum. Corrigido reestruturando as duas
  (`#viewport-tabs` + `#history-field-bar`) como filhos de um `#viewport-toolbar` com
  `flex-wrap:wrap`, então a barra de grandeza cai pra uma segunda linha em vez de sobrepor. Isso
  por sua vez expôs um segundo problema (mesma categoria da Fase 2: espaço reservado insuficiente
  pra um overlay externo) -- com 2 linhas de toolbar, o topo reservado de 52px não bastava mais,
  a barra invadia o título do gráfico. Corrigido com uma classe `with-field-bar` (alternada via JS
  junto com a visibilidade da própria barra) que só aumenta o `top` do `#history-chart`
  especificamente quando a barra pode estar visível, sem afetar os outros 3 gráficos.
- `WEB_VERSION`: `1.4.1` → `1.4.2`.

**Verificação real** (2026-08-16, mesmo harness): selecionar elemento → trocar grandeza do
histórico (própria barra) → título do gráfico muda, aba continua em "Histórico" (bug corrigido);
trocar a grandeza de COLORAÇÃO 3D (seletor não relacionado) enquanto na aba Histórico → não navega
pra fora, histórico não muda (desacoplamento confirmado); selecionar nó → barra some. Screenshot
confirmou visualmente a quebra de linha sem sobreposição e o título do gráfico totalmente visível
abaixo das duas linhas da toolbar. Zero erros de console.

#### Fase 4 — ✅ IMPLEMENTADA: aposentar JSON de resultados, completar/comprimir o H5, nomear pelo caso

Usuário perguntou se os resultados iam todos pro H5 -- não iam (só posição+tração; momento/
curvatura/von Mises/MBR só existiam no JSON, com o loader JS mascarando a ausência com `0.0`/`5.0`
default). Ao discutir tamanho/performance (JSON de uma rodada pequena já media 5,38 MB nesta
sessão, com os passos dinâmicos gravados EM DOBRO -- `dynamic_steps` + o array `steps` de
compatibilidade), decisão do usuário: aposentar o JSON por completo, completar o schema do H5,
ativar compressão gzip, e nomear o arquivo de resultados pelo projeto+caso em vez de um nome
genérico.

- **C++** (`simulation_exporter.cpp`): `write_hdf5_group()` passa a gravar os 5 campos por
  elemento (antes só tração) e comprime todos os datasets (posições + elementos) com
  `H5::DSetCreatPropList::setChunk()+setDeflate(6)` -- gzip é filtro padrão do HDF5, pesquisa
  prévia confirmou que o h5wasm 0.4.10 (já usado no pós-processador) lê sem plugin nenhum.
  `export_json()`/`write_snapshots_json_array()` removidos inteiros, junto com o binding pybind11
  e a chamada em `Simulation::export_results()`.
- **Nome do arquivo pelo caso**: o binário C++ não tem noção de "projeto"/"caso" (sem campo de
  nome em `RiserModel`), então continua escrevendo `catenary_results.h5` fixo; `run_worker.py`
  RENOMEIA pra `<Projeto>_<Caso>_results.h5` assim que o solver termina
  (`ProjectStore.finalize_results_filename()`, novo, com `_sanitize_filename_component()` --
  espaço vira `_`, preserva maiúsculas ao contrário do `_slugify()` já existente pro ID do
  projeto), gravando o nome real em `run.json["results_filename"]`. Decisão do usuário: a
  INTERFACE busca e usa esse nome real (não um nome estável traduzido por trás no servidor) --
  `app.js::resolveResultsUrl()` virou assíncrona, busca `GET .../runs/<id>` primeiro pra ler
  `results_filename` antes de montar a URL de resultados; `project.js`'s "⬇ Resultados" usa o
  mesmo campo (já disponível, sem fetch extra). `run_server.py::api_run_results()` valida o nome
  pedido contra `run.json["results_filename"]` em vez de um dicionário estático -- funciona como
  controle de acesso también (só o arquivo real da rodada é servível). Fora de escopo, deliberado:
  o workflow manual `run_from_aml.py` (sem `ProjectStore`) continua com nome fixo.
- **JS**: `DataLoaderService.js` perde `loadJSON()`/`parseRawStepsArray()` e a lógica de
  detecção de extensão + fallback pra JSON -- `load()` só chama `loadHDF5()`.
- `WEB_VERSION`: `1.4.2` → `1.5.0`.

**Bug real achado e corrigido durante a verificação, sem relação com o plano acima**: a URL do
`<script>` do h5wasm em `posprocessor.html` sempre apontou pra
`.../h5wasm@0.4.10/dist/h5wasm.js`, que **não existe** no pacote (404) -- o bundle real do browser
fica em `dist/iife/h5wasm.js`. Como o caminho H5 nunca tinha sido de fato exercitado em produção
(nada pedia `?format=h5` por padrão), esse 404 nunca foi notado -- o carregamento H5 estava
quebrado desde sempre, silenciosamente. Só foi pego agora porque H5 virou o único formato e um
harness headless real tentou carregar um resultado de verdade. Corrigido (path certo confirmado
batendo `curl` contra o CDN antes de aplicar).

**Verificação real** (2026-08-16, Docker + MSBuild): Catch2 361/361 sem regressão. CLI
(`run_from_aml.py`, caso Cross): só `.h5` gerado, T_eff=217,29 kN batendo o valor já validado
(217,3 kN); H5 inspecionado via h5py -- 6 datasets presentes (`node_positions` +
5 campos de elemento), todos `compression=gzip`; tamanho **menor** que o H5 antigo mesmo com 4
datasets a mais (533.440 → 404.640 bytes) graças à compressão. Pipeline web (rodada real do Far):
`run.json` com `results_filename="Exemplo_01a_Far_results.h5"`; arquivo já nasce com esse nome
dentro do diretório da rodada (confirmado via `docker compose exec`); API serve o nome real (200),
rejeita o nome genérico antigo e o `.json` (404 nos dois), `Content-Disposition` do download traz
o nome certo. Pós-processador carregado via harness headless real (não só screenshot) -- painel de
detalhe mostra Momento/Curvatura/von Mises/MBR REAIS (antes viriam zerados sob o H5 incompleto),
envoltória e histórico com seletor de grandeza funcionando sobre dado H5 de verdade. H5 desta
rodada (920.787 bytes, 33 passos) vs. o JSON medido antes pra essa mesma rodada (5.382.923 bytes,
com a duplicação) -- **~5,85x menor**, já confirma a direção esperada mesmo numa rodada pequena.
Zero erros de console em todo o fluxo.

#### Fase 5 — ✅ IMPLEMENTADA: escrever com o nome certo de saída, não renomear depois

Usuário questionou o desenho da Fase 4 (renomear `catenary_results.h5` pra
`<Projeto>_<Caso>_results.h5` DEPOIS que o solver termina) -- preferiu que o nome certo já nascesse
junto com o arquivo. Projeto+caso já são conhecidos no momento em que a rodada é CRIADA (antes do
solver sequer começar), então não havia motivo real pra esperar o fim da execução só pra renomear.

- **C++**: `Simulation::export_results(output_dir, filename="catenary_results.h5")` ganhou um
  segundo parâmetro (nome do arquivo, com default que preserva o comportamento do workflow manual
  `run_from_aml.py`, que não passa esse argumento); `main()` (`main.cpp`) lê um 3º `argv` opcional
  e repassa. `risersim_test_main <input.json> <output_dir> [nome_do_arquivo]`.
- **`risersim_projects.py::create_run()`**: passa a calcular e gravar `results_filename` já na
  criação da rodada (mesma fórmula `<Projeto>[_<Caso>]_results.h5` de antes), não mais depois que
  o solver termina. Método `finalize_results_filename()` (Fase 4) removido inteiro -- não existe
  mais rename, só escrita direta.
- **`run_worker.py`**: lê `results_filename` do `run.json` (já preenchido desde a criação) e passa
  como 3º argumento pro binário -- `cmd = [exe, input_json, run_dir, results_filename]`. O arquivo
  nasce com o nome certo; nenhum `catenary_results.h5` intermediário chega a existir no disco.
- **`app.js::resolveResultsUrl()`**: como `results_filename` agora está sempre presente (mesmo
  numa rodada `pending`/`running`, já que o nome é conhecido antes do arquivo existir), a checagem
  de "tá pronto pra carregar" passou a olhar `run.status` (`converged`/`failed`) em vez da
  presença do campo.
- `WEB_VERSION`: sem bump adicional (mesmo `1.5.0` da Fase 4 -- ainda não commitado).

**Verificação real** (2026-08-16, Docker): rebuild C++ local confirmou o binário aceita o 3º
argumento (com ele, escreve direto no nome pedido; sem ele, mantém `catenary_results.h5` -- CLI
manual não quebrou). Catch2 361/361. Rodada real disparada via API (`Near`, `exemplo-01a`):
`results_filename="Exemplo_01a_Near_results.h5"` já presente com `status:"pending"`, antes do
solver começar; `stdout.log` do solver confirma que ele mesmo escreveu direto nesse caminho
(`✅ ... exported to: .../Exemplo_01a_Near_results.h5`); diretório da rodada terminada só tem esse
arquivo, nunca teve um `catenary_results.h5` intermediário. Pós-processador carregado via harness
headless real sobre essa rodada -- 21 passos dinâmicos/12 estáticos, zero erros de console.

#### Fase 6 — ✅ IMPLEMENTADA: entrada sempre obrigatória (sem fallback sintético), saída sempre ao
lado da entrada

Usuário perguntou se `main()` sempre recebe o arquivo de entrada -- na verdade não: sem `argv[1]`
(ou com um JSON que falhasse ao carregar), `Simulation::load()` caía silenciosamente num modelo
sintético de catenária parabólica (geometria de conveniência histórica do risersim, sem
equivalente real de ANFLEX). Pediu pra remover essa opção -- entrada sempre via arquivo real -- e,
já que o `<output_dir>` da Fase 5 sempre foi o mesmo diretório do arquivo de entrada em todo
caminho de chamada existente (`run_worker.py`: `input_json = run_dir / "input_simulation.json"`;
`run_from_aml.py`: `input_json_path = out_dir / "input_simulation.json"`), derivar esse diretório
automaticamente a partir do próprio caminho de entrada em vez de recebê-lo como argumento
separado.

- **C++**: `Simulation::load()` perdeu o branch de fallback inteiro -- se o JSON não existir/não
  parsear, `parsed_from_json` fica `false` (erro já impresso por `ModelBuilder::load_from_json()`)
  e não há mais geometria sintética alternativa. `ModelBuilder::build_synthetic_fallback()` era
  chamado só por esse branch (confirmado via grep -- zero outros call sites em produção ou
  bindings Python) -- removido inteiro (declaração + definição), não só desligado.
  `tests/test_static_analysis.cpp` tem sua própria cópia local da mesma geometria
  (`build_synthetic_catenary_model()`, nunca chamou o método do ModelBuilder), então nenhum teste
  foi afetado.
- **`main.cpp`**: `argc < 2` agora é erro fatal (`Uso: ... <input.json> [nome_arquivo_resultado]`,
  retorna 1) em vez de deixar `input_json_path` vazio pro fallback absorver. O antigo 2º argumento
  (`output_dir`) foi removido -- `main()` deriva o diretório de saída direto do `parent_path()` do
  caminho de entrada (`std::filesystem`); o antigo 3º argumento (nome do arquivo de resultado,
  opcional) virou o 2º. Novo uso: `risersim_test_main <input.json> [nome_arquivo_resultado]`. Se
  `sim.parsed_from_json` for `false` depois de `load()`, `main()` retorna 1 sem chamar
  `run()`/`export_results()`.
- **Python**: todo call site do binário precisou dropar o argumento de `output_dir` (posição
  mudou -- passar a pasta ali agora seria interpretado como nome de arquivo de resultado):
  `run_worker.py` (`cmd = [exe, input_json, results_filename]`, `run_dir` removido -- já era o
  diretório do próprio `input_json`), `run_from_aml.py` (`cmd = [exe, input_json_path]`, já estava
  dentro de `out_dir`), e `risersim_runner.py::run_simulation_subprocess()` (função exportada mas
  sem call sites hoje -- corrigida por consistência, mesmo padrão).

**Verificação real** (2026-08-16): rebuild local de `risersim_test_main` + `risersim_tests`,
Catch2 361/361 sem regressão. CLI: sem argumentos → erro de uso, exit 1, sem geometria sintética;
caminho inexistente → erro de carga, exit 1; caminho de entrada real (`Exemplo_01a`, estático,
`T_eff≈217,3 kN` -- mesmo valor histórico já validado nesta sessão) com e sem nome de arquivo
explícito, ambos gravando `.h5` no mesmo diretório do `input_simulation.json`, incluindo o caso de
caminho relativo sem diretório (`has_parent_path() == false` → usa `.`). Docker rebuild +
`docker compose up -d --build web worker`; rodada real disparada via API (`Near`, `exemplo-01a`,
force=true por já existir uma rodada com o mesmo `model_hash`) convergiu normalmente --
`Exemplo_01a_Near_results.h5` apareceu sozinho no diretório da rodada, junto de
`input_simulation.json`, sem `output_dir` separado. `run_from_aml.py` (workflow manual, fora do
Docker) testado contra o `.aml` real do Exemplo_01a com `--static-only`, reproduzindo
`T_eff≈217,3 kN` e gerando `catenary_results.h5` ao lado do `input_simulation.json` gerado.

#### Fase 7 — ✅ IMPLEMENTADA: nomear também o arquivo de entrada pelo caso (mesma regra do de saída)

Usuário pediu a mesma regra de nome do `results_filename` (Fase 5/6) pro arquivo de entrada
per-rodada -- até aqui o snapshot congelado do config que o solver efetivamente lê continuava
com o nome genérico `input_simulation.json` dentro de `runs/<run_id>/`, enquanto o resultado ao
lado já se chamava `<Projeto>[_<Caso>]_results.h5`. Escopo continua o mesmo da Fase 5/6: só o
arquivo POR RODADA, dentro de `runs/<run_id>/` -- o `input_simulation.json` de NÍVEL DE PROJETO
(`projects/<id>/input_simulation.json`, o "config atual" fora de qualquer rodada específica, sem
caso associado) fica como estava, sem mudança.

- **`risersim_projects.py::create_run()`**: computa `stem = "_".join([projeto, caso])` uma vez só
  e deriva os dois nomes a partir dele -- `input_filename = f"{stem}_input.json"` e
  `results_filename = f"{stem}_results.h5"` (mesma sanitização via `_sanitize_filename_component`
  já usada pra `results_filename`). O snapshot é gravado direto sob `input_filename` (não mais
  `input_simulation.json`); o campo novo `input_filename` é gravado em `run.json` junto do já
  existente `results_filename`.
- **`run_worker.py`**: lê `run.get("input_filename")` (com fallback pra `"input_simulation.json"`
  -- rodadas criadas antes desta fase não têm o campo) em vez do nome fixo, e passa esse caminho
  como `argv[1]` pro binário.
- **`run_server.py`**: `RESULT_FILENAMES` (dict estático que só tinha a entrada
  `"input_simulation.json"`) removido inteiro -- virou código morto assim que o nome parou de ser
  fixo. `api_run_results()` agora valida `filename` contra os DOIS campos dinâmicos do run
  (`input_filename` OU `results_filename`), não mais um allowlist estático + um campo dinâmico.
- **`preprocessor_app.js::resolveInputUrl()`**: virou assíncrona, mesmo padrão de
  `app.js::resolveResultsUrl()` -- no caminho `?project=&run=`, busca `GET .../runs/<run_id>`
  primeiro pra ler o `input_filename` real, então monta a URL com esse nome. Diferença importante
  em relação ao dos resultados: o arquivo de entrada é gravado SÍNCRONA e IMEDIATAMENTE na
  criação da rodada (antes do solver sequer começar), então não precisa checar `status` como
  `resolveResultsUrl()` faz -- só falha se a rodada em si não existir.
- `WEB_VERSION`: `1.5.0` → `1.6.0` (novo campo em `run.json`, nova exigência de nome dinâmico na
  rota de resultados).

**Verificação real** (2026-08-16, Docker): rodada real disparada via API (`Cross`, `exemplo-01a`,
force=true): resposta da criação já veio com `input_filename:"Exemplo_01a_Cross_input.json"` e
`results_filename:"Exemplo_01a_Cross_results.h5"`; após convergir, o diretório da rodada só tinha
esses dois arquivos (mais `run.json`/`stdout.log`) -- nenhum `input_simulation.json`/
`catenary_results.h5` genérico. Rota de resultados: nome real do input → 200; nome real dos
resultados → 200; `input_simulation.json` (nome genérico antigo) → 404; `catenary_results.h5`
(idem) → 404 -- confirma que a rota não aceita mais nomes fixos, só os dois nomes reais desta
rodada. Pós-processador de ENTRADA (`preprocessor.html?project=&run=`) carregado via harness
Chrome headless real sobre essa rodada -- 501 nós / 500 elementos, `inputValid: true`, sem erro no
console.

## Qualidade de código — encapsulamento (2026-08-17)

Pedido do usuário, revisando `model.hpp` no IDE: membros de classe muito expostos como `public`,
preferência por estado privado acessado por método. Levantamento prévio (26 `class`/12 `struct` em
`include/risersim/*.hpp`, contagem de pontos de acesso externo por grep) separou "exposição
idiomática" (structs de config/DTO -- público é o idioma certo em C++) de "exposição que esconde um
invariante violável de verdade". `ModelBuilder`/`VesselMotion`/`ConvergenceTest`/`Hydrostatics` já
seguiam o padrão desejado (estado privado com sufixo `_`, método público sem sufixo) e serviram de
modelo de estilo.

**Encapsuladas** (4 classes, escopo acordado com o usuário via `AskUserQuestion`):
1. **`SeabedInteraction`** (`seabed.hpp`) -- invariante real: `axial_friction`/`lateral_friction` só
   herdam `friction_coeff` no construtor, reatribuir depois não propaga.
2. **`CurrentProfile`** (`current_profile.hpp`) -- invariante real: os 3 vetores paralelos do perfil
   tabulado (`depth_below_surface_m`/`velocities_ms`/`angles_deg`) precisam ter o mesmo tamanho e
   ordem ascendente. `set_profile()` (já o mutador certo) ganhou validação nova que rejeita e avisa
   (`std::cerr`) em vez de instalar um perfil inconsistente -- única mudança de COMPORTAMENTO deste
   refactor inteiro, todo o resto é puramente sintático.
3. **`RiserModel`** (`model.hpp`) -- invariante real: `nodes`/`elements` são
   `vector<unique_ptr<...>>` dos quais `CorotationalBeam3D::node1()/node2()` guardam ponteiro cru;
   nada impedia um `model.nodes.erase(...)` direto, que deixaria esses ponteiros pendurados.
   `nodes()`/`elements()` agora só-leitura (escrita exclusivamente via `add_node()`/`add_element()`/
   `clear()`, já as APIs certas -- confirmado por grep: zero mutação externa hoje). Por pedido do
   usuário (consistência), `environmental()`/`analysis_options()`/`references()`/`connections()`/
   `lines()` também viraram acessores (pares const/não-const, já que `model_builder.cpp` precisa de
   referência mutável direta pra popular campo a campo).
4. **`CorotationalBeam3D`** (`element_beam.hpp`) -- pedido explícito do usuário, apesar de ser a
   classe mais "quente" do motor (~30x leitura de `node1`/`node2`, ~25x `props`, ~10x
   `tension_effective` em `src/*.cpp`). Confirmado por grep que a ESCRITA já estava inteiramente
   concentrada no construtor + `update_effective_tension()` -- exceto `BuoyancyModule`/
   `BendRestrictor` (`buoyancy_and_restrictor.hpp`), que mutam `props`/`net_upward_buoyancy` de
   fora (só achado pelo compilador ao tentar linkar `risersim.vcxproj`, já que esse header não é
   usado por nenhum `src/*.cpp` -- só por `bindings.cpp`) -- resolvido dando a `props()` um overload
   não-const (mutável in-place, mesmo padrão de `environmental()`/`analysis_options()`).

**Fora de escopo, decisão explícita do usuário**: `Node3D` -- mesma classe "quente"
(`disp`/`rot`/`coords`/`eq_numbers`/`friction_force`/`delta_disp_xy`, 100+ pontos de acesso dentro
do Newton-Raphson estático/dinâmico já afinado ao longo de ~18 rodadas de debug), mas sem
consumidor externo real pra proteger -- risco desproporcional ao ganho.

**Revisão de documentação/comentários** (pedida junto): avaliada como já forte/uniforme no
levantamento prévio (Doxygen `@brief`/`@param` com racional citando o real ANFLEX), sem gap
prioritário -- ficou embutida no próprio trabalho de encapsulamento (cada campo privado manteve seu
comentário detalhado, cada acessor novo ganhou `@brief` de uma linha).

**Verificação**: os 4 alvos MSBuild (`risersim_test_main`, `risersim_tests`, `risersim` [módulo
pybind], `risersim_diag_isolated_segment`) recompilam limpos após cada uma das 4 classes. Suíte
Catch2: 405/405, zero regressão, em todas as 4 rodadas. Comparação numérica ponto-a-ponto
(Near/Transverse/Cross vs `.SAI` real, mesma metodologia da Atualização 18) idêntica antes/depois em
todas as 4 rodadas (erro horizontal médio/máximo batendo a 4 casas decimais). Smoke test manual em
Python (bindings pybind) confirmou getters/setters, `props()` mutável, e `BuoyancyModule`/
`BendRestrictor` funcionando nos dois sentidos.

## Backlog de recursos faltantes do motor (não bloqueante — puxar sob demanda)

Achados documentados em `mapa_aml_exemplos_e_web_interface.md`. Flexjoint (`cScalar`, real uso em 6
`.aml`s) e boias/tendões como entidade própria (`cTruss`/`cWinch`/`cBuoyElement`, real uso em 3-7
`.aml`s cada, incluindo peso/empuxo/corrente pro Truss/Winch) — **implementados de ponta a ponta**:
motor (Fases 0-4 do Eixo 2d), backend do pipeline web (Atualização 10 do Eixo 3a) e UI do editor
(Atualização 11 do Eixo 3a), todos 2026-08-18. Resta só boia via importação de `.aml` real
(`%LINE.SEGMENT.BUOY_ID` está sempre em -1 nos exemplos reais deste repo — o mecanismo real do
ANFLEX usa outra seção, não segmentos de linha; sem caso de teste real pra verificar contra) -- o
caminho de JSON de interface escrita à mão ou pelo editor já funciona normalmente. Ainda faltam de
verdade:
drilljoint, turret com movimento prescrito 6-GDL por caso de carga (`Turret.aml`), ruptura de
elemento em tempo de execução dinâmico (`Ruptura.aml`), verificação de código DNV como
pós-processamento (`DNV_Check.aml`), corpo rígido/contato tubo-em-tubo (`cRigidBodyElement`/
`cContactElement` — zero ocorrências em qualquer `.aml` deste repo). Cada um só vale a pena quando
um caso de teste real concreto precisar dele — não faz sentido implementar especulativamente.
(Múltiplas linhas com corpo flutuante compartilhado — implementado e com convergência estática
resolvida pra águas muito profundas/linhas muito longas, ver Eixo 2c.)

## Ordem sugerida (não é obrigatória — ponto de partida pra decidir)

> Atualizado 2026-08-18 (2ª vez no dia) -- versão anterior já estava desatualizada de novo: o
> backlog listava boias/tendões como "não implementadas", mas o Eixo 2d (motor, 4 tipos de
> elemento) e a Atualização 10 do Eixo 3a (backend do pipeline web pros mesmos 4 tipos) fecharam
> isso no mesmo dia. Praticamente todo o escopo original dos Eixos 1-3 está concluído -- o que
> resta é só investigação pausada (sem lead), backlog explicitamente sob demanda, e a UI do editor
> web pros 4 tipos de elemento novos (deliberadamente adiada, ver Eixo 3a). Ver os eixos acima para
> o histórico completo.

1. ~~**1a** (bug estático)~~ — ✅ resolvido.
2. ~~**1b** (dinâmica)~~ — ✅ resolvido (Exemplo_01a converge os 20 passos completos).
3. ~~**2a** (religar `aml_reader.py`)~~ — ✅ resolvido, refinamento residual **pausado** (gap de
   ~5-7% em heave/roll do Far, sem parâmetro corrigível encontrado).
4. ~~**2c** (múltiplas linhas)~~ — ✅ implementado, convergência real verificada.
5. ~~**3a** (entrada de dados)~~ — ✅ v1 implementada, incluindo a UI do editor pros 4 tipos de
   elemento novos (Truss/Winch/Scalar/Buoy, Atualização 11). Fora de escopo do v1: RAO/movimento de
   topo real, editar projeto importado de AML pelo editor.
6. ~~**3b** (controle de simulação)~~ — ✅ Fases 1-3 implementadas (projetos/rodadas, proveniência
   de versões, caso de carregamento por rodada).
7. ~~**3c** (pós-processamento)~~ — ✅ Fases 1-7 implementadas (envoltórias, histórico no tempo,
   nomenclatura de arquivo por caso, integração completa com projeto/rodada).
8. ~~**2d** (múltiplos tipos de elemento)~~ — ✅ motor + backend do pipeline web + UI do editor,
   todos concluídos (2026-08-18). Resta só boia via `.aml` real (sem caso de teste real
   disponível).
9. **2b** (múltiplas zonas de solo por segmento) e o resto do **Backlog de recursos faltantes**
   (drilljoint, turret, ruptura, DNV check, corpo rígido/contato) — sob demanda, sem gatilho
   concreto no momento; não vale implementar especulativamente.
10. **Gap da catenária estática sob corrente** (5-8m, Eixo 2a) — **pausado**, 4 sub-hipóteses da
    mecânica corrotacional descartadas com evidência direta (Atualizações 19-20); precisa de uma
    pista genuinamente nova (ex. instrumentação comparativa iteração-por-iteração) pra retomar.
11. **Morison/massa molhada na dinâmica** (fases 4-5 do plano original do Eixo 1b, nunca decidido
    se ainda compensa perseguir agora que a divergência que motivou o plano já foi resolvida por
    outro caminho) — sem decisão tomada, sob demanda.

## Ver também

- [`mapa_classes_anflex_estatica.md`](mapa_classes_anflex_estatica.md) — investigação do solver e
  do bug de convergência solo+corrente (Eixo 1a).
- [`mapa_classes_anflex_interface.md`](mapa_classes_anflex_interface.md) — mapa da interface
  gráfica real do ANFLEX (base para o Eixo 3a).
- [`mapa_aml_exemplos_e_web_interface.md`](mapa_aml_exemplos_e_web_interface.md) — censo de
  exemplos, lacuna do `aml_reader.py` (Eixo 2a), e arquitetura de dois JSONs (Eixo 3a).
