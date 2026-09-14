# Contributing to MarketLab

Keep contributions focused on one coherent change. Discuss major behavioral or architectural changes before implementation, especially changes to exchange semantics, determinism, event ordering, ownership, or accounting.

Changes to exchange behavior require tests that establish the intended semantics. Relevant tests must pass before merge, and feature work should not be mixed with unrelated refactoring. Performance claims must include reproducible measurements and enough environment and workload detail for another contributor to verify them.

Pull requests should explain what changed, why it is needed, and how it was validated. Public documentation must remain consistent with user-visible behavior.

## Release policy

MarketLab will use semantic versioning once releases begin. Releases should correspond to usable functionality rather than arbitrary dates. Before a release, relevant tests must pass, public documentation must match behavior, benchmark claims must be reproducible, and release notes must describe user-visible changes.

Creating a release or tag requires explicit maintainer approval.
