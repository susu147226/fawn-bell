//! 基础设施层（执行版 §12.2）——唯一允许做 IO 的地方。

pub mod db;
pub mod exif;
pub mod hash;
pub mod library;
pub mod paths;
pub mod refscan;
pub mod shell;
pub mod volume;
pub mod walker;
