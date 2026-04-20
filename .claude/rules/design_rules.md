# Design Rules

Apply these rules whenever reading or writing any file in `src/` (C++) or `*.py` (Python).
Check compliance before finishing any edit — do not leave violations in new or modified code.

---

## C++ Rules

### Naming: camelCase

| Applies to | Rule | Example |
|---|---|---|
| Member variables | camelCase + trailing `_` | `myLaneIndex_`, `vehicleDB_` |
| Local variables | camelCase | `numEntries`, `batchIndex` |
| Function names | camelCase | `computePassOrder()`, `sendDbResponse()` |
| Struct fields | camelCase | `vehicleId`, `laneIndex`, `isFirstInLane` |
| Enum values | UPPER_SNAKE | `PASS_ORDER`, `VEHICLE_LEFT` |
| Classes | PascalCase | `RaftAppBase`, `UdpRaftApplication` |

Do not use snake_case, PascalCase for variables, or Hungarian notation.

### Braces: always required

**Every `if`, `else`, `for`, `while`, and `do` body must use `{}`**, even single-line bodies.

```cpp
// WRONG — never do this
if (condition)
    doSomething();

for (int i = 0; i < n; i++)
    process(i);

// CORRECT
if (condition) {
    doSomething();
}

for (int i = 0; i < n; i++) {
    process(i);
}

if (a) {
    foo();
} else {
    bar();
}
```

No exceptions. Single-line `{}` on the same line is acceptable only for trivial getters:
```cpp
int getId() const { return myId_; }
```

### Comments

Write no comments by default. Only add one when the WHY is non-obvious: a hidden constraint, a workaround for a specific bug, or behaviour that would surprise a reader. Never describe WHAT the code does — well-named identifiers already do that.

### No dead code

Remove any variable, function, or parameter that is no longer used. Do not leave commented-out code.

---

## Python Rules (`plot_comparison.py` and any other `.py` files)

### Naming: snake_case

| Applies to | Rule | Example |
|---|---|---|
| Variables | snake_case | `run_data`, `fallback_rate` |
| Functions | snake_case | `load_runs_for_mode()`, `plot_ambulance()` |
| Constants | UPPER_SNAKE | `RESULTS_DIR`, `VEHICLE_COUNTS` |
| Classes | PascalCase | `RunData` |

### Braces / block bodies

Python has no braces, but the same "no implicit single-liners" spirit applies:
- Never use ternary expressions that span more than one concept
- Never chain more than two list comprehensions on one line — split to a loop instead

### Imports

Keep imports grouped: stdlib first, then third-party (`numpy`, `matplotlib`), then local. One blank line between groups. No wildcard imports.

### Magic numbers

No bare numeric literals in plot logic. Use named constants at the top of the file:
```python
VEHICLE_COUNTS = [4, 8, 16]
PROTOCOLS = ['wave', 'udp']
```

### No silent failures

Do not use bare `except: pass`. At minimum log with `print(f"WARNING: {e}")` so anomalies are visible.
