//! The tierlink byte formats that the server reads and writes (include/tierlink/wire.hpp).

pub struct Header {
    pub seq: u64,
}

fn u64_at(b: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(b[at..at + 8].try_into().unwrap())
}

/// The data header in the Zenoh attachment: seq, stream_seq, sent_ns, keyframe.
pub fn decode_header(b: &[u8]) -> Option<Header> {
    (b.len() == 25).then(|| Header { seq: u64_at(b, 0) })
}

/// The sequence acknowledged and the receiver's clock when it arrived.
pub fn encode_ack(seq: u64, received_ns: u64) -> Vec<u8> {
    let mut out = seq.to_le_bytes().to_vec();
    out.extend_from_slice(&received_ns.to_le_bytes());
    out
}

pub struct CatalogEntry {
    pub name: String,
    pub msg_type: String,
    pub topic: String,
}

/// One stream per line: name, type, topic and delivery, tab separated.
pub fn decode_catalog(b: &[u8]) -> Vec<CatalogEntry> {
    String::from_utf8_lossy(b)
        .lines()
        .filter_map(|l| {
            let f: Vec<&str> = l.split('\t').collect();
            (f.len() == 4).then(|| CatalogEntry { name: f[0].into(), msg_type: f[1].into(), topic: f[2].into() })
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_ack_and_catalog_layouts() {
        let mut b = 3u64.to_le_bytes().to_vec();
        b.extend_from_slice(&9u64.to_le_bytes());
        b.extend_from_slice(&5u64.to_le_bytes());
        b.push(0);
        let h = decode_header(&b).unwrap();
        assert_eq!(h.seq, 3);
        assert!(decode_header(&b[..24]).is_none());
        assert_eq!(&encode_ack(3, 4)[8..], &4u64.to_le_bytes());
        let c = decode_catalog(b"odom\tnav_msgs/msg/Odometry\t/odom\tlatest\nbad line\n");
        assert_eq!(c.len(), 1);
        assert_eq!(c[0].topic, "/odom");
    }
}
