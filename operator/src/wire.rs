//! Byte formats shared with the vehicle that are not tierlink's own (those come from
//! `tierlink_wire`): the teleop control messages (`include/tierlink/teleop_messages.hpp`),
//! fleet_telemetry commands (`command_gate.cpp`), and the few CDR fields the screen shows.

pub use tierlink_wire::{decode_catalog, decode_header, decode_history, encode_ack, Reassembler};

const MESSAGE_VERSION: u8 = 1;

// Teleop control messages: version byte, u16-length strings.
struct Writer(Vec<u8>);

impl Writer {
    fn new() -> Self {
        Writer(vec![MESSAGE_VERSION])
    }
    fn str(mut self, s: &str) -> Self {
        self.0.extend_from_slice(&(s.len() as u16).to_le_bytes());
        self.0.extend_from_slice(s.as_bytes());
        self
    }
    fn u64(mut self, v: u64) -> Self {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    fn f64(self, v: f64) -> Self {
        self.u64(v.to_bits())
    }
    fn u8(mut self, v: u8) -> Self {
        self.0.push(v);
        self
    }
}

pub fn encode_arm(robot: &str, session: &str, epoch: u64) -> Vec<u8> {
    Writer::new().str(robot).str(session).u64(epoch).0
}

pub fn encode_stop(robot: &str, session: &str, epoch: u64) -> Vec<u8> {
    Writer::new().str(robot).str(session).u64(epoch).0
}

pub struct Motion<'a> {
    pub robot: &'a str,
    pub session: &'a str,
    pub epoch: u64,
    pub sequence: u64,
    pub sent_unix_ns: u64,
    pub valid_for_ns: u64,
    pub linear_mps: f64,
    pub angular_rps: f64,
    pub deadman: bool,
}

pub fn encode_motion(m: &Motion) -> Vec<u8> {
    Writer::new()
        .str(m.robot)
        .str(m.session)
        .u64(m.epoch)
        .u64(m.sequence)
        .u64(m.sent_unix_ns)
        .u64(m.valid_for_ns)
        .f64(m.linear_mps)
        .f64(m.angular_rps)
        .u8(m.deadman as u8)
        .0
}

pub struct ArmState {
    pub epoch: u64,
    pub armed: bool,
    pub reason: String,
}

struct Reader<'a> {
    b: &'a [u8],
    at: usize,
    ok: bool,
}

impl<'a> Reader<'a> {
    fn new(b: &'a [u8], version: u8) -> Self {
        Reader { b, at: 1, ok: b.first() == Some(&version) }
    }
    fn take(&mut self, n: usize) -> &'a [u8] {
        self.ok = self.ok && self.at + n <= self.b.len();
        if !self.ok {
            return &[];
        }
        self.at += n;
        &self.b[self.at - n..self.at]
    }
    fn u8(&mut self) -> u8 {
        self.take(1).first().copied().unwrap_or(0)
    }
    fn u64(&mut self) -> u64 {
        let b = self.take(8);
        if b.len() == 8 { u64::from_le_bytes(b.try_into().unwrap()) } else { 0 }
    }
    fn str16(&mut self) -> String {
        let n = self.take(2);
        let n = if n.len() == 2 { u16::from_le_bytes([n[0], n[1]]) as usize } else { 0 };
        String::from_utf8_lossy(self.take(n)).into_owned()
    }
    fn str64(&mut self) -> String {
        let n = self.u64() as usize;
        String::from_utf8_lossy(self.take(n)).into_owned()
    }
    fn done(&self) -> bool {
        self.ok && self.at == self.b.len()
    }
}

/// The vehicle's round trip to the server in nanoseconds (`encode_hop`).
pub fn decode_hop(b: &[u8]) -> Option<u64> {
    let mut r = Reader::new(b, MESSAGE_VERSION);
    let rtt = r.u64();
    r.done().then_some(rtt)
}

pub fn decode_state(b: &[u8]) -> Option<ArmState> {
    let mut r = Reader::new(b, MESSAGE_VERSION);
    let _robot = r.str16();
    let _session = r.str16();
    let s = ArmState { epoch: r.u64(), armed: r.u8() != 0, reason: r.str16() };
    r.done().then_some(s)
}

// fleet_telemetry commands: version byte, u64-length strings and payload.
pub fn encode_command(cmd_id: &str, interface: &str, issued_unix_ms: u64, ttl_ms: u64, payload: &[u8]) -> Vec<u8> {
    let mut out = vec![MESSAGE_VERSION];
    for s in [cmd_id.as_bytes(), interface.as_bytes()] {
        out.extend_from_slice(&(s.len() as u64).to_le_bytes());
        out.extend_from_slice(s);
    }
    out.extend_from_slice(&issued_unix_ms.to_le_bytes());
    out.extend_from_slice(&ttl_ms.to_le_bytes());
    out.extend_from_slice(&(payload.len() as u64).to_le_bytes());
    out.extend_from_slice(payload);
    out
}

