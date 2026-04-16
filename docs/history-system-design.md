# DoRun History System Design

## Goal

Add a launcher history system that:

- records successful launches
- improves ranking based on frequency and recency
- supports graceful score aging
- keeps the implementation practical for Win32/C++ and MSVC
- avoids turning search quality into a pure "most used wins" list

This document is a design draft for implementation, not a final user manual.

## Storage Choice

Use SQLite for history persistence.

Storage location:

- `%LOCALAPPDATA%\DoRun\history.db`

Reasons:

- suitable for indexed lookup and updates
- easy to keep data bounded
- future-proof if history later expands into aliases, favorites, or analytics
- more robust than ad-hoc text formats for concurrent reads/writes and schema changes

Recommended companion files under the same directory:

- `%LOCALAPPDATA%\DoRun\history.db`
- `%LOCALAPPDATA%\DoRun\history.db-wal`
- `%LOCALAPPDATA%\DoRun\history.db-shm`

If SQLite WAL mode is enabled, DoRun should tolerate the `-wal` and `-shm` side files.

## Scope

The history system should only record successful launches triggered by DoRun.

It should apply to:

- items loaded from `Command.conf`
- items discovered from filesystem scanning

It should not record:

- failed launches
- mere search impressions
- selection changes without execution

## Functional Goals

The history system should provide:

1. launch history persistence
2. rank accumulation on successful launch
3. global rank aging when total rank grows too large
4. recency-aware ordering
5. fuzzy matching that remains primarily search-driven
6. bounded storage through pruning

## Config Surface

Recommended additions to `DoRun.toml`:

```toml
HISTORY_ENABLED = 1
HISTORY_DB_PATH = ""
HISTORY_RANK_SUM_LIMIT = 5000
HISTORY_DECAY_FACTOR = 0.9
HISTORY_PRUNE_BELOW = 1
HISTORY_MAX_ROWS = 5000
HISTORY_RECENCY_ENABLED = 1
HISTORY_FUZZY_ENABLED = 1
```

Notes:

- `HISTORY_DB_PATH = ""` means use the default `%LOCALAPPDATA%\DoRun\history.db`
- `HISTORY_RANK_SUM_LIMIT` controls when global decay is triggered
- `HISTORY_DECAY_FACTOR` should be in `(0, 1)`, default `0.9`
- `HISTORY_PRUNE_BELOW` default `1`
- `HISTORY_MAX_ROWS` is an additional safety ceiling

## Data Model

Use one primary table for launch history.

Suggested schema:

```sql
CREATE TABLE IF NOT EXISTS launch_history (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_key TEXT NOT NULL UNIQUE,
    display_name TEXT NOT NULL,
    command_line TEXT NOT NULL,
    working_directory TEXT NOT NULL DEFAULT '',
    source_kind INTEGER NOT NULL DEFAULT 0,
    run_count INTEGER NOT NULL DEFAULT 0,
    rank REAL NOT NULL DEFAULT 0,
    last_run_utc TEXT NOT NULL DEFAULT '',
    created_utc TEXT NOT NULL DEFAULT '',
    updated_utc TEXT NOT NULL DEFAULT ''
);
```

Suggested indexes:

```sql
CREATE INDEX IF NOT EXISTS idx_launch_history_rank
ON launch_history(rank DESC);

CREATE INDEX IF NOT EXISTS idx_launch_history_last_run
ON launch_history(last_run_utc DESC);

CREATE INDEX IF NOT EXISTS idx_launch_history_display_name
ON launch_history(display_name);
```

## Field Semantics

- `item_key`
  Stable logical key used to merge repeated launches of the same target.

- `display_name`
  Current item label shown in DoRun.

- `command_line`
  Raw command string as configured or discovered before environment expansion.

- `working_directory`
  Raw working directory string before environment expansion.

- `source_kind`
  Source classification.
  Suggested values:
  - `0 = scanned_file`
  - `1 = command_conf`
  - `2 = synthetic_or_future`

