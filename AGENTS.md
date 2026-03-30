# Agent Instructions

## Post-Change Validation
- After making any changes, compile the project to validate correctness.
- Do not proceed if compilation fails.

## Feature Changes
- If any features are added or modified:
  - Create new unit tests and/or update existing ones as needed.
  - Include edge cases and negative scenarios.
  - Add tests that intentionally trigger compile errors where applicable.

## Test Execution
- Run all unit tests using 'run_tests.bat'
  - Ensure all tests pass unless failures are explicitly expected (e.g., intentional compile error cases).
