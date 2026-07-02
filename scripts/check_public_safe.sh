#!/usr/bin/env bash
# Guards against re-leaking private edge (IBKR code, curated tuning/watchlists,
# personal PII) into the public showcase repo. Run automatically by the
# pre-push hook (.githooks/pre-push) and by CI (.github/workflows/public-safety.yml).
#
# Usage: check_public_safe.sh <ref> [<base_ref>]
#   <ref>       git ref/sha to check the tree of (required)
#   <base_ref>  if given, only the diff base_ref..ref is scanned for secret
#               patterns (fast path for pushes); if omitted, the whole tree
#               at <ref> is scanned instead (used for full-history checks).
set -euo pipefail

REF="${1:?usage: check_public_safe.sh <ref> [<base_ref>]}"
BASE="${2:-}"
ROOT="$(git rev-parse --show-toplevel)"
DENYLIST_FILE="$ROOT/.public-denylist"
EMPTY_TREE="4b825dc642cb6eb9a060e54bf8d69288fbee4904"
FAIL=0

echo "[check_public_safe] Checking ${REF} against ${DENYLIST_FILE}..."

# 1. Forbidden paths must not exist anywhere in the tree at REF.
while IFS= read -r pattern; do
  pattern="$(echo "$pattern" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
  [[ -z "$pattern" || "$pattern" == \#* ]] && continue
  matches="$(git ls-tree -r --name-only "$REF" | grep -F "$pattern" || true)"
  if [[ -n "$matches" ]]; then
    echo "[check_public_safe] FORBIDDEN PATH present at ${REF}:"
    echo "$matches" | sed 's/^/    /'
    FAIL=1
  fi
done < "$DENYLIST_FILE"

# 2. Secret/PII regex scan — prefers gitleaks if available, else a basic
#    fallback so the hook still catches the obvious cases with zero deps.
DIFF_RANGE="${BASE:-$EMPTY_TREE}..${REF}"
if command -v gitleaks >/dev/null 2>&1; then
  GITLEAKS_ARGS=()
  [[ -f "$ROOT/.gitleaks.toml" ]] && GITLEAKS_ARGS+=(--config "$ROOT/.gitleaks.toml")
  [[ -f "$ROOT/.gitleaks-baseline.json" ]] && GITLEAKS_ARGS+=(--baseline-path "$ROOT/.gitleaks-baseline.json")
  # Note: --redact is deliberately omitted — it breaks --baseline-path
  # matching in current gitleaks builds (fingerprints stop matching), and a
  # genuinely new finding needs to be visible to whoever's fixing it anyway.
  if ! gitleaks detect --source "$ROOT" "${GITLEAKS_ARGS[@]}" --log-opts="$DIFF_RANGE" -v; then
    echo "[check_public_safe] gitleaks flagged a potential secret in ${DIFF_RANGE}."
    FAIL=1
  fi
else
  echo "[check_public_safe] gitleaks not found — using fallback regex scan (install gitleaks for stronger coverage)."
  DIFF_CONTENT="$(git diff "$DIFF_RANGE" -- . 2>/dev/null || true)"
  ADDED_LINES="$(echo "$DIFF_CONTENT" | grep -E '^\+' | grep -Ev '^\+\+\+' || true)"
  if echo "$ADDED_LINES" | grep -EIn '[A-Za-z0-9._%+-]+@(gmail|yahoo|outlook|hotmail|proton|icloud)\.[a-z]{2,}' >/dev/null; then
    echo "[check_public_safe] Possible personal email address added in ${DIFF_RANGE}:"
    echo "$ADDED_LINES" | grep -EIn '[A-Za-z0-9._%+-]+@(gmail|yahoo|outlook|hotmail|proton|icloud)\.[a-z]{2,}' | sed 's/^/    /'
    FAIL=1
  fi
  if echo "$ADDED_LINES" | grep -EIn '(api[_-]?key|secret[_-]?key|access[_-]?token|bearer)["'\'']?\s*[:=]\s*["'\''][A-Za-z0-9_\-]{16,}["'\'']' >/dev/null; then
    echo "[check_public_safe] Possible hardcoded key/token added in ${DIFF_RANGE}:"
    echo "$ADDED_LINES" | grep -EIn '(api[_-]?key|secret[_-]?key|access[_-]?token|bearer)["'\'']?\s*[:=]\s*["'\''][A-Za-z0-9_\-]{16,}["'\'']' | sed 's/^/    /'
    FAIL=1
  fi
fi

if [[ "$FAIL" -ne 0 ]]; then
  echo "[check_public_safe] BLOCKED — fix the above before this reaches the public repo."
  exit 1
fi
echo "[check_public_safe] OK."
