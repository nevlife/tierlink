//! What each stream is, how its pending data is kept, and which stream sends next.

mod outbox;
mod policy;
mod scheduler;
mod spec;

pub use outbox::{Item, Outbox, Pushed};
pub use policy::{Allowance, TierPolicy};
pub use scheduler::Scheduler;
pub use spec::{Delivery, StreamSpec, Tier};
