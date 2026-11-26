# ADR 0073 — Pull request previews on the production origin of GitHub Pages: a recorded risk

- Status: Proposed, 26 Nov 2026 (the choice is the owner's: Pierre)
- Raised by: an audit of the release and CI machinery, 26 Nov 2026; written in ticket FIXDOC.
- Concerns: `.github/workflows/pages.yml` (its input `preview_pr`), ADR 0020 (GitHub Pages), ADR
  0072 (the site: the v0.19.0 lab at the root, the C lab under `c/`).
- Reversal cost: low for each option (one workflow).

## Context

A manual run of `pages.yml` with `preview_pr: <number>` builds that pull request's C lab and
publishes it under `https://guilbep.github.io/bimble/preview/pr-<number>/`, beside the published
labs, until the next deployment. The build runs without any right to deploy (the job `preview`),
so the pull request's code cannot touch the deployment itself.

The risk is in the browser. The preview is served from the same origin as the published labs
(`https://guilbep.github.io`). Pages of one origin share its storage: `localStorage` and
IndexedDB. The C lab keeps the user's settings in `localStorage` (`web-ui/src/ccore.ts`). A
preview's JavaScript can read and change what the published labs keep there, for anyone who opens
the preview's link. A pull request from a fork is code nobody here wrote.

What limits it today: only someone who can run workflows in the repository can start a preview;
the labs keep no secret and no token (the C lab in the browser has no server and no account); the
preview disappears at the next deployment.

## Options

1. **Keep it as it is** (the current behaviour), with the rule written in the workflow's header:
   start a preview only for a pull request whose code you have read. Cost: none. Risk: as above,
   bounded by who can start a run.
2. **Previews only of this repository's own branches**: refuse `preview_pr` when the pull
   request comes from a fork (its head repository is not this one ; `gh pr view --json headRepositoryOwner`).
   Cost: a few lines. Risk: what the repository's own branches do.
3. **Previews on another origin**: a second repository (or another Pages site) that hosts only
   previews, so they never share the labs' storage. Cost: a repository and a token with the right
   to push there. Risk: none for the production origin.
4. **Previews as artifacts only**: build the preview and keep it as a workflow artifact (or a
   release asset) to download and open locally; no web address. Cost: none. Loses the reason the
   preview exists (opening a branch on a phone).
5. **Remove the preview.** Not done here: the ticket left the preview in place.

## Decision

None yet: the owner chooses. Until then option 1 holds, and the header of `pages.yml` names the
risk.
