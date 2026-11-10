//! Little-endian headers. Data carries its header in the Zenoh attachment, so the payload stays
//! exactly what the codec produced.

pub const HEADER_LEN: usize = 17;
pub const ACK_LEN: usize = 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Header {
    /// Per session, increasing in sending order across all streams of the tier.
    pub seq: u64,
    pub sent_ns: i64,
    pub keyframe: bool,
}

impl Header {
    pub fn encode(&self) -> [u8; HEADER_LEN] {
        let mut b = [0; HEADER_LEN];
        b[..8].copy_from_slice(&self.seq.to_le_bytes());
        b[8..16].copy_from_slice(&self.sent_ns.to_le_bytes());
        b[16] = u8::from(self.keyframe);
        b
    }

    pub fn decode(b: &[u8]) -> Option<Self> {
        let b: &[u8; HEADER_LEN] = b.try_into().ok()?;
        Some(Self {
            seq: u64::from_le_bytes(b[..8].try_into().ok()?),
            sent_ns: i64::from_le_bytes(b[8..16].try_into().ok()?),
            keyframe: b[16] != 0,
        })
    }
}

/// The sequence acknowledged and the receiver's clock when it arrived.
pub fn encode_ack(seq: u64, received_ns: i64) -> [u8; ACK_LEN] {
    let mut b = [0; ACK_LEN];
    b[..8].copy_from_slice(&seq.to_le_bytes());
    b[8..].copy_from_slice(&received_ns.to_le_bytes());
    b
}

pub fn decode_ack(b: &[u8]) -> Option<(u64, i64)> {
    let b: &[u8; ACK_LEN] = b.try_into().ok()?;
    Some((
        u64::from_le_bytes(b[..8].try_into().ok()?),
        i64::from_le_bytes(b[8..].try_into().ok()?),
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_and_ack_round_trip() {
        let h = Header {
            seq: 42,
            sent_ns: -7,
            keyframe: true,
        };
        assert_eq!(Header::decode(&h.encode()), Some(h));
        assert_eq!(decode_ack(&encode_ack(9, 123)), Some((9, 123)));
        assert_eq!(Header::decode(&[0; 3]), None);
    }
}
