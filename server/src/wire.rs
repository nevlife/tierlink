//! The tierlink byte formats that the server reads and writes (include/tierlink/wire.hpp).

pub struct Header {
    pub seq: u64,
    pub stream_seq: u64,
    pub chunk: u16,
    pub chunks: u16,
    pub fec_group: u8,
}

fn u64_at(b: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(b[at..at + 8].try_into().unwrap())
}

fn u16_at(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([b[at], b[at + 1]])
}

/// The data header in the Zenoh attachment: seq, stream_seq, sent_ns, keyframe, chunk, chunks,
/// fec_group. A chunk at or past `chunks` is a parity piece.
pub fn decode_header(b: &[u8]) -> Option<Header> {
    if b.len() != 30 {
        return None;
    }
    let h = Header { seq: u64_at(b, 0), stream_seq: u64_at(b, 8), chunk: u16_at(b, 25), chunks: u16_at(b, 27), fec_group: b[29] };
    (h.chunks > 0 && h.chunk < h.chunks + parity_count(h.chunks, h.fec_group)).then_some(h)
}

/// Parity pieces of an item: one per group of `fec_group` data pieces (tierlink `parity_count`).
pub fn parity_count(chunks: u16, fec_group: u8) -> u16 {
    if fec_group == 0 { 0 } else { chunks.div_ceil(fec_group as u16) }
}

/// One stream's pieces back into items (tierlink `Reassembler`), rebuilding one lost piece per
/// FEC group from its XOR parity: a newer item drops an unfinished older one, and a late piece of
/// an older item is ignored.
#[derive(Default)]
pub struct Reassembler {
    newest: Option<u64>,
    fec_group: u8,
    pieces: Vec<Option<Vec<u8>>>,
    parity: Vec<Option<Vec<u8>>>,
    received: usize,
    done: bool,
}

impl Reassembler {
    pub fn add(&mut self, h: &Header, piece: Vec<u8>) -> Option<Vec<u8>> {
        if self.newest.is_some_and(|n| h.stream_seq < n) {
            return None;
        }
        if h.chunks == 1 && h.fec_group == 0 {
            self.newest = Some(h.stream_seq);
            self.done = true;
            return Some(piece);
        }
        if self.newest != Some(h.stream_seq) || self.pieces.len() != h.chunks as usize || self.fec_group != h.fec_group {
            self.newest = Some(h.stream_seq);
            self.fec_group = h.fec_group;
            self.pieces = vec![None; h.chunks as usize];
            self.parity = vec![None; parity_count(h.chunks, h.fec_group) as usize];
            self.received = 0;
            self.done = false;
        }
        if self.done || piece.is_empty() {
            return None;
        }
        let group = if h.chunk >= h.chunks {
            let g = (h.chunk - h.chunks) as usize;
            if self.parity[g].is_some() {
                return None;
            }
            self.parity[g] = Some(piece);
            g
        } else {
            let slot = &mut self.pieces[h.chunk as usize];
            if slot.is_some() {
                return None;
            }
            *slot = Some(piece);
            self.received += 1;
            if self.fec_group == 0 { 0 } else { h.chunk as usize / self.fec_group as usize }
        };
        if self.fec_group != 0 {
            self.recover(group);
        }
        if self.received < self.pieces.len() {
            return None;
        }
        self.done = true;
        self.parity.clear();
        Some(std::mem::take(&mut self.pieces).into_iter().flatten().flatten().collect())
    }

    /// The parity holds the XOR of the group's sizes (2 bytes) and of its bytes padded to the
    /// longest, so with one piece missing the others give it back.
    fn recover(&mut self, group: usize) {
        let Some(parity) = &self.parity[group] else { return };
        if parity.len() < 2 {
            return;
        }
        let first = group * self.fec_group as usize;
        let last = (first + self.fec_group as usize).min(self.pieces.len());
        let missing: Vec<usize> = (first..last).filter(|&i| self.pieces[i].is_none()).collect();
        if missing.len() != 1 {
            return;
        }
        let mut size = u16::from_le_bytes([parity[0], parity[1]]) as usize;
        let mut out = parity[2..].to_vec();
        for p in self.pieces[first..last].iter().flatten() {
            size ^= p.len();
            for (o, b) in out.iter_mut().zip(p) {
                *o ^= b;
            }
        }
        if size > out.len() {
            return;
        }
        out.truncate(size);
        self.pieces[missing[0]] = Some(out);
        self.received += 1;
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
    fn a_parity_piece_rebuilds_one_lost_piece_of_its_group() {
        // Pieces {1,2,3} {4,5} in groups of two with parity after each group; the last is short.
        let data: Vec<Vec<u8>> = vec![vec![1, 2, 3], vec![4, 5, 6], vec![7, 8]];
        let parity = |group: &[Vec<u8>]| {
            let mut p = vec![0u8; 2 + group.iter().map(|g| g.len()).max().unwrap()];
            for g in group {
                p[0] ^= g.len() as u8;
                for (o, b) in p[2..].iter_mut().zip(g) {
                    *o ^= b;
                }
            }
            p
        };
        let sent = [(0, data[0].clone()), (1, data[1].clone()), (3, parity(&data[0..2])), (2, data[2].clone()), (4, parity(&data[2..3]))];
        for lost in 0..sent.len() {
            let mut r = Reassembler::default();
            let mut got = None;
            for (i, (chunk, p)) in sent.iter().enumerate() {
                if i != lost {
                    let h = Header { seq: i as u64, stream_seq: 7, chunk: *chunk, chunks: 3, fec_group: 2 };
                    got = r.add(&h, p.clone()).or(got);
                }
            }
            assert_eq!(got, Some(vec![1, 2, 3, 4, 5, 6, 7, 8]), "lost {lost}");
        }
    }

    #[test]
    fn header_ack_and_catalog_layouts() {
        let mut b = 3u64.to_le_bytes().to_vec();
        b.extend_from_slice(&9u64.to_le_bytes());
        b.extend_from_slice(&5u64.to_le_bytes());
        b.push(0);
        b.extend_from_slice(&1u16.to_le_bytes());
        b.extend_from_slice(&2u16.to_le_bytes());
        b.push(0);
        let h = decode_header(&b).unwrap();
        assert!(h.seq == 3 && h.stream_seq == 9 && h.chunk == 1 && h.chunks == 2);
        assert!(decode_header(&b[..29]).is_none());
        let mut r = Reassembler::default();
        let piece = |seq, chunk, chunks| Header { seq, stream_seq: seq, chunk, chunks, fec_group: 0 };
        assert!(r.add(&piece(4, 1, 2), vec![3]).is_none());
        assert!(r.add(&piece(3, 0, 2), vec![9]).is_none());
        assert_eq!(r.add(&piece(4, 0, 2), vec![1, 2]), Some(vec![1, 2, 3]));
        assert_eq!(&encode_ack(3, 4)[8..], &4u64.to_le_bytes());
        let c = decode_catalog(b"odom\tnav_msgs/msg/Odometry\t/odom\tlatest\nbad line\n");
        assert_eq!(c.len(), 1);
        assert_eq!(c[0].topic, "/odom");
    }
}
