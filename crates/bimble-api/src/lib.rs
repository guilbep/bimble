//! One API over the model for the web UI, the viewer, the CLI and MCP agents (ADR 0048).
//! Queries first: small serde summaries, never whole meshes.

pub mod query;
pub mod verbs;
