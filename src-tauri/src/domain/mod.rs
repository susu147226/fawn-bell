//! 核心域（执行版 §12.2）——纯逻辑，**不做任何 IO**，可穷举单测。
//!
//! 模块名用 `domain` 而不是 `core`，避免与 Rust 内置的 `core` crate 在路径解析上打架。

pub mod aggregate;
pub mod archive;
pub mod dedupe;
pub mod drafts;
pub mod guard;
pub mod ignore;
pub mod kind;
pub mod naming;
