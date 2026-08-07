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

### 1b. Investigar a análise dinâmica — 🟡 EM PROGRESSO (achado real, ainda não fechado)

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
pra pior" já vista na estática). Fidelidade conhecidamente mais baixa que a estática em pelo menos
dois pontos ainda não auditados: amortecimento de Rayleigh (`alpha_rayleigh`/`beta_rayleigh`
hardcoded no construtor de `DynamicAnalysis`, nunca lidos do JSON) e o movimento prescrito do topo
(ainda usa a técnica direta antiga, `top_node->disp = ...` fora do sistema — a técnica de
penalidade `PrescribedMotion` só foi migrada pra estática, Passo 7 do roadmap de modernização).

**Recomendação pro próximo passo**: continuar auditando dados antes de tentar técnica numérica --
Rayleigh damping é o candidato mais óbvio ainda não verificado (não lido do JSON real de jeito
nenhum hoje). Se isso não resolver, isolar um caso mínimo (malha sintética pequena) e comparar
diretamente contra `cDynamicAnalysis`/`cDynamicIntegrator` do ANFLEX real (`trunk/src`), mesma
metodologia que achou a rotação total-vs-local na estática.

## Eixo 2 — Pipeline de dados (desbloqueia mais casos de teste reais, baixo risco)

### 2a. Religar `aml_reader.py` ao schema real do `ModelBuilder`

Achado em [`mapa_aml_exemplos_e_web_interface.md`](mapa_aml_exemplos_e_web_interface.md): hoje, 23
dos 30 exemplos disponíveis (todos sem pasta `_analysis/` com XML/H5 pré-exportado) rodam
silenciosamente com o modelo sintético de fallback — `aml_reader.py` produz um schema que
`ModelBuilder` nunca entende. Consertar isso (gerar `model.nodes`/`model.elements` a partir dos
segmentos já extraídos, resolver corrente/solo por ID como `xml_h5_reader.py` já faz) é um trabalho
bem-escopado e de baixo risco (não toca o C++, só o parser Python), e destrava candidatos reais
novos ao bug solo+corrente — em especial `DNV_Check.aml` (solo e corrente confirmados casando por
ID, caminho estático).

### 2b. Suporte a múltiplas zonas de solo por segmento (opcional, avaliar sob demanda)

Achado no `Boiao/P52_Boiao.aml` (uma linha atravessando 3 solos diferentes ao longo do próprio
comprimento) — nem `aml_reader.py` nem `xml_h5_reader.py` correlacionam isso hoje, ambos assumem
solo único global. Só vale a pena se um caso de teste real desse tipo entrar em uso.

## Eixo 3 — Interfaces (podem começar em paralelo aos eixos 1-2, escopadas ao que o motor já suporta)

### 3a. Interface de entrada de dados

Arquitetura já desenhada em `mapa_aml_exemplos_e_web_interface.md`: dois JSONs (um "de interface",
próximo ao modelo lógico — linha com segmentos referenciando material/solo/corrente por nome; um
"de simulação", o schema atual e inalterado que `ModelBuilder` já lê), com um compilador JS
rodando no navegador. Falta: desenhar o formato concreto do JSON de interface, construir o
formulário (reaproveitando a casca de UI já existente — `TabPanel`, `PanelResizer`, `ThemeToggle`,
toolbar de câmera 3D — hoje só usada para visualização, nunca para edição), escrever o compilador.
Escopo inicial deve ficar restrito ao que o motor já resolve hoje: uma linha só, um material de
seção genérico, sem boias/tendões/turret/ruptura (ver Eixo 5).

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

### 3c. Pós-processamento

Já existe uma base sólida (`posprocessor.html`, viewer 3D, gráficos Plotly). O trabalho principal
aqui é integrar com o conceito de "projeto/rodada" do item 3b (carregar resultados de uma rodada
específica do histórico, em vez de sempre apontar para um arquivo fixo) — depende de 3b existir
primeiro para fazer sentido completo, mas a base de visualização já não precisa de retrabalho.

## Backlog de recursos faltantes do motor (não bloqueante — puxar sob demanda)

Achados documentados em `mapa_aml_exemplos_e_web_interface.md`, nenhum implementado: múltiplas
linhas com corpo flutuante compartilhado (`Multilinhas.aml`), boias/tendões como entidade própria
(hoje só existe `BuoyancyModule`/`BendRestrictor`, modificadores locais não lidos por
`ModelBuilder::load_from_json`), conexões articuladas tipo flexjoint/drilljoint, turret com
movimento prescrito 6-GDL por caso de carga (`Turret.aml`), ruptura de elemento em tempo de
execução dinâmico (`Ruptura.aml`), verificação de código DNV como pós-processamento
(`DNV_Check.aml`). Cada um só vale a pena quando um caso de teste real concreto precisar dele — não
faz sentido implementar especulativamente.

## Ordem sugerida (não é obrigatória — ponto de partida pra decidir)

1. ~~**1a** (bug estático)~~ — ✅ resolvido (era um bug de dados no perfil de corrente, não
   numérico). Confiança no motor pra Exemplo_01a estático agora estabelecida.
2. **2a** (religar `aml_reader.py`) — barato, baixo risco, abre mais um caso de teste real
   (`DNV_Check`) que pode ajudar a confirmar 1a de forma independente.
3. 🟡 **1b** (dinâmica) — em progresso: achado e corrigido um bug real de massa (`rho_structural`),
   dinâmica foi de 0/20 pra 15/20 passos de tempo convergindo no Exemplo_01a. Continuar com a
   mesma disciplina (Rayleigh damping é o próximo dado ainda não auditado).
4. **3a** (entrada de dados) — pode começar o desenho/construção em paralelo a tudo acima, já que é
   front-end puro e não depende do motor mudar; só precisa ficar escopado ao que o motor já resolve.
5. **3b** (controle de simulação) — a peça que mais se beneficia de vir depois, já que introduz
   infraestrutura nova; faz mais sentido desenhar quando já houver mais clareza sobre 1b (o que
   "acompanhar uma simulação" precisa mostrar depende de quão bem-comportado o solver já está).
6. **3c** (pós-processamento) — reboca 3b.
7. **Backlog de recursos faltantes** — sob demanda, conforme cada item vire necessário pra um caso
   de teste real específico.

## Ver também

- [`mapa_classes_anflex_estatica.md`](mapa_classes_anflex_estatica.md) — investigação do solver e
  do bug de convergência solo+corrente (Eixo 1a).
- [`mapa_classes_anflex_interface.md`](mapa_classes_anflex_interface.md) — mapa da interface
  gráfica real do ANFLEX (base para o Eixo 3a).
- [`mapa_aml_exemplos_e_web_interface.md`](mapa_aml_exemplos_e_web_interface.md) — censo de
  exemplos, lacuna do `aml_reader.py` (Eixo 2a), e arquitetura de dois JSONs (Eixo 3a).
