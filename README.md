# MarketLab

> Deterministic local exchange infrastructure for quantitative trading research and trading-system testing.

MarketLab is designed to provide a controlled electronic-market environment in which strategies and trading systems can interact with exchange state under reproducible conditions. It addresses experiments that ordinary historical backtests cannot model faithfully, including order-level interaction, queue priority, liquidity, latency, competing participants, and the sequence of events that produces an execution.

Unlike a conventional backtester, which generally starts from prices and trades that have already occurred, MarketLab targets the process that creates them:

**strategy decision -> order submission -> exchange processing -> matching -> execution -> market update -> strategy reaction**

## Initial scope

The initial scope focuses on a deterministic local exchange simulation with:

- continuous limit order books and price-time priority;
- limit and market orders, cancellations, partial fills, and executions;
- exact internal prices and deterministic logical time;
- controlled synthetic participants and reproducible scenarios;
- participant positions, cash, and profit-and-loss accounting;
- multiple independent instruments and simulated latency;
- event recording and a research-facing interface after the core is stable.

The first reference experiment targets a market-making strategy trading against controlled background participants. Live trading, brokerage connectivity, production exchange protocols, derivatives, multi-venue routing, and a sophisticated GUI are outside the initial scope.

## Engineering principles

- Correct exchange semantics come before performance.
- Identical configurations, engine versions, starting states, and seeds should produce identical logical outcomes.
- Prices inside the matching core use exact representations.
- Each order book has one logical writer; independent instruments may later have separate owners.
- Simulated timing uses logical time rather than wall-clock delays.
- Positions and cash change through executions, and market data is derived from exchange state.
- Performance work follows correctness, testing, and reproducible measurement.

## Development workflow

Development is organized around small, validated implementation steps. After each completed step, the local `PROGRESS.md` is updated with completed checklist items, concise implementation details, relevant validation, important design decisions, and real benchmark results where applicable. The implementation sequence prioritizes specification -> correctness -> testing -> measurement -> optimization. `PROGRESS.md` remains local and is not part of the public repository.

MarketLab is simulation infrastructure and does not place real trades.
