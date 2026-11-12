//! The tierlink byte formats that the server reads and writes (include/tierlink/wire.hpp).

pub struct Header {
    pub seq: u64,
    pub stream_seq: u64,
    pub chunk: u16,
    pub chunks: u16,
}

fn u64_at(b: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(b[at..at + 8].try_into().unwrap())
}

fn u16_at(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([b[at], b[at + 1]])
}

/// The data header in the Zenoh attachment: seq, stream_seq, sent_ns, keyframe, chunk, chunks.
pub fn decode_header(b: &[u8]) -> Option<Header> {
    if b.len() != 29 {
        return None;
    }
    let h = Header { seq: u64_at(b, 0), stream_seq: u64_at(b, 8), chunk: u16_at(b, 25), chunks: u16_at(b, 27) };
    (h.chunks > 0 && h.chunk < h.chunks).then_some(h)
}

/// One stream's pieces back into items (tierlink `Reassembler`): a newer item drops an unfinished
/// older one, and a late piece of an older item is ignored.
#[derive(Default)]
pub struct Reassembler {
    newest: Option<u64>,
    pieces: Vec<Option<Vec<u8>>>,
    received: usize,
    done: bool,
}

impl Reassembler {
    pub fn add(&mut self, h: &Header, piece: Vec<u8>) -> Option<Vec<u8>> {
        if self.newest.is_some_and(|n| h.stream_seq < n) {
            return None;
        }
        if h.chunks == 1 {
            self.newest = Some(h.stream_seq);
            self.done = true;
            return Some(piece);
        }
        if self.newest != Some(h.stream_seq) || self.pieces.len() != h.chunks as usize {
            self.newest = Some(h.stream_seq);
            self.pieces = vec![None; h.chunks as usize];
            self.received = 0;
            self.done = false;
        }
        let slot = &mut self.pieces[h.chunk as usize];
        if self.done || slot.is_some() {
            return None;
        }
        *slot = Some(piece);
        self.received += 1;
        if self.received < self.pieces.len() {
            return None;
        }
        self.done = true;
        Some(std::mem::take(&mut self.pieces).into_iter().flatten().flatten().collect())
    }
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
        b.extend_from_slice(&1u16.to_le_bytes());
        b.extend_from_slice(&2u16.to_le_bytes());
        let h = decode_header(&b).unwrap();
        assert!(h.seq == 3 && h.stream_seq == 9 && h.chunk == 1 && h.chunks == 2);
        assert!(decode_header(&b[..28]).is_none());
        let mut r = Reassembler::default();
        let piece = |seq, chunk, chunks| Header { seq, stream_seq: seq, chunk, chunks };
        assert!(r.add(&piece(4, 1, 2), vec![3]).is_none());
        assert!(r.add(&piece(3, 0, 2), vec![9]).is_none());
        assert_eq!(r.add(&piece(4, 0, 2), vec![1, 2]), Some(vec![1, 2, 3]));
        assert_eq!(&encode_ack(3, 4)[8..], &4u64.to_le_bytes());
        let c = decode_catalog(b"odom\tnav_msgs/msg/Odometry\t/odom\tlatest\nbad line\n");
        assert_eq!(c.len(), 1);
        assert_eq!(c[0].topic, "/odom");
    }
}
