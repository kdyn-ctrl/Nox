# Clean Two-Repo Workflow

Single local repo, two remotes. Work locally, push selectively.

---

## Setup Summary

✅ **One folder:** `/root/Nox` (source of truth — has all files)  
✅ **Two remotes:** 
- `private` → full repo (kdyn-ctrl/Nocturnal on GitHub)
- `upstream` → public-safe subset (kdyn-ctrl/Nox on GitHub)

✅ **No feature branches on remotes** → Keep remotes clean, work locally only  
✅ **Pre-push hook** → Auto-checks before pushing to upstream

---

## Daily Workflow

### **Feature work (local only)**

1. **Create a feature branch locally:**
   ```bash
   git checkout -b my-feature
   # (Never pushed to remotes — stays local)
   ```

2. **Make your changes:**
   ```bash
   git add .
   git commit -m "your message"
   ```

3. **Merge back to main and delete the branch:**
   ```bash
   git checkout main
   git merge my-feature
   git branch -d my-feature
   ```

### **Push to private (always)**

After merging, push to private:
```bash
git push private main
```

✅ Your work is backed up on private. Feature branches keep remotes clean.

### **Push to public (selective)**

When you want to share code:

```bash
# Review what you're about to push
git diff upstream/main..HEAD

# Push to public
git push upstream main
```

The pre-push hook checks automatically:
- ✅ Blocks files in `.public-denylist` (career docs, IBKR files, etc.)
- ✅ Scans for hardcoded secrets
- ✅ Allows everything else

---

## What Gets Checked Before Pushing to Public

When you do `git push upstream main`, the pre-push hook automatically:

✅ Checks `.public-denylist` (blocks IBKR files, career docs, etc.)  
✅ Scans for hardcoded secrets/credentials  
✅ Stops the push if anything flagged  

**You'll see this output:**
```
[check_public_safe] Checking...
[gitleaks] no leaks found
[check_public_safe] OK.
```

If something is flagged, you'll get an error and the push will be blocked.

---

## Common Scenarios

### **I made changes I only want in private (not public)**

1. Make your commits normally
2. Push to private: `git push private main` ✅
3. DON'T push to upstream
4. Next time you pull from private, those commits will come with you

### **I want to push only some commits to public**

```bash
# See what's different
git log --oneline upstream/main..HEAD

# Cherry-pick specific commits (or tell me to do it)
# For now, either:
# - Push everything if it's all public-safe
# - OR ask me to filter/cherry-pick specific changes
```

### **I want to edit the public repo directly**

```bash
git pull upstream main  # Get latest from public
# Make edits
git push upstream main  # Push back
```

Then next time you pull from private, you'll need to merge:
```bash
git pull private main
# Git will merge upstream and private versions
```

---

## What You Can Trust Me To Do

✅ I will run the safety check before any push  
✅ I will never push career docs, IBKR code, or secrets to public  
✅ I will review the diff (`git diff`) before pushing  
✅ I will tell you what I pushed (commit hashes + messages)  

**The pre-push hook is your safety net** — it will block anything flagged even if I miss it.

---

## Emergency: "Oops, something got pushed to public that shouldn't be"

1. Tell me immediately
2. I can revert the commit: `git revert <commit-hash>`
3. Or in critical cases, force-push a clean version

(This is rare thanks to the pre-push hook, but it's here if needed)

---

## Commands Cheat Sheet

| What You Want | Command |
|---|---|
| Create local feature branch | `git checkout -b my-feature` |
| Switch back to main | `git checkout main` |
| Merge feature to main | `git merge my-feature` |
| Delete merged feature branch | `git branch -d my-feature` |
| Push to private | `git push private main` |
| Push to public | `git push upstream main` |
| Pull latest from private | `git pull private main` |
| See what's not on public yet | `git log --oneline upstream/main..HEAD` |
| See diff vs public | `git diff upstream/main..HEAD` |
| Check status | `git status` |

---

## That's It!

You work in private. When you want to share, push to upstream. The hook checks automatically. Done.

Any questions, just ask.
