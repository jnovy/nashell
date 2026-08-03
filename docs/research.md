# Research Foundations

Nash's design is grounded in recent research on agentic memory systems, cognitive architectures, and LLM reasoning:

## Memory Architecture
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [Generative Agents](https://arxiv.org/abs/2304.03442) | 2023 | Composite scoring (recency x importance x relevance) | Hybrid scoring with semantic + substring + Bayesian validation |
| [Memory Survey](https://arxiv.org/abs/2404.13501) | 2024 | Five critical memory operations including validation | Bayesian validation scoring (hits/misses) |
| [CALMem](https://arxiv.org/abs/2605.20724) | 2026 | Token-budget-adaptive injection (MOIM) | Inspired budget-aware memory injection design |
| [Mem-pi](https://arxiv.org/abs/2605.21463) | 2026 | Generative memory policy, learned abstention +59% | Query-time synthesis with semantic abstention ("NONE") |
| [DeferMem](https://arxiv.org/abs/2605.22411) | 2026 | Query-time evidence distillation | Memory synthesis produces faithful, self-contained guidance |
| [MemForest](https://arxiv.org/abs/2605.23986) | 2026 | Temporal indexing, memory relevance changes over time | Inspired temporal relevance awareness in scoring design |
| [MemFail](https://arxiv.org/abs/2605.26667) | 2026 | Weak memory injection hurts performance | Bayesian scoring + abstention gate filters low-quality memories |
| [MemMorph](https://arxiv.org/abs/2605.26154) | 2026 | Raw storage insufficient, needs active management | Post-loop pruning + consolidation |
| [ByteRover](https://arxiv.org/abs/2604.01599) | 2026 | Agent-native hierarchical memory with zero external infrastructure; LLM curates its own Context Tree | Episodic recall from session index -- journals as agent-native episodic memory with no vector DB |
| [CogniFold](https://arxiv.org/abs/2605.13438) | 2026 | Always-on proactive memory via cognitive folding; extends CLS theory to 3 layers with graph self-organization | Working memory auto-promotion -- harness auto-saves findings to scratchpad without explicit agent action |
| [MemCog](https://arxiv.org/abs/2605.28046) | 2026 | Memory-as-Cognition: navigable memory store with associative link graphs and proactive reasoning protocol; SOTA on LoCoMo (92.98) and LongMemEval (95.8) | Memory-as-Cognition principle -- harness controls all retrieval timing; associative graph walk follows refs[] on recalled memories |
| [MRAgent](https://arxiv.org/abs/2606.06036) | 2026 | Memory is reconstructed, not retrieved: associative Cue-Tag-Content graph with active reconstruction; +23% on LoCoMo/LongMemEval (ICML 2026) | Associative graph walk: depth-1 ref following injects referenced memories during recall |
| [MemRefine](https://arxiv.org/abs/2606.13177) | 2026 | LLM-guided compression for budget-constrained long-term memory; similarity-based candidate pairs with delete/merge/preserve decisions | Informed design of memory pruning: aggressive dead-weight removal (73% never-recalled entries deleted) |

## Cognitive Architecture
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [OpenDev / Terminal AI Agents](https://arxiv.org/abs/2603.05344) | 2026 | Structural thinking separation -- remove tools during reasoning phase | Structural reasoning mode (default): two-call pattern (reason without tools -> act with tools) |
| [TriMem](https://arxiv.org/abs/2605.19952) | 2026 | Three-tier memory (working/episodic/semantic) | Scratchpad (working) + journal (episodic) + memory (semantic) |
| ["Language Models Need Sleep"](https://arxiv.org/abs/2605.26099) | 2026 | Dreaming/consolidation essential for memory health | Post-loop Bayesian pruning + dedup + consolidation |
| [MMPO](https://arxiv.org/abs/2605.30159) | 2026 | Belief Entropy H_BE measures memory clarity | Belief Entropy monitoring for memory quality signal |
| [Harness-1](https://arxiv.org/abs/2606.02373) | 2026 | Stateful cognitive offloading -- move bookkeeping from LLM to environment-side harness | Importance-tagged messages, multi-pass progressive eviction, sentence-BM25 compression, CRC32 context dedup, auto-seeding scratchpad, tool diversity nudge |

## Skill Extraction
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [CODESKILL](https://arxiv.org/abs/2605.25430) | 2026 | RL-trained skill extraction from completions | Post-task reflection extracts reusable lessons/strategies |
| [MUSE-Autoskill](https://arxiv.org/abs/2605.27366) | 2026 | Self-evolving skill library | Skills recalled semantically per query, refined via validation |

## Self-Improvement & Spec Optimization
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [OpenJarvis](https://arxiv.org/abs/2605.17172) | 2026 | Personal AI = 5 typed primitives (Intelligence, Engine, Agents, Tools, Learning) in a jointly-optimizable spec. LLM-guided spec search across all primitives recovers cloud-level accuracy on-device. | Unified spec (`--spec` / `--load-spec`), layered model profiles with 30+ override fields, sentinel-based cascade (Model Profiles, Unified Spec sections) |
| [Self-Harness](https://arxiv.org/abs/2606.09498) | 2026 | Weakness mining + proposal + validation gate | Postmortem analysis + regression testing + validation gate |
| [DCPM](https://arxiv.org/abs/2606.09483) | 2026 | Dual-process cognitive memory with async consolidation | Auto-dream: usage-based memory consolidation trigger |
| [SWE-Shepherd](https://arxiv.org/abs/2604.10493) | 2026 | Process Reward Models (PRMs) for step-level supervision in code agents | Step-level trajectory scoring in postmortem: productive/wasteful/harmful/spinning classification per tool call, causal step attribution for failures |
| [EvolveMem](https://arxiv.org/abs/2605.13941) | 2026 | Self-evolving memory architecture -- expose retrieval config as structured action space optimized by LLM diagnosis | Memory quality telemetry (journal `memory_quality` entries), self-harness retrieval diagnosis pass, data-driven tuning of retrieval params |
| [RHI](https://arxiv.org/abs/2607.15524) | 2026 | Recursive Harness Self-Improvement -- harnesses are data-generating components; pairwise feedback over revision history; gains from context management outweigh longer reasoning | `--optimize` as standard local-model onboarding step; validates prompt-level harness optimization yields disproportionate gains on low-reasoning-effort models |

## Context Management
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [CWL -- Context Window Lifecycle](https://arxiv.org/abs/2606.11213) | 2026 | Typed, dependency-linked episodes; deterministic LLM-free eviction based on recoverability | Recoverability-aware eviction: messages annotated with `RECOVER_NONE/SCRATCHPAD/STORE/FILE/MEMORY`, sorted by recoverability during Pass 3 eviction |
| [LCM -- Lossless Context Management](https://arxiv.org/abs/2605.04050) | 2026 | Recursive context compression via hierarchical summary DAG with lossless pointers | LCM-Lite: breadcrumb index of evicted store refs injected at eviction point, making eviction lossless via `file_read` recovery |

## Agentic Search & Retrieval
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [Chronos](https://arxiv.org/abs/2603.16862) | 2026 | Agentic harness evaluation framework; temporal event structuring as most impactful component; harness = retriever in impact on accuracy | Temporal event calendar; event-driven architecture validated by harness-vs-retriever finding |
| ["Is Grep All You Need?"](https://arxiv.org/abs/2605.15184) | 2026 | Grep beats vector for inline delivery; file-based delivery inverts rankings; temporal events most impactful single component; rendering = retrieval + orchestration | 40/60 semantic/substring blend (grep-favoring); inline-only memory delivery; temporal calendar; enriched rendering with recency/confidence metadata |
| [Retrieval Timing Bottleneck](https://arxiv.org/abs/2605.30621) | 2026 | Retrieval timing, not storage quality, is the bottleneck in memory-augmented agents; single retrieval at task start creates timing mismatch as agent needs evolve | Event-driven re-retrieval: eviction-triggered, cycling-triggered, error-triggered -- all using event content as the retrieval query |
| [Recursive Agent Harnesses](https://arxiv.org/abs/2606.13643) | 2026 | Parent agents spawn sub-agent harnesses; harness recursion improves Codex from 71.75% -> 81.36% on Oolong-Synthetic | Validates harness-investment approach -- orchestration matters more than model capability |
| [Ask Early, Ask Late, Ask Right](https://arxiv.org/abs/2605.07937) | 2026 | Clarification timing matters: goal clarification loses value after 10% execution; no frontier model asks within optimal window | `user_ask` tool with system prompt guidance to ask early when uncertainty >= 0.5 |
| [MemMachine](https://arxiv.org/abs/2604.04853) | 2026 | Ground-truth-preserving memory combining short-term, long-term episodic, and profile memory | Episodic recall: raw journal chunks preserve ground-truth tool sequences alongside distilled L4 memories |

## Additional References
| Paper | Year | Key Insight | Nash Implementation |
|-------|------|-------------|---------------------|
| [ActiveGraph](https://arxiv.org/abs/2605.21997) | 2026 | Typed edges between memory nodes | Memory tagging and cross-reference system |
| [MemIR](https://arxiv.org/abs/2605.25869) | 2026 | Provenance chains linking raw evidence | Journal + store provide full provenance for every artifact |
