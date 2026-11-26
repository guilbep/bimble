/* `bimble-ccore mcp [--read-only] <model>`: an MCP server over stdio (L6 carrier, ADR 0068 rule 5,
 * ADR 0048), the twin of crates/bimble-cli/src/mcp.rs. JSON-RPC 2.0, one message per line on
 * stdin (at most 1 MiB; a longer line is refused without being parsed), one answer per line on
 * stdout, logs on stderr. Methods initialize, ping, tools/list, tools/call; the tools list_verbs,
 * describe_verb, run_verb, query, undo, redo and save, with Rust's names, texts and error codes.
 * The tools run on the verb table (verb_table.h), the model queries (query.h) and the history
 * (history.h), on the server's own model, as mcp.rs runs dispatch and History: nothing is written
 * until `save`. golden/mcp/ holds Rust's transcript; test/mcp_test.mjs replays it.
 *
 * C additions (golden/DEVIATIONS.md "MCP"), outside Rust's transcript:
 * - run_verb and describe_verb also know the C additions of the verb table (rename, …);
 *   list_verbs stays Rust's 14, as the session's `verbs` query.
 * - run_verb's "args" may carry "dry_run": true (answers {"commands":[…],"rev":N}, nothing applied)
 *   and "if_rev": N (runs only when N is the server's rev, else the session's "conflict: …" text).
 *   rev is 1 after the load and + 1 for each history change (a step applied, undone or redone).
 * - --read-only: run_verb (but a dry run), undo, redo and save are refused. */
#ifndef CCORE_HOST_MCP_H
#define CCORE_HOST_MCP_H

/* argv[0] is "mcp". Serves until the end of stdin. Returns the process exit code: 0 at the end of
 * input, 1 when the model does not load or stdout fails, 2 on a usage error. */
int cc_mcp_main(int argc, char **argv);
#endif