pub fn decode_result(b: &[u8]) -> Option<(String, &'static str)> {
    let mut r = Reader::new(b, MESSAGE_VERSION);
    let id = r.str64();
    let status = match r.u8() {
        0 => "done",
        1 => "unknown",
        2 => "expired",
        3 => "duplicate",
        _ => "invalid",
    };
    r.done().then_some((id, status))
}

// CDR (little endian), aligned from the end of the 4-byte encapsulation header.
struct Cdr<'a> {
    b: &'a [u8],
    at: usize,
}

impl<'a> Cdr<'a> {
    fn new(b: &'a [u8]) -> Option<Self> {
        (b.len() >= 4 && b[1] == 1).then_some(Cdr { b, at: 4 })
    }
    fn align(&mut self, n: usize) {
        self.at = 4 + (self.at - 4).div_ceil(n) * n;
    }
    fn skip(&mut self, n: usize) -> Option<()> {
        (self.at + n <= self.b.len()).then(|| self.at += n)
    }
    fn u32(&mut self) -> Option<u32> {
        self.align(4);
        let v = u32::from_le_bytes(self.b.get(self.at..self.at + 4)?.try_into().ok()?);
        self.at += 4;
        Some(v)
    }
    fn f64(&mut self) -> Option<f64> {
        self.align(8);
        let v = f64::from_le_bytes(self.b.get(self.at..self.at + 8)?.try_into().ok()?);
        self.at += 8;
        Some(v)
    }
    fn string(&mut self) -> Option<()> {
        let n = self.u32()? as usize;
        self.skip(n)
    }
    fn header(&mut self) -> Option<()> {
        self.u32()?;
        self.u32()?;
        self.string()
    }
}

/// twist.twist.linear.x of nav_msgs/msg/Odometry.
pub fn odometry_speed(b: &[u8]) -> Option<f64> {
    let mut c = Cdr::new(b)?;
    c.header()?;
    c.string()?;
    for _ in 0..(7 + 36) {
        c.f64()?;
    }
    c.f64()
}

/// latitude and longitude of sensor_msgs/msg/NavSatFix.
pub fn navsatfix(b: &[u8]) -> Option<(f64, f64)> {
    let mut c = Cdr::new(b)?;
    c.header()?;
    c.skip(1)?;
    c.align(2);
    c.skip(2)?;
    Some((c.f64()?, c.f64()?))
}

/// hunter_mux_msgs/msg/CmdMode in CDR: the source hunter_mux selects (1 navigation, 2 remote).
pub fn cmd_mode_cdr(mode: i8) -> Vec<u8> {
    vec![0, 1, 0, 0, mode as u8]
}

#[derive(Clone, Copy)]
pub struct MuxStatus {
    pub mode: i8,
    pub source: i8,
    pub remote_allowed: bool,
}

/// hunter_mux_msgs/msg/MuxStatus: mode, cmd_source, remote_status and three activity flags.
pub fn mux_status(b: &[u8]) -> Option<MuxStatus> {
    let f = b.get(4..10)?;
    (b[1] == 1).then(|| MuxStatus { mode: f[0] as i8, source: f[1] as i8, remote_allowed: f[2] != 0 })
}

/// is_estop of hunter_mux_msgs/msg/EStopStatus: true while the e-stop is latched.
pub fn estop_latched(b: &[u8]) -> Option<bool> {
    let f = b.get(4..7)?;
    (b[1] == 1).then_some(f[0] != 0)
}

#[cfg(test)]
mod tests {
    use super::*;

    // The same bytes as TeleopMessages.LayoutMatchesTheOperator in test/test_teleop_messages.cpp.
    #[test]
    fn arm_layout_matches_the_vehicle() {
        assert_eq!(encode_arm("r01", "s", 7), [1, 3, 0, b'r', b'0', b'1', 1, 0, b's', 7, 0, 0, 0, 0, 0, 0, 0]);
    }

    #[test]
    fn state_round_trips_the_vehicle_layout() {
        let mut b = vec![1u8];
        for s in ["r01", "s"] {
            b.extend_from_slice(&(s.len() as u16).to_le_bytes());
            b.extend_from_slice(s.as_bytes());
        }
        b.extend_from_slice(&7u64.to_le_bytes());
        b.push(1);
        b.extend_from_slice(&0u16.to_le_bytes());
        let s = decode_state(&b).unwrap();
        assert!(s.armed && s.epoch == 7 && s.reason.is_empty());
        assert!(decode_state(&b[..b.len() - 1]).is_none());
    }

    #[test]
    fn hop_and_mux_layouts() {
        let mut b = vec![1u8];
        b.extend_from_slice(&42_000_000u64.to_le_bytes());
        assert_eq!(decode_hop(&b), Some(42_000_000));
        assert!(decode_hop(&b[..8]).is_none());
        let s = mux_status(&[0, 1, 0, 0, 2, 1, 1, 0, 1, 1]).unwrap();
        assert!(s.mode == 2 && s.source == 1 && s.remote_allowed);
        assert_eq!(estop_latched(&[0, 1, 0, 0, 1, 0, 0]), Some(true));
        assert!(estop_latched(&[0, 1, 0, 0, 1]).is_none());
    }
}