- `run_count`
  Lifetime successful launch count.
  Never decayed.

- `rank`
  Dynamic score used for historical preference.
  Decayed over time by the global aging mechanism.

- `last_run_utc`
  Last successful launch time in UTC ISO 8601.

- `created_utc`, `updated_utc`
  Audit and migration convenience.

## Stable Key Strategy

Do not key history by `display_name` alone.

Recommended key material:

```text
source_kind + '\n' + normalized_command_line + '\n' + normalized_working_directory
```

Normalization rules:

- trim surrounding whitespace
- preserve case for stored values
- use lowercase only for key comparison if needed
- keep raw `%ENVVAR%` text rather than expanded values for identity
- normalize empty working directory to `""`

Reason:

- the same display name may point to different targets
- the same executable may appear from different sources
- raw configured values should map consistently across runs

## Update Flow

On successful launch:

1. resolve the target item in memory
2. compute `item_key`
3. open SQLite database
4. begin transaction
5. upsert history record
6. increment `run_count`
7. increment `rank` by `1.0`
8. set `last_run_utc` and `updated_utc` to now
9. compute total `rank`
10. if total rank exceeds threshold, apply global decay
11. prune rows with `rank < HISTORY_PRUNE_BELOW`
12. optionally prune excess rows if `row_count > HISTORY_MAX_ROWS`
13. commit transaction

If any step fails:

- do not block launching the application
- keep launcher behavior successful
- fail the history update silently or log internally

## Aging Mechanism

Default global aging strategy:

1. after each successful launch update, compute:

```sql
SELECT COALESCE(SUM(rank), 0) FROM launch_history;
```

2. if total `rank > HISTORY_RANK_SUM_LIMIT`, run:

```sql
UPDATE launch_history
SET rank = rank * :decay_factor,
    updated_utc = :now_utc;
```

3. prune:

```sql
DELETE FROM launch_history
WHERE rank < :prune_below;
```

Recommended defaults:

- `HISTORY_RANK_SUM_LIMIT = 5000`
- `HISTORY_DECAY_FACTOR = 0.9`
- `HISTORY_PRUNE_BELOW = 1`

Why this works:

- hot commands remain hot
- abandoned commands fade naturally
- global score mass stays bounded
- implementation remains deterministic and cheap

## Recency Model

Do not replace rank with a full Mozilla frecency clone in the first implementation.

Use a simpler frecency-lite approach:

- historical preference comes from `rank`
- fresh usage gets a recency bonus at search time

Suggested recency bonus buckets:

- last 1 day: `+30`
- last 7 days: `+20`
- last 30 days: `+10`
- last 90 days: `+5`
- older: `+0`

Alternative smoother function:

```text
recency_bonus = max(0, 30 - log2(hours_since_last_run + 1) * 3)
```

Recommendation:

- start with bucketed recency
- move to continuous scoring only if tuning later requires it

## Matching Model

Search should remain match-first, history-second.

Recommended final score:

```text
final_score = match_score * 1000 + history_score * 20 + recency_bonus
```

Where:

- `match_score` dominates ranking
- `history_score` comes from `rank`
- `recency_bonus` breaks ties and boosts recently used commands

This prevents a popular but weakly matching command from outranking an obvious exact match.

## Fuzzy Matching Design

Reference inspiration:

- Mozilla frecency for recency + frequency thinking
- zoxide for practical fuzzy and subsequence-oriented ranking

Do not copy either algorithm verbatim.

Suggested matching stages:

1. exact case-insensitive match on `display_name`
2. prefix match on `display_name`
3. contiguous substring match on `display_name`
4. subsequence match on `display_name`
5. match on `command_line`

Suggested tokenization:

- split user query on spaces
- each token must match somewhere in the candidate for strong results
- allow weaker fallback when only part of the tokens match

Suggested `match_score` tiers:

- exact `display_name`: `100`
- prefix `display_name`: `90`
- contiguous substring in `display_name`: `75`
- subsequence in `display_name`: `60`
- prefix in basename of command target: `55`
- substring in `command_line`: `40`
- subsequence in `command_line`: `25`

