use serde::Deserialize;

/// Remote control goes first; monitoring uses what is left.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Tier {
    Teleop,
    Monitor,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Delivery {
    /// Only the newest value matters.
    Latest,
    /// Every value is kept, in order, up to `capacity`.
    Reliable,
    /// Encoded frames; an unsent frame is replaced by a newer one.
    Frame,
}

#[derive(Clone, Debug, Deserialize)]
pub struct StreamSpec {
    pub name: String,
    pub tier: Tier,
    pub delivery: Delivery,
    /// Frames reference earlier ones (H.264), so a dropped frame breaks decoding until a keyframe.
    #[serde(default)]
    pub inter_frame: bool,
    #[serde(default = "default_capacity")]
    pub capacity: usize,
}

fn default_capacity() -> usize {
    256
}
