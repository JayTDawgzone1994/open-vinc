# Working Style

This document records design philosophy, aesthetics, and preferences for comparing approaches. [AGENTS.md](AGENTS.md) owns workflow, authorization, and mandatory shared contracts. [NAMING.md](NAMING.md) owns formal terminology.

## 1. What the Owner Values

- Architectural consistency over short-term development speed.
- Readability and clear responsibility boundaries over clever techniques.
- Generality must not be sacrificed for labels such as zero-copy or lock-free.
- Real workloads and practical deployment over impressive benchmarks.
- The framework should expose pressure and bottlenecks rather than silently conceal them.
- Names should let a first-time reader roughly infer their purpose. Avoid excessive metaphor.
- Component locality matters: entering a component directory should reveal its API, implementation, tests, and build configuration.
- Solve problems at build time when appropriate; do not defer them to runtime without a reason.

## 2. How to Discuss Designs

- Explain trade-offs, rather than only whether something is possible.
- When comparing approaches, prioritize responsibility boundaries, maintenance cost, user intuition, and runtime cost.
- For framework APIs, first distinguish programmer errors from runtime failures, then identify runtime causes such as capacity exhaustion or resource acquisition failure. Reporting and exception boundaries follow the error contracts in [AGENTS.md](AGENTS.md). This classification does not authorize expanding exception handling.
- Do not adopt an approach simply because it is common. Explain whether it fits Open-VINC's philosophy.
- If the user's proposal is simpler, do not add abstraction merely to appear more sophisticated.

## 3. Expectations for AI Output

- Successful compilation alone does not constitute completion.
- Proactively check naming, duplicate logic, hidden dependencies, error paths, lifecycle, and ownership.
- Prefer code that clearly lays out operation order. Follow the concrete [AGENTS code readability rules](AGENTS.md#code-readability).
- Do not silently introduce exceptions, allocations, background threads, or runtime registries.
- Explain the specific problem solved by a new abstraction, target, directory, or helper, along with its maintenance cost.
- Readability refactors need concrete benefits. Avoid large rewrites of functionally correct code based only on personal taste. Confirm the change scope through the authorization process in AGENTS.