Additional bonuses:

- earlier match start index
- longer contiguous run
- token order preserved

Penalties:

- large gaps in subsequence matching
- matches only in deep path segments

## zoxide-Inspired Behavior

Useful ideas to borrow:

- multi-token query support
- subsequence-style matching rather than plain substring only
- stronger weight for matches on short human-facing names
- path-aware fallback when name is weak

Ideas not necessary in first version:

- path jumping semantics
- advanced directory segment heuristics specific to shell navigation

## Launch Ranking Pipeline

Recommended ranking order:

1. build candidate set
2. compute `match_score`
3. fetch history entry by `item_key`
4. derive:
   - `history_score = rank`
   - `recency_bonus`
5. compute `final_score`
6. sort descending by:
   - `final_score`
   - stronger `match_score`
   - higher `rank`
   - newer `last_run_utc`
   - alphabetic fallback

## SQL Operations

Suggested upsert:

```sql
INSERT INTO launch_history (
    item_key,
    display_name,
    command_line,
    working_directory,
    source_kind,
    run_count,
    rank,
    last_run_utc,
    created_utc,
    updated_utc
)
VALUES (
    :item_key,
    :display_name,
    :command_line,
    :working_directory,
    :source_kind,
    1,
    1.0,
    :now_utc,
    :now_utc,
    :now_utc
)
ON CONFLICT(item_key) DO UPDATE SET
    display_name = excluded.display_name,
    command_line = excluded.command_line,
    working_directory = excluded.working_directory,
    source_kind = excluded.source_kind,
    run_count = launch_history.run_count + 1,
    rank = launch_history.rank + 1.0,
    last_run_utc = excluded.last_run_utc,
    updated_utc = excluded.updated_utc;
```

## Lifecycle and Initialization

At startup:

1. resolve history DB path
2. ensure `%LOCALAPPDATA%\DoRun` exists
3. open SQLite database
4. initialize schema if needed
5. enable pragmatic SQLite settings

Suggested SQLite pragmas:

```sql
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA foreign_keys=ON;
```

If WAL causes deployment concerns, fallback to default journaling is acceptable.

## Error Handling

History should be best-effort.

Rules:

- launcher startup must not fail just because SQLite cannot open
- search must continue even if history is temporarily unavailable
- failed history updates should not show modal dialogs during normal use
- optional debug logging can be added later

## Migration Considerations

If old history formats ever exist, migration order should be:

1. detect old format
2. import into SQLite
3. validate row count
4. rename old file to backup

For the first release of this feature, no migration is needed.

## Implementation Notes for C/C++

Suggested modules:

- `src/history.h`
- `src/history.cpp`
- `src/history_sqlite.cpp`

Suggested responsibilities:

- configuration resolution
- DB open/init
- launch record upsert
- aging/pruning
- score lookup for current results

Potential in-memory helper structure:

```text
struct HistoryInfo {
    double rank;
    int runCount;
    std::wstring lastRunUtc;
};
```

Current search rebuild can load matching history metadata into memory once per launcher show, rather than executing one query per row during painting.

## Suggested Rollout Plan

Phase 1:

- SQLite schema
- write history on successful launch
- simple rank lookup

Phase 2:

- global aging and pruning
- recency bonus

Phase 3:

- improved fuzzy matching
- scoring tune-up

Phase 4:

- optional UI exposure, such as recent items or debugging details

## Open Questions

1. Should history include failed launches for debugging only, in a separate table?
2. Should there be a user-visible command to clear history?
3. Should `command_line` and `working_directory` updates overwrite old values when the same `item_key` changes?
4. Should an item removed from `Command.conf` remain in history but simply never surface unless rediscovered?

## Recommendation

Implement the first production version as:

- SQLite-backed
- rank-based with global decay
- bucketed recency bonus
- lightweight zoxide-inspired fuzzy matching

This gives DoRun a strong practical ranking model without over-engineering the initial release.
